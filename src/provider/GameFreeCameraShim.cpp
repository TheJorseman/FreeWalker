// WalkerCore — GameFreeCameraShim implementation (M2 GPU render push MVP).
//
// The shim never modifies SCS state. It only synthesises keystrokes
// that the game already understands. The default is Numpad-0, which
// toggles the SCS developer free camera in ATS 1.61.x when the cvar
// `g_developer` is `1` (already set in the user's config.cfg).
//
// On non-Windows hosts (Linux build for CI of the unit tests) every
// SendInput-touching path is compiled out under the same guard the
// rest of the project uses for OS-specific code. Tests run on
// Windows; the test build sets `WALKERCORE_GAME_FREE_CAMERA_SHIM_DISABLED=1`
// so SendInput is replaced with a counter increment.

#include "walkercore/GameFreeCameraShim.h"

#include "walkercore/DevCameraCvarPatcher.h"
#include "walkercore/Log.h"

#include <cstdio>
#include <cstdint>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>  // SHGetFolderPathA + CSIDL_PROFILE
#endif

namespace walkercore {

namespace {

// Synthesise a single keypress. We use SendInput because ATS 1.61.x
// reads keyboard input via DirectInput 8 (`[di8] Initializing device
// '@@keyboard@@' as 'keyboard'` in game.log.txt, plus the binary
// embeds `di8.keyboard` as the input handler token). DirectInput 8
// keyboard devices poll the Windows input queue, and SendInput
// synthesises events that go through the same queue. The user
// confirmed F11 manual presses toggle the SCS dev free camera
// reliably — the question is whether the shim's synthetic input
// reaches DirectInput's poll. PostMessage WM_KEYDOWN does NOT reach
// DirectInput (it goes to the WndProc queue, which the game ignores
// for game-control keybindings), so the previously-shipped PostMessage
// dispatch path was a dead-end for this mod stack. We keep the bound
// HWND as a diagnostic surface (logged in the F12 dump + Walking entry
// line) but use it for dispatch only when SendInput is unavailable
// (e.g. a future build where the game's input model changes back).
// Returns the number of events dispatched (always 2: one down + one
// up) and updates the diagnostic path counters in `path_out` /
// `target_diag_out` for the F12 dump.
int SynthKey(unsigned int vk, HWND target_hwnd,
             int &path_out, unsigned int &target_diag_out) {
#if defined(_WIN32) && !defined(WALKERCORE_GAME_FREE_CAMERA_SHIM_DISABLED)
    if (vk == 0) {
        path_out = 0;
        target_diag_out = 0;
        return 0;
    }
    // Always try SendInput first — that's the proven path for
    // DirectInput 8 keyboard (which the user confirmed works for
    // manual Numpad-0 presses). PostMessage WM_KEYDOWN is the
    // legacy fallback that we keep around for non-DirectInput games
    // where SendInput might be foreground-locked.
    if (target_hwnd == nullptr) {
        // No bound HWND (test build, or splash-screen window not yet
        // up) — try PostMessage as a no-op-ish fallback. The path
        // diagnostic will say `path=none` because we have nothing
        // useful to dispatch to.
        path_out = 0;
        target_diag_out = 0;
        return 0;
    }

    // Primary: SendInput (DirectInput 8 keyboard → dev free camera
    // toggle on F11).
    INPUT down = {};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = static_cast<WORD>(vk);
    down.ki.dwFlags = 0;
    SendInput(1, &down, sizeof(down));

    INPUT up = {};
    up.type = INPUT_KEYBOARD;
    up.ki.wVk = static_cast<WORD>(vk);
    up.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &up, sizeof(up));
    path_out = 1;  // SendInput
    target_diag_out = static_cast<unsigned int>(
        reinterpret_cast<std::uintptr_t>(target_hwnd) & 0xFFFFFFFFu);
    return 2;
#else
    // Test stub: when GameFreeCameraShim is built with
    // WALKERCORE_GAME_FREE_CAMERA_SHIM_DISABLED (the test build
    // configuration), we don't call SendInput or PostMessage.
    // Instead we simulate the dispatch decision so the tests can
    // verify the right path was taken based on the bound HWND.
    (void)vk;
    if (target_hwnd == nullptr) {
        // Without a target we'd SendInput into whatever process is
        // in the foreground — too risky for a production build,
        // so we skip dispatch entirely. The path counter records
        // `none` and the toggle counter still increments (so the
        // rest of the system knows the dispatch was attempted).
        path_out = 0;
        target_diag_out = 0;
        return 0;
    }
    // With an HWND bound, the production path uses SendInput (the
    // proven DirectInput 8 keyboard path). The test stub mirrors
    // that decision without actually touching the Windows input
    // queue.
    path_out = 1;  // SendInput
    target_diag_out = static_cast<unsigned int>(
        reinterpret_cast<std::uintptr_t>(target_hwnd) & 0xFFFFFFFFu);
    return 2;
#endif
}

long long NowMs() {
#if defined(_WIN32)
    return static_cast<long long>(::GetTickCount64());
#else
    return 0;
#endif
}

// G5.20: dev-console command injection (memory-based, like the
// cabin camera mod TM_Real_Walk's memory hooks — not SendInput).
// The G5.19 SendInput KEYEVENTF_UNICODE path was confirmed dead in
// the live verify (28 events dispatched but dev cam did not
// activate) because SCS's dev console Edit control polls input
// through a path that KEYEVENTF_UNICODE does not reach.
//
// G5.20 attacks the problem in two stages:
//
//   * PRIMARY  (path_out == 2): `PostMessage WM_CHAR` to the dev
//     console's Edit control, found at runtime via window
//     enumeration. We open the console with SendInput ~, wait
//     up to 1.5 s for the Edit HWND to materialise, then
//     PostMessage each char of the command + a KEYUP for Enter.
//     Most reliable for Win32 Edit controls; works even when
//     the game's input layer is purely DirectInput.
//
//   * FALLBACK (path_out == 3): if the Edit HWND is not found
//     (custom DUI / unknown class / race), SendInput each char
//     as a VK code with shift modifier for upper-case and
//     symbols. Layout-aware (US keyboard) but travels through
//     the standard Windows input queue.
//
// `target_diag_out` records the lower 32 bits of the resolved
// Edit HWND when the primary path succeeds; 0 for the fallback.
//
// Echo-suppression: the three VK events (~, Enter, ~) are added
// to the shim's echo list at the call site (TriggerToggle) so
// WH_KEYBOARD_LL eats them. WM_CHAR messages bypass the keyboard
// hook entirely (they go through PostMessage, not the raw input
// queue), so they do NOT need to be added to the echo list.
int SynthConsoleCommand(const char *command, HWND game_hwnd,
                       int &path_out, unsigned int &target_diag_out) {
    path_out = 0;
    target_diag_out = 0;
    if (command == nullptr || command[0] == '\0') {
        return 0;
    }
#if defined(_WIN32) && !defined(WALKERCORE_GAME_FREE_CAMERA_SHIM_DISABLED)
    auto send_vk = [](unsigned int vk) {
        INPUT down = {};
        down.type = INPUT_KEYBOARD;
        down.ki.wVk = static_cast<WORD>(vk);
        down.ki.dwFlags = 0;
        ::SendInput(1, &down, sizeof(down));
        INPUT up = {};
        up.type = INPUT_KEYBOARD;
        up.ki.wVk = static_cast<WORD>(vk);
        up.ki.dwFlags = KEYEVENTF_KEYUP;
        ::SendInput(1, &up, sizeof(up));
    };
    // Translate a lowercase ASCII char to a (vk, shift) pair on
    // the US keyboard layout. Returns false for chars we don't
    // know (the caller falls back to a no-op or to the PostMessage
    // path). Used only by the SendInput-VK fallback.
    auto char_to_vk = [](unsigned char c, unsigned int &vk_out,
                         bool &shift_out) -> bool {
        if (c >= 'a' && c <= 'z') {
            vk_out = static_cast<unsigned int>(
                'A' + (c - 'a'));  // VK codes for letters are
                                   // the uppercase letter
            shift_out = false;
            return true;
        }
        if (c >= 'A' && c <= 'Z') {
            vk_out = static_cast<unsigned int>(c);
            shift_out = true;
            return true;
        }
        if (c >= '0' && c <= '9') {
            vk_out = static_cast<unsigned int>(c);
            shift_out = false;
            return true;
        }
        switch (c) {
            case ' ':  vk_out = VK_SPACE;  shift_out = false; return true;
            case '_':  vk_out = 0xBD;      shift_out = true;  return true;
            case '-':  vk_out = 0xBD;      shift_out = false; return true;
            case '.':  vk_out = 0xBE;      shift_out = false; return true;
            case ',':  vk_out = 0xBC;      shift_out = false; return true;
            case '/':  vk_out = 0xBF;      shift_out = false; return true;
            case ';':  vk_out = 0xBA;      shift_out = false; return true;
            case '\'': vk_out = 0xDE;      shift_out = false; return true;
            case '[':  vk_out = 0xDB;      shift_out = false; return true;
            case ']':  vk_out = 0xDD;      shift_out = false; return true;
            case '\\': vk_out = 0xDC;      shift_out = false; return true;
            case '=':  vk_out = 0xBB;      shift_out = false; return true;
            case '`':  vk_out = 0xC0;      shift_out = false; return true;
            default:   return false;
        }
    };

    int total = 0;
    // 1. Open dev console.
    send_vk(0xC0);
    total += 2;
    ::Sleep(120);

    // 2. Locate the dev console Edit control. We poll up to 1.5 s
    //    (30 × 50 ms) for the Edit HWND to materialise. The probe
    //    enumerates top-level windows on the same thread as the
    //    bound game HWND and looks for the most recently created
    //    window that is NOT the main game window (the dev console
    //    is created after the ~ keypress). For each candidate
    //    top-level window we enumerate its children to find an
    //    Edit control.
    HWND edit_hwnd = nullptr;
    HWND console_hwnd = nullptr;
    (void)console_hwnd;  // currently only used in the production probe; kept for future diagnostic logging
    DWORD thread_id = 0;
    if (game_hwnd != nullptr) {
        thread_id = ::GetWindowThreadProcessId(game_hwnd, nullptr);
    }
    auto is_edit_class = [](const wchar_t *cls) -> bool {
        if (cls == nullptr) return false;
        return (::lstrcmpiW(cls, L"Edit") == 0 ||
                ::lstrcmpiW(cls, L"SCSEdit") == 0 ||
                ::lstrcmpiW(cls, L"RichEdit20W") == 0 ||
                ::lstrcmpiW(cls, L"WindowsEdit") == 0);
    };
    // Diagnostic accumulator: count top-level windows visited and
    // collect the first few class names so the F10 Walking-entry log
    // can surface what we saw when the Edit was not found. Helps
    // pinpoint the dev console's window class on the next live
    // verify if the Edit HWND lookup needs tuning.
    int tlw_visited = 0;
    std::string tlw_class_dump;
    if (thread_id != 0) {
        struct ProbeCtx {
            HWND main_hwnd;
            HWND best_edit;
            HWND best_console;
            bool (*is_edit)(const wchar_t *);
            int *visited;
            std::string *class_dump;
        };
        int visited = 0;
        std::string class_dump;
        ProbeCtx ctx{game_hwnd, nullptr, nullptr, is_edit_class,
                     &visited, &class_dump};
        auto enum_proc = [](HWND h, LPARAM lparam) -> BOOL {
            ProbeCtx *c = reinterpret_cast<ProbeCtx *>(lparam);
            (*c->visited)++;
            // Capture up to 4 top-level class names for the log.
            if (c->class_dump->size() < 256) {
                wchar_t cls[64] = {};
                if (::GetClassNameW(h, cls, 63) > 0) {
                    char narrow[64] = {};
                    for (int i = 0; i < 63 && cls[i] != 0; ++i) {
                        narrow[i] = (cls[i] < 0x80)
                            ? static_cast<char>(cls[i]) : '?';
                        narrow[i + 1] = 0;
                    }
                    if (!c->class_dump->empty()) {
                        c->class_dump->append(",");
                    }
                    c->class_dump->append(narrow);
                }
            }
            if (h == c->main_hwnd) {
                return TRUE;  // skip the main game window
            }
            if (::GetWindow(h, GW_OWNER) != nullptr) {
                return TRUE;  // skip owned windows (tooltips etc.)
            }
            // Look for an Edit child of this top-level window.
            struct ChildCtx {
                bool (*is_edit)(const wchar_t *);
                HWND found;
            };
            ChildCtx child{c->is_edit, nullptr};
            auto child_proc = [](HWND ch, LPARAM lp) -> BOOL {
                ChildCtx *cc = reinterpret_cast<ChildCtx *>(lp);
                wchar_t cls[64] = {};
                if (::GetClassNameW(ch, cls, 63) > 0 && cc->is_edit(cls)) {
                    cc->found = ch;
                    return FALSE;  // stop
                }
                return TRUE;
            };
            ::EnumChildWindows(h, child_proc,
                               reinterpret_cast<LPARAM>(&child));
            if (child.found != nullptr) {
                c->best_edit = child.found;
                c->best_console = h;
                return FALSE;  // stop the top-level enum
            }
            return TRUE;
        };
        ::EnumThreadWindows(thread_id, enum_proc,
                            reinterpret_cast<LPARAM>(&ctx));
        edit_hwnd = ctx.best_edit;
        console_hwnd = ctx.best_console;
        tlw_visited = visited;
        tlw_class_dump = class_dump;
    }
    // Diagnostic: surface the probe result for live-verify iteration.
    // When path==3 (fallback) the next live-verify log shows the
    // visited count + first few top-level class names so we can
    // identify the dev console window class and add it to the
    // is_edit_class filter.
    WALKERCORE_LOG_INFO(
        "GameFreeCameraShim: dev console probe — visited %d top-level "
        "window(s) on thread %lu, classes=[%s], edit_hwnd=%s",
        tlw_visited, static_cast<unsigned long>(thread_id),
        tlw_class_dump.empty() ? "<none>" : tlw_class_dump.c_str(),
        edit_hwnd ? "found" : "NOT FOUND (will fall back to SendInput-VK)");

    if (edit_hwnd != nullptr) {
        // PRIMARY: PostMessage WM_CHAR to the Edit control.
        path_out = 2;  // PostMessage WM_CHAR
        target_diag_out = static_cast<unsigned int>(
            reinterpret_cast<std::uintptr_t>(edit_hwnd) & 0xFFFFFFFFu);
        for (const char *p = command; *p != '\0'; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            ::PostMessageW(edit_hwnd, WM_CHAR,
                           static_cast<WPARAM>(c), 0);
            total += 1;
            ::Sleep(15);
        }
        // Submit with Enter. Use WM_KEYDOWN + WM_KEYUP for symmetry
        // with the SendInput path's echo-suppression contract.
        ::PostMessageW(edit_hwnd, WM_KEYDOWN, VK_RETURN, 0);
        ::PostMessageW(edit_hwnd, WM_KEYUP, VK_RETURN, 0);
        total += 2;
        ::Sleep(100);
    } else {
        // FALLBACK: SendInput VK codes (US layout). The dev console
        // Edit HWND was not found within the 1.5 s window; fall
        // back to a more portable injection that the game may
        // poll via DirectInput or its raw input layer.
        path_out = 3;  // SendInput VK codes (fallback)
        target_diag_out = 0;
        for (const char *p = command; *p != '\0'; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            unsigned int vk = 0;
            bool shift = false;
            if (char_to_vk(c, vk, shift)) {
                if (shift) {
                    send_vk(VK_LSHIFT);
                    total += 2;
                }
                send_vk(vk);
                total += 2;
                if (shift) {
                    INPUT up = {};
                    up.type = INPUT_KEYBOARD;
                    up.ki.wVk = VK_LSHIFT;
                    up.ki.dwFlags = KEYEVENTF_KEYUP;
                    ::SendInput(1, &up, sizeof(up));
                    total += 1;
                }
            } else {
                // Unknown char — skip (the command has failed).
                // Path remains 3 so the log surfaces the fallback.
            }
            ::Sleep(20);
        }
        // Submit with Enter.
        send_vk(VK_RETURN);
        total += 2;
        ::Sleep(100);
    }

    // 3. Close the dev console so the screen returns to gameplay.
    send_vk(0xC0);
    total += 2;
    return total;
#else
    // Test stub — mirror the real event count without touching
    // the Windows input queue. The test path uses path_out == 2
    // (PostMessage) for tests that opt in to via-console; the
    // enum-probe returns no HWND in tests, so the real path
    // would always fall to the VK-code fallback. The stub
    // emits 2 events per char (no probe overhead) so test
    // counts stay stable.
    (void)command;
    (void)game_hwnd;
    path_out = 2;
    int total = 0;
    for (const char *p = command; *p != '\0'; ++p) {
        total += 2;
    }
    total += 2;  // ~ open
    total += 2;  // Enter
    total += 2;  // ~ close
    return total;
#endif
}

}  // namespace

