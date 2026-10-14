"""WalkerCore smoke test — launches American Truck Simulator with the
WalkerCore DLL loaded, polls game.log.txt for WalkerCore diagnostic
lines, and asserts the expected init/diagnostic surfaces are present.

This is a real-game integration test, not a unit test. It exercises:
  * DLL load via the SCS plugin discovery path
  * PluginEntry init sequence (Settings, GameFreeCameraShim, conflict
    scan, dev_cvar_patcher, g_developer probe)
  * game.log.txt emission of every diagnostic surface we shipped in
    the M2 diagnostic logging + M2.7 auto-pin commits

Run with:
  python tests/walker_smoke_test.py
Exit code 0 on pass, non-zero on first failed assertion. Each
assertion produces one PASS/FAIL line so the developer can grep the
output.

The script does NOT require the user to interact with the game UI;
it just verifies the DLL loaded + diagnostics wrote. A future
live-test script would drive keystrokes via the Windows SendInput
API, but that's optional and gated by a `--interact` flag.
"""

import argparse
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path


DEFAULT_GAME_ROOT = (
    r"C:\Program Files (x86)\Steam\steamapps\common\American Truck Simulator"
)
DEFAULT_LOG_PATH = (
    r"C:\Users\migue\OneDrive\Documentos\American Truck Simulator\game.log.txt"
)


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--game-root", default=DEFAULT_GAME_ROOT,
                   help="Path to ATS install root (contains bin/win_x64/)")
    p.add_argument("--log-path", default=DEFAULT_LOG_PATH,
                   help="Path to game.log.txt")
    p.add_argument("--wait-seconds", type=int, default=45,
                   help="Max seconds to wait for WalkerCore init lines")
    p.add_argument("--keep-game-running", action="store_true",
                   help="Do NOT terminate the game at the end (for "
                        "manual interactive verification)")
    return p.parse_args()


def find_game_exe(game_root):
    candidate = Path(game_root) / "bin" / "win_x64" / "amtrucks.exe"
    if not candidate.exists():
        sys.exit(f"FAIL: amtrucks.exe not found at {candidate}")
    return candidate


def snapshot_game_log(log_path):
    """Back up game.log.txt to game.log.txt.smoke-test-<ts> before
    launching so the assertion phase can read only lines produced by
    THIS launch (without prefix noise from previous sessions).

    Returns the path of the snapshot. The original log is renamed so
    the game opens a fresh log when it starts (Steam convention is
    that the game opens game.log.txt for write; renaming is the safe
    way to ensure clean separation)."""
    p = Path(log_path)
    if not p.exists():
        return None  # first-ever run: no prior log to snapshot
    ts = int(time.time())
    backup = p.with_suffix(f".smoke-test-{ts}.bak")
    shutil.copy2(p, backup)
    # Truncate the live log so the next launch starts clean.
    # We do NOT delete; some Steam launches detect "missing log" and
    # refuse to write the first frame.
    with p.open("w", encoding="utf-8") as f:
        f.truncate(0)
    return backup


def launch_game(game_exe, log_path, log_offset):
    """Launch amtrucks.exe and return the Popen object. stdout/stderr
    are redirected to DEVNULL because the game does not write to
    console — it writes to game.log.txt only.

    log_offset is the byte length of `game.log.txt` at launch time
    (the smoke test will only consider lines written AFTER this
    offset in the post-launch polling phase)."""
    env = os.environ.copy()
    # Pin ATS to a window the smoke test can grab + focus if we want
    # to send keystrokes. Keep windowed so the test doesn't require a
    # desktop session with a virtual display.
    env["SDL_VIDEO_WINDOW_POS"] = "100,100"
    creationflags = 0
    if sys.platform == "win32":
        # CREATE_NEW_PROCESS_GROUP so we can send Ctrl+Break to the
        # game cleanly without killing the smoke test process.
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP
    return subprocess.Popen(
        [str(game_exe)],
        cwd=str(game_exe.parent),
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=creationflags,
    )


def wait_for_lines(log_path, log_offset, patterns, timeout):
    """Poll game.log.txt for lines matching any of the regex patterns.
    Returns the list of matched (regex, full_line) tuples and the
    offset where the read stopped. Times out after `timeout` seconds
    and returns whatever matched by then."""
    p = Path(log_path)
    matched = []
    deadline = time.monotonic() + timeout
    seen = set()
    while time.monotonic() < deadline:
        if not p.exists():
            time.sleep(0.5)
            continue
        try:
            with p.open("r", encoding="utf-8", errors="replace") as f:
                f.seek(log_offset)
                chunk = f.read()
        except OSError:
            time.sleep(0.5)
            continue
        for line in chunk.splitlines():
            for pat in patterns:
                if pat in line and (pat, line) not in seen:
                    seen.add((pat, line))
                    matched.append((pat, line))
        # If every pattern has at least one match, we can stop early.
        if len({m[0] for m in matched}) == len(patterns):
            break
        time.sleep(0.5)
    return matched


