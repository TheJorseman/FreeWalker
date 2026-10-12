// WalkerCore — D3D12 + DXGI factory + Present hook (M2 lander, D3D12 path).
//
// D3D12Hook is the parallel path to D3D11Hook for games that use
// Direct3D 12 for rendering. ATS 1.61.x uses D3D12 — without this
// class, our per-frame Present hook never fires because the game
// never calls d3d11!D3D11CreateDeviceAndSwapChain.
//
// Capture chain:
//
//   1. MinHook D3D12CreateDevice in d3d12.dll.
//   2. MinHook CreateDXGIFactory1 in dxgi.dll.
//   3. When the game calls factory->CreateSwapChain(device, desc, &sc),
//      our vtable-patched CreateSwapChain wrapper captures the
//      returned IDXGISwapChain.
//   4. We then vtable-patch the swap chain's Present (slot 8) so each
//      frame calls walker_per_frame_tick(dt).
//
// Present itself lives on IDXGISwapChain, so step 4 is identical
// between the D3D11 and D3D12 paths — only the capture sequence
// differs. Once the swap chain is captured, our HookedPresent
// implementation is the same code (lives in this file).
//
// WALKERCORE_HAVE_MINHOOK gates the real implementation. The class
// degrades to a no-op stub (with a one-shot warn) when MinHook is
// not vendored, identical to D3D11Hook's contract.

#ifndef WALKERCORE_D3D12_HOOK_H
#define WALKERCORE_D3D12_HOOK_H

#include <stdint.h>

namespace walkercore {

class FrameLoop;

class D3D12Hook {
public:
    D3D12Hook();
    ~D3D12Hook();

    D3D12Hook(const D3D12Hook &) = delete;
    D3D12Hook &operator=(const D3D12Hook &) = delete;

    // Bind the FrameLoop that drives each frame. The hook does not
    // own it. Pass nullptr to clear.
    void BindFrameLoop(FrameLoop *frame_loop);

    // Install all hooks. Idempotent (returns false on the second
    // call). Both D3D12CreateDevice and CreateDXGIFactory1 are
    // attempted; either may legitimately fail (game not yet loaded
    // d3d12.dll, or game does not use D3D12). Returns true only if
    // at least one hook installed successfully.
    bool Install();

    // Remove all hooks. Safe to call when not installed.
    void Uninstall();

    bool isInstalled() const { return installed_; }

private:
    FrameLoop *frame_loop_ = nullptr;
    bool installed_ = false;
};

// Free accessor: increments every time the swap chain capture
// succeeds (regardless of D3D11 vs D3D12 path). The
// abi_contract test pins this contract; a future test that
// exercises a real D3D12 capture loop will exercise the
// counter too. Returns 0 in a fresh process before any hook
// has captured a swap chain.
uint64_t CurrentD3D12SwapChainGeneration();

}  // namespace walkercore

#endif  // WALKERCORE_D3D12_HOOK_H
