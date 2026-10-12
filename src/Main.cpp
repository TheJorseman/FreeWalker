// WalkerCore — composition root.
//
// M0 is just the plugin entry point; the rest of the modules don't exist
// yet. As we add M1+ phases, this file grows to:
//  - Construct the ScsTelemetryProvider, Config, FrameLoop, etc.
//  - Wire dependencies.
//  - Pass the constructed objects to the adapter (PluginEntry) which
//    exposes them to the SCS SDK via scs_telemetry_init.
//
// The composition root MUST stay small and readable. Each module's
// constructor takes its dependencies as parameters; nothing here should
// reach into globals.

#include "walkercore/BuildInfo.h"
#include "walkercore/Log.h"

namespace walkercore {

// Hook for future phases. M1 will construct the ScsTelemetryProvider and
// register a frame callback here. Keeping the symbol out of the binary
// until we need it avoids a "ready to register" but "not yet registering"
// state.
int Main(int /*argc*/, char * /*argv*/[]) {
  WALKERCORE_LOG_INFO("composition root reached (M0: no subsystems yet)");
  return 0;
}

}  // namespace walkercore

// Currently unused entrypoint retained for future diagnostic tools that
// may want to load WalkerCore as a regular executable for unit testing
// without going through the SCS loader path. M0 keeps the symbol but
// leaves the entry conditional on a CMake option.
extern "C" __declspec(dllexport) int walkercore_main(int argc, char **argv) {
  return walkercore::Main(argc, argv);
}