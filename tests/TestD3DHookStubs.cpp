// Test stubs for DLL-adapter functions referenced by D3D11Hook and D3D12Hook
// when the test build defines WALKERCORE_HAVE_MINHOOK=1. The real
// implementations live in src/Main.cpp and are part of the DLL adapter
// layer (not the test static library). The stubs are no-ops so the
// linker resolves the symbols and the test exercises the real hook
// code path without needing the rest of the adapter layer.
//
// This file is only compiled into WalkerCoreTestLib when
// WALKERCORE_HAVE_MINHOOK=1; in the stub build (no MinHook), the hook
// bodies don't reference these symbols so the stubs are not needed.

#include "walkercore/PerFrame.h"

#include <cstdint>

#if defined(WALKERCORE_TEST_NO_PER_FRAME_TICK)

extern "C" void walker_per_frame_tick(double dt_seconds) {
    (void)dt_seconds;
}

extern "C" void walker_set_swapchain_aspect(float aspect) {
    (void)aspect;
}

#endif  // WALKERCORE_TEST_NO_PER_FRAME_TICK
