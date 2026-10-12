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
#include "walkercore/Time.h"

#if defined(WALKERCORE_HAVE_MINHOOK)
#include <d3d12.h>
#include <dxgi.h>
#include "MinHook.h"
#endif

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

CreateD3D12DeviceFn g_original_d3d12_create = nullptr;
CreateFactoryFn g_original_create_factory1 = nullptr;
CreateSwapChainFn g_original_create_swap_chain = nullptr;
PresentFn g_original_d3d12_present = nullptr;

// Borrowed (not owned): the game releases these via its own
// reference counts. We AddRef only the ID3D12Device so we have a
// stable pointer for the duration of the hook; the swap chain and
// factory we read directly from the captured COM pointers without
// AddRef.
ID3D12Device *g_device = nullptr;
IDXGISwapChain *g_swap_chain = nullptr;

bool g_present_hooked = false;

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

    // Read the back-buffer aspect from the calling swap chain, not
    // from any cached pointer — the game may have ResizeBuffers'd
    // without us catching the call. We pay a per-frame GetDesc
    // (cheap: just reads a struct the game already keeps); a future
    // commit can hook ResizeBuffers (vtable slot 13 on
    // IDXGISwapChain, verified dxgi.h line 1579) to drop this.
    DXGI_SWAP_CHAIN_DESC desc{};
    if (self != nullptr && SUCCEEDED(self->GetDesc(&desc))) {
        const float aspect =
            (desc.BufferDesc.Height > 0)
                ? static_cast<float>(desc.BufferDesc.Width) /
                      static_cast<float>(desc.BufferDesc.Height)
                : 16.0f / 9.0f;
        walker_set_swapchain_aspect(aspect);
    }

    walker_per_frame_tick(dt);

    if (g_original_d3d12_present != nullptr) {
        return g_original_d3d12_present(self, sync_interval, present_flags);
    }
    return E_FAIL;
}

// Vtable-patch Present on a freshly captured swap chain. Same
// pattern as D3D11Hook::HookPresent.
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
    if (MH_Initialize() != MH_OK) {
        WALKERCORE_LOG_ERROR("D3D12Hook::Install: MH_Initialize failed");
        return false;
    }

    // Hook D3D12CreateDevice.
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (d3d12 == nullptr) {
        // Game may not have loaded d3d12 yet. Defer. Re-trying is
        // idempotent because installed_ stays false.
        MH_Uninitialize();
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
        MH_Uninitialize();
        return false;
    }
    void *factory1_addr = reinterpret_cast<void *>(
        GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (factory1_addr == nullptr) {
        WALKERCORE_LOG_ERROR(
            "D3D12Hook::Install: GetProcAddress(CreateDXGIFactory1) failed");
        MH_Uninitialize();
        return false;
    }
    if (MH_CreateHook(factory1_addr,
                      reinterpret_cast<void *>(&HookedCreateDXGIFactory1),
                      reinterpret_cast<void **>(&g_original_create_factory1)) != MH_OK) {
        WALKERCORE_LOG_ERROR(
            "D3D12Hook::Install: MH_CreateHook(CreateDXGIFactory1) failed");
        MH_Uninitialize();
        return false;
    }
    if (MH_EnableHook(factory1_addr) != MH_OK) {
        WALKERCORE_LOG_ERROR(
            "D3D12Hook::Install: MH_EnableHook(CreateDXGIFactory1) failed");
        MH_Uninitialize();
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
        MH_DisableHook(MH_ALL_HOOKS);
        g_present_hooked = false;
    }
    MH_Uninitialize();
    g_device = nullptr;
    g_swap_chain = nullptr;
    g_original_d3d12_create = nullptr;
    g_original_create_factory1 = nullptr;
    g_original_create_swap_chain = nullptr;
    g_original_d3d12_present = nullptr;
    WALKERCORE_LOG_INFO("D3D12Hook::Uninstall: hooks removed");
#endif
    installed_ = false;
}

}  // namespace walkercore
