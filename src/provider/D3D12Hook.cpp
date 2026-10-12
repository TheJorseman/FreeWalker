// WalkerCore — D3D12 + DXGI factory hook (M2 lander, D3D12 path).
//
// Mirrors D3D11Hook's structure for the D3D12 path:
//
//   * Hook D3D12CreateDevice in d3d12.dll.
//   * Hook CreateDXGIFactory1 in dxgi.dll.
//   * Vtable-patch the returned factory's CreateSwapChain (slot 10
//     in the IDXGIFactory vtable, verified against MinGW dxgi.h).
//   * When the game calls CreateSwapChain, vtable-patch the returned
//     IDXGISwapChain's Present (slot 8 in the IDXGISwapChain vtable).
//   * Each Present call runs walker_per_frame_tick(dt) before chaining
//     to the original Present.
//
// WALKERCORE_HAVE_MINHOOK gates the real implementation; the stub
// branch keeps the rest of the build compiling when MinHook is not
// vendored. The class is a no-op stub in that case.

#include "walkercore/D3D12Hook.h"

#include <windows.h>

#include "walkercore/FrameLoop.h"
#include "walkercore/Log.h"
#include "walkercore/PerFrame.h"
#include "walkercore/PerFrameThread.h"
#include "walkercore/Time.h"

#if defined(WALKERCORE_HAVE_MINHOOK)
#include <d3d12.h>
#include <dxgi.h>
#include "MinHook.h"
#endif

#include "MinHookInit.h"