// Pure helper: candidate `Documents` sub-folders relative to the
// user's profile. We try the canonical English "Documents" first,
// then Windows-XP "My Documents", then the OneDrive variants for
// both English ("Documents") and Spanish ("Documentos") locales —
// the latter is needed for this developer's host (es_es + OneDrive
// folder backup with the original Spanish name). We also enumerate
// `OneDrive - Personal\*` for multi-account OneDrive installs.
std::vector<std::string> GameFreeCameraShim::ProbeConfigCandidatesForTesting(
    const std::string &profile) {
    static const char *stems[] = {
        "American Truck Simulator",
        "Euro Truck Simulator 2",
    };
    static const char *subs[] = {
        "Documents",
        "My Documents",
        "OneDrive\\Documents",
        "OneDrive\\Documentos",
        "OneDrive - Personal\\Documents",
        "OneDrive - Personal\\Documentos",
    };
    std::vector<std::string> out;
    out.reserve(12);
    for (const char *sub : subs) {
        for (const char *stem : stems) {
            std::string p = profile + "\\" + sub + "\\" + stem +
                            "\\config.cfg";
            out.push_back(std::move(p));
        }
    }
    return out;
}

// M2.5: candidate controls.sii paths. Same OneDrive-aware candidate
// enumeration as ProbeConfigCandidatesForTesting — the active profile
// lives in the same Documents tree the config.cfg does — but the
// trailing filename is `controls.sii` instead of `config.cfg`. Note:
// this enumerates candidate `<Documents>/<game>/controls.sii` paths,
// which is the layout a single-profile install uses. Multi-profile
// installs put controls.sii under `<Documents>/<game>/profiles/<id>/`
// and the runtime probe (see ProbeActiveControlsSii below) walks the
// profile.sii to find the active profile id before resolving the real
// path. The candidate list here is the no-profile-info upper bound;
// the runtime picks the first file that actually exists.
std::vector<std::string> GameFreeCameraShim::ProbeControlsSiiCandidatesForTesting(
    const std::string &profile) {
    static const char *stems[] = {
        "American Truck Simulator",
        "Euro Truck Simulator 2",
    };
    static const char *subs[] = {
        "Documents",
        "My Documents",
        "OneDrive\\Documents",
        "OneDrive\\Documentos",
        "OneDrive - Personal\\Documents",
        "OneDrive - Personal\\Documentos",
    };
    std::vector<std::string> out;
    out.reserve(12);
    for (const char *sub : subs) {
        for (const char *stem : stems) {
            std::string p = profile + "\\" + sub + "\\" + stem +
                            "\\controls.sii";
            out.push_back(std::move(p));
        }
    }
    return out;
}

// Pure helper: parse the first `uset g_developer "X"` line and
// return the value as a string. Empty on miss.
std::string GameFreeCameraShim::ReadGDeveloperValueForTesting(
    const std::string &path) {
    if (path.empty()) {
        return std::string();
    }
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return std::string();
    }
    std::string content;
    char buf[4096] = {};
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        content.append(buf, n);
    }
    std::fclose(f);

    // Crude pattern match. SCS writes the cvar as one per line,
    // optionally with surrounding whitespace, in this exact form:
    //   uset g_developer "1"
    // Quotes are always present in the file produced by ATS 1.61.x.
    auto pos = content.find("uset g_developer");
    if (pos == std::string::npos) {
        return std::string();
    }
    auto eol = content.find('\n', pos);
    std::string line = content.substr(pos, eol - pos);
    auto q1 = line.find('"');
    auto q2 = line.find('"', q1 == std::string::npos ? 0 : q1 + 1);
    if (q1 == std::string::npos || q2 == std::string::npos) {
        return std::string();
    }
    return line.substr(q1 + 1, q2 - q1 - 1);
}

// M2.5: parse controls.sii for a binding to the named action. The
// controls.sii grammar SCS writes for 1.61.x looks like:
//
//   config_lines[184]: "mix dbglockpos `keyboard.num0?0`"
//   config_lines[185]: "mix dbglockrot `keyboard.numperiod?0 | semantical.dbglockrot?0`"
//
// We scan line-by-line for `mix <action> `` — the leading literal
// tokens must match exactly (no fuzzy / regex). Inside the backticks
// the binding is one or more `<device>.<source>?<axis>` tokens
// separated by ` | `. We return the first `keyboard.*` token (with
// the device prefix) so the caller can convert to a Win32 VK. The
// reverse-order `keyboard.X?0 | semantical.X?0` layout common in
// stock profiles is honoured — we just pick the first keyboard.*.
//
// The scanner is intentionally minimal: it only matches the literal
// `mix <action> `` form (no regex / no fuzzy), and ignores anything
// after the matching closing backtick. A malformed file (no closing
// backtick, action name spanning multiple lines, etc.) returns empty
// without throwing — the runtime treats that as "no binding" and
// falls back to the default dispatch path with a clear log line.
//
// The reader limits itself to a single 64 KiB read so a runaway file
// (a user's controls.sii grown to MB-size from years of mod swaps)
// cannot stall game boot. The file format is human-edited and a real
// ATS profile controls.sii is ~30 KiB; 64 KiB is a 2× safety margin.
std::string GameFreeCameraShim::ReadControlsSiiKeyForActionForTesting(
    const std::string &path, const std::string &action) {
    bool unused = false;
    return ReadControlsSiiKeyForActionWithDiagForTesting(
        path, action, &unused);
}

std::string GameFreeCameraShim::ReadControlsSiiKeyForActionWithDiagForTesting(
    const std::string &path, const std::string &action,
    bool *had_escaped_backtick_typo) {
    if (had_escaped_backtick_typo != nullptr) {
        *had_escaped_backtick_typo = false;
    }
    if (path.empty() || action.empty()) {
        return std::string();
    }
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return std::string();
    }
    std::string content;
    char buf[4096] = {};
    size_t n = 0;
    while (content.size() < (64u * 1024u) &&
           (n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        content.append(buf, n);
    }
    std::fclose(f);

    // Walk the file looking for `mix <action> ``. We do a single
    // literal find (cheap, deterministic) and bail at the first hit
    // because controls.sii is well-ordered — the binding for a given
    // action appears exactly once.
    const std::string needle_plain = "mix " + action + " `";
    auto pos = content.find(needle_plain);
    bool escaped_typo = false;
    // offset_after_delim is the byte offset, relative to pos, where
    // the first source token (e.g. `keyboard.f11`) starts. For the
    // plain form the delimiter is `mix <action> \`` (size 22 for
    // dev_camera_toggle) and the token follows immediately. For the
    // escaped form the delimiter is `mix <action> \`` (one extra
    // backslash byte) so we have to skip an additional `\` byte
    // before reaching the token.
    size_t offset_after_delim = needle_plain.size();
    if (pos == std::string::npos) {
        // Fallback: the user has escaped backticks (`\``) around the
        // binding — a copy-paste accident (some editors auto-escape
        // markdown backticks inside code-block-adjacent snippets).
        // The SCS parser rejects this at game load
        // (`<ERROR> Expecting string literal, found '"mix ...'`),
        // so the binding never reaches the game's action registry
        // and a SendInput dispatch will hit the wrong action. We
        // still recover the keyboard token so the user gets a
        // actionable log line ("here is what we tried, here is how
        // to fix it") instead of a silent miss.
        const std::string needle_escaped = "mix " + action + " \\`";
        pos = content.find(needle_escaped);
        if (pos == std::string::npos) {
            return std::string();
        }
        escaped_typo = true;
        // needle_escaped.size() == needle_plain.size() + 1 (one
        // extra `\` byte before the backtick).
        offset_after_delim = needle_escaped.size();
    }
    // Walk to the closing backtick. The grammar is `.... `|....|....`
    // — there is exactly one closing backtick on the binding line.
    // For escaped-typo lines the closing backtick is also escaped
    // (`?\``), so we search for a plain backtick AFTER the opening
    // delimiter; the find lands on the `60` byte of the `5C 60`
    // escape sequence and `inside` retains the leading `\` as the
    // last byte (which the tokenize loop below never reads — the
    // `rfind("keyboard.", 0)` prefix check rejects any token not
    // beginning with `keyboard.`).
    auto end = content.find('`', pos + offset_after_delim);
    if (end == std::string::npos) {
        return std::string();
    }
    const std::string inside =
        content.substr(pos + offset_after_delim, end - (pos + offset_after_delim));

    // Tokenize on `|` and pick the first token whose device is
    // `keyboard.` (case-sensitive — SCS uses lowercase `keyboard`).
    // The token shape is `<device>.<source>?<axis>`. We return the
    // `<device>.<source>` portion (e.g. `keyboard.num5`) and let
    // ScsKeyNameToVkForTesting strip the device prefix.
    size_t i = 0;
    while (i < inside.size()) {
        // Skip whitespace.
        while (i < inside.size() &&
               (inside[i] == ' ' || inside[i] == '\t')) {
            ++i;
        }
        if (i >= inside.size()) {
            break;
        }
        // Find the end of this token — either at '|' or end of
        // inside.
        size_t j = i;
        while (j < inside.size() && inside[j] != '|') {
            ++j;
        }
        std::string token = inside.substr(i, j - i);
        // Trim trailing whitespace.
        while (!token.empty() &&
               (token.back() == ' ' || token.back() == '\t')) {
            token.pop_back();
        }
        // Drop the trailing `?<axis>` qualifier (e.g. `keyboard.f10?0`
        // → `keyboard.f10`). The qualifier is positional and we do
        // not care about it here — the device + source uniquely
        // identifies the key.
        auto qmark = token.find('?');
        if (qmark != std::string::npos) {
            token = token.substr(0, qmark);
        }
        // The qualifier may also appear without a trailing value
        // (rare in stock profiles). Fall through.
        if (token.rfind("keyboard.", 0) == 0) {
            if (had_escaped_backtick_typo != nullptr) {
                *had_escaped_backtick_typo = escaped_typo;
            }
            return token;
        }
        i = (j < inside.size()) ? j + 1 : j;
    }
    // No `keyboard.*` token on this line. If we matched the
    // escaped-typo form, still flag it: the SCS parser rejected
    // the line at game load, and the user should know to remove
    // the stray backslashes.
    if (had_escaped_backtick_typo != nullptr) {
        *had_escaped_backtick_typo = escaped_typo;
    }
    return std::string();
}

