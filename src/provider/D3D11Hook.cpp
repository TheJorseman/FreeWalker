// WalkerCore — D3D11 hook implementation (M2 lander, real).
//
// Uses MinHook (vendored at third_party/minhook) to patch the d3d11.dll
// entry D3D11CreateDeviceAndSwapChain. Our wrapper captures the
// IDXGISwapChain that the game creates, then patches the swap chain's
// Present() vtable entry so we run one FrameLoop tick per frame.
//
// ABI note: this file uses raw COM vtable types. The d3d11.h and dxgi.h
// headers come with the Windows SDK; MinHook requires only <windows.h>.
//
// Lifetime: the device, swap chain and context returned by
// D3D11CreateDeviceAndSwapChain are owned by the game (the caller holds
// the reference that was AddRef'd by D3D11CreateDeviceAndSwapChain).
// We only borrow them — no Release() at Uninstall — because the game
// decides when to release. A generation counter (g_swap_chain_gen)
// guards against the rare case where the game destroys and recreates
// the swap chain between our capture and a Present() call.
//
// When WALKERCORE_HAVE_MINHOOK is not defined (MinHook not vendored),
// the class becomes a no-op stub so the rest of the build still compiles.

#include "walkercore/D3D11Hook.h"

#include <windows.h>

#include "walkercore/FrameLoop.h"
#include "walkercore/ImGuiOverlay.h"
#include "walkercore/Log.h"
#include "walkercore/PerFrame.h"
#include "walkercore/PerFrameThread.h"
#include "walkercore/Time.h"

#if defined(WALKERCORE_HAVE_MINHOOK)
#include <d3d11.h>
#include <dxgi.h>
#include "MinHook.h"
#endif

#include "MinHookInit.h"