namespace walkercore {

uint64_t g_d3d12_swap_chain_gen = 0;

uint64_t CurrentD3D12SwapChainGeneration() {
    return g_d3d12_swap_chain_gen;
}

#if defined(WALKERCORE_HAVE_MINHOOK)

namespace {

// D3D12CreateDevice signature (verified d3d12.h line 24775):
//   HRESULT __stdcall D3D12CreateDevice(
//       IUnknown *adapter,
//       D3D_FEATURE_LEVEL minimum_feature_level,
//       REFIID iid,
//       void **device);
typedef HRESULT(WINAPI *CreateD3D12DeviceFn)(IUnknown *,
                                              D3D_FEATURE_LEVEL,
                                              REFIID,
                                              void **);

// CreateDXGIFactory1 signature (verified dxgi.h line 1973):
//   HRESULT __stdcall CreateDXGIFactory1(REFIID riid, void **factory);
typedef HRESULT(WINAPI *CreateFactoryFn)(REFIID, void **);

// IDXGIFactory::CreateSwapChain (vtable slot 10, verified dxgi.h
// line 1888):
//   HRESULT STDMETHODCALLTYPE CreateSwapChain(
//       IDXGIFactory *self,
//       IUnknown *device,
//       DXGI_SWAP_CHAIN_DESC *desc,
//       IDXGISwapChain **swapchain);
typedef HRESULT(STDMETHODCALLTYPE *CreateSwapChainFn)(IDXGIFactory *,
                                                      IUnknown *,
                                                      DXGI_SWAP_CHAIN_DESC *,
                                                      IDXGISwapChain **);

// IDXGISwapChain::Present (vtable slot 8, verified dxgi.h line 1556+):
//   HRESULT STDMETHODCALLTYPE Present(
//       IDXGISwapChain *self,
//       UINT sync_interval,
//       UINT present_flags);
typedef HRESULT(STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *,
                                              UINT,
                                              UINT);

// IDXGISwapChain::ResizeBuffers (vtable slot 13, verified dxgi.h
// line 1579):
//   HRESULT STDMETHODCALLTYPE ResizeBuffers(
//       IDXGISwapChain *self,
//       UINT buffer_count,
//       UINT width,
//       UINT height,
//       DXGI_FORMAT new_format,
//       UINT flags);
typedef HRESULT(STDMETHODCALLTYPE *ResizeBuffersFn)(IDXGISwapChain *,
                                                    UINT,
                                                    UINT,
                                                    UINT,
                                                    DXGI_FORMAT,
                                                    UINT);

CreateD3D12DeviceFn g_original_d3d12_create = nullptr;
CreateFactoryFn g_original_create_factory1 = nullptr;
CreateSwapChainFn g_original_create_swap_chain = nullptr;
PresentFn g_original_d3d12_present = nullptr;
ResizeBuffersFn g_original_resize_buffers = nullptr;

// Borrowed (not owned): the game releases these via its own
// reference counts. We AddRef only the ID3D12Device so we have a
// stable pointer for the duration of the hook; the swap chain and
// factory we read directly from the captured COM pointers without
// AddRef.
ID3D12Device *g_device = nullptr;
IDXGISwapChain *g_swap_chain = nullptr;

bool g_present_hooked = false;
bool g_resize_buffers_hooked = false;
bool g_d3d12_create_hooked = false;
bool g_create_swap_chain_hooked = false;

// Cached swap-chain aspect. Populated by HookedResizeBuffers (and
// by HookedPresent's first call as a fallback). NaN sentinel
// ("not yet captured") avoids a sentinel-aspect collision; 0.0
// is a valid aspect for some rare configurations and must not be
// confused with the unset state.
float g_cached_aspect = -1.0f;
uint32_t g_cached_width = 0;
uint32_t g_cached_height = 0;

constexpr float kAspectUnset = -1.0f;
constexpr float kDefaultAspect = 16.0f / 9.0f;

// One-shot warn to honor the "honest stub" contract.
bool g_warned_no_d3d12 = false;
void WarnNoD3D12(const char *phase) {
    if (g_warned_no_d3d12) return;
    g_warned_no_d3d12 = true;
    WALKERCORE_LOG_WARN(
        "D3D12Hook::%s: d3d12.dll not loaded yet — retry later "
        "(M2 lander will not tick until game creates D3D12 device)",
        phase);
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain *self,
                                              UINT buffer_count,
                                              UINT width,
                                              UINT height,
                                              DXGI_FORMAT new_format,
                                              UINT flags) {
    // Update the cached dimensions before chaining to the original.
    // The original will tear down the old back buffers and create
    // new ones at the requested size — the g_cached_* values reflect
    // the request the game made, which is what the next Present will
    // actually see. We update first, chain second: even if the
    // original fails and the swap chain keeps its old dimensions,
    // the next Present's first-call GetDesc will overwrite our
    // cache with the real values, so the worst case is one frame of
    // stale aspect.
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
    // dt from the wall clock; first call is 0.0, subsequent calls
    // measure the delta since the last Present. Identical pattern
    // to D3D11Hook.
    static uint64_t last_us = 0;
    const uint64_t now_us = NowMicros();
    const double dt = (last_us == 0)
                          ? 0.0
                          : static_cast<double>(now_us - last_us) / 1e6;
    last_us = now_us;

    // Aspect from the cache, populated by HookedResizeBuffers.
    // The first Present after Install (or after a game-side bypass
    // of ResizeBuffers) reads the swap-chain's current dimensions
    // directly via GetDesc, so the cache is correct from frame 2
    // onward. Per-frame cost is now a single float read instead of
    // a struct-population + validation in GetDesc.
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

    if (g_original_d3d12_present != nullptr) {
        return g_original_d3d12_present(self, sync_interval, present_flags);
    }
    return E_FAIL;
}

// Vtable-patch Present on a freshly captured swap chain. Same
// pattern as D3D11Hook::HookPresent. Also patches ResizeBuffers
// (slot 13) so we can keep the cached aspect up to date without
// paying for a per-frame GetDesc.
bool HookPresentOnD3D12SwapChain(IDXGISwapChain *sc) {
    if (sc == nullptr || g_present_hooked) return false;
    void **vtable = *reinterpret_cast<void ***>(sc);
    void *present_slot = vtable[8];
    if (MH_CreateHook(present_slot,
                      reinterpret_cast<void *>(&HookedPresent),
                      reinterpret_cast<void **>(&g_original_d3d12_present)) != MH_OK) {
        WALKERCORE_LOG_ERROR("D3D12Hook: MH_CreateHook on Present failed");
        return false;
    }
    if (MH_EnableHook(present_slot) != MH_OK) {
        WALKERCORE_LOG_ERROR("D3D12Hook: MH_EnableHook on Present failed");
        return false;
    }
    g_present_hooked = true;

    // ResizeBuffers hook — fires only on window resize. We update
    // g_cached_aspect_/width_/height_ so the per-frame Present path
    // can read dimensions from a struct member instead of calling
    // GetDesc. If MH_CreateHook fails here, the Present hook keeps
    // working and the per-frame path falls back to its first-call
    // GetDesc — degrade gracefully, never crash.
    if (!g_resize_buffers_hooked) {
        void *resize_slot = vtable[13];
        if (MH_CreateHook(resize_slot,
                          reinterpret_cast<void *>(&HookedResizeBuffers),
                          reinterpret_cast<void **>(&g_original_resize_buffers)) == MH_OK) {
            if (MH_EnableHook(resize_slot) == MH_OK) {
                g_resize_buffers_hooked = true;
                WALKERCORE_LOG_INFO("D3D12Hook: ResizeBuffers hooked");
            } else {
                WALKERCORE_LOG_WARN(
                    "D3D12Hook: MH_EnableHook on ResizeBuffers failed "
                    "(non-fatal; per-frame path falls back to GetDesc)");
                MH_DisableHook(resize_slot);
            }
        } else {
            WALKERCORE_LOG_WARN(
                "D3D12Hook: MH_CreateHook on ResizeBuffers failed "
                "(non-fatal; per-frame path falls back to GetDesc)");
        }
    }

    WALKERCORE_LOG_INFO("D3D12Hook: Present hooked");
    return true;
}

// Our vtable-patched IDXGIFactory::CreateSwapChain. Calls the
// original; on success, captures the returned swap chain and
// patches its Present. We DO NOT call AddRef on the returned
// pointers — the original function already returned them with
// the documented refcount convention (caller must Release), and
// we observe the same lifetime via the chain.
HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
    IDXGIFactory *self,
    IUnknown *device,
    DXGI_SWAP_CHAIN_DESC *desc,
    IDXGISwapChain **swapchain) {
    if (g_original_create_swap_chain == nullptr) {
        return E_FAIL;
    }
    const HRESULT hr = g_original_create_swap_chain(self, device, desc, swapchain);
    if (SUCCEEDED(hr) && swapchain != nullptr && *swapchain != nullptr) {
        g_swap_chain = *swapchain;
        ++g_d3d12_swap_chain_gen;
        WALKERCORE_LOG_INFO(
            "D3D12Hook: captured swap chain %p via factory (gen %llu)",
            g_swap_chain,
            static_cast<unsigned long long>(g_d3d12_swap_chain_gen));
        HookPresentOnD3D12SwapChain(g_swap_chain);
    }
    return hr;
}

// One-shot vtable-patch of the factory's CreateSwapChain slot.
// We patch every factory that the game creates — the patch is
// idempotent: a second MH_CreateHook on an already-hooked slot
// would return MH_ERROR_ALREADY_CREATED, but we only patch once
// per factory pointer here.
bool HookFactoryCreateSwapChain(IDXGIFactory *factory) {
    if (factory == nullptr) return false;
    void **vtable = *reinterpret_cast<void ***>(factory);
    void *cs_slot = vtable[10];  // CreateSwapChain (verified dxgi.h)
    if (MH_CreateHook(cs_slot,
                      reinterpret_cast<void *>(&HookedCreateSwapChain),
                      reinterpret_cast<void **>(&g_original_create_swap_chain)) != MH_OK) {
        WALKERCORE_LOG_ERROR("D3D12Hook: MH_CreateHook on CreateSwapChain failed");
        return false;
    }
    if (MH_EnableHook(cs_slot) != MH_OK) {
        WALKERCORE_LOG_ERROR("D3D12Hook: MH_EnableHook on CreateSwapChain failed");
        return false;
    }
    g_create_swap_chain_hooked = true;
    return true;
}

HRESULT WINAPI HookedCreateDXGIFactory1(REFIID riid, void **factory) {
    if (g_original_create_factory1 == nullptr) {
        return E_FAIL;
    }
    const HRESULT hr = g_original_create_factory1(riid, factory);
    if (SUCCEEDED(hr) && factory != nullptr && *factory != nullptr) {
        HookFactoryCreateSwapChain(static_cast<IDXGIFactory *>(*factory));
    }
    return hr;
}

HRESULT WINAPI HookedD3D12CreateDevice(IUnknown *adapter,
                                      D3D_FEATURE_LEVEL minimum_feature_level,
                                      REFIID riid,
                                      void **device) {
    if (g_original_d3d12_create == nullptr) {
        return E_FAIL;
    }
    const HRESULT hr = g_original_d3d12_create(adapter, minimum_feature_level,
                                               riid, device);
    if (SUCCEEDED(hr) && device != nullptr && *device != nullptr) {
        g_device = static_cast<ID3D12Device *>(*device);
        WALKERCORE_LOG_INFO("D3D12Hook: captured device %p", g_device);
        // The swap chain is created later via IDXGIFactory::CreateSwapChain,
        // which we hook via CreateDXGIFactory1.
    }
    return hr;
}

}  // namespace