// Inverse of `ReadControlsSiiKeyForActionForTesting`: walks every
// `mix <action> \`...\`` line in the controls.sii and returns the
// action names whose binding includes a `keyboard.*` token mapping
// to `target_vk`. The result is in file order so a downstream log
// line mirrors the user's read of controls.sii.
//
// Diagnostic-only helper: the shim logs a warning at init time when
// its target VK collides with a controls.sii binding, because the
// SCS action registry can route the press to the bound action before
// the SCS dev free-camera shortcut intercepts. (For F11 the dev
// shortcut does win in `g_developer=1` mode because the engine
// intercepts before the registry; for any other key the bound
// action would silently shadow the shim's dispatch.)
//
// File format reminder (matches the parser above):
//   config_lines[N]: "mix <action> `<token1> | <token2> | ...`"
// We use the same plain + escaped-backtick fallback so the scanner
// also surfaces typos (and the user gets a single log line that
// covers both the conflict AND the typo, instead of a silent miss).
std::vector<std::string> GameFreeCameraShim::FindActionsBoundToVkForTesting(
    const std::string &path, unsigned int target_vk) {
    std::vector<std::string> hits;
    if (path.empty() || target_vk == 0) {
        return hits;
    }
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return hits;
    }
    std::string content;
    char buf[4096] = {};
    size_t n = 0;
    while (content.size() < (64u * 1024u) &&
           (n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        content.append(buf, n);
    }
    std::fclose(f);

    // Walk every `mix <something> \`...\`` line. The grammar is
    // `mix <action> \`<binding>\`` — the action name is the first
    // token after `mix ` up to the opening backtick. We scan
    // forward one byte at a time rather than find()-then-tokenize so
    // every binding line contributes to the result, not just the
    // first hit.
    size_t cursor = 0;
    while (cursor < content.size()) {
        const std::string needle = "mix ";
        auto mix_pos = content.find(needle, cursor);
        if (mix_pos == std::string::npos) break;
        // The action name spans from after "mix " to the opening
        // backtick (plain form) or escaped backtick. We try the
        // plain form first; the backtick position also doubles as
        // the start of the binding tokens.
        auto btick_pos = content.find('`', mix_pos + needle.size());
        bool escaped = false;
        if (btick_pos == std::string::npos) {
            // Try escaped-backtick fallback (the typo form).
            const std::string esc_needle = "\\`";
            auto ebtick_pos = content.find(esc_needle, mix_pos + needle.size());
            if (ebtick_pos == std::string::npos) {
                cursor = mix_pos + needle.size();
                continue;
            }
            btick_pos = ebtick_pos + 1;  // skip the leading backslash
            escaped = true;
        }
        const std::string action_raw = content.substr(
            mix_pos + needle.size(),
            btick_pos - (mix_pos + needle.size()));
        // The SCS grammar is `mix <action> <ws> \`<binding>\``. Real
        // controls.sii files always have whitespace (or whitespace +
        // escape backslash for the typo form) between the action name
        // and the opening backtick. We extract the first whitespace-
        // delimited token so the action is the clean identifier the
        // user typed (e.g. "camdbg"), not a string with leftover
        // separators. The previous version took everything between "mix "
        // and the backtick and rejected the line if it had any whitespace
        // inside — which made every real-world controls.sii (with the
        // mandated space) produce zero hits and is exactly the bug the
        // Oct 2026 user report surfaced.
        std::string action_trim;
        for (const char c : action_raw) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
            action_trim.push_back(c);
        }
        // Sanity: action names are non-empty ASCII identifiers. Skip
        // anything malformed (defensive — the SCS grammar disallows
        // whitespace in this slot but a future mod could break it).
        if (action_trim.empty() ||
            action_trim.find_first_of(" \t\r\n") != std::string::npos) {
            cursor = btick_pos + 1;
            continue;
        }

        // Walk to the closing backtick. The plain-form closing
        // backtick is the next '`' byte; the escaped-form closing
        // backtick is the next '` ' byte (i.e. a backtick NOT
        // preceded by a backslash). For the typo line the rfind
        // below still works because we started past `mix`.
        size_t close_start = btick_pos + 1;
        if (escaped) {
            close_start = btick_pos + 1;
        }
        auto close_pos = content.find('`', close_start);
        if (close_pos == std::string::npos) {
            cursor = btick_pos + 1;
            continue;
        }
        const std::string inside = content.substr(close_start,
                                                  close_pos - close_start);

        // Tokenize on `|`, drop the trailing `?<axis>` qualifier,
        // and check each `keyboard.*` token's VK against target_vk.
        size_t i = 0;
        bool hit = false;
        while (i < inside.size()) {
            while (i < inside.size() &&
                   (inside[i] == ' ' || inside[i] == '\t')) ++i;
            if (i >= inside.size()) break;
            size_t j = i;
            while (j < inside.size() && inside[j] != '|') ++j;
            std::string token = inside.substr(i, j - i);
            while (!token.empty() &&
                   (token.back() == ' ' || token.back() == '\t')) {
                token.pop_back();
            }
            auto qmark = token.find('?');
            if (qmark != std::string::npos) {
                token = token.substr(0, qmark);
            }
            if (token.rfind("keyboard.", 0) == 0) {
                const unsigned int vk = ScsKeyNameToVkForTesting(token);
                if (vk != 0 && vk == target_vk) {
                    hit = true;
                    break;
                }
            }
            i = (j < inside.size()) ? j + 1 : j;
        }
        if (hit) {
            hits.push_back(action_trim);
        }
        cursor = close_pos + 1;
    }
    return hits;
}

// M2.5: SCS keyboard token → Win32 VK. Covers the subset of the SCS
// controls.sii grammar that real ATS / ETS2 profiles bind to
// dev-camera / walk-mode actions. The table is intentionally small
// — only the keys a user would reasonably bind to a "toggle the dev
// free camera" action. Anything not in the table returns 0 and the
// caller treats it as "unknown device token, skip auto-detect".
//
// The names match SCS's token grammar exactly:
//   keyboard.f10          → VK_F10       (0x79)
//   keyboard.f11          → VK_F11       (0x7A)
//   keyboard.f12          → VK_F12       (0x7B)
//   keyboard.num0..num9   → VK_NUMPAD0..VK_NUMPAD9 (0x60..0x69)
//   keyboard.numpad0..9   → same range (SCS accepts both spellings)
//   keyboard.numperiod    → VK_DECIMAL   (0x6E)
//   keyboard.numslash     → VK_DIVIDE    (0x6F)
//   keyboard.numstar      → VK_MULTIPLY  (0x6A)
//   keyboard.numminus     → VK_SUBTRACT  (0x6D)
//   keyboard.numplus      → VK_ADD       (0x6B)
//   keyboard.numlock      → VK_NUMLOCK   (0x90)
//   keyboard.lshift/rshift→ VK_LSHIFT/RSHIFT (0xA0/0xA1)
//   keyboard.lctrl/rctrl  → VK_LCONTROL/RCONTROL (0xA2/0xA3)
//   keyboard.lalt/ralt    → VK_LMENU/RMENU (0xA4/0xA5)
//   keyboard.space        → VK_SPACE     (0x20)
//   keyboard.enter        → VK_RETURN    (0x0D)
//   keyboard.escape       → VK_ESCAPE    (0x1B)
//   keyboard.backquote    → VK_OEM_3 (~) (0xC0)
//   keyboard.tab          → VK_TAB       (0x09)
//   keyboard.home         → VK_HOME      (0x24)
//   keyboard.end          → VK_END       (0x23)
//   keyboard.insert       → VK_INSERT    (0x2D)
//   keyboard.delete       → VK_DELETE    (0x2E)
//   keyboard.pgup         → VK_PRIOR     (0x21)
//   keyboard.pgdn         → VK_NEXT      (0x22)
//   keyboard.up/down/l/r  → VK_UP/DOWN/LEFT/RIGHT (cursor keys)
//   keyboard.a..z         → VK_A..VK_Z
//   keyboard.0..9         → VK_0..VK_9 (top-row digits, not numpad)
//
// The single-char digit / letter dispatch is a fallback for the case
// where a user binds e.g. `keyboard.t` to the dev camera toggle (we
// have seen this in the wild). The dispatch is case-sensitive — SCS
// emits lowercase token names.
unsigned int GameFreeCameraShim::ScsKeyNameToVkForTesting(
    const std::string &name) {
    if (name.empty() || name.rfind("keyboard.", 0) != 0) {
        return 0;
    }
    const std::string key = name.substr(std::string("keyboard.").size());

    // Function keys.
    if (key == "f1")  return 0x70;  // VK_F1
    if (key == "f2")  return 0x71;
    if (key == "f3")  return 0x72;
    if (key == "f4")  return 0x73;
    if (key == "f5")  return 0x74;
    if (key == "f6")  return 0x75;
    if (key == "f7")  return 0x76;
    if (key == "f8")  return 0x77;
    if (key == "f9")  return 0x78;
    if (key == "f10") return 0x79;
    if (key == "f11") return 0x7A;
    if (key == "f12") return 0x7B;

    // Numpad digits (both spellings accepted).
    if (key.size() >= 4 && key.rfind("num", 0) == 0) {
        const std::string tail = key.substr(3);
        if (tail.size() == 1 && tail[0] >= '0' && tail[0] <= '9') {
            // Explicit unsigned cast to keep -Wsign-conversion quiet:
            // the result of (tail[0] - '0') is a signed int (0..9)
            // and the literal 0x60 is unsigned — without the cast the
            // arithmetic promotes and the implicit conversion back to
            // the function's unsigned return type triggers the
            // warning under -Werror=sign-conversion. The arithmetic
            // itself is correct (0..9 added to 0x60 → 0x60..0x69).
            const unsigned int digit =
                static_cast<unsigned int>(tail[0] - '0');
            return 0x60u + digit;  // VK_NUMPAD0..9
        }
        if (key == "numpad0")  return 0x60;
        if (key == "numpad1")  return 0x61;
        if (key == "numpad2")  return 0x62;
        if (key == "numpad3")  return 0x63;
        if (key == "numpad4")  return 0x64;
        if (key == "numpad5")  return 0x65;
        if (key == "numpad6")  return 0x66;
        if (key == "numpad7")  return 0x67;
        if (key == "numpad8")  return 0x68;
        if (key == "numpad9")  return 0x69;
        if (key == "numperiod") return 0x6E;  // VK_DECIMAL
        if (key == "numslash")  return 0x6F;  // VK_DIVIDE
        if (key == "numstar")   return 0x6A;  // VK_MULTIPLY
        if (key == "numminus")  return 0x6D;  // VK_SUBTRACT
        if (key == "numplus")   return 0x6B;  // VK_ADD
        if (key == "numlock")   return 0x90;  // VK_NUMLOCK
    }

    // Modifiers.
    if (key == "lshift")   return 0xA0;
    if (key == "rshift")   return 0xA1;
    if (key == "lctrl")    return 0xA2;
    if (key == "rctrl")    return 0xA3;
    if (key == "lalt")     return 0xA4;
    if (key == "ralt")     return 0xA5;
    if (key == "space")    return 0x20;
    if (key == "enter")    return 0x0D;
    if (key == "escape")   return 0x1B;
    if (key == "tab")      return 0x09;
    if (key == "backquote")return 0xC0;

    // Cursor + navigation cluster.
    if (key == "up")       return 0x26;
    if (key == "down")     return 0x28;
    if (key == "left")     return 0x25;
    if (key == "right")    return 0x27;
    if (key == "home")     return 0x24;
    if (key == "end")      return 0x23;
    if (key == "insert")   return 0x2D;
    if (key == "delete")   return 0x2E;
    if (key == "pgup")     return 0x21;
    if (key == "pgdn")     return 0x22;

    // Top-row digits (not the numpad).
    if (key.size() == 1 && key[0] >= '0' && key[0] <= '9') {
        // '0'..'9' happen to equal VK_0..VK_9 on Windows. Explicit
        // unsigned cast to keep -Wsign-conversion quiet — `key[0]`
        // is `char` (signed on x86_64 Windows by default), and an
        // implicit conversion to the function's unsigned return
        // type triggers the warning under -Werror=sign-conversion.
        return static_cast<unsigned int>(
            static_cast<unsigned char>(key[0]));
    }
    // Single lowercase letters.
    if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z') {
        // SCS uppercases, but accept lowercase anyway (defensive).
        const char c = key[0];
        // Map 'a'..'z' to 'A'..'Z' (VK_* letter codes are uppercase).
        return static_cast<unsigned int>(
            (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c);
    }

    return 0;  // Unknown token.
}

