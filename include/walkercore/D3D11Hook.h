// WalkerCore — D3D11/DXGI Present hook (M2 lander).
//
// D3D11Hook captures the game's ID3D11Device and IDXGISwapChain and hooks
// the swap chain's Present() so we can run one FrameLoop tick per frame.
//
// Implementation (src/provider/D3D11Hook.cpp) is real and ships with
// WALKERCORE_HAVE_MINHOOK defined. The MinHook call is gated so builds
// without the vendored library get a no-op stub instead of a build
// failure (see Install() in D3D11Hook.cpp).
//
// Algorithm:
//   1. Install on scs_telemetry_init.
//   2. Use MinHook to patch D3D11CreateDeviceAndSwapChain in d3d11.dll
//      so we see when the game creates its device/swap chain.
//   3. In our wrapper, save the IDs and call the original.
//   4. After IDXGISwapChain::Present is set up, patch the vtable entry
//      for Present() with MinHook so we control the per-frame hook.
//   5. On every Present: read the back-buffer aspect via GetDesc, walk
//      the FrameLoop once, then call the original Present.
//
// Limit (real bug, see D3D12Hook for the D3D12 fix):
//   This hook fires ONLY when the game creates its device via
//   d3d11!D3D11CreateDeviceAndSwapChain. ATS 1.61.x uses D3D12, so the
//   present path goes through d3d12!D3D12CreateDevice +
//   IDXGIFactory::CreateSwapChain — the Present vtable patch in step 4
//   still runs (it is on the DXGI swap chain, not the D3D runtime) but
//   the device/swap-chain capture in step 3 misses the D3D12 creation
//   path, and we never install the per-frame hook at all. The fix is
//   the parallel D3D12Hook class that owns the d3d12!D3D12CreateDevice
//   capture. WalkerCore instantiates both at scs_telemetry_init; the
//   first one to win the race owns the per-frame tick.
//
// We respect TMRealWalk's observation that some games load a wrapper
// dxgi.dll (MSI Afterburner, RTSS, etc.). The hook itself goes through
// d3d11.dll / d3d12.dll, so wrapper dxgi.dll does not affect the
// patching — but a hooked dxgi.dll would intercept the swap chain
// creation and could mask the Present vtable patch. We do not currently
// scan for dxgi wrappers; if a future build needs that, add a probe here.

#ifndef WALKERCORE_D3D11_HOOK_H
#define WALKERCORE_D3D11_HOOK_H

#include <stdint.h>

namespace walkercore {

class FrameLoop;

// The D3D11 / DXGI COM types are not exposed in this header — the
// implementation file is the only consumer of the real ABI. This keeps
// the rest of the codebase free of d3d11.h / dxgi.h includes, which is
// what we want: domain code and tests don't pay the compile cost.

class D3D11Hook {
public:
    D3D11Hook();
    ~D3D11Hook();

    D3D11Hook(const D3D11Hook &) = delete;
    D3D11Hook &operator=(const D3D11Hook &) = delete;

    // Bind the FrameLoop that drives each frame. The hook does not own
    // it. Pass nullptr to clear.
    void BindFrameLoop(FrameLoop *frame_loop);

    // Install all hooks. Idempotent (returns false on the second call).
    // Logs progress and any failures via walkercore::Log.
    bool Install();

    // Remove all hooks. Safe to call when not installed.
    void Uninstall();

    bool isInstalled() const { return installed_; }

private:
    FrameLoop *frame_loop_ = nullptr;
    bool installed_ = false;
};

// Free accessor: the swap-chain generation counter is stored in a free
// variable because the hooked COM callbacks don't have an `this` pointer.
// Returns 0 when no device has been captured yet. Increments every time
// the game creates a fresh D3D11 device + swap chain (alt-tab in
// fullscreen, resolution change, monitor swap). A rapidly-increasing
// value in the dump means the game is thrashing device creation.
uint64_t CurrentSwapChainGeneration();

}  // namespace walkercore

#endif  // WALKERCORE_D3D11_HOOK_H