// WalkerCore — test-only reset hooks for the honest-stub providers.
//
// The one-shot warning idiom across the M-lander stubs is
//
//     namespace { bool g_warned = false; }
//     void WarnStub() { if (g_warned) return; g_warned = true; ... }
//
// This contract is meaningful but test-hostile: the warning fires
// exactly once per binary, so any unit test must reset the flag
// before observing the first emission. Each provider exposes an
// `extern "C"` reset hook for this; the prototypes live here.

#ifndef WALKERCORE_TEST_STUB_RESET_H
#define WALKERCORE_TEST_STUB_RESET_H

extern "C" void ResetPhysXStubWarnForTesting();
extern "C" void ResetKnockoutStubWarnForTesting();
extern "C" void ResetCouplingStubWarnForTesting();
extern "C" void ResetPolishStubWarnForTesting();
extern "C" void ResetFuelPumpStubWarnForTesting();

#endif  // WALKERCORE_TEST_STUB_RESET_H