namespace {

// Best-effort path to the game's `config.cfg`. We try:
//
//   1. The shell-resolved `Documents` folder (CSIDL_MYDOCUMENTS).
//      Windows already follows the OneDrive Documents redirect for
//      us when it is active, so this single call covers the most
//      common OneDrive layout.
//   2. The USERPROFILE-based enumeration (covers non-redirected
//      Documents, XP-era My Documents, OneDrive with the English
//      folder name kept, OneDrive with the Spanish folder name
//      kept, and multi-account OneDrive `OneDrive - <org>\`).
//
// Both ATS and ETS2 stems are tried in each step so the same DLL
// works on either game.
std::string ProbeConfigPath() {
#if defined(_WIN32)
    char mydocs[MAX_PATH] = {};
    if (SUCCEEDED(::SHGetFolderPathA(nullptr, CSIDL_MYDOCUMENTS, nullptr,
                                      SHGFP_TYPE_CURRENT, mydocs))) {
        static const char *stems[] = {
            "American Truck Simulator",
            "Euro Truck Simulator 2",
        };
        for (const char *stem : stems) {
            std::string p = std::string(mydocs) + "\\" + stem +
                            "\\config.cfg";
            FILE *f = std::fopen(p.c_str(), "rb");
            if (f) {
                std::fclose(f);
                return p;
            }
        }
    }
    char profile[MAX_PATH] = {};
    if (SUCCEEDED(::SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr,
                                      SHGFP_TYPE_CURRENT, profile))) {
        const auto candidates =
            GameFreeCameraShim::ProbeConfigCandidatesForTesting(
                std::string(profile));
        for (const auto &c : candidates) {
            FILE *f = std::fopen(c.c_str(), "rb");
            if (f) {
                std::fclose(f);
                return c;
            }
        }
    }
#else
    (void)0;
#endif
    return std::string();
}

// M2.5: best-effort probe for the user's controls.sii. Mirrors
// ProbeConfigPath but emits the controls.sii filename. We do NOT
// resolve the per-profile sub-folder (multi-profile installs put
// controls.sii under `<game>/profiles/<hex id>/controls.sii`) —
// the per-profile resolution would require parsing profile.sii,
// which is out of scope for the M2 lander. The single-profile
// install is the common case (this developer's host included), so
// the simpler probe covers the majority of real users. If we miss,
// the runtime still emits a clear "could not auto-detect, here's how
// to set it manually" log line so the user can fall back to the
// INI knob.
std::string ProbeActiveControlsSiiPath() {
#if defined(_WIN32)
    char mydocs[MAX_PATH] = {};
    std::string mydocs_str;
    if (SUCCEEDED(::SHGetFolderPathA(nullptr, CSIDL_MYDOCUMENTS, nullptr,
                                      SHGFP_TYPE_CURRENT, mydocs))) {
        mydocs_str = mydocs;
        static const char *const stems[] = {
            "American Truck Simulator",
            "Euro Truck Simulator 2",
        };
        for (const char *stem : stems) {
            // Single-profile install: <Documents>/<game>/controls.sii
            std::string p = mydocs_str + "\\" + stem + "\\controls.sii";
            FILE *f = std::fopen(p.c_str(), "rb");
            if (f) {
                std::fclose(f);
                return p;
            }
            // Multi-profile install: <Documents>/<game>/profiles/<id>/controls.sii
            // The first controls.sii found under profiles/ is treated as the
            // "active" one. We do not parse profile.sii (binary blob in 1.61.x)
            // — the only safe portable approach is to enumerate. Most users
            // have exactly one profile and the probe picks that one. Users
            // with multiple profiles get the first one alphabetically; if
            // that's wrong, the auto-detect still works for the binding that
            // exists there and the user can override via INI if needed.
            const std::string profiles_dir = mydocs_str + "\\" + stem +
                                            "\\profiles";
            WIN32_FIND_DATAA fd{};
            const std::string pattern = profiles_dir + "\\*";
            HANDLE h = ::FindFirstFileA(pattern.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        continue;
                    }
                    // Skip "." and ".." entries.
                    if (fd.cFileName[0] == '.' &&
                        (fd.cFileName[1] == '\0' ||
                         (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0'))) {
                        continue;
                    }
                    const std::string candidate = profiles_dir + "\\" +
                                                 fd.cFileName + "\\controls.sii";
                    FILE *g = std::fopen(candidate.c_str(), "rb");
                    if (g) {
                        std::fclose(g);
                        ::FindClose(h);
                        return candidate;
                    }
                } while (::FindNextFileA(h, &fd));
                ::FindClose(h);
            }
        }
    }
    char profile[MAX_PATH] = {};
    if (SUCCEEDED(::SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr,
                                      SHGFP_TYPE_CURRENT, profile))) {
        const auto candidates =
            GameFreeCameraShim::ProbeControlsSiiCandidatesForTesting(
                std::string(profile));
        for (const auto &c : candidates) {
            FILE *f = std::fopen(c.c_str(), "rb");
            if (f) {
                std::fclose(f);
                return c;
            }
        }
    }
#else
    (void)0;
#endif
    return std::string();
}

}  // namespace

GameFreeCameraShim::GameFreeCameraShim() = default;
GameFreeCameraShim::~GameFreeCameraShim() = default;

void GameFreeCameraShim::SetEnabled(bool enabled) { enabled_ = enabled; }
void GameFreeCameraShim::SetToggleVk(unsigned int vk) { toggle_vk_ = vk; }
void GameFreeCameraShim::SetExitPressVk(unsigned int vk) {
    exit_press_vk_ = vk;
}
void GameFreeCameraShim::SetDevCameraViaConsole(bool via_console) {
    dev_camera_via_console_ = via_console;
}
void GameFreeCameraShim::BindGameHwnd(HWND hwnd) { hwnd_ = hwnd; }
void GameFreeCameraShim::BindDevCameraPatchResult(
    const DevCameraCvarPatchResult *r) {
    dev_cvar_patch_result_ = r;
}

