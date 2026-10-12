// WalkerCore — D3D11 Present hook (M2).
//
// D3D11Hook captures the game's ID3D11Device and IDXGISwapChain and hooks
// the swap chain's Present() so we can (1) read the back buffer and (2)
// modify the renderer state before each present call.
//
// This file is the **interface only**. The implementation in
// src/provider/D3D11Hook.cpp is intentionally a stub for M2:
//   - The actual hook requires MinHook (third_party/minhook) and is the
//     riskiest part of the plugin (a bug here crashes the game on load).
//   - Until we have a compiler we cannot smoke-test it.
//   - The stub documents the algorithm so the M2 implementation lands in
//     one well-defined pass when the toolchain is ready.
//
// Algorithm (target M2 implementation):
//
//   1. Install on scs_telemetry_init.
//   2. Use MinHook to patch D3D11CreateDeviceAndSwapChain in d3d11.dll
//      so we see when the game creates its device/swap chain.
//   3. In our wrapper, save the IDs and call the original.
//   4. After IDXGISwapChain::Present is set up, patch the vtable entry
//      for Present() with MinHook so we control the per-frame hook.
//   5. On every Present: walk the FrameLoop once, push the camera, then
//      call the original Present.
//
// We respect TMRealWalk's observation that some games load a wrapper
// dxgi.dll (MSI Afterburner, RTSS, etc.). Our hook scans for these
// wrappers before patching to avoid double-hooking.

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