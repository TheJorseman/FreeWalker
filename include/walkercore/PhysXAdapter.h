// WalkerCore — PhysXAdapter provider.
//
// Owns the runtime extraction of the game's collision scene. The
// implementation lives in src/provider/PhysXAdapter.cpp.
//
// M3 SCOPE: partial. The adapter:
//   1. Tries the public PhysX 3.4 SDK approach: locate PxPhysics and
//      PxScene via the game binary's exported symbols or via a known
//      global accessor (game-version-specific).
//   2. If the public path fails, falls back to memory scanning for the
//      PxScene vtable (TMRealWalk's strategy; brittle across builds).
//   3. Builds a local triangle/box representation of the scene for
//      fast sweep queries. Periodically refreshes as the game streams
//      geometry in/out.
//
// Why we ship a partial M3 implementation now:
//   - Public PhysX SDK linkage is a non-trivial CMake setup that requires
//     a vendored PhysX 3.4 SDK package (deliberately deferred to keep
//     the build hermetic).
//   - The memory-scan path requires offset tables per game build (see the
//     VersionEntry for PhysXScene). We populate the slot table per build
//     as runtime verification confirms each entry.
//   - The signature-based memory scan (find the PxScene vtable by looking
//     for a run of code pointers near "PxScene::release()" assertion
//     strings) is fully implemented and unit-tested; the only remaining
//     piece is per-build slot offset verification, which requires a real
//     running PxScene instance.
//
// Until the slot table is complete, Raycast / SphereSweep / groundY fall
// back to "no hit" so the player slides through geometry but the game
// stays stable. The M2 (camera) + M4 (walking state) are unaffected and
// remain operational.
//
// The interface here is stable; we will swap the implementation when
// the slot table is populated.

#ifndef WALKERCORE_PHYSX_ADAPTER_H
#define WALKERCORE_PHYSX_ADAPTER_H

#include <stdint.h>

#include "walkercore/WorldState.h"

namespace walkercore {

// Configuration knobs. Read from M6 INI; defaults match TMRealWalk's
// ground-snap parameters.
struct PhysXAdapterConfig {
    // Player capsule radius. TMRealWalk defaults to ~0.35 m.
    double capsule_radius = 0.35;

    // Player capsule total height (top of head to bottom of feet).
    // Used for "where on the body did the collision happen".
    double capsule_height = 1.85;

    // Maximum forward sweep per movement step (clamping).
    double max_sweep_distance = 1.0;

    // Slope angle limit in radians (climbing up beyond this slides the
    // player back down). TMRealWalk uses ~50° (≈ 0.87 rad).
    double max_slope_angle = 0.87;

    // If true, ignore collisions while descending / pending.
    bool disable_collisions = false;
};

// Discovered PxScene vtable. The fields are filled by MemoryScan() in the
// adapter; missing fields (slot == -1) keep the no-hit fallback path.
struct PhysXVtable {
    // Base address of the vtable in amtrucks.exe (PxScene::*). Zero when
    // the scan did not find a confident candidate.
    uint64_t vtable_base = 0;

    // Slot offset of PxScene::raycast. -1 if not yet verified for this
    // build.
    int32_t raycast_slot = -1;
    // Slot offset of PxScene::overlap (any-shape sphere query). -1 if
    // not yet verified.
    int32_t overlap_slot = -1;
    // Slot offset of PxScene::sweep (sphere sweep along a vector). -1
    // if not yet verified.
    int32_t sweep_slot = -1;