namespace walkercore {

// g_swap_chain_gen is always defined so CurrentSwapChainGeneration() works
// in both DLL and test builds. The test build doesn't define
// WALKERCORE_HAVE_MINHOOK so we can't reference it inside the gated
// block. It's only ever incremented in the real-implementation branch.
uint64_t g_swap_chain_gen = 0;

#if defined(WALKERCORE_HAVE_MINHOOK)

namespace {

// Real signature (verified Microsoft Learn 2024-12):
//   HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
//       IDXGIAdapter              *pAdapter,
//       D3D_DRIVER_TYPE            DriverType,
//       HMODULE                    Software,
//       UINT                       Flags,
//       const D3D_FEATURE_LEVEL   *pFeatureLevels,
//       UINT                       FeatureLevels,
//       UINT                       SDKVersion,
//       const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
//       IDXGISwapChain           **ppSwapChain,
//       ID3D11Device             **ppDevice,
//       D3D_FEATURE_LEVEL         *pFeatureLevel,
//       ID3D11DeviceContext      **ppImmediateContext);
typedef HRESULT(WINAPI *CreateFn)(IDXGIAdapter *,
                                  D3D_DRIVER_TYPE,
                                  HMODULE,
                                  UINT,
                                  const D3D_FEATURE_LEVEL *, UINT, UINT,
                                  const DXGI_SWAP_CHAIN_DESC *,
                                  IDXGISwapChain **,
                                  ID3D11Device **,
                                  D3D_FEATURE_LEVEL *,
                                  ID3D11DeviceContext **);

// COM vtable typedef for IDXGISwapChain::Present. Method index 8 in the
// IDXGISwapChain vtable.
typedef HRESULT(STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *self,
                                              UINT sync_interval,
                                              UINT present_flags);

// IDXGISwapChain::ResizeBuffers (vtable slot 13, verified dxgi.h
// line 1579). Mirrors the D3D12Hook path so both rendering backends
// benefit from the cached aspect and avoid the per-frame GetDesc.
typedef HRESULT(STDMETHODCALLTYPE *ResizeBuffersFn)(IDXGISwapChain *,
                                                    UINT,
                                                    UINT,
                                                    UINT,
                                                    DXGI_FORMAT,
                                                    UINT);

// Original (unhooked) function pointers. Saved so we can chain.
PresentFn g_original_present = nullptr;
ResizeBuffersFn g_original_resize_buffers = nullptr;
void *g_original_d3d11_create = nullptr;

// Captured device/swap chain (borrowed, not owned). Single-instance —
// WalkerCore assumes the game has exactly one D3D11 device and one
// swap chain.
ID3D11Device *g_device = nullptr;
IDXGISwapChain *g_swap_chain = nullptr;
ID3D11DeviceContext *g_context = nullptr;

// TRUE once the Present vtable entry has been patched.
bool g_present_hooked = false;
bool g_resize_buffers_hooked = false;

// Find the top-level window owned by the current process. Used
// to wire ImGui_ImplWin32_Init to the game's HWND. The game
// window is almost always the foreground window when SCS
// plugins load (the loader runs the telemetry after graphics
// init), but a foreground-only lookup would miss minimised
// sessions, so we fall back to EnumWindows +
// GetWindowThreadProcessId.
struct FindGameWindowContext {
    DWORD pid;
    HWND result;
};

static BOOL CALLBACK FindGameWindowEnumProc(HWND hwnd, LPARAM lparam) {
    auto *ctx = reinterpret_cast<FindGameWindowContext *>(lparam);
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != ctx->pid) return TRUE;  // keep enumerating
    // Skip invisible windows (the foreground game window is
    // always WS_VISIBLE). Some games have a hidden owner
    // window that GetParent(hwnd) != nullptr would catch, but
    // the visible filter is sufficient for ATS / ETS2.
    if (!IsWindowVisible(hwnd)) return TRUE;
    ctx->result = hwnd;
    return FALSE;  // stop enumerating
}

HWND FindGameWindowHandle() {
    FindGameWindowContext ctx{GetCurrentProcessId(), nullptr};
    EnumWindows(&FindGameWindowEnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

// Cached swap-chain aspect, populated by HookedResizeBuffers (and
// by HookedPresent's first call as a fallback). The sentinel -1.0
// is unreachable as a real aspect ratio in any sane game config
// (a 1:1 aspect is 1.0, an ultrawide 21:9 is ~2.33), so the
// "unset" state is unambiguous. Mirrors D3D12Hook's cache.
float g_cached_aspect = -1.0f;
uint32_t g_cached_width = 0;
uint32_t g_cached_height = 0;

constexpr float kAspectUnset = -1.0f;
constexpr float kDefaultAspect = 16.0f / 9.0f;

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain *self,
                                              UINT buffer_count,
                                              UINT width,
                                              UINT height,
                                              DXGI_FORMAT new_format,
                                              UINT flags) {
    // Update the cached dimensions before chaining to the original.
    // The original will tear down the old back buffers and create new
    // ones at the requested size; the g_cached_* values reflect the
    // request the game made, which is what the next Present will
    // actually see. We update first, chain second: even if the
    // original fails and the swap chain keeps its old dimensions,
    // the next Present's first-call GetDesc will overwrite the cache
    // with the real values, so the worst case is one frame of stale
    // aspect. See D3D12Hook::HookedResizeBuffers for the symmetric
    // rationale.
    g_cached_width = width;
    g_cached_height = height;
    g_cached_aspect = (height > 0)
                           ? static_cast<float>(width) /
                                 static_cast<float>(height)
                           : kDefaultAspect;
    if (g_original_resize_buffers != nullptr) {
        return g_original_resize_buffers(self, buffer_count, width, height,
                                          new_format, flags);
    }
    return E_FAIL;
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain *self,
                                        UINT sync_interval,
                                        UINT present_flags) {
    // Compute dt from the wall clock. On the very first call, dt = 0;
    // subsequent calls measure the delta since the last Present.
    static uint64_t last_us = 0;
    const uint64_t now_us = NowMicros();
    const double dt = (last_us == 0)
                          ? 0.0
                          : static_cast<double>(now_us - last_us) / 1e6;
    last_us = now_us;

    // Aspect from the cache, populated by HookedResizeBuffers. The
    // first Present after Install (or after a game-side bypass of
    // ResizeBuffers) reads the swap-chain's current dimensions
    // directly via GetDesc, so the cache is correct from frame 2
    // onward. Per-frame cost is now a single float read instead of
    // a struct-population + validation in GetDesc. Mirrors the
    // D3D12Hook path; both share the same ResizeBuffers vtable slot
    // (13, verified dxgi.h).
    if (g_cached_aspect == kAspectUnset && self != nullptr) {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (SUCCEEDED(self->GetDesc(&desc))) {
            g_cached_width = desc.BufferDesc.Width;
            g_cached_height = desc.BufferDesc.Height;
            g_cached_aspect = (g_cached_height > 0)
                                   ? static_cast<float>(g_cached_width) /
                                         static_cast<float>(g_cached_height)
                                   : kDefaultAspect;
        }
    }
    if (g_cached_aspect != kAspectUnset) {
        walker_set_swapchain_aspect(g_cached_aspect);
    }

    // Per-frame simulation tick.
    //
    // The CAS lock coordinates with the background PerFrameThread ticker.
    // Whoever claims the slot first runs the tick; the other skips this
    // frame. When the game's swap chain was created BEFORE our DLL
    // loaded (the SCS loader runs plugins AFTER graphics init, so this
    // is the normal case for ATS 1.61.x), `HookedPresent` never fires
    // and the background ticker carries the whole frame loop. Either
    // way, only one tick runs per frame.
    if (walker_try_enter_per_frame_tick() != 0) {
        walker_per_frame_tick(dt);
        walker_exit_per_frame_tick();
    }

    // M11 ImGui overlay. Render AFTER the per-frame tick so the
    // walker's walk-entered counter and the F10 toggle state are
    // current when the overlay draws its status line. No-op when
    // the overlay is disabled (default), the D3D11 device has not
    // been captured, or WALKERCORE_HAVE_IMGUI=0. Mirrors the
    // D3D12Hook path's symmetric RenderFrameIfEnabled call.
    walkercore::RenderImGuiFrameIfEnabled();

    // Chain to the original. We deliberately do NOT modify any D3D state
    // here yet — pushing view/projection constants into the game's shader
    // pipeline is the M2 lander's next deliverable. The walker camera
    // state is exposed via walker_camera() for the integration step.
    if (g_original_present != nullptr) {
        return g_original_present(self, sync_interval, present_flags);
    }
    return E_FAIL;
}

// Helper: hook the Present vtable slot on the captured swap chain.
bool HookPresent(IDXGISwapChain *sc) {
    if (sc == nullptr || g_present_hooked) return false;

    // The COM vtable layout has Present at index 8 for IDXGISwapChain.
    // We cast through LPVOID * and read the function pointer.
    void **vtable = *reinterpret_cast<void ***>(sc);
    void *present_slot = vtable[8];
    if (MH_CreateHook(present_slot,
                      reinterpret_cast<void *>(&HookedPresent),
                      reinterpret_cast<void **>(&g_original_present)) != MH_OK) {
        WALKERCORE_LOG_ERROR("D3D11Hook: MH_CreateHook on Present failed");
        return false;
    }
    if (MH_EnableHook(present_slot) != MH_OK) {
        WALKERCORE_LOG_ERROR("D3D11Hook: MH_EnableHook on Present failed");
        return false;
    }
    g_present_hooked = true;

    // ResizeBuffers hook (slot 13). Same graceful-degrade contract
    // as D3D12Hook: if MH_CreateHook fails, the per-frame Present
    // path falls back to its first-call GetDesc and everything
    // keeps working. See D3D12Hook::HookPresentOnD3D12SwapChain
    // for the symmetric rationale.
    if (!g_resize_buffers_hooked) {
        void *resize_slot = vtable[13];
        if (MH_CreateHook(resize_slot,
                          reinterpret_cast<void *>(&HookedResizeBuffers),
                          reinterpret_cast<void **>(&g_original_resize_buffers)) == MH_OK) {
            if (MH_EnableHook(resize_slot) == MH_OK) {
                g_resize_buffers_hooked = true;
                WALKERCORE_LOG_INFO("D3D11Hook: ResizeBuffers hooked");
            } else {
                WALKERCORE_LOG_WARN(
                    "D3D11Hook: MH_EnableHook on ResizeBuffers failed "
                    "(non-fatal; per-frame path falls back to GetDesc)");
                MH_DisableHook(resize_slot);
            }
        } else {
            WALKERCORE_LOG_WARN(
                "D3D11Hook: MH_CreateHook on ResizeBuffers failed "
                "(non-fatal; per-frame path falls back to GetDesc)");
        }
    }

    WALKERCORE_LOG_INFO("D3D11Hook: Present hooked");
    return true;
}

// Our wrapper for D3D11CreateDeviceAndSwapChain. Calls the original; on
// success, captures the device + swap chain and hooks Present. The
// signature must match the real d3d11.dll export exactly, including
// DriverType and Software between pAdapter and Flags — otherwise the
// trampoline reads the arguments at the wrong stack positions and
// dereferences an integer as a pointer.
HRESULT WINAPI HookedCreateDeviceAndSwapChain(
    IDXGIAdapter *p_adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL *feature_levels, UINT feature_level_count,
    UINT sdk_version,
    const DXGI_SWAP_CHAIN_DESC *swap_chain_desc,
    IDXGISwapChain **swap_chain,
    ID3D11Device **device,
    D3D_FEATURE_LEVEL *returned_feature_level,
    ID3D11DeviceContext **immediate_context) {

    auto original = reinterpret_cast<CreateFn>(g_original_d3d11_create);
    const HRESULT hr = original(
        p_adapter, driver_type, software, flags, feature_levels,
        feature_level_count, sdk_version, swap_chain_desc, swap_chain, device,
        returned_feature_level, immediate_context);
    if (SUCCEEDED(hr)) {
        if (swap_chain != nullptr && *swap_chain != nullptr) {
            g_swap_chain = *swap_chain;
        }
        if (device != nullptr && *device != nullptr) {
            // If the game is replacing an existing D3D11 device
            // (rare but possible on graphics-restart), release the
            // ImGui GPU resources first. The new device init comes
            // through OnD3D11DeviceCaptured below. This is the
            // D3D11 equivalent of IDXGISwapChain::ResizeBuffers
            // freeing the old back buffers.
            if (g_device != nullptr && g_device != *device) {
                walkercore::OnD3D11DeviceLost();
            }
            g_device = *device;
            // GetImmediateContext AddRef's the context. We must release
            // our extra reference on uninstall, but not the device/swap
            // chain (the game owns those).
            if (g_context != nullptr) {
                g_context->Release();
                g_context = nullptr;
            }
            (*device)->GetImmediateContext(&g_context);
        }
        ++g_swap_chain_gen;
        WALKERCORE_LOG_INFO(
            "D3D11Hook: captured device %p swap chain %p (gen %llu)",
            g_device, g_swap_chain,
            static_cast<unsigned long long>(g_swap_chain_gen));
        HookPresent(g_swap_chain);

        // M11 ImGui overlay. Initialize now that we have a D3D11
        // device + context + swap chain. The HWND comes from
        // EnumWindows (the game's top-level window for the current
        // process). When EnumWindows fails (rare — headless
        // services, dedicated server builds) we pass nullptr and
        // the overlay still creates the ImGui context but stays
        // supported=false so the render path is off.
        HWND game_hwnd = nullptr;
        if (swap_chain != nullptr && *swap_chain != nullptr) {
            game_hwnd = FindGameWindowHandle();
        }
        walkercore::OnD3D11DeviceCaptured(g_device, g_context, game_hwnd);
    }
    return hr;
}

}  // namespace

#endif  // WALKERCORE_HAVE_MINHOOK

uint64_t CurrentSwapChainGeneration() {
    return g_swap_chain_gen;
}

D3D11Hook::D3D11Hook() = default;

D3D11Hook::~D3D11Hook() {
    if (installed_) {
        Uninstall();
    }
}

void D3D11Hook::BindFrameLoop(FrameLoop *frame_loop) {
    frame_loop_ = frame_loop;
}

bool D3D11Hook::Install() {
    if (installed_) {
        return false;
    }
#if defined(WALKERCORE_HAVE_MINHOOK)
    if (!walkercore::MinHookInit()) {
        WALKERCORE_LOG_ERROR("D3D11Hook::Install: MinHookInit failed");
        return false;
    }
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (d3d11 == nullptr) {
        // Game might not have loaded d3d11 yet. Defer. Re-trying is
        // idempotent because installed_ stays false. Release the
        // MinHook ref the call above took — the per-provider
        // bail-out contract is "leave no global state perturbed".
        walkercore::MinHookShutdown();
        WALKERCORE_LOG_WARN(
            "D3D11Hook::Install: d3d11.dll not loaded yet — retry later");
        return false;
    }
    void *create_addr = reinterpret_cast<void *>(
        GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    if (create_addr == nullptr) {
        walkercore::MinHookShutdown();
        WALKERCORE_LOG_ERROR(
            "D3D11Hook::Install: GetProcAddress(D3D11CreateDeviceAndSwapChain) failed");
        return false;
    }
    if (MH_CreateHook(create_addr,
                      reinterpret_cast<void *>(&HookedCreateDeviceAndSwapChain),
                      reinterpret_cast<void **>(&g_original_d3d11_create)) !=
        MH_OK) {
        walkercore::MinHookShutdown();
        WALKERCORE_LOG_ERROR(
            "D3D11Hook::Install: MH_CreateHook(D3D11CreateDeviceAndSwapChain) failed");
        return false;
    }
    if (MH_EnableHook(create_addr) != MH_OK) {
        walkercore::MinHookShutdown();
        WALKERCORE_LOG_ERROR(
            "D3D11Hook::Install: MH_EnableHook(D3D11CreateDeviceAndSwapChain) failed");
        return false;
    }
    installed_ = true;
    WALKERCORE_LOG_INFO(
        "D3D11Hook::Install: hooked D3D11CreateDeviceAndSwapChain");
    return true;
#else
    WALKERCORE_LOG_WARN(
        "D3D11Hook::Install: stub — MinHook not vendored. "
        "Run `git clone --depth=1 https://github.com/TsudaKageyu/minhook.git "
        "third_party/minhook` and rebuild.");
    installed_ = true;
    return true;
#endif
}

void D3D11Hook::Uninstall() {
    if (!installed_) {
        return;
    }
    // M11 ImGui overlay. Shutdown BEFORE we release the D3D11
    // context reference so the ImGui_ImplDX11_Shutdown can still
    // call context->Release on the font texture view. After this
    // call, the ImGui context is destroyed and the render path
    // becomes a no-op (RenderImGuiFrameIfEnabled checks the
    // supported_ flag and skips).
    walkercore::ShutdownImGuiOverlay();
#if defined(WALKERCORE_HAVE_MINHOOK)
    if (g_present_hooked) {
        // Only disable OUR hooks (D3D11 lander). The D3D12 lander
        // shares this MinHook instance via the refcount helper; if it
        // is still installed we MUST NOT call MH_DisableHook(MH_ALL_HOOKS)
        // because that would tear out the D3D12 swap-chain vtable
        // patch as well, blanking the screen in D3D12 games.
        // MH_DisableHook on the specific function pointer we hooked
        // is the safe path.
        MH_DisableHook(reinterpret_cast<void *>(&HookedCreateDeviceAndSwapChain));
        MH_DisableHook(reinterpret_cast<void *>(&HookedPresent));
        if (g_resize_buffers_hooked) {
            MH_DisableHook(reinterpret_cast<void *>(&HookedResizeBuffers));
        }
        g_present_hooked = false;
    }
    walkercore::MinHookShutdown();
    // Release ONLY the context reference we obtained from
    // GetImmediateContext. The device and swap chain were returned to
    // the game and the game owns them — releasing here would corrupt
    // the game's reference counts and could crash the renderer when
    // the game later tries to use them.
    if (g_context != nullptr) {
        g_context->Release();
        g_context = nullptr;
    }
    g_device = nullptr;
    g_swap_chain = nullptr;
    g_original_present = nullptr;
    g_original_d3d11_create = nullptr;
    g_original_resize_buffers = nullptr;
    g_resize_buffers_hooked = false;
    g_cached_aspect = kAspectUnset;
    g_cached_width = 0;
    g_cached_height = 0;
    WALKERCORE_LOG_INFO("D3D11Hook::Uninstall: hooks removed");
#endif
    installed_ = false;
}

}  // namespace walkercore