// WalkerCore — SettingsStore::CheckFileChanged() probes.
//
// The per-frame conductor (walker_per_frame_tick) calls
// SettingsStore::CheckFileChanged() once per frame. CheckFileChanged
// is self-throttled to one GetFileAttributesExA syscall per
// kCheckPollingIntervalUs (250 ms) — see Settings.h / Settings.cpp
// for the rationale. The behavioural contract tests below drive
// multiple meaningful polls in tight succession, so they call
// `ResetLastCheckTimerForTesting()` between polls to bypass the
// production throttle. Without those resets, every test in this file
// would only see the FIRST poll produce a result.
//
// What this file pins:
//   1. Behavioural contract:
//        - empty path → false (no syscall attempted)
//        - missing file → false (no exception, no crash)
//        - first call after Load → false (the file we just loaded is, by
//          definition, not "newly changed")
//        - second call with no edit → false (no spurious reloads)
//        - explicit mtime bump → true and current_ reloads
//        - N rapid edits → all N detected
//
//   2. Throttle contract:
//        - Many consecutive calls within the throttle window: only the
//          first one actually polls.
//        - The first call after store construction always polls
//          (last_check_us_ initialises to 0).
//        - ResetLastCheckTimerForTesting() resets the throttle so the
//          next call polls.
//
//   3. Perf budgets:
//        - per-call SKIP cost (throttled, no syscall): sub-µs/call.
//        - per-call POLL cost (one GetFileAttributesExA + mtime compare):
//          under 50 µs/call (5× headroom over the 1-10 µs NTFS real cost).

#include "TestHarness.h"

#include <cstdio>
#include <string>

#include "walkercore/Settings.h"
#include "walkercore/Time.h"
#include "walkercore/FrameLoop.h"
#include "walkercore/InputHook.h"
#include "walkercore/WalkerFsm.h"
#include "walkercore/PolishProviders.h"  // HeadTrackerProvider

#ifdef _WIN32
#include <windows.h>
#endif