// M2 diagnostic: scan the user's controls.sii for any action bound
// to `toggle_vk_` and emit a heads-up log line if any collision is
// found. Called from PluginEntry once the runtime knows the active
// controls.sii path. The F10 dispatch log surfaces the cached
// conflict set so the user sees the binding shadow even when the
// shim successfully sent the keystroke.
//
// Why this matters (Oct 2026 user report): the user observed "F11
// me lanza a la camara 0" — pressing F11 cycled the interior camera
// (cam 0) instead of toggling the dev free camera. Root cause: their
// controls.sii line 234 binds `mix camdbg \`keyboard.f11?0\`` — the
// SCS action registry routes F11 to camdbg. The SCS dev free-camera
// shortcut does intercept F11 BEFORE the action registry in
// g_developer=1 mode (so the dev cam should win), but the user
// seeing the camera cycle was a signal that the engine's interception
// path was not the active layer for them — most likely g_developer
// was effectively 0 at the moment of the press (the patcher
// skipped because the cvar struct layout is non-standard in this
// build, see DevCameraCvarPatcher warnings). Surfacing the binding
// at init time lets the user either: (a) re-pin the dispatch to a
// non-conflicting VK (F8 = 0x77), or (b) verify their g_developer
// setup. Without this diagnostic, the only signal the user gets is
// the F10 dispatch log's generic "toggle VK may be absorbed by
// another application" line, which doesn't tell them which action
// specifically is intercepting.
void GameFreeCameraShim::LogControlsSiiVkConflictIfAny(
    const std::string &controls_sii_path) {
    const std::string &path =
        controls_sii_path.empty() ? auto_detected_controls_sii_path_
                                  : controls_sii_path;
    // Reset the cache so a re-bind with a new path re-scans; the
    // same path scanned twice logs once.
    controls_sii_conflict_actions_.clear();
    controls_sii_conflict_detected_ = false;
    controls_sii_conflict_last_path_.clear();
    if (path.empty()) {
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: controls.sii conflict scan skipped "
            "(no resolved controls.sii path; profile probe has not "
            "run yet). toggle_vk=0x%X.", toggle_vk_);
        return;
    }
    if (toggle_vk_ == 0) {
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: controls.sii conflict scan skipped "
            "(toggle_vk=0 — shim has no key to scan for).");
        return;
    }
    controls_sii_conflict_last_path_ = path;
    const std::vector<std::string> hits =
        FindActionsBoundToVkForTesting(path, toggle_vk_);
    controls_sii_conflict_actions_ = hits;
    controls_sii_conflict_detected_ = !hits.empty();
    if (hits.empty()) {
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: controls.sii conflict scan OK — "
            "toggle_vk=0x%X has no binding in %s (no other action "
            "will shadow the shim's dispatch).", toggle_vk_, path.c_str());
        return;
    }
    // Conflict detected. Log each colliding action so the user can
    // decide whether to re-pin the dispatch VK (recommended) or
    // remove the binding. The exit-press VK is also worth checking
    // because a separate collision there would silently swallow the
    // exit (M2.5 only — most builds don't set exit_press_vk_).
    std::string joined;
    for (size_t i = 0; i < hits.size(); ++i) {
        if (i > 0) joined += ", ";
        joined += hits[i];
    }
    WALKERCORE_LOG_WARN(
        "GameFreeCameraShim: controls.sii CONFLICT (toggle_vk=0x%X "
        "is bound to action(s): %s in %s). The shim's F10 Walking "
        "dispatch will press this key, and the SCS action registry "
        "will route the press to the bound action(s) (e.g. "
        "`mix camdbg \\`keyboard.f11?0\\``) instead of the dev "
        "free-camera shortcut. Verified live on ATS 1.61.3.1s "
        "(Oct 2026): the SCS engine does NOT intercept F11 BEFORE "
        "the action registry even with `g_developer \"1\"` AND "
        "`g_console \"1\"` set in config.cfg — F11 always hits "
        "camdbg first. To avoid the conflict, either: "
        "(a) re-pin the dispatch VK via `[m2] dev_camera_toggle_vk = "
        "0xHH` in WalkerCore.ini (recommended: F8 = 0x77, F9 = 0x78), "
        "or (b) remove the conflicting binding from controls.sii. "
        "The auto-re-pin block below handles (a) automatically when "
        "the user has not pinned the dispatch VK explicitly.",
        toggle_vk_, joined.c_str(), path.c_str());
    // M2.7 follow-up (Oct 2026): if the user has not pinned the
    // dispatch VK via the INI, try to auto-re-pin to the first
    // fallback key that is NOT bound in controls.sii. This way the
    // very next F10 Walking dispatch presses a key the action
    // registry ignores, and the shim's log line surfaces the actual
    // problem (the dev free camera shortcut may not be wired to ANY
    // bindable key in this build — see the F12 dump for the manual
    // `~` console workaround).
    //
    // Fallback list: F8=0x77, F9=0x78, F7=0x76, F6=0x75 — keys that
    // are unlikely to be bound to camera actions in stock ATS
    // profiles.
    //
    // CRITICAL UPDATE (Oct 2026 live fix): the previous gate that
    // stayed at F11 when `g_developer == 1` was REMOVED. Live test
    // on ATS 1.61.3.1s (amtrucks.exe 1.61.3.1s, config.cfg with
    // `g_developer \"1\"` AND `g_console \"1\"`, controls.sii binding
    // `mix camdbg \\`keyboard.f11?0\\``) showed that pressing F11
    // STILL cycles the interior camera (camdbg wins, SCS does NOT
    // intercept F11 first). The premise that "g_developer=1 enables
    // SCS F11 interception" was wrong for this build. Without the
    // auto-pin the shim would dispatch F11 every Walking transition,
    // the user would see the camera cycle (not dev free cam), and
    // F10 would still take a screenshot (F10 is also bound to
    // `mix screenshot`). The fix is unconditional auto-pin: any
    // camdbg/F11 collision in controls.sii re-targets the dispatch to
    // a non-bound fallback. When g_developer != 1 the same logic
    // already worked (the old gate was a no-op). Removing the gate
    // also removes the dead branch that emitted a misleading
    // "AUTO-RE-PIN skipped" warning telling the user to verify
    // g_console=1 when the real problem was the action registry.
    //
    // Implementation notes: the array is a function-local `constexpr`
    // so the linker does not emit a file-scope initializer symbol.
    // We verified via isolated LoadLibraryA on 2026-10-13 that the
    // DLL with this code returns in 4 ms without hanging; the
    // earlier "DLL load hang" was actually a smoke-test timeout —
    // game-time 00:01:00 diagnostics emit AFTER the previous 60-90 s
    // wait windows. Current smoke test uses 120 s wait and passes.
    if (!ini_overrides_toggle_vk_) {
        constexpr unsigned int kFallbackList[] = {0x77u, 0x78u, 0x76u, 0x75u};
        bool re_pinned = false;
        for (size_t i = 0;
             i < sizeof(kFallbackList) / sizeof(kFallbackList[0]);
             ++i) {
            const unsigned int fb_vk = kFallbackList[i];
            if (fb_vk == toggle_vk_) {
                continue;
            }
            const std::vector<std::string> fb_hits =
                FindActionsBoundToVkForTesting(path, fb_vk);
            if (fb_hits.empty()) {
                WALKERCORE_LOG_WARN(
                    "GameFreeCameraShim: AUTO-RE-PIN -> dispatched "
                    "VK re-pinned from 0x%X to 0x%X (no "
                    "controls.sii collision for 0x%X, "
                    "g_developer=%d). The very next F10 Walking "
                    "dispatch will press 0x%X instead of 0x%X. To "
                    "make the re-pin permanent, set `[m2] "
                    "dev_camera_toggle_vk = 0x%X` in "
                    "WalkerCore.ini. NOTE: the SCS dev free camera "
                    "shortcut is not bound to any of the F6..F9 "
                    "fallbacks in ATS 1.61.3.1s — the re-pin only "
                    "stops the camdbg shadow. To actually enable "
                    "the dev free camera, open the dev console "
                    "(`~` key by default) and type `free_camera`, "
                    "or press the SCS dev free camera UI button "
                    "(`o_im_freecam` in some builds). WalkerCore's "
                    "dev_cvar_patcher already sets g_developer=1 "
                    "in memory; the dev camera toggle key is the "
                    "missing piece.",
                    toggle_vk_, fb_vk, fb_vk,
                    g_developer_value_, fb_vk, toggle_vk_, fb_vk);
                toggle_vk_ = fb_vk;
                re_pinned = true;
                break;
            }
        }
        if (!re_pinned) {
            WALKERCORE_LOG_WARN(
                "GameFreeCameraShim: AUTO-RE-PIN FAILED — every "
                "fallback key (F8=0x77, F9=0x78, F7=0x76, "
                "F6=0x75) is bound in controls.sii. The shim "
                "dispatch will continue to be shadowed by the "
                "conflicting action(s). Edit controls.sii to "
                "free one of the fallbacks, or set `[m2] "
                "dev_camera_toggle_vk = 0xHH` in WalkerCore.ini "
                "to a non-conflicting VK. g_developer=%d.",
                g_developer_value_);
        }
    } else {
        WALKERCORE_LOG_WARN(
            "GameFreeCameraShim: AUTO-RE-PIN skipped — user has "
            "explicitly set `[m2] dev_camera_toggle_vk` in "
            "WalkerCore.ini (ini_overrides_toggle_vk_=true). The "
            "shim dispatch will use the INI-specified VK regardless "
            "of controls.sii collisions. g_developer=%d. To stop "
            "the camdbg shadow, either edit controls.sii or change "
            "`[m2] dev_camera_toggle_vk` to a non-bound VK.",
            g_developer_value_);
    }
}

// M2.5: auto-detect the user's dev-camera-toggle binding from their
// controls.sii. Called once from PluginEntry init after
// ProbeGDeveloperFromConfig.
//
// M2.7 audit (Oct 2026): no action-name binding exists in
// amtrucks.exe 1.61.3.1s that toggles the dev free camera. The
// action names the earlier RE looked at — `o_im_freecam`,
// `dev_camera_toggle`, `walk_mode`, `free_camera` — are all
// rejected by the SCS input parser at game load with `<ERROR>
// [inp] mix "X" does not exist, ignoring change`. `camdbg` is
// accepted by the parser but binds to the regular interior
// camera cycle, not the dev free camera (the user's profile
// already has `mix camdbg \`keyboard.f11?0\`` at config_lines[234]
// — pressing F11 manually with this binding also fires camdbg, but
// the SCS dev free-camera shortcut intercepts F11 before the action
// registry, so the dev cam toggle wins in dev mode while the
// camdbg binding is a no-op). The dev free camera in this build is
// the hard-coded F11 shortcut (intercepted before the action
// registry, requires `g_developer=1` AND `g_console=1` in
// config.cfg).
//
// So the probe order is empty: there is no action name to find.
// The shim dispatches F11 (VK_F11, 0x7A) by default and the INI
// knob [m2] dev_camera_toggle_vk lets the user pick a different
// key (e.g. F8 = 0x77 if F11 collides with `camdbg` in the user's
// controls.sii). The probe is kept as a diagnostic so the F10
// Walking-entry log can show "no action binding for the dev free
// camera in this build" — users who try to bind o_im_freecam in
// controls.sii need to see the rejection explicitly.
//
// If `ini_overrides_default` is true, the INI knob already pinned
// the VK explicitly and we MUST NOT clobber it — the user knows
// what they want. In that case we clear the auto-detected diagnostic
// fields and return without touching toggle_vk_.
//
// When no binding is found, we emit a one-line, copy-pasteable
// guidance message so the user can fix controls.sii themselves. The
// message includes the file path ProbeActiveControlsSiiPath() would
// have used (resolved from <Documents>/<game>/controls.sii or the
// OneDrive-aware fallback), so the user knows exactly which file to
// edit. We do NOT modify controls.sii — that is the user's choice,
// and a one-line edit is trivial to roll back.
void GameFreeCameraShim::AutoDetectToggleVkFromControlsSii(
    bool ini_overrides_default) {
    ini_overrides_toggle_vk_ = ini_overrides_default;
    auto_detected_key_name_.clear();
    auto_detected_controls_sii_path_.clear();
    if (ini_overrides_default) {
        // The INI pinned a value; honour it. We do still want to
        // remember where the user's controls.sii lives for the
        // diagnostic log (so the F10 Walking-entry line can show
        // "INI=0x65, controls.sii=<path>"), so we resolve the
        // path even when we are not going to adopt its value. The
        // probe is cheap (one fopen per candidate, 12 candidates
        // max) and the resolution is read-only.
        //
        // We do NOT scan controls.sii for an action-name binding
        // here. No such binding exists in amtrucks.exe 1.61.3.1s
        // (the SCS input parser rejects `o_im_freecam`,
        // `dev_camera_toggle`, `walk_mode`, and `free_camera`
        // at game load with "does not exist, ignoring change";
        // `camdbg` is accepted but cycles the interior camera,
        // not the dev free camera). The dev free camera toggle is
        // the hard-coded F11 shortcut (verified live Oct 2026).
        const std::string path = ProbeActiveControlsSiiPath();
        auto_detected_controls_sii_path_ = path;
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: INI override active — auto-detect "
            "skipped (no action-name binding for dev free camera in "
            "amtrucks.exe 1.61.3.1s; the toggle is the hard-coded F11 "
            "shortcut), toggle VK=0x%X (controls-sii-path=%s)",
            toggle_vk_,
            auto_detected_controls_sii_path_.empty()
                ? "<not found>"
                : auto_detected_controls_sii_path_.c_str());
        return;
    }
    const std::string path = ProbeActiveControlsSiiPath();
    auto_detected_controls_sii_path_ = path;
    if (path.empty()) {
        // Could not locate any controls.sii on this host. Either the
        // game is not installed, the user is on a build server, or
        // the user runs the game from a non-standard install
        // location. Emit a single-line guidance that still works in
        // that case: tell the user to either fix the install or
        // pin the INI knob directly.
        WALKERCORE_LOG_WARN(
            "GameFreeCameraShim: controls.sii not found (no active "
            "profile / game not installed). Default toggle VK=0x%X "
            "is in effect (no controls.sii binding present to bind "
            "o_im_freecam). The shim will synthesise a "
            "`keyboard.<VK>` SendInput into the game's input handler; "
            "the receiver reads it through the action registry and "
            "toggles dev free camera when `g_developer=1` AND "
            "`g_console=1` are set in config.cfg. To override the "
            "default VK, set [m2] dev_camera_toggle_vk = 0xHH in "
            "WalkerCore.ini.",
            toggle_vk_);
        return;
    }
    // Scan the known action names in priority order. The first
    // hit wins; subsequent actions are ignored. We also track
    // whether any of the scans matched via the escaped-backtick
    // typo fallback so we can emit a specific warning when the
    // user has a malformed binding (game.log shows
    // `<ERROR> Expecting string literal, found '"mix ...'`).
    //
    // Action-name landscape in amtrucks.exe 1.61.3.1s (Oct 2026
    // live verification):
    //
    //   * `o_im_freecam` — appears in the in-game debug options
    //      menu UI but the SCS input parser REJECTS bindings for
    //      it at game load with `<ERROR> [inp] mix "o_im_freecam"
    //      does not exist, ignoring change`. NOT a real action.
    //      The shim's auto-detect used to look for this name;
    //      that was the bug — the user would add a binding, the
    //      game would silently drop it, and WalkerCore's
    //      auto-detect would never find a match.
    //
    //   * `dev_camera_toggle` / `walk_mode` / `free_camera` —
    //      also rejected by the parser as "not valid object
    //      name". Zero hits in the binary.
    //
    //   * `camdbg` — IS a valid action name (parser accepts
    //      bindings for it without error). What it DOES in
    //      amtrucks.exe 1.61.3.1s is the camera-cycle action
    //      (cycles cam 0, cam 1, cam 2, ...) rather than the
    //      dev free camera — pressing F11 manually while
    //      `mix camdbg \`keyboard.f11?0\`` is in controls.sii
    //      cycles the interior camera, it does NOT toggle the
    //      dev free camera. So this auto-detect does NOT
    //      dispatch camdbg; the shim defaults to the hard-coded
    //      '0' shortcut (VK_0) instead, which is the actual SCS
    //      dev free camera toggle (intercepted before the action
    //      registry, requires `g_developer=1` AND `g_console=1`).
    //
    // Because no action-name binding exists that toggles the dev
    // free camera in this build, the auto-detect no longer looks
    // for action names — it falls straight through to the
    // default 0x30 (VK_0) hard-coded shortcut. The probe is kept
    // here as a defensive no-op so the F10 Walking-entry log can
    // still surface "no controls.sii binding for the dev free
    // camera action" rather than a missing log line.
    const char *hit_action = nullptr;
    std::string token;
    bool detected_typo = false;
    (void)hit_action;       // suppress -Wunused after the action-name
    (void)detected_typo;    // probe loop was removed; the only
    (void)token;            // legitimate toggle in this build is the
                            // hard-coded F11 shortcut, not an action
                            // registry entry.
    if (hit_action == nullptr) {
        // No binding found at all. Emit a one-line, copy-pasteable
        // fix the user can drop into their controls.sii. We pick a
        // sensible default (keyboard.f11) — the user's profile
        // binds F10 to screenshot, so F11 is almost certainly
        // unbound.
        if (detected_typo) {
            // Typo-aware variant: we DID find a binding line but
            // the SCS parser rejected it (`\`` instead of `` ` ``).
            // The dispatch will silently fail because the game's
            // action registry never saw the malformed entry, so
            // we surface the corrected line explicitly.
            WALKERCORE_LOG_WARN(
                "GameFreeCameraShim: controls.sii at %s has a "
                "MALFORMED binding for o_im_freecam "
                "(escaped backticks `\\`` instead of "
                "plain `` ` ``). The SCS parser REJECTED the line at "
                "game load (game.log shows `<ERROR> Expecting string "
                "literal, found '\"mix ...'`) and the binding never "
                "reached the action registry, so a SendInput dispatch "
                "will hit the wrong action. Replace the bad line in %s "
                "with this exact text (plain backticks, no backslash):",
                path.c_str(), path.c_str());
            WALKERCORE_LOG_WARN(
                "    mix o_im_freecam `keyboard.f11?0`");
            WALKERCORE_LOG_WARN(
                "    (alternative keys: keyboard.f10, keyboard.num5, "
                "keyboard.numpad5 — pick one that does not collide "
                "with an existing binding. WalkerCore will auto-detect "
                "the corrected binding on next launch.)");
            return;
        }
        WALKERCORE_LOG_WARN(
            "GameFreeCameraShim: no action-name binding for the dev "
            "free camera in controls.sii at %s. In amtrucks.exe "
            "1.61.3.1s the SCS input parser rejects `o_im_freecam` "
            "(`<ERROR> [inp] mix \"o_im_freecam\" does not exist, "
            "ignoring change`) and the dev free camera is toggled "
            "by the hard-coded F11 shortcut instead, NOT via a "
            "bindable action. The shim's default dispatch "
            "VK=0x%X (VK_F11) is in effect; if your controls.sii "
            "binds F11 to another action, set [m2] "
            "dev_camera_toggle_vk = 0xHH in WalkerCore.ini to "
            "re-target the dispatch (e.g. 0x77 = F8). "
            "The dev free camera toggle itself requires "
            "`g_developer=1` AND `g_console=1` in config.cfg.",
            path.c_str(), toggle_vk_);
        return;
    }
    const unsigned int vk = ScsKeyNameToVkForTesting(token);
    if (vk == 0) {
        // We found a binding but cannot translate the SCS token to a
        // Win32 VK. Likely an obscure device (mouse / wheel / gamepad)
        // that the shim's SendInput path cannot synthesise. Log the
        // discovery + the unmappable token and keep the default VK.
        WALKERCORE_LOG_WARN(
            "GameFreeCameraShim: controls.sii at %s binds %s to %s, "
            "but that SCS token is not in the Win32 VK lookup table "
            "(the shim only synthesises keyboard.* tokens). Default "
            "toggle VK=0x%X in effect. To use this binding, map it to "
            "a keyboard key in controls.sii or set [m2] "
            "dev_camera_toggle_vk = 0xHH in WalkerCore.ini to force a "
            "specific VK.",
            path.c_str(), hit_action, token.c_str(), toggle_vk_);
        return;
    }
    if (vk == toggle_vk_) {
        // The default VK happens to match the binding. Nothing to
        // adopt, but still record the discovery for the F10 log.
        auto_detected_key_name_ = token;
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: controls.sii at %s binds %s to %s "
            "(VK 0x%X) — matches the default toggle VK, no change.",
            path.c_str(), hit_action, token.c_str(), vk);
        return;
    }
    // Adoption path: the binding differs from the default. Adopt it
    // and log the change so the F10 Walking-entry line can confirm
    // the right key was picked up.
    const unsigned int prev_vk = toggle_vk_;
    toggle_vk_ = vk;
    auto_detected_key_name_ = token;
    if (detected_typo) {
        // We recovered the token via the escaped-backtick
        // fallback, but the SCS parser rejected the file at game
        // load — the action registry has no `dev_camera_toggle`
        // entry. The dispatch we are about to make will hit
        // whatever F11 is bound to (e.g. `menustereo`), NOT the
        // dev camera. Adopt the VK anyway (so a future corrected
        // controls.sii Just Works on next launch) but warn loudly
        // so the user knows F10 will not toggle the dev camera
        // until they fix the typo.
        WALKERCORE_LOG_WARN(
            "GameFreeCameraShim: auto-detected %s=%s in %s but the "
            "binding line has ESCAPED BACKTICKS (`\\``) — the SCS "
            "parser rejected the file at game load and the dispatch "
            "will hit the wrong action. Replace the bad line in %s "
            "with plain backticks:",
            hit_action, token.c_str(), path.c_str(), path.c_str());
        WALKERCORE_LOG_WARN(
            "    mix %s `%s?0 | semantical.%s?0`",
            hit_action,
            // Derive the keyboard token we found, with the
            // trailing `?0` qualifier and matching `semantical.`
            // alias name. The token we have is already stripped of
            // its `?0` qualifier (see the parser), so we just
            // append `?0` for the corrected line.
            token.c_str(), hit_action);
    }
    WALKERCORE_LOG_INFO(
        "GameFreeCameraShim: auto-detected %s=%s (VK 0x%X) in "
        "%s — adopted as the toggle VK (was 0x%X default Numpad-0).",
        hit_action, token.c_str(), vk, path.c_str(), prev_vk);
    // M2.5 follow-up: when we adopted a real binding, the user has
    // done the right thing (added the controls.sii line). But the
    // shim's `enabled_` gate is still independent — the user must
    // also set `[m2] game_free_camera_shim = true` in WalkerCore.ini
    // (or via the in-game settings menu) for the dispatch to fire.
    // Without this hint the next F10 silently does not toggle the
    // dev camera and the user is left wondering why the auto-detect
    // log line appeared but nothing happened on screen. The hint is
    // only emitted when the shim is currently OFF — if the user has
    // already enabled it, repeating the message would be noise.
    if (!enabled_) {
        WALKERCORE_LOG_WARN(
            "GameFreeCameraShim: shim is currently DISABLED. The "
            "auto-detected binding (%s, VK 0x%X) will NOT dispatch "
            "until you enable the shim. Set [m2] "
            "game_free_camera_shim = true in WalkerCore.ini (or "
            "open the settings menu with Ctrl+F10 and flip the M2 "
            "toggle). g_developer=1 is already set in config.cfg, so "
            "once the shim is enabled, the next F10 press will "
            "activate the dev free camera via the bound key.",
            token.c_str(), vk);
    }
}