    // Number of contiguous code-pointer entries starting at vtable_base
    // that the signature scan accepted. A real PxScene vtable on ATS
    // 1.61.3.1s measures 436 entries (with 150+ slots pointing at the
    // default placeholder 0x141D22E00).
    int32_t accepted_run = 0;
};

// Catalog of known PxScene method function addresses (absolute VAs in
// the live process). Slot offsets are pinned to the open-source PhysX
// 3.4 SDK header layout (PxScene.h). The runtime MATCH is "vtable slot
// N's pointer equals this VA" — equivalent to "slot N is `method`".
//
// Sources for each VA:
//   - VA of method reachable by following the LEA ref to the matching
//     "PxScene::<method>(...) ..." assertion string in .text (the
//     catalog in scripts/physx-vta-scan/README.md records every step).
//   - Image base 0x140000000 + RVA = VA.
//
// Each entry corresponds to a row of the vtable M3 is trying to map.
// The slot index is the published PhysX 3.4 SDK slot for that method
// in the document order (release at 0, then addActor-ish at higher
// slots). When the slot index is left at -1 we have not yet verified
// the catalog; when non-negative, M3 has matched a vtable entry's
// pointer against a known VA, confirming slot -> method.
struct PhysXKnownMethod {
    const char *name;
    uint64_t va;
    int32_t slot;
};

constexpr int kPhysXKnownMethodCount = 8;

// The runtime catalog for ATS 1.61.3.1s. RVA computed from the VA using
// image base 0x140000000. The five "stable" entries (release,
// addActor, simulate, shiftOrigin, sweep) were located by walking
// back from the LEA r64, [rip+disp32] that loads the matching
// "PxScene::<method>(...) ..." assertion string in .text. The
// PxScene::raycast, PxScene::overlap and PxScene::removeActor entries
// were identified by disassembling the vtable at RVA 0x25020A0
// (PxScene vtable, 118 slots):
//   - raycast (slot 15): 0x490-byte stack frame and a 64-slot hit
//     buffer; full implementation at VA 0x141AE1610.
//   - overlap (slot 13): thin dispatcher (xor r9d, r9d; jmp tail)
//     at VA 0x141AE0F70. Tail target has a 0x2c0-byte stack frame and
//     an 8-slot hit buffer (overlap's default buffer is smaller than
//     raycast's).
//   - removeActor (slot 14): thin dispatcher at VA 0x141AE1C60
//     identified by disambiguating the error-path string
//     "PxScene::removeActor(): Actor" (followed by source-file
//     "D:\\manual\\third\\physx\\version_patched\\PhysX_3.4\\Source\\PhysX\\src\\NpScene.cpp"
//     and message "%s not assigned to scene or assigned to another
//     scene. Call will be ignored!"). The function tail-jumps to
//     0x141AE1CF0 which dispatches over the actor kind and ultimately
//     calls the per-type detach helper.
//
//   PxScene::release      VA=0x141AE07B6  RVA=0x1AE07B6
//   PxScene::addActor     VA=0x141AE0B62  RVA=0x1AE0162
//   PxScene::simulate     VA=0x141AE4D80  RVA=0x1AE4380
//   PxScene::shiftOrigin  VA=0x141AE6C28  RVA=0x1AE6228
//   PxScene::sweep        VA=0x141B06F10  RVA=0x1B06F10
//   PxScene::raycast      VA=0x141AE1610  RVA=0x1AE1310
//   PxScene::overlap      VA=0x141AE0F70  RVA=0x1AE0C70
//   PxScene::removeActor  VA=0x141AE1C60  RVA=0x1AE1360
//
// ResolveKnownMethods cross-checks all 8 VAs against the vtable at
// RVA 0x25020A0 (118 slots): release->2, addActor->11,
// simulate->57, shiftOrigin->117, sweep->NOT IN THIS VTABLE
// (sweep's slot lives in a separate 5-slot run at RVA 0x2507C08),
// raycast->15, overlap->13, removeActor->14. See
// docs/observations/ats_1_61_3_1s.physx.functions.md for the full
// provenance and docs/observations/m3-progress-2026-10.md for the
// slot-14 disassembly record.
extern const PhysXKnownMethod kPhysXKnownMethods[kPhysXKnownMethodCount];

// Result of running the known-method matcher against a buffer that
// holds the candidate vtable. The fields are populated when at least
// one PhysXKnownMethod was located inside [bytes, bytes+size) at
// the byte offset (vtable_base + slot * 8). Otherwise each slot field
// is left at -1.
//
// Distinct from PhysXScanResult: this one is the "slot verified"
// follow-up. PhysXScanResult tells us the *signature* accepts a
// vtable; this one says "yes, the vtable at vtable_base + slot*8 has
// a pointer equal to PxScene::release's VA, so slot 0 IS release".
struct PhysXVtableResolveResult {
    // Slot index of PxScene::release within the candidate vtable. -1
    // when the catalog did not match.
    int32_t release_slot = -1;
    // Slot index of PxScene::addActor. -1 when no match.
    int32_t add_actor_slot = -1;
    // Slot index of PxScene::simulate. -1 when no match.
    int32_t simulate_slot = -1;
    // Slot index of PxScene::shiftOrigin. -1 when no match.
    int32_t shift_origin_slot = -1;
    // Slot index of PxScene::sweep. -1 when no match. sweep is the
    // first PxScene method whose catalog VA was found via a
    // sweep-specific assertion string
    // ("PxScene::sweep(): first geometry object ...") instead of
    // the generic LEA-walk-back heuristic. The catalog has it at
    // VA 0x141B06F10 (RVA 0x1B06F10); on ATS 1.61.3.1s sweep is
    // NOT in the 0x25020A0 PxScene vtable (it lives in a separate
    // 5-slot run at RVA 0x2507C08) so this field stays at -1 when
    // ResolveKnownMethods runs against the main vtable.
    int32_t sweep_slot = -1;
    // Slot index of PxScene::raycast. Located by disassembling the
    // 0x25020A0 vtable: slot 15's pointer
    // (VA 0x141AE1610) points at a function whose prologue is
    // `mov rax, rsp; ... sub rsp, 0x490` followed by the
    // `nbMaxTouches = 0x40 = 64` hit-buffer init pattern. -1
    // when the catalog did not match.
    int32_t raycast_slot = -1;
    // Slot index of PxScene::overlap. Located by disassembling the
    // 0x25020A0 vtable: slot 13's pointer (VA 0x141AE0F70) is
    // `xor r9d, r9d; jmp 0x141AE0FD0`, where 0x141AE0FD0 is a
    // function with `sub rsp, 0x2c0` and `nbMaxTouches = 0x8 = 8`
    // hit-buffer init (overlap's default buffer is smaller than
    // raycast's). -1 when the catalog did not match.
    int32_t overlap_slot = -1;
    // Slot index of PxScene::removeActor. Located by disassembling the
    // 0x25020A0 vtable: slot 14's pointer (VA 0x141AE1C60) is a thin
    // dispatcher whose error-path strings uniquely identify
    // "PxScene::removeActor(): Actor" (followed by the same
    // PhysX 3.4 NpScene.cpp source path used by other PxScene
    // methods, with the message "%s not assigned to scene or
    // assigned to another scene. Call will be ignored!"). The
    // tail target at 0x141AE1CF0 dispatches over the actor kind and
    // calls the per-type detach helper. -1 when the catalog did not
    // match.
    int32_t remove_actor_slot = -1;
    // True when sweep was located via the secondary 5-slot vtable
    // run @ RVA 0x2507C08 (ResolveSweepInSecondaryVtable), not the
    // main 0x25020A0 PxScene vtable. When true, sweep_slot is the
    // SECONDARY-LOCAL slot index (0..4 on ATS 1.61.3.1s; usually 4),
    // not the main vtable numbering. Callers that need the absolute
    // pointer read (rdata_ptr + kPhysXSweepSecondaryVtableRVA -
    // rdata_rva + sweep_slot * 8) at runtime; for static RE the
    // pointer is at the supplied catalog VA 0x141B06F10.
    bool has_secondary_sweep = false;
};

// Result of a memory scan over a module's .rdata. success is true when
// at least one of {signature run, string probe} confirmed that amtrucks.exe
// contains a PxScene vtable, leaving only slot-offset verification pending.
// false means we fell back to no-hit queries.
//
// vtable: populated by the signature pass when at least one code-pointer
// run >= kMinPhysXVtableRun was accepted. Always zero-initialised when the
// signature pass is skipped.
//
// string_probes_hit: how many of the kPhysXProbeStringCount known PxScene
// error strings were located verbatim. >= 1 means the .rdata carries the
// public PhysX strings we expect; this is the cheapest confirmation
// (TMRealWalk also prints when this fails).
//
// strategy: "signature" | "strings" | "hybrid" | "no_match".
//
// The probe strings are sourced from the open PhysX 3.4 SDK headers and
// confirmed present in amtrucks.exe (ATS 1.61.3.1s) by the
// [physx] scene strings check. TMRealWalk's collision subsystem logs
// exactly the same set.
struct PhysXScanResult {
    bool success = false;
    PhysXVtable vtable{};
    int32_t string_probes_hit = 0;
    // Short ASCII tag describing the lookup strategy used.
    const char *strategy = "no_match";
};

// Number of distinct PxScene error strings scanned for. Each string is a
// verbatim copy of a string emitted by the public PhysX 3.4 SDK when the
// scene is misused.
constexpr int kPhysXProbeStringCount = 4;

// Minimum accepted vtable run length (count of consecutive 8-byte code
// pointers in .rdata). Below this threshold, the signature scan reports
// no match — TMRealWalk uses the same threshold and reports "scene vtable
// not found" below it. The earlier value of 200 was based on outdated
// docs that claimed the PxScene vtable on ATS 1.61.3.1s had 436
// entries; the actual vtable @ RVA 0x25020A0 is 118 slots in the
// 0x141AD0000..0x141B20000 PhysX TU range. 100 covers all real
// vtables (118 > 100) while excluding random pointer runs (most have
// < 100 contiguous PhysX-TU pointers).
constexpr int kMinPhysXVtableRun = 100;

// Maximum absolute byte distance between a vtable slot's pointer and a
// catalog VA for ResolveKnownMethods to consider them the same method.
// The catalog VAs were located by LEA-walking back from each method's
// `PxScene::<method>(...)` assertion string, which can land a few bytes
// AFTER the actual function entry (e.g., on a non-prologue instruction
// or after an int3 padding boundary). The largest drift observed on
// ATS 1.61.3.1s is 8 bytes (shiftOrigin: catalog VA 0x141AE6C28, slot
// VA 0x141AE6C20 = -8). A tolerance of 8 bytes covers all observed
// drifts. Larger tolerances risk false-positive matches against
// neighbouring slots whose pointers drift in the opposite direction
// (e.g., with tolerance=16, catalog overlap 0x141AE0F70 would
// tolerance-match slot 12 at VA 0x141AE0F80 (delta +16) before
// reaching slot 13 at VA 0x141AE0F70 (delta 0)).
constexpr uint64_t kPhysXResolveSlotTolerance = 8;

// ATS 1.61.3.1s secondary vtable target for PxScene::sweep. The main
// 0x25020A0 PxScene vtable does NOT contain sweep — sweep's catalog
// VA 0x141B06F10 lives in a separate 5-slot run at RVA 0x2507C08
// (slot 4). The slot index is zero-based relative to that run; -1
// when the secondary scan misses.
constexpr uint64_t kPhysXSweepSecondaryVtableRVA = 0x2507C08;
constexpr int32_t kPhysXSweepSecondarySlotRun = 5;
constexpr int32_t kPhysXSweepSecondarySlotIndex = 4;

// Verbatim copy of every probe string the adapter looks for. C++17
// inline constexpr gives every translation unit the same array without
// an ODR violation. Exposed in the header so tests + diagnostic dumps
// can reference the same set without relying on the .cpp-side
// anonymous-namespace constants. Order is stable; the SDK string
// index matches this array index.
inline constexpr const char *const kPhysXProbeStrings[kPhysXProbeStringCount] = {
    // Verbatim copy of every PxScene assertion string the M3 scan
    // looks for in amtrucks.exe. The strings are taken from the
    // ATS 1.61.3.1s binary (SHA 5D3B4E343154DE6B25DD366B6048C5BC5
    // C452A4DAA50BC38B33FD2E571D54257), not the open-source PhysX
    // 3.4 SDK headers — the SDK has "PxScene::addActor() not
    // allowed" but ATS uses "PxScene::addActor(): actor is in a
    // pruning structure and cannot be added to a scene directly,
    // use addActors(const PxPruningStructure& )" instead. Using the
    // SDK strings verbatim would fail to match the live binary.
    // When the SDK re-syncs the message, update this list alongside
    // the VersionTable entry so future ATS patches don't break the
    // string-probe strategy.
    "PxScene::release(): Scene is still being simulated!",
    "PxScene::shiftOrigin() not allowed",
    "PxScene::addActor(): actor is in a pruning structure",
    "PxScene::removeActor(): Actor",
};

class PhysXAdapter : public IWorldGeometry {
public:
    PhysXAdapter();
    ~PhysXAdapter() override;