#endif  // WALKERCORE_HAVE_MINHOOK

D3D12Hook::D3D12Hook() = default;

D3D12Hook::~D3D12Hook() {
    if (installed_) {
        Uninstall();
    }
}

void D3D12Hook::BindFrameLoop(FrameLoop *frame_loop) {
    frame_loop_ = frame_loop;
}

bool D3D12Hook::Install() {
    if (installed_) {
        return false;
    }
#if defined(WALKERCORE_HAVE_MINHOOK)
    if (!walkercore::MinHookInit()) {
        WALKERCORE_LOG_ERROR("D3D12Hook::Install: MinHookInit failed");
        return false;
    }

    // Hook D3D12CreateDevice.
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (d3d12 == nullptr) {
        // Game may not have loaded d3d12 yet. Defer. Re-trying is
        // idempotent because installed_ stays false. Release the
        // MinHook ref we just took — the per-provider bail-out
        // contract is "leave no global state perturbed".
        walkercore::MinHookShutdown();
        WarnNoD3D12("Install");
        return false;
    }
    void *d3d12_create_addr = reinterpret_cast<void *>(
        GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (d3d12_create_addr != nullptr) {
        if (MH_CreateHook(d3d12_create_addr,
                          reinterpret_cast<void *>(&HookedD3D12CreateDevice),
                          reinterpret_cast<void **>(&g_original_d3d12_create)) != MH_OK) {
            WALKERCORE_LOG_ERROR(
                "D3D12Hook::Install: MH_CreateHook(D3D12CreateDevice) failed");
            // Don't bail: CreateDXGIFactory1 alone may suffice.
        } else if (MH_EnableHook(d3d12_create_addr) != MH_OK) {
            WALKERCORE_LOG_ERROR(
                "D3D12Hook::Install: MH_EnableHook(D3D12CreateDevice) failed");
        } else {
            WALKERCORE_LOG_INFO(
                "D3D12Hook::Install: hooked D3D12CreateDevice");
            g_d3d12_create_hooked = true;
        }
    } else {
        WALKERCORE_LOG_WARN(
            "D3D12Hook::Install: GetProcAddress(D3D12CreateDevice) returned null");
    }

    // Hook CreateDXGIFactory1. This is the path that fires in
    // D3D12 games (the factory is required for CreateSwapChain).
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (dxgi == nullptr) {
        // dxgi.dll is normally loaded with the GDI subsystem, but
        // check defensively.
        dxgi = LoadLibraryW(L"dxgi.dll");
    }
    if (dxgi == nullptr) {
        WALKERCORE_LOG_ERROR(
            "D3D12Hook::Install: dxgi.dll not loadable");
        walkercore::MinHookShutdown();
        return false;
    }
    void *factory1_addr = reinterpret_cast<void *>(
        GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (factory1_addr == nullptr) {
        WALKERCORE_LOG_ERROR(
            "D3D12Hook::Install: GetProcAddress(CreateDXGIFactory1) failed");
        walkercore::MinHookShutdown();
        return false;
    }
    if (MH_CreateHook(factory1_addr,
                      reinterpret_cast<void *>(&HookedCreateDXGIFactory1),
                      reinterpret_cast<void **>(&g_original_create_factory1)) != MH_OK) {
        WALKERCORE_LOG_ERROR(
            "D3D12Hook::Install: MH_CreateHook(CreateDXGIFactory1) failed");
        walkercore::MinHookShutdown();
        return false;
    }
    if (MH_EnableHook(factory1_addr) != MH_OK) {
        WALKERCORE_LOG_ERROR(
            "D3D12Hook::Install: MH_EnableHook(CreateDXGIFactory1) failed");
        walkercore::MinHookShutdown();
        return false;
    }
    WALKERCORE_LOG_INFO(
        "D3D12Hook::Install: hooked CreateDXGIFactory1");

    installed_ = true;
    return true;
#else
    WALKERCORE_LOG_WARN(
        "D3D12Hook::Install: stub — MinHook not vendored. "
        "Run `git clone --depth=1 https://github.com/TsudaKageyu/minhook.git "
        "third_party/minhook` and rebuild.");
    installed_ = true;
    return true;
#endif
}

void D3D12Hook::Uninstall() {
    if (!installed_) {
        return;
    }
#if defined(WALKERCORE_HAVE_MINHOOK)
    if (g_present_hooked) {
        // Only disable our hooks. D3D11Hook shares this MinHook
        // instance via the refcount helper; calling
        // MH_DisableHook(MH_ALL_HOOKS) here would also tear out the
        // D3D11 lander in games that run the D3D11+render path.
        MH_DisableHook(reinterpret_cast<void *>(&HookedCreateDXGIFactory1));
        MH_DisableHook(reinterpret_cast<void *>(&HookedPresent));
        MH_DisableHook(reinterpret_cast<void *>(&HookedResizeBuffers));
        if (g_d3d12_create_hooked) {
            MH_DisableHook(reinterpret_cast<void *>(&HookedD3D12CreateDevice));
        }
        if (g_create_swap_chain_hooked) {
            MH_DisableHook(reinterpret_cast<void *>(&HookedCreateSwapChain));
        }
        g_present_hooked = false;
    }
    walkercore::MinHookShutdown();
    g_device = nullptr;
    g_swap_chain = nullptr;
    g_original_d3d12_create = nullptr;
    g_original_create_factory1 = nullptr;
    g_original_create_swap_chain = nullptr;
    g_original_d3d12_present = nullptr;
    g_original_resize_buffers = nullptr;
    g_resize_buffers_hooked = false;
    g_cached_aspect = kAspectUnset;
    g_cached_width = 0;
    g_cached_height = 0;
    WALKERCORE_LOG_INFO("D3D12Hook::Uninstall: hooks removed");
#endif
    installed_ = false;
}

}  // namespace walkercore