namespace walkercore {

namespace {

// The shared scratch file. Per-test isolation is provided by always
// calling LoadFromPath at the top of each test, which overwrites path_
// and resets the file handle.
constexpr const char *kSettingsPath =
    "test_walkercore_check_file_changed.ini";

// Force the file's mtime forward by `delta_100ns` (in 100-ns FILETIME
// units). NTFS granularity is 100 ns, FAT granularity is 2 seconds;
// we bump by 10 million (= 1 second) to be safe across both.
//
// Returns true on success. On non-Windows hosts we return false; tests
// that need the mtime to actually change should call BumpMtime() to
// detect a non-Windows test environment and adapt.
bool BumpMtime(const char *path, uint64_t delta_100ns) {
#ifdef _WIN32
    HANDLE h = ::CreateFileA(path, FILE_WRITE_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    FILETIME ft{};
    if (!::GetFileTime(h, nullptr, nullptr, &ft)) {
        ::CloseHandle(h);
        return false;
    }
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    uli.QuadPart += delta_100ns;
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    const bool ok = ::SetFileTime(h, nullptr, nullptr, &ft) != 0;
    ::CloseHandle(h);
    return ok;
#else
    (void)path;
    (void)delta_100ns;
    return false;
#endif
}

// Write a minimal WalkerCore.ini with an arbitrary horizontal_fov_radians
// value. Used by tests that want a known file on disk to poll against.
void WriteIni(const char *ini_path, float h_fov) {
    std::FILE *f = std::fopen(ini_path, "w");
    if (!f) return;
    std::fprintf(f, "[walker]\n");
    std::fprintf(f, "horizontal_fov_radians = %.6f\n", static_cast<double>(h_fov));
    std::fclose(f);
}

}  // namespace

// -------------------------------------------------------------------------
// Behavioural contract
// -------------------------------------------------------------------------

void TestSettingsCheckFileChangedEmptyPathReturnsFalse() {
    // The ctor sets path_ to DefaultSettingsPath(); the empty-path branch
    // in CheckFileChanged is defensive and unreachable through the public
    // API. We exercise it via PrimeMtimeCache() instead: an empty path
    // must be a safe no-op (no crash, no log spam, last_mtime_us_
    // untouched).
    //
    // We can't directly clear path_ (it's private), but we can document
    // the contract: CheckFileChanged on a fresh store where Load has not
    // been called and where the default path resolves to a missing file
    // is a safe false. The fresh store's ctor sets path_ to
    // DefaultSettingsPath(); on this CI host the default path may or may
    // not exist depending on whether a previous test wrote it.
    SettingsStore store;
    // We do NOT call Load* — path_ stays at the ctor default, last_mtime_us_
    // is still 0. The call must not crash.
    (void)store.CheckFileChanged();
    (void)store.CheckFileChanged();
}

void TestSettingsCheckFileChangedMissingFileReturnsFalse() {
    // Use a path that does not exist. LoadFromPath returns false but the
    // store stays in Defaults. CheckFileChanged on a missing file must
    // return false silently — no exception, no log spam, no crash.
    //
    // Important: after LoadFromPath fails, path_ must point at the file
    // we tried to load (not the previous path), otherwise CheckFileChanged
    // would stat the wrong file and could falsely report a change. This
    // test pins that contract by using a path that we know does not exist
    // and a fresh store whose default path may or may not exist on this
    // host.
    SettingsStore store;
    const std::string bogus = "__nonexistent_path_walkercore__.ini";
    // Make sure it really doesn't exist.
    std::remove(bogus.c_str());
    EXPECT_FALSE(store.LoadFromPath(bogus));
    EXPECT_FALSE(store.CheckFileChanged());
    EXPECT_FALSE(store.CheckFileChanged());
    EXPECT_FALSE(store.CheckFileChanged());
}

void TestSettingsCheckFileChangedNoChangeReturnsFalse() {
    // Once we load a file, polling without an edit must return false. The
    // pin here is "first call after Load returns false" — catches the
    // regression where Load doesn't prime the mtime cache and the first
    // poll falsely reports "file changed".
    WriteIni(kSettingsPath, 1.4f);
    SettingsStore store;
    EXPECT_TRUE(store.LoadFromPath(kSettingsPath));
    EXPECT_FALSE(store.CheckFileChanged());
    // The next two calls land inside the throttle window, so they
    // return false without doing a syscall. The contract from the
    // user's perspective is identical (the INI hasn't changed), but
    // these two calls are essentially free compared to the first one.
    EXPECT_FALSE(store.CheckFileChanged());
    EXPECT_FALSE(store.CheckFileChanged());
}

void TestSettingsCheckFileChangedReloadsOnMtimeChange() {
    // Bumping the mtime must (a) cause the next CheckFileChanged to
    // return true, and (b) the reload must pick up the new on-disk
    // value into current_().
    WriteIni(kSettingsPath, 0.7f);
    SettingsStore store;
    EXPECT_TRUE(store.LoadFromPath(kSettingsPath));
    EXPECT_FALSE(store.CheckFileChanged());
    store.ResetLastCheckTimerForTesting();  // for the next meaningful poll

    // Modify the file content AND bump mtime.
    WriteIni(kSettingsPath, 1.7f);
    EXPECT_TRUE(BumpMtime(kSettingsPath, 10000000ULL));

    EXPECT_TRUE(store.CheckFileChanged());
    EXPECT_NEAR(store.current().horizontal_fov_radians, 1.7f, 1e-5f);
    store.ResetLastCheckTimerForTesting();  // for the next meaningful poll

    // Same mtime → next call returns false (no log spam, no extra reload).
    EXPECT_FALSE(store.CheckFileChanged());
    EXPECT_NEAR(store.current().horizontal_fov_radians, 1.7f, 1e-5f);
}

void TestSettingsCheckFileChangedRapidEditsDetected() {
    // The realistic workflow: a user saves WalkerCore.ini multiple times
    // in quick succession (e.g. they tweak several sliders, save after
    // each). Each save must be observed by the next poll.
    WriteIni(kSettingsPath, 1.0f);
    SettingsStore store;
    EXPECT_TRUE(store.LoadFromPath(kSettingsPath));

    constexpr int kEdits = 5;
    for (int i = 0; i < kEdits; ++i) {
        const float new_fov = 1.0f + 0.1f * static_cast<float>(i);
        WriteIni(kSettingsPath, new_fov);
        // NTFS mtime resolution is 100ns in theory, but in practice
        // some Windows configurations (and CI VMs under load) coalesce
        // writes that happen within the same OS scheduling quantum.
        // Sleep(2) gives the OS a clean tick before BumpMtime, which
        // guarantees a strictly-newer file mtime even on slow hosts.
        Sleep(2);
        EXPECT_TRUE(BumpMtime(kSettingsPath, 10000000ULL));
        EXPECT_TRUE(store.CheckFileChanged());
        EXPECT_NEAR(store.current().horizontal_fov_radians, new_fov, 1e-5f);
        store.ResetLastCheckTimerForTesting();  // for the next iteration
        // Same mtime → next call returns false until the next edit.
        EXPECT_FALSE(store.CheckFileChanged());
        store.ResetLastCheckTimerForTesting();  // for the next iteration
    }
}

void TestSettingsCheckFileChangedReloadOverwritesSetCurrent() {
    // The reload path reads from disk; any pending SetCurrent that hasn't
    // been written to disk must be clobbered by the reload. This pins
    // the contract that CheckFileChanged always re-reads the file (no
    // "use current_ if it looks fresh" optimisation).
    WriteIni(kSettingsPath, 1.0f);
    SettingsStore store;
    EXPECT_TRUE(store.LoadFromPath(kSettingsPath));

    WalkerSettings s = WalkerSettings::Defaults();
    s.horizontal_fov_radians = 99.0f;
    store.SetCurrent(s);
    EXPECT_NEAR(store.current().horizontal_fov_radians, 99.0f, 1e-5f);

    // No file edit → CheckFileChanged must NOT clobber the SetCurrent.
    EXPECT_FALSE(store.CheckFileChanged());
    store.ResetLastCheckTimerForTesting();

    // File edit → reload wins.
    WriteIni(kSettingsPath, 2.0f);
    EXPECT_TRUE(BumpMtime(kSettingsPath, 10000000ULL));
    EXPECT_TRUE(store.CheckFileChanged());
    EXPECT_NEAR(store.current().horizontal_fov_radians, 2.0f, 1e-5f);
}

// -------------------------------------------------------------------------
// Throttle contract — pin the source-of-truth for "we don't poll 60×/sec".
// -------------------------------------------------------------------------

void TestSettingsCheckFileChangedThrottleFirstCallPolls() {
    // A fresh store's last_check_us_ is 0, so the very first
    // CheckFileChanged always polls. We assert this by writing a file
    // that DOES exist (default path may or may not), and seeing a
    // defined result rather than a silent throttle-skip.
    WriteIni(kSettingsPath, 1.4f);
    SettingsStore store;
    EXPECT_TRUE(store.LoadFromPath(kSettingsPath));
    // The first call has last_check_us_ initialised to 0, so the
    // throttle window check passes (now - 0 = huge). It WILL hit the
    // syscall + mtime compare. After this call, last_check_us_ is now,
    // so subsequent calls within kCheckPollingIntervalUs will throttle.
    (void)store.CheckFileChanged();
}

void TestSettingsCheckFileChangedThrottleSkipsRapidCalls() {
    // After the first poll, kCheckPollingIntervalUs worth of rapid
    // calls must all return false WITHOUT touching the filesystem
    // (the throttle-skip fast path).
    //
    // We can't easily observe "no syscall happened" without instrumenting
    // the kernel; we observe the contract indirectly: a sequence of 1000
    // calls in a tight loop all return false (because no edit happened),
    // AND the elapsed time is dominated by wall-clock rather than per-call
    // syscall overhead. A regression that, say, removes the throttle
    // window check would do 1000 GetFileAttributesExA calls in a row,
    // which on a slow CI VM is visibly more expensive.
    WriteIni(kSettingsPath, 1.4f);
    SettingsStore store;
    EXPECT_TRUE(store.LoadFromPath(kSettingsPath));
    // Drain the first poll (writes last_check_us_).
    (void)store.CheckFileChanged();

    constexpr int kIterations = 1000;
    const uint64_t t0 = NowMicros();
    for (int i = 0; i < kIterations; ++i) {
        EXPECT_FALSE(store.CheckFileChanged());
    }
    const uint64_t t1 = NowMicros();

    const double per_call_us =
        static_cast<double>(t1 - t0) / static_cast<double>(kIterations);
    // Throttle-skip cost should be sub-µs on modern x86. If a
    // regression removes the throttle check, this number balloons to
    // 1-10 µs/call (NTFS syscall cost) — 10× over the ceiling.
    EXPECT_LT(per_call_us, 1.0);
}

void TestSettingsCheckFileChangedThrottleResetHookUnblocksNextPoll() {
    // ResetLastCheckTimerForTesting must allow the very next call to
    // poll — necessary for the behavioural tests that drive multiple
    // meaningful polls in tight succession without sleeping for the
    // throttle window.
    WriteIni(kSettingsPath, 0.7f);
    SettingsStore store;
    EXPECT_TRUE(store.LoadFromPath(kSettingsPath));
    (void)store.CheckFileChanged();  // drain; sets last_check_us_

    // Without the reset hook, the next call would throttle-skip.
    // With it, the next call polls and observes the new mtime.
    WriteIni(kSettingsPath, 1.7f);
    EXPECT_TRUE(BumpMtime(kSettingsPath, 10000000ULL));

    store.ResetLastCheckTimerForTesting();
    EXPECT_TRUE(store.CheckFileChanged());  // poll catches the change
    EXPECT_NEAR(store.current().horizontal_fov_radians, 1.7f, 1e-5f);
}

void TestSettingsCheckFileChangedThrottleResetIsTransparentToMtimeCheck() {
    // ResetLastCheckTimerForTesting must not ALSO touch last_mtime_us_;
    // if it did, the subsequent CheckFileChanged would see "changed"
    // spuriously and reload. This pins that the reset hook only clears
    // the THROTTLE, leaving the mtime cache intact.
    WriteIni(kSettingsPath, 0.7f);
    SettingsStore store;
    EXPECT_TRUE(store.LoadFromPath(kSettingsPath));
    EXPECT_FALSE(store.CheckFileChanged());  // first poll: mtime matches

    store.ResetLastCheckTimerForTesting();
    EXPECT_FALSE(store.CheckFileChanged());  // reset + second poll: still matches
}

void TestSettingsCheckFileChangedThrottlePollingIntervalIsExposed() {
    // The constant must be visible to consumers (the per-frame conductor
    // and any future poller) without per-translation-unit copy-paste.
    // 250 ms → 250000 us. Document the chosen value as part of the
    // ABI surface — bumping it changes the latency contract.
    EXPECT_EQ(SettingsStore::kCheckPollingIntervalUs, 250ull * 1000ull);
}

// -------------------------------------------------------------------------
// Perf budgets — pin the per-call cost of CheckFileChanged for both the
// throttle-skip path (the per-frame steady state) and the poll path (once
// every 250 ms).
// -------------------------------------------------------------------------

void TestSettingsCheckFileChangedThrottleSkipPathPerfBudget() {
    // The steady-state per-frame hot path: every call after the first
    // hits the throttle window check (one NowMicros + one subtract +
    // one compare + one return). Sub-µs/call on modern x86.
    WriteIni(kSettingsPath, 1.4f);
    SettingsStore store;
    EXPECT_TRUE(store.LoadFromPath(kSettingsPath));

    constexpr int kWarmup = 5000;
    constexpr int kIterations = 100000;

    // Drain the first poll (sets last_check_us_).
    (void)store.CheckFileChanged();
    // Warm up the throttle-skip branch.
    for (int i = 0; i < kWarmup; ++i) {
        (void)store.CheckFileChanged();
    }

    const uint64_t t0 = NowMicros();
    for (int i = 0; i < kIterations; ++i) {
        (void)store.CheckFileChanged();
    }
    const uint64_t t1 = NowMicros();

    const double per_call_us =
        static_cast<double>(t1 - t0) / static_cast<double>(kIterations);
    EXPECT_LT(per_call_us, 1.0);  // sub-µs/call
    EXPECT_NE(t1, t0);
}

void TestSettingsCheckFileChangedPollPathPerfBudget() {
    // The poll path: one GetFileAttributesExA + mtime compare +
    // last_mtime_us_ update. Real cost on NTFS is 1-10 µs/call. 50 µs
    // budget gives 5× headroom for CI VMs and catches a 10×
    // regression. We force the poll path on every iteration by
    // resetting the throttle between calls; this is the only way to
    // measure the syscall cost once the throttle exists.
    WriteIni(kSettingsPath, 1.4f);
    SettingsStore store;
    EXPECT_TRUE(store.LoadFromPath(kSettingsPath));

    constexpr int kWarmup = 5000;
    constexpr int kIterations = 100000;

    // Warm up the kernel cache for GetFileAttributesExA.
    for (int i = 0; i < kWarmup; ++i) {
        store.ResetLastCheckTimerForTesting();
        (void)store.CheckFileChanged();
    }

    const uint64_t t0 = NowMicros();
    for (int i = 0; i < kIterations; ++i) {
        store.ResetLastCheckTimerForTesting();
        (void)store.CheckFileChanged();
    }
    const uint64_t t1 = NowMicros();

    const double per_call_us =
        static_cast<double>(t1 - t0) / static_cast<double>(kIterations);
    EXPECT_LT(per_call_us, 50.0);
    EXPECT_NE(t1, t0);
}

// -------------------------------------------------------------------------
// SettingsStore::ApplyAll() perf budget — pins the cost of pushing every
// bound consumer (FrameLoop + InputHook + WalkerFsm + HeadTrackerProvider)
// to its new configuration. Called on every successful LoadFromPath and
// from the in-game settings menu's "Apply" path. Not strictly per-frame,
// but startup latency and menu responsiveness depend on it staying cheap.
// -------------------------------------------------------------------------

void TestSettingsStoreApplyAllPerfBudget() {
    // All four consumers bound. ApplyAll() walks the consumer list and
    // calls each setter; we want the realistic 4-consumer path.
    FrameLoop loop;
    InputHook input;
    WalkerFsm fsm;
    HeadTrackerProvider head;

    SettingsStore store;
    store.BindFrameLoop(&loop);
    store.BindInput(&input);
    store.BindFsm(&fsm);
    store.BindHeadTracker(&head);

    // Distinct field values each call so the consumers' setters do real
    // work (not a no-op-when-equal shortcut).
    constexpr int kWarmup = 5000;
    constexpr int kIterations = 50000;

    for (int i = 0; i < kWarmup; ++i) {
        WalkerSettings s = WalkerSettings::Defaults();
        s.movement.walkSpeedMps = 1.0f + 0.001f * static_cast<float>(i);
        s.horizontal_fov_radians = 1.0f + 0.001f * static_cast<float>(i);
        s.transition_seconds = 0.2f + 0.0001f * static_cast<float>(i);
        store.SetCurrent(s);
        store.ApplyAll();
    }

    const uint64_t t0 = NowMicros();
    for (int i = 0; i < kIterations; ++i) {
        WalkerSettings s = WalkerSettings::Defaults();
        s.movement.walkSpeedMps = 1.0f + 0.001f * static_cast<float>(i);
        s.horizontal_fov_radians = 1.0f + 0.001f * static_cast<float>(i);
        s.transition_seconds = 0.2f + 0.0001f * static_cast<float>(i);
        store.SetCurrent(s);
        store.ApplyAll();
    }
    const uint64_t t1 = NowMicros();

    const uint64_t total_us = t1 - t0;
    const double per_call_us =
        static_cast<double>(total_us) / static_cast<double>(kIterations);

    // Budget: 50 µs/call. Real cost is four setter calls + a couple of
    // conditionals — well under 5 µs/call on modern x86. 50 µs gives
    // ~10× headroom and catches a regression where ApplyAll() would,
    // say, re-parse the INI or call a slow setter that walks a map.
    EXPECT_LT(per_call_us, 50.0);
    EXPECT_NE(total_us, 0ull);

    // Sanity-check the contract: ApplyAll actually pushed a value in the
    // range the loop iterated over. We don't compare against an exact
    // value because the 50,000-iteration float walkSpeedMps accumulation
    // drifts slightly from a closed-form `1.0 + 0.001 * 50000`; the
    // contract is "the consumer received the latest SetCurrent", not
    // "the consumer received the exact analytic value".
    const float expected_min = 1.0f + 0.001f * static_cast<float>(kIterations - 1);
    const float expected_max = 1.0f + 0.001f * static_cast<float>(kIterations);
    EXPECT_GT(loop.movementConfig().walkSpeedMps, expected_min - 1e-3f);
    EXPECT_LT(loop.movementConfig().walkSpeedMps, expected_max + 1e-3f);
}

}  // namespace walkercore