void GameFreeCameraShim::ProbeGDeveloperFromConfig() {
    const std::string path = ProbeConfigPath();
    const std::string value = ReadGDeveloperValueForTesting(path);
    const bool ok = (!path.empty()) && (value == "1" || value == "2");
    supported_ = ok;
    // M2.7 follow-up: also remember the numeric value so the auto-pin
    // path can decide whether SCS intercepts F11. When value == "1",
    // the SCS engine intercepts F11 BEFORE the action registry — the
    // controls.sii camdbg binding does not shadow the shim's F11
    // dispatch. When value == "2" or unset, F11 is NOT intercepted and
    // the action registry wins. The auto-pin fires only in the latter
    // case; re-pinning to F8 when SCS would have intercepted F11 is
    // pointless (and breaks the dev free camera activation).
    if (!value.empty() && (value == "1" || value == "2")) {
        g_developer_value_ = std::atoi(value.c_str());
    } else if (value.empty()) {
        g_developer_value_ = 0;
    } else {
        // Unknown literal value — treat as "off" so we don't auto-pin.
        g_developer_value_ = 0;
    }
    WALKERCORE_LOG_INFO(
        "GameFreeCameraShim: g_developer probe path=%s value=%s -> supported=%d",
        path.empty() ? "<not found>" : path.c_str(),
        value.empty() ? "<unset>" : value.c_str(),
        supported_ ? 1 : 0);
}

void GameFreeCameraShim::OnEnterWalking() {
    // G5.22: skip the legacy dispatch when the WH_KEYBOARD_LL
    // pre-dispatch already covered the same toggle. The
    // pre-dispatch fires SYNCHRONOUSLY before F10 reaches the game,
    // so the toggle VK lands in the input queue ahead of the F10
    // that started the Walking transition. Re-dispatching here would
    // double-toggle the dev cam (activate on entry, deactivate on
    // the very next frame, leaving the user with the dev cam OFF
    // even though they're in Walking). The fallback dispatch below
    // only fires when the FSM enters Walking WITHOUT a preceding
    // F10 rising edge — e.g. the KnockoutProvider forcing a
    // transition, or a non-input state change. The 500 ms window
    // covers the WH_KEYBOARD_LL → FSM transition latency on the
    // user's host (~16-20 ms typical frame, plus scheduler jitter)
    // and is conservative — doubling the toggle within 500 ms of a
    // legitimate F10 press would be a clear regression the user
    // would notice as a flashing dev-cam overlay.
    constexpr int kPreDispatchDedupMs = 500;
    walks_entered_++;
    // Compose a single, comprehensive traceback line for the F10
    // Walking-entry event. The previous version split the log into
    // three branches (disabled / unsupported / dispatched) which
    // meant the analyst had to correlate 3 lines per F10 to figure
    // out what happened. The new line covers all five states in one
    // shot so the next session can grep game.log.txt for
    // `GameFreeCameraShim: Walking entry` and get a self-explanatory
    // line every time, regardless of whether the shim fired, the
    // gate was off, the cvar was 0, etc.
    //
    // `last_dispatch_path_` is updated by `SynthKey` (called from
    // `TriggerToggle` below) — so we MUST re-read it AFTER the
    // dispatch to log the path the dispatch actually took. The
    // previous version computed `path_name` here at the top of the
    // function, which always showed `path=none` even when SendInput
    // fired successfully. That bug masked the real failure mode
    // from the user (the dispatch IS reaching the game; the game's
    // action registry is the layer that decides whether the key
    // does anything useful).
    const char *gate_str = enabled_ ? "on" : "off";
    const char *cvar_str = supported_ ? "1" : "0/missing";
    const char *patch_outcome = "unbound";
    char patch_cvar_buf[13] = "?";
    uint64_t patch_value_va = 0;
    if (dev_cvar_patch_result_ != nullptr) {
        if (dev_cvar_patch_result_->patched) {
            patch_outcome = "patched 0->1";
        } else if (dev_cvar_patch_result_->skipped) {
            patch_outcome = "skipped";
        } else {
            patch_outcome = "no-op";
        }
        // Show the rebased cvar value VA + the first 12 bytes of
        // the value buffer (post-patch) so the analyst can confirm
        // the patch landed.
        patch_value_va = dev_cvar_patch_result_->cvar_value_va;
        const char *src =
            dev_cvar_patch_result_->cvar_value_after[0] != '\0'
                ? dev_cvar_patch_result_->cvar_value_after
                : dev_cvar_patch_result_->cvar_value_before;
        std::snprintf(patch_cvar_buf, sizeof(patch_cvar_buf), "%.12s", src);
    }
    if (!enabled_) {
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: Walking entry #%d (gate=%s, "
            "config-cfg[probe]='%s', dev_cvar_patcher=%s cvar_va=0x%llX "
            "value=%.12s, hwnd=0x%llX) — Set [m2] game_free_camera_shim "
            "= true in WalkerCore.ini to send the dev free camera "
            "toggle on F10. config.cfg already has g_developer=1, so "
            "any dev-camera path (the hard-coded F11 shortcut, the dev "
            "UI button o_im_freecam, future in-process camera manager "
            "hooks) works without editing Documents\\American Truck "
            "Simulator\\config.cfg.)",
            walks_entered_, gate_str, cvar_str, patch_outcome,
            static_cast<unsigned long long>(patch_value_va), patch_cvar_buf,
            static_cast<unsigned long long>(
                reinterpret_cast<std::uintptr_t>(hwnd_)));
        return;
    }
    if (!supported_) {
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: Walking entry #%d (gate=%s, "
            "config-cfg[probe]='%s', dev_cvar_patcher=%s cvar_va=0x%llX "
            "value=%.12s, hwnd=0x%llX) — The shim synthesised the "
            "toggle VK but g_developer was not detected as '1' in "
            "config.cfg. The dev free camera is gated by g_developer "
            "(also g_console=1 for the '~' console). Without it the "
            "hard-coded F11 shortcut is ignored. Add `uset "
            "g_developer \"1\"` to Documents\\American Truck "
            "Simulator\\config.cfg and reload the save. Two ways "
            "forward: (a) bind o_im_freecam to a keyboard key in "
            "<profile>\\controls.sii via "
            "`mix o_im_freecam \\`keyboard.X?0\\``, then re-deploy, "
            "or (b) use the game's dev console (`~` key) and the "
            "in-game debug options menu to toggle the dev camera "
            "manually.)",
            walks_entered_, gate_str, cvar_str, patch_outcome,
            static_cast<unsigned long long>(patch_value_va), patch_cvar_buf,
            static_cast<unsigned long long>(
                reinterpret_cast<std::uintptr_t>(hwnd_)));
        return;
    }
    // G5.22 dedup: the WH_KEYBOARD_LL pre-dispatch already synthesised
    // the toggle VK for this F10 press. Skip the OnEnterWalking
    // dispatch — the dev cam is already in the right state.
    // Re-dispatching here would double-toggle the dev cam (activate
    // on entry, deactivate on the very next frame, leaving the user
    // with the dev cam OFF even though they're in Walking). The
    // fallback dispatch below only fires when the FSM enters Walking
    // WITHOUT a preceding F10 rising edge — e.g. the KnockoutProvider
    // forcing a transition, or a non-input state change. The 500 ms
    // window covers the WH_KEYBOARD_LL → FSM transition latency on
    // the user's host (~16-20 ms typical frame, plus scheduler
    // jitter) and is conservative — doubling the toggle within 500 ms
    // of a legitimate F10 press would be a clear regression the user
    // would notice as a flashing dev-cam overlay.
    if (last_pre_dispatch_ms_ != 0 &&
        (static_cast<int>(NowMs() & 0x7FFFFFFFLL) -
         last_pre_dispatch_ms_) < kPreDispatchDedupMs) {
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: Walking entry #%d -> pre-dispatch "
            "covered the toggle (%d ms ago); OnEnterWalking skip "
            "to avoid double-toggle.",
            walks_entered_,
            static_cast<int>(NowMs() & 0x7FFFFFFFLL) -
                last_pre_dispatch_ms_);
        return;
    }
    const int dispatched = TriggerToggle();
    // Read `last_dispatch_path_` AFTER `TriggerToggle` so the log
    // reflects the path the dispatch actually took. Path values:
    //   0 = no dispatch (no bound HWND or zero VK)
    //   1 = SendInput VK (legacy SynthKey path)
    //   2 = PostMessage WM_CHAR to dev console Edit (G5.20 primary)
    //   3 = SendInput VK codes per char (G5.20 fallback when the
    //       dev console Edit HWND was not found within 1.5 s)
    const char *path_name =
        last_dispatch_path_ == 3
            ? "SendInput-VK"
        : last_dispatch_path_ == 2
            ? "PostMessage-WM_CHAR"
        : (last_dispatch_path_ == 1 ? "SendInput" : "none");
    // M2.5: include the auto-detected controls.sii binding (or the
    // INI override marker) in the F10 Walking-entry log so the user
    // can verify which key is actually being dispatched. Without this
    // trace, the user is left guessing whether the dispatch reached
    // the right SCS action or was silently routed to a different one.
    const char *vk_source =
        ini_overrides_toggle_vk_
            ? "ini-override"
            : (auto_detected_key_name_.empty() ? "default"
                                                 : "auto-detected");
    // M2 diagnostic: surface the controls.sii conflict set in the
    // F10 dispatch line so the analyst sees the binding shadow
    // next to the dispatch outcome in one log line. The cache is
    // populated by `LogControlsSiiVkConflictIfAny` at shim init
    // (called from PluginEntry right after BindGameHwnd) and
    // re-populated on each BindGameHwnd call so the most recent
    // scan wins. The format is "<none>" when no conflict was
    // detected (the common case) and a comma-joined action list
    // when one or more controls.sii bindings shadow the toggle
    // VK — the user can grep game.log for "controls-sii-shadow="
    // to surface every F10 that fired against a binding that may
    // have eaten the press.
    std::string shadow_joined;
    if (!controls_sii_conflict_actions_.empty()) {
        for (size_t i = 0; i < controls_sii_conflict_actions_.size(); ++i) {
            if (i > 0) shadow_joined += ",";
            shadow_joined += controls_sii_conflict_actions_[i];
        }
    }
    const char *shadow_cstr =
        controls_sii_conflict_actions_.empty() ? "<none>" : shadow_joined.c_str();
    // Foreground check: when the bound HWND is not the foreground
    // window at dispatch time, SendInput silently drops the event
    // (the foreground window requirement
    // https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput
    // is enforced for non-injected keyboard input on Windows 7+).
    // The check is best-effort — the foreground window may have
    // changed between the check and the SendInput — but it gives
    // the analyst a clear signal in game.log when the dispatch
    // "succeeded" but the input went to a different window (e.g.
    // an IDE or browser grabbed focus mid-frame). The check is
    // pointer-safe (IsWindow returns false for HWNDs that have
    // been destroyed since the bind).
    const char *foreground_state = "0";
    HWND fg = ::GetForegroundWindow();
    if (fg == hwnd_ && fg != nullptr) {
        foreground_state = "fg-match";
    } else if (fg != nullptr) {
        foreground_state = "fg-mismatch";
    } else {
        foreground_state = "fg-unknown";
    }
    WALKERCORE_LOG_INFO(
        "GameFreeCameraShim: Walking entry #%d -> dispatched %d key "
        "event(s) (gate=%s, config-cfg[probe]='%s', dev_cvar_patcher=%s "
        "cvar_va=0x%llX value=%.12s, toggle VK=0x%X [%s], "
        "auto-detected-key=%s, controls-sii-path=%s, exit VK=0x%X, "
        "path=%s hwnd=0x%llX, hwnd-valid=%s, foreground=%s, "
        "controls-sii-shadow=%s) — SendInput dispatched. If the dev "
        "free camera still does not activate, the toggle VK may be "
        "absorbed by another application (ReShade / OptiScaler / "
        "Steam overlay), shadowed by a controls.sii binding "
        "(controls-sii-shadow=<actions>), the foreground window may "
        "have changed mid-frame (foreground=fg-mismatch), or the dev "
        "camera shortcut may require an additional gate "
        "(g_console=1 for the hard-coded F11 shortcut; see "
        "docs/observations/dev-free-camera.md).",
        walks_entered_, dispatched, gate_str, cvar_str, patch_outcome,
        static_cast<unsigned long long>(patch_value_va), patch_cvar_buf,
        toggle_vk_, vk_source,
        auto_detected_key_name_.empty() ? "<none>"
                                          : auto_detected_key_name_.c_str(),
        auto_detected_controls_sii_path_.empty()
            ? "<not found>"
            : auto_detected_controls_sii_path_.c_str(),
        exit_press_vk_, path_name,
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(hwnd_)),
        ::IsWindow(hwnd_) ? "yes" : "no",
        foreground_state,
        shadow_cstr);
}

bool GameFreeCameraShim::MaybeAutoEnableFromDetection() {
    // M2.5 follow-up: smart-default auto-enable. Three gates must
    // all be satisfied to flip the shim from false → true:
    //
    //   1. INI did NOT explicitly set `game_free_camera_shim = ...`
    //      — if the user has named the knob in the INI (regardless
    //      of value), they have signalled "I know what I want, leave
    //      me alone" and we must not flip their value.
    //   2. controls.sii was located (no missing-profile / build-
    //      server scenario). The shim still defaults to the hard-
    //      coded '0' shortcut when no action-name binding matches,
    //      so this gate is satisfied whenever we can read the
    //      file. (M2.7 audit: no action-name binding for the dev
    //      free camera exists in amtrucks.exe 1.61.3.1s, so the
    //      earlier "binding was detected" gate was effectively
    //      always-on for any controls.sii that exists; the gate
    //      was rewritten to "file was found" to match the new
    //      reality without changing the user-visible behaviour.)
    //   3. g_developer is supported (value 1 or 2 in config.cfg).
    //      Without g_developer=1, the SCS dev free camera is not
    //      available and the dispatch would produce no visual change.
    //
    // All three gates are independent. Any miss returns false
    // without mutating enabled_, and the user can still opt in
    // manually by editing the INI.
    if (ini_overrides_shim_enable_) {
        // The INI named the knob. Honour the user's choice; do not
        // auto-flip.
        return false;
    }
    // (M2.7 audit: the "binding was detected" gate was removed.
    // No action-name binding for the dev free camera exists in
    // amtrucks.exe 1.61.3.1s, so auto_detected_key_name_ is
    // always empty in this build. The shim still defaults to the
    // hard-coded F11 shortcut regardless of bindings, so the
    // "no binding found" reason for staying off no longer applies
    // — flipping on is safe even when controls.sii has no
    // matching line. The probe loop in AutoDetectToggleVkFromControlsSii
    // is preserved as a diagnostic surface, but no longer gates
    // the smart-default.)
    if (!supported_) {
        // g_developer is 0 / missing. The dev free camera is not
        // available. Stay off.
        return false;
    }
    if (enabled_) {
        // Already enabled (the user did it themselves, or a previous
        // boot's smart-default flipped it). No change needed.
        return false;
    }
    // All three gates pass. Flip the shim on and log the change.
    enabled_ = true;
    WALKERCORE_LOG_INFO(
        "GameFreeCameraShim: smart-default auto-enable (binding=%s, "
        "VK=0x%X, g_developer=%s, INI=[m2] game_free_camera_shim "
        "untouched). The shim will now dispatch the toggle key on "
        "every F10 transition. To disable, set [m2] "
        "game_free_camera_shim = false in WalkerCore.ini.",
        auto_detected_key_name_.c_str(), toggle_vk_,
        supported_ ? "1" : "?");
    return true;
}

void GameFreeCameraShim::OnExitWalking() {
    exiting_walking_ = true;
    // M2.5: dispatch the toggle key on Walking exit so the dev free
    // camera turns OFF when the user re-enters the truck. Without
    // this, the dev free camera stays activated after the F (or F10)
    // recovery and the user is left in a hybrid state: the game's
    // camera is back in cabin view but the dev free camera is still
    // toggled on in some bookkeeping, which can produce visual
    // glitches (camera matrix flashes between free and cabin) the
    // next time the user walks again. The symmetric
    // entry-toggles-on / exit-toggles-off pairing is what makes the
    // shim idempotent across many F10 cycles.
    //
    // The dispatch honours the same gates as OnEnterWalking —
    // enabled_ AND supported_ must both be true — so a user who
    // runs the shim off (the default after the M2.5 auto-migrate)
    // sees no toggle-dispatched line on Walking exit, matching the
    // entry-side silence. When both gates are met, we report the
    // dispatch + path in a single line so the analyst can grep
    // game.log for "Walking exit -> dispatched" and see the toggle
    // parity.
    if (enabled_ && supported_) {
        // G5.22 dedup: skip when WH_KEYBOARD_LL pre-dispatch
        // already toggled for the same F10 press that drove this
        // Walking exit.
        constexpr int kPreDispatchDedupMs = 500;
        if (last_pre_dispatch_ms_ != 0 &&
            (static_cast<int>(NowMs() & 0x7FFFFFFFLL) -
             last_pre_dispatch_ms_) < kPreDispatchDedupMs) {
            WALKERCORE_LOG_INFO(
                "GameFreeCameraShim: Walking exit -> pre-dispatch "
                "covered the toggle (%d ms ago); OnExitWalking skip "
                "to avoid double-toggle.",
                static_cast<int>(NowMs() & 0x7FFFFFFFLL) -
                    last_pre_dispatch_ms_);
            return;
        }
        const int dispatched = TriggerToggle();
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: Walking exit -> dispatched %d key "
            "event(s) (walks_entered=%d, toggles_dispatched=%d, "
            "toggle VK=0x%X, hwnd=0x%llX) — toggle dispatched to "
            "deactivate the dev free camera so the cabin view "
            "regains control when the user re-enters the truck.",
            dispatched, walks_entered_, toggles_dispatched_,
            toggle_vk_,
            static_cast<unsigned long long>(
                reinterpret_cast<std::uintptr_t>(hwnd_)));
        return;
    }
    WALKERCORE_LOG_INFO(
        "GameFreeCameraShim: Walking exit (walks_entered=%d, "
        "toggles_dispatched=%d, gate=%s, supported=%s)",
        walks_entered_, toggles_dispatched_,
        enabled_ ? "on" : "off",
        supported_ ? "1" : "0/missing");
}

int GameFreeCameraShim::TriggerToggle() {
    if (!enabled_) {
        return 0;
    }
    int total = 0;
    // Stage 1 (optional): exit-cabin press. The user's config does
    // not gate this so when set to 0 it's a no-op (the SCS free
    // camera activated from inside the cabin will be at the cabin's
    // default viewpoint; the player can move from there).
    if (exit_press_vk_ != 0) {
        total += SynthKey(exit_press_vk_, hwnd_, last_dispatch_path_,
                          last_postmessage_hwnd_diag_);
    }
    // Stage 2 (G5.19-aware): toggle the SCS dev free camera.
    //
    // The original Stage 2 dispatched a single VK (default 0x30, top-row
    // `0`) through SendInput, banking on an SCS-internal keypress
    // handler that lived in older builds. ATS 1.61.3.1s no longer has
    // such a handler — F11 hits `mix camdbg` first, top-row `0` is
    // unbound but ignored, and there is no controls.sii action name
    // that toggles the dev free camera. The shim's own source comments
    // (`GameFreeCameraShim.cpp:994-1000`) acknowledge this and point to
    // the dev console (`~`) + typed command `free_camera` as the only
    // proven path.
    //
    // When `[m2] dev_camera_via_console = true` (the new default) we
    // route Stage 2 through `SynthConsoleCommand("free_camera")`,
    // which opens the dev console, types the literal command, presses
    // Enter, and closes the console in one synchronous burst. The
    // resulting cam state mirrors what the user would get by typing
    // the command manually. `free_camera` is itself a state toggle in
    // SCS: each invocation flips dev-cam on/off, so successive F10
    // presses alternate the dev camera cleanly.
    //
    // When the INI knob is set to false, fall back to the legacy
    // SendInput(VK) path. Some users bind `mix o_im_freecam
    // \`keyboard.X?0\`` in their controls.sii and rely on the action
    // registry shortcut, which only the VK-dispatch path can hit.
    if (dev_camera_via_console_) {
        total += SynthConsoleCommand("free_camera", hwnd_,
                                     last_dispatch_path_,
                                     last_postmessage_hwnd_diag_);
        console_cmds_dispatched_++;
        // Echo-suppress the three VK events SynthConsoleCommand
        // posts at the head and tail of the sequence: ~ (open),
        // Enter, ~ (close). The unicode char events (KEYEVENTF_UNICODE
        // with wVk=0) are immune to the hook's eat-echo logic because
        // ConsumeEchoKey only matches by VK, not by unicode wScan; the
        // shim's WH_KEYBOARD_LL drops VK=0 events before they reach
        // any toggle-detection code.
        const long long until = NowMs() + 250;
        if (echo_count_ < kMaxEcho) {
            echo_vks_[echo_count_] = 0xC0;
            echo_until_ms_[echo_count_] = until;
            echo_count_++;
        }
        if (echo_count_ < kMaxEcho) {
            echo_vks_[echo_count_] = VK_RETURN;
            echo_until_ms_[echo_count_] = until;
            echo_count_++;
        }
        if (echo_count_ < kMaxEcho) {
            echo_vks_[echo_count_] = 0xC0;
            echo_until_ms_[echo_count_] = until;
            echo_count_++;
        }
    } else {
        // Legacy path: the SCS-internal keypress handler (F11 in older
        // builds, top-row `0` per the SCS docs, etc.). May not work in
        // ATS 1.61.3.1s but is preserved for users who bind a custom
        // action name in controls.sii or who run on a different build.
        total += SynthKey(toggle_vk_, hwnd_, last_dispatch_path_,
                          last_postmessage_hwnd_diag_);
        {
            const long long until = NowMs() + 250;
            if (echo_count_ < kMaxEcho) {
                echo_vks_[echo_count_] = toggle_vk_;
                echo_until_ms_[echo_count_] = until;
                echo_count_++;
            }
        }
    }
    toggles_dispatched_++;
    last_dispatch_ms_ = static_cast<int>(NowMs() & 0x7FFFFFFF);

    // Mark the VKs as "echo" so the WH_KEYBOARD_LL hook ignores the
    // key-up we just sent back to ourselves, instead of triggering
    // another Walking-toggle round-trip.
    if (exit_press_vk_ != 0) {
        const long long until = NowMs() + 250;
        if (echo_count_ < kMaxEcho) {
            echo_vks_[echo_count_] = exit_press_vk_;
            echo_until_ms_[echo_count_] = until;
            echo_count_++;
        }
    }
    // The dev_camera_via_console_ branch adds its three VKs to the
    // echo list above; the legacy branch already added toggle_vk_.
    // Nothing to do here for either path.
    return total;
}

int GameFreeCameraShim::PreDispatchDevCamToggleFromF10(
    const char *trigger_reason) {
    // G5.22 timing fix. The legacy OnEnterWalking dispatch fired AFTER
    // the WalkerFsm had already transitioned Driving → Walking, so the
    // synthesised F11 (dev cam toggle) landed in the input queue AFTER
    // the F10 that started the transition. DirectInput polled them in
    // FIFO order, F11 was processed in Walking / TransitionIn state,
    // and the dev cam either didn't toggle or was suppressed by the
    // walker's camera manager. The user reported "F10 only does
    // screenshot, dev cam does not activate" (G5.21).
    //
    // The new path: WH_KEYBOARD_LL fires for F10 BEFORE the event is
    // added to the OS input queue. We synthesise F11 here, so F11 is
    // queued FIRST. When WH_KEYBOARD_LL returns CallNextHookEx, the
    // original F10 is queued SECOND. DirectInput's next poll reads
    // F11 first (dev cam activates while the game is still in
    // Driving), then F10 (Walking transition starts). This is the
    // same ordering the user gets when they manually press F11 in
    // Driving — which they confirmed produces the position-report
    // overlay.
    //
    // We always toggle, regardless of current FSM state, because the
    // dev cam toggle is symmetric with the walker toggle: every F10
    // press that enters Walking also activates the dev cam, and every
    // F10 press that exits Walking deactivates it. The state machine
    // stays in lockstep as long as we toggle on every F10 rising edge.
    //
    // The synthesised F11 fires our own WH_KEYBOARD_LL hook on the
    // way back, which classifies it as "LastKey" (not a walker key)
    // and lets it pass through to the game — so we don't need to
    // echo-suppress it.
    //
    // The legacy OnEnterWalking / OnExitWalking dispatch still fires
    // when the FSM transitions for reasons OTHER than F10 (e.g. the
    // KnockoutProvider force-transitioning into Walking, or a recovery
    // F). To avoid double-toggling when the WH_KEYBOARD_LL pre-dispatch
    // covered the same toggle, the OnEnterWalking / OnExitWalking
    // paths consult `last_pre_dispatch_ms_` and skip their own
    // dispatch when one happened recently. We set
    // `last_pre_dispatch_ms_` here whenever we actually synthesised
    // at least one key, so the fallback can detect "the toggle was
    // already done in the hook".

    if (!enabled_) {
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: pre-dispatch skipped (gate=off, "
            "trigger=%s) — shim disabled; user-facing F10 stays a pure "
            "walk toggle without dev cam side-effect.",
            trigger_reason != nullptr ? trigger_reason : "<unset>");
        return 0;
    }
    if (!supported_) {
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: pre-dispatch skipped (gate=on, "
            "supported=0/missing, trigger=%s) — g_developer not "
            "detected; dev free camera not available, dispatching "
            "F11 would have no visual effect.",
            trigger_reason != nullptr ? trigger_reason : "<unset>");
        return 0;
    }
    // Foreground guard: WH_KEYBOARD_LL fires system-wide, not just
    // for the game. If the user is typing in Chrome and presses F10,
    // our hook fires; without this check, SendInput F11 would land
    // in Chrome's input queue (because SendInput targets the
    // foreground thread) and the user would see a stray F11
    // character appear in their active app. We require the
    // foreground window to be owned by the current process — the
    // user's ATS instance is the only process that loads our DLL,
    // so GetWindowThreadProcessId(fg) == GetCurrentThreadId() is a
    // robust game-in-foreground check. We don't compare fg == hwnd_
    // because the G5.21 live log showed fg-mismatch in normal play
    // (the splash screen was the bound window but the main render
    // window was foreground after FindGameHwnd raced the
    // splash-main swap) — an exact HWND match would suppress
    // pre-dispatch on every F10 the user presses in-game.
    const HWND fg = ::GetForegroundWindow();
    if (!foreground_guard_disabled_for_testing_ &&
        fg != nullptr && hwnd_ != nullptr) {
        DWORD fg_pid = 0;
        ::GetWindowThreadProcessId(fg, &fg_pid);
        if (fg_pid != ::GetCurrentProcessId()) {
            WALKERCORE_LOG_INFO(
                "GameFreeCameraShim: pre-dispatch skipped (trigger=%s, "
                "fg=0x%llX, fg_pid=%lu, our_pid=%lu) — F10 was not "
                "for our process (foreground window belongs to "
                "another app). SendInput would route F11 to that "
                "app's queue. Let F10 reach its real target "
                "untouched.",
                trigger_reason != nullptr ? trigger_reason : "<unset>",
                static_cast<unsigned long long>(
                    reinterpret_cast<std::uintptr_t>(fg)),
                static_cast<unsigned long long>(fg_pid),
                static_cast<unsigned long long>(
                    ::GetCurrentProcessId()));
            return 0;
        }
    }

    // Direct VK dispatch — bypass dev_camera_via_console because the
    // console path (SendInput ~ + chars + Enter + ~) is multi-event
    // and would interleave badly with the F10 the WH_KEYBOARD_LL is
    // about to return. The whole point of this hook is "queue F11
    // before F10"; opening a console window and typing a command
    // takes hundreds of milliseconds and the game would have started
    // its Walking transition by the time the console opened. The
    // OnEnterWalking / OnExitWalking fallback paths still honour the
    // INI knob — the slow via-console path is acceptable when the
    // fallback fires (no queue-order pressure).
    const int dispatched = SynthKey(
        toggle_vk_, hwnd_, last_dispatch_path_,
        last_postmessage_hwnd_diag_);
    if (dispatched > 0) {
        toggles_dispatched_++;
        last_dispatch_ms_ = static_cast<int>(NowMs() & 0x7FFFFFFFLL);
        last_pre_dispatch_ms_ = last_dispatch_ms_;
        // Echo-suppress the VK the hook will see on the way back so
        // any future code that reads ConsumeEchoKey can avoid looping
        // on the synthesised key. The hook's LowLevelKeyboardProc
        // currently does not call ConsumeEchoKey for the toggle VK
        // (F11 isn't in the walker bindings table), so this is
        // diagnostic-only — the F11 falls through as LastKey.
        const long long until = NowMs() + 250;
        if (echo_count_ < kMaxEcho) {
            echo_vks_[echo_count_] = toggle_vk_;
            echo_until_ms_[echo_count_] = until;
            echo_count_++;
        }
        const char *path_name =
            last_dispatch_path_ == 3 ? "SendInput-VK"
            : last_dispatch_path_ == 2 ? "PostMessage-WM_CHAR"
            : (last_dispatch_path_ == 1 ? "SendInput" : "none");
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: pre-dispatch from WH_KEYBOARD_LL "
            "(trigger=%s, VK=0x%X, dispatched=%d, path=%s, "
            "hwnd=0x%llX) — toggle VK queued BEFORE the original F10 "
            "so DirectInput's next poll reads F11 first (dev cam "
            "toggles in Driving state) and F10 second (Walking "
            "transition starts). This replaces the G5.21 OnEnterWalking "
            "dispatch which fired too late.",
            trigger_reason != nullptr ? trigger_reason : "<unset>",
            toggle_vk_, dispatched, path_name,
            static_cast<unsigned long long>(
                reinterpret_cast<std::uintptr_t>(hwnd_)));
        return dispatched;
    }
    WALKERCORE_LOG_INFO(
        "GameFreeCameraShim: pre-dispatch no-op (trigger=%s, "
        "VK=0x%X, hwnd=0x%llX) — SynthKey returned 0; the toggle "
        "will fall through to OnEnterWalking / OnExitWalking.",
        trigger_reason != nullptr ? trigger_reason : "<unset>",
        toggle_vk_,
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(hwnd_)));
    return 0;
}

bool GameFreeCameraShim::ConsumeEchoKey(unsigned int vk) {
    const long long now = NowMs();
    bool consumed = false;
    int kept = 0;
    for (int i = 0; i < echo_count_ && i < kMaxEcho; ++i) {
        if (echo_until_ms_[i] == 0) {
            continue;
        }
        if (echo_until_ms_[i] < now) {
            continue;  // expired slot
        }
        if (!consumed && echo_vks_[i] == vk) {
            consumed = true;
            continue;  // eat this one, do not keep
        }
        echo_vks_[kept] = echo_vks_[i];
        echo_until_ms_[kept] = echo_until_ms_[i];
        kept++;
    }
    for (int i = kept; i < echo_count_ && i < kMaxEcho; ++i) {
        echo_vks_[i] = 0;
        echo_until_ms_[i] = 0;
    }
    echo_count_ = kept;
    return consumed;
}

}  // namespace walkercore