def terminate_game(p):
    """Send Ctrl+Break to the game so it can flush its log cleanly.
    On Windows this triggers the SCS engine's graceful-shutdown path,
    which writes a final "Quit" line. Falls back to terminate() if the
    graceful path takes longer than 5s."""
    if p.poll() is not None:
        return  # already exited
    try:
        if sys.platform == "win32":
            p.send_signal(signal.CTRL_BREAK_EVENT)
            try:
                p.wait(timeout=5)
                return
            except subprocess.TimeoutExpired:
                pass
        p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
    except Exception as exc:
        print(f"WARN: failed to terminate game cleanly: {exc}")


def main():
    args = parse_args()

    game_exe = find_game_exe(args.game_root)
    log_path = Path(args.log_path)

    print(f"[walker] game binary : {game_exe}")
    print(f"[walker] game log    : {log_path}")
    print(f"[walker] wait window : {args.wait_seconds}s")

    # 1) Snapshot existing log + truncate live log so the assertion
    #    phase only sees lines from THIS launch.
    backup = snapshot_game_log(str(log_path))
    if backup:
        print(f"[walker] snapshot    : {backup}")

    log_offset = 0
    if log_path.exists():
        log_offset = log_path.stat().st_size

    # 2) Launch the game.
    proc = launch_game(game_exe, str(log_path), log_offset)
    print(f"[walker] launched    : pid={proc.pid}")

    # 3) Poll for the diagnostic lines we expect to see. The
    #    expectation set is the union of the surfaces we shipped in
    #    the M2 diagnostic logging + M2.7 auto-pin commits.
    expected_patterns = [
        # M2.5 settings load
        "[WalkerCore] Settings: loaded",
        # M2.7 per-key resolution diagnostic
        "[WalkerCore] Settings: per-key resolution",
        # M2.5 toggle VK line
        "[WalkerCore] GameFreeCameraShim: toggle VK",
        # M2.5 g_developer probe
        "[WalkerCore] GameFreeCameraShim: g_developer probe",
        # M2.7 controls.sii conflict detector (WARN line)
        "GameFreeCameraShim: controls.sii CONFLICT",
        # M2.7 dev-cvar patcher skipped warning (the patcher cannot
        # find the cvar struct in this build; WARN is expected).
        "DevCameraCvarPatcher: skipped",
        # M2.7 auto-pin log line (WARN only when conflict detected).
        # The auto-pin fires on this host (F11 bound to camdbg in
        # the user's controls.sii) so we expect either the
        # "AUTO-RE-PIN toggle VK" line OR the "AUTO-RE-PIN FAILED"
        # line depending on whether the fallback list is clean.
        "AUTO-RE-PIN",
        # Bound game HWND
        "GameFreeCameraShim: bound game hwnd=",
    ]

    try:
        matched = wait_for_lines(
            str(log_path), log_offset, expected_patterns, args.wait_seconds)
    finally:
        if not args.keep_game_running:
            terminate_game(proc)

    # 4) Assert each expected pattern matched.
    found_patterns = {m[0] for m in matched}
    failed = []
    for pat in expected_patterns:
        if pat in found_patterns:
            print(f"[PASS]  {pat}")
        else:
            print(f"[FAIL]  {pat}")
            failed.append(pat)

    # 5) Print the actual log lines for the analyst's reference.
    print()
    print("[walker] matched diagnostic lines:")
    for pat, line in matched:
        print(f"  {line}")

    if failed:
        print()
        print(f"[FAIL] {len(failed)} expected pattern(s) did not match:")
        for pat in failed:
            print(f"  - {pat}")
        print("[walker] the game may need more time to boot, or the DLL")
        print("[walker] may not be loaded by the SCS plugin loader.")
        print("[walker] check that libWalkerCore.dll is in:")
        print("[walker]   bin/win_x64/plugins/libWalkerCore.dll")
        return 1

    print()
    print(f"[PASS] all {len(expected_patterns)} diagnostic surfaces emitted")
    print("[walker] smoke test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())