    PhysXAdapter(const PhysXAdapter &) = delete;
    PhysXAdapter &operator=(const PhysXAdapter &) = delete;

    // Configure from a config block. Safe to call before Install.
    void SetConfig(const PhysXAdapterConfig &cfg);

    // Install the adapter. Returns true on success.
    // Logs which strategy was used (public SDK vs memory scan) and the
    // time taken to build the local representation.
    bool Install();

    // Idempotent.
    void Uninstall();

    bool isInstalled() const { return installed_; }

    // Read the live config for the diagnostic dump. The config is
    // mutable (SetConfig) but the dump only reads, so we return by
    // const-reference. Tests that want to compare configurations can
    // also use this to capture a snapshot before SetConfig.
    const PhysXAdapterConfig &config_for_dump() const { return cfg_; }

    // IWorldGeometry overrides. M3 stub: returns "no hit" so the player
    // does not get stuck waiting for collision.
    bool Raycast(Vec3 origin,
                Vec3 direction,
                double max_distance,
                Vec3 *hit_point,
                Vec3 *hit_normal) override;

    MoveResult SphereSweep(Vec3 origin,
                           double radius,
                           Vec3 displacement) override;

    bool groundY(Vec3 probe, double *out_y) override;

    // Re-run the vtable memory scan against a caller-supplied byte
    // buffer. Test-only hook so the unit test can inject a synthetic
    // .rdata payload without touching amtrucks.exe. Both the signature
    // and the string-probe strategies run; the returned result carries
    // which ones succeeded.
    static PhysXScanResult MemoryScanFromBuffer(const uint8_t *bytes,
                                                size_t size);

    // Same as MemoryScanFromBuffer but with explicit .text VA range.
    // When the buffer is the on-disk amtrucks.exe (not a synthetic
    // .rdata slice), the caller passes the .text section's [low, high]
    // VA range so the signature scan only accepts pointers that
    // actually live in .text — otherwise any non-zero 8-byte value
    // qualifies and the scanner would mis-locate the vtable in some
    // unrelated data structure. The real-binary integration test
    // uses this overload after parsing the PE headers.
    static PhysXScanResult MemoryScanFromBufferWithTextRange(const uint8_t *bytes,
                                                              size_t size,
                                                              uint64_t text_lo,
                                                              uint64_t text_hi);

    // Dump the first `max_entries` 8-byte values from the vtable the
    // signature scan identifies. The output is the function pointer
    // (absolute VA when running with the real .text range, raw value
    // in synthetic mode) of each entry, separated by " | " so a single
    // log line carries enough to spot row 18 (release), row N (raycast),
    // etc. against the open-source PhysX 3.4 SDK headers.
    //
    // This is the bridge from "signature scan accepts the vtable" to
    // "I know which slot is which method". Each slot is one 8-byte
    // pointer; comparing them against the open-source symbols
    // (PxScene::release is slot 0, PxScene::raycast slot 18, ...) lets
    // the maintainer pin `raycast_slot`, `overlap_slot`, `sweep_slot`
    // without a live PxScene instance.
    //
    // `out` is a caller-supplied buffer; the function writes a
    // null-terminated string. Returns the number of bytes written
    // (excluding the trailing NUL). The buffer must be at least 256
    // bytes to accommodate `max_entries = 16`; bigger values need a
    // bigger buffer (the per-entry size is ~21 bytes incl. separators).
    //
    // `strategy` is the strategy tag returned by MemoryScanFromBuffer;
    // `out_strategy` lets the caller display the canonical name next
    // to the entry list.
    static size_t DumpVtableEntries(const uint8_t *bytes,
                                    size_t size,
                                    size_t max_entries,
                                    char *out,
                                    size_t out_size);

    // Dump the first `max_entries` 8-byte values from the vtable the
    // signature scan identifies, with catalog-match annotations.
    // Each entry is rendered as either `0x<VA> [name]` (when the
    // entry's VA matches a row in `kPhysXKnownMethods`) or just
    // `0x<VA>`. Entries are separated by " | ". This is the
    // "maintainer-friendly" form of `DumpVtableEntries`: when
    // reading the dump, the matched slots are immediately
    // identified without needing to compare against the catalog by
    // hand.
    //
    // Same buffer-size constraints as `DumpVtableEntries` apply;
    // the per-entry size grows to ~32 bytes when a name is
    // present, so a 512-byte buffer holds ~16 entries with full
    // annotations.
    static size_t DumpVtableEntriesAnnotated(const uint8_t *bytes,
                                             size_t size,
                                             size_t max_entries,
                                             char *out,
                                             size_t out_size);

    // Resolve the candidate vtable at vtable_base by matching each
    // entry's 8-byte pointer against the catalog of known PxScene
    // function VAs (kPhysXKnownMethods). When a catalog row's VA
    // matches entry N at vtable_base + N*8, N is the slot for that
    // method. The result is populated per-method; -1 means "no
    // match" (slot not in the candidate). Returns true when at least
    // one slot was matched.
    static bool ResolveKnownMethods(const uint8_t *bytes,
                                    size_t size,
                                    uint64_t vtable_base,
                                    int32_t accepted_run,
                                    PhysXVtableResolveResult *out);

    // Sweep-specific secondary vtable resolver. PxScene::sweep is the
    // only catalog entry that does NOT live in the main PxScene
    // vtable @ RVA 0x25020A0 — on ATS 1.61.3.1s it lives in a
    // separate 5-slot run at RVA 0x2507C08 (slot 4). The main
    // resolver uses kMinPhysXVtableRun=100 to reject spurious runs;
    // a 5-slot run is below that floor. This resolver walks exactly
    // 5 slots at the supplied offset (relative to `bytes`) and looks
    // for the catalog sweep VA. Returns the slot index (0..4) when
    // matched; returns -1 when the secondary vtable is not at the
    // supplied offset or the catalog VA is absent.
    static int32_t ResolveSweepInSecondaryVtable(const uint8_t *bytes,
                                                 size_t size,
                                                 uint64_t secondary_vtable_offset);

    // Cached install-time results. PhysXAdapter::Install() runs the
    // real-binary scan + ResolveKnownMethods against the live
    // amtrucks.exe process (when GetModuleHandleA resolves it) and
    // caches the output here. walker_dump_physx() reads this so the
    // F12 dump shows the actual resolved slot per method instead of
    // -1 placeholders. When amtrucks.exe is not the host process
    // (e.g. unit tests) the cache stays at the no_match default and
    // the dump reports "(no live process)".
    struct RuntimeCache {
        PhysXScanResult scan{};
        PhysXVtableResolveResult resolve{};
        bool have_scan = false;
        bool have_resolve = false;
    };

    // Read-only accessor for the diagnostic dump. Returns the live
    // cache; when the adapter has not run the scan, every slot field
    // is -1 and have_scan / have_resolve are false.
    const RuntimeCache &runtime_cache() const { return cache_; }

private:
    PhysXAdapterConfig cfg_;
    bool installed_ = false;
    RuntimeCache cache_;
};

}  // namespace walkercore

#endif  // WALKERCORE_PHYSX_ADAPTER_H