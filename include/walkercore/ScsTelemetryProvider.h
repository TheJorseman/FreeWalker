// WalkerCore — SCS telemetry provider.
//
// Owns the lifecycle of SCS channel registrations: RegisterAll() is
// called from scs_telemetry_init, UnregisterAll() from
// scs_telemetry_shutdown. Stores its callback updates into a
// TruckState held by value.
//
// The header intentionally does NOT include any SCS SDK type. The SDK
// pointers (register_for_channel, channel callbacks, scs_value_t) are
// an implementation detail of ScsTelemetryProvider.cpp. Callers see only
// the domain interface: RegisterAll(function, opaque), UnregisterAll(function),
// state() const, mutable_state().

#ifndef WALKERCORE_SCS_TELEMETRY_PROVIDER_H
#define WALKERCORE_SCS_TELEMETRY_PROVIDER_H

#include "walkercore/TruckState.h"

namespace walkercore {

class ScsTelemetryProvider {
public:
    // Construct empty. RegisterAll() must be called before any channel
    // callback can fire.
    ScsTelemetryProvider();

    // No copy / no move — the singleton lives in the plugin entry.
    ScsTelemetryProvider(const ScsTelemetryProvider &) = delete;
    ScsTelemetryProvider &operator=(const ScsTelemetryProvider &) = delete;

    // Register all channels we care about (M1 + M8 set). Returns the
    // number of successful registrations. Logs each failure individually.
    //
    // We take the SDK function pointer as `void *` to keep this header
    // SDK-free; the implementation downcasts to the correct function
    // pointer type. The caller (PluginEntry) holds the SDK-typed pointer.
    int RegisterAll(void *register_for_channel_ptr);

    // Mirror of RegisterAll. Idempotent: safe to call twice.
    void UnregisterAll(void *unregister_from_channel_ptr);

    // Read-only access to the latest snapshot. Thread-safety: the SCS SDK
    // calls our callbacks from the game thread, and WalkerCore's main
    // loop runs on the same thread. M1 has no background workers, so no
    // mutex is needed yet. When we add one in M3, we add a lock here.
    const TruckState &state() const { return state_; }

    // Non-const accessor. Used by (a) the channel callbacks in the .cpp
    // (which can't befriend us without exposing the SDK types in the
    // header) and (b) tests that exercise state-mutation paths without
    // a running SDK.
    TruckState &mutable_state() { return state_; }

private:
    TruckState state_;
};

}  // namespace walkercore

#endif  // WALKERCORE_SCS_TELEMETRY_PROVIDER_H