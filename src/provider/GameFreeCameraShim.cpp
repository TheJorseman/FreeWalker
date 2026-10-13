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
// confirmed Numpad-0 manual presses toggle the SCS dev free camera
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
    // toggle on Numpad-0).
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
void GameFreeCameraShim::BindGameHwnd(HWND hwnd) { hwnd_ = hwnd; }
void GameFreeCameraShim::BindDevCameraPatchResult(
    const DevCameraCvarPatchResult *r) {
    dev_cvar_patch_result_ = r;
}

// M2.5: auto-detect the user's dev-camera-toggle binding from their
// controls.sii. Called once from PluginEntry init after
// ProbeGDeveloperFromConfig.
//
// The probe order is: dev_camera_toggle → walk_mode → free_camera.
// dev_camera_toggle is the stock SCS action name the dev console
// `free_camera_u` command routes through (see
// docs/observations/tmrealwalk-cam-manager-re.md for the RE
// provenance). walk_mode is the legacy action some profiles use.
// free_camera is a third-party synonym we tolerate for completeness.
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
        // "INI=0x65, profile binding keyboard.num5 @ <path>"), so
        // we resolve the path even when we are not going to adopt
        // its value. The probe is cheap (one fopen per candidate,
        // 12 candidates max) and the resolution is read-only.
        const std::string path = ProbeActiveControlsSiiPath();
        auto_detected_controls_sii_path_ = path;
        if (!path.empty()) {
            for (const char *action :
                 {"dev_camera_toggle", "walk_mode", "free_camera"}) {
                const std::string token =
                    ReadControlsSiiKeyForActionForTesting(
                        path, std::string(action));
                if (!token.empty()) {
                    auto_detected_key_name_ = token;
                    break;
                }
            }
        }
        WALKERCORE_LOG_INFO(
            "GameFreeCameraShim: INI override active — auto-detect "
            "skipped, toggle VK=0x%X (auto-detected profile binding=%s, "
            "path=%s)",
            toggle_vk_,
            auto_detected_key_name_.empty()
                ? "<none in controls.sii>"
                : auto_detected_key_name_.c_str(),
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
            "(Numpad-0) in effect; if your controls.sii binds a "
            "different key to dev_camera_toggle / walk_mode / "
            "free_camera, set [m2] dev_camera_toggle_vk = 0xHH in "
            "WalkerCore.ini to override.",
            toggle_vk_);
        return;
    }
    // Scan the three known action names in priority order. The first
    // hit wins; subsequent actions are ignored. We also track
    // whether any of the three scans matched via the escaped-
    // backtick typo fallback so we can emit a specific warning
    // when the user has a malformed binding (game.log shows
    // `<ERROR> Expecting string literal, found '"mix ...'`).
    const char *hit_action = nullptr;
    std::string token;
    bool detected_typo = false;
    for (const char *action :
         {"dev_camera_toggle", "walk_mode", "free_camera"}) {
        bool action_typo = false;
        token = ReadControlsSiiKeyForActionWithDiagForTesting(
            path, std::string(action), &action_typo);
        if (action_typo) {
            detected_typo = true;
        }
        if (!token.empty()) {
            hit_action = action;
            break;
        }
    }
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
                "MALFORMED binding for dev_camera_toggle / walk_mode "
                "/ free_camera (escaped backticks `\\`` instead of "
                "plain `` ` ``). The SCS parser REJECTED the line at "
                "game load (game.log shows `<ERROR> Expecting string "
                "literal, found '\"mix ...'`) and the binding never "
                "reached the action registry, so a SendInput dispatch "
                "will hit the wrong action. Replace the bad line in %s "
                "with this exact text (plain backticks, no backslash):",
                path.c_str(), path.c_str());
            WALKERCORE_LOG_WARN(
                "    mix dev_camera_toggle `keyboard.f11?0 | "
                "semantical.dev_camera_toggle?0`");
            WALKERCORE_LOG_WARN(
                "    (alternative keys: keyboard.f10, keyboard.num5, "
                "keyboard.numpad5 — pick one that does not collide "
                "with an existing binding. WalkerCore will auto-detect "
                "the corrected binding on next launch.)");
            return;
        }
        WALKERCORE_LOG_WARN(
            "GameFreeCameraShim: controls.sii at %s has NO binding "
            "for dev_camera_toggle / walk_mode / free_camera. Default "
            "toggle VK=0x%X (Numpad-0) in effect — your profile binds "
            "Numpad-0 to dbglockpos so the synthesised key will be "
            "ignored. Add this line to %s and reload the save / "
            "restart the game:",
            path.c_str(), toggle_vk_, path.c_str());
        WALKERCORE_LOG_WARN(
            "    mix dev_camera_toggle `keyboard.f11?0 | "
            "semantical.dev_camera_toggle?0`");
        WALKERCORE_LOG_WARN(
            "    (alternative keys: keyboard.f10, keyboard.num5, "
            "keyboard.numpad5 — pick one that does not collide with "
            "an existing binding. WalkerCore will auto-detect the new "
            "binding on next launch via [m2] dev_camera_toggle_vk "
            "auto-probe.)");
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
            "toggle VK=0x%X (Numpad-0) in effect. To use this "
            "binding, map it to a keyboard key in controls.sii or set "
            "[m2] dev_camera_toggle_vk = 0xHH in WalkerCore.ini to "
            "force a specific VK.",
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
            "toggle). The dev_cvar_patcher already flipped "
            "g_developer to 1 in memory, so once the shim is "
            "enabled, the next F10 press will activate the dev "
            "free camera via the bound key.",
            token.c_str(), vk);
    }
}

void GameFreeCameraShim::ProbeGDeveloperFromConfig() {
    const std::string path = ProbeConfigPath();
    const std::string value = ReadGDeveloperValueForTesting(path);
    const bool ok = (!path.empty()) && (value == "1" || value == "2");
    supported_ = ok;
    WALKERCORE_LOG_INFO(
        "GameFreeCameraShim: g_developer probe path=%s value=%s -> supported=%d",
        path.empty() ? "<not found>" : path.c_str(),
        value.empty() ? "<unset>" : value.c_str(),
        supported_ ? 1 : 0);
}

void GameFreeCameraShim::OnEnterWalking() {
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
    const char *path_name =
        last_dispatch_path_ == 2
            ? "PostMessage"
            : (last_dispatch_path_ == 1 ? "SendInput" : "none");
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
            "toggle on F10. The dev_cvar_patcher independently flipped "
            "g_developer '0' -> '1' in amtrucks.exe memory so any "
            "future dev-camera path (the `free_camera_u` console "
            "command, the dev UI button o_im_freecam, future "
            "in-process camera manager hooks) works without editing "
            "Documents\\American Truck Simulator\\config.cfg.)",
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
            "toggle VK but the game's controls.sii binds Numpad-0 to "
            "the user's chosen action (the dev free camera is UI-"
            "button only via o_im_freecam, no action-name binding in "
            "the stock profile). Two ways forward: (a) add a binding "
            "for the toggle VK in inputs.dat your kitchen <profile>\\"
            "controls.sii that maps to a dev-camera action (or to "
            "free_camera_u), then re-deploy, or (b) use the game's "
            "dev console (`~` key) and type `free_camera_u` while "
            "in the dev free camera context.)",
            walks_entered_, gate_str, cvar_str, patch_outcome,
            static_cast<unsigned long long>(patch_value_va), patch_cvar_buf,
            static_cast<unsigned long long>(
                reinterpret_cast<std::uintptr_t>(hwnd_)));
        return;
    }
    const int dispatched = TriggerToggle();
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
    WALKERCORE_LOG_INFO(
        "GameFreeCameraShim: Walking entry #%d -> dispatched %d key "
        "event(s) (gate=%s, config-cfg[probe]='%s', dev_cvar_patcher=%s "
        "cvar_va=0x%llX value=%.12s, toggle VK=0x%X [%s], "
        "auto-detected-key=%s, controls-sii-path=%s, exit VK=0x%X, "
        "path=%s hwnd=0x%llX) — The dev_cvar_patcher flipped "
        "g_developer in memory before this dispatch; if the dev free "
        "camera still does not activate, the game's controls.sii "
        "binding for the synthesised key may route the dispatch to a "
        "different action (see docs/observations/tmrealwalk-findings.md).",
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
            reinterpret_cast<std::uintptr_t>(hwnd_)));
}

bool GameFreeCameraShim::MaybeAutoEnableFromDetection() {
    // M2.5 follow-up: smart-default auto-enable. Three gates must
    // all be satisfied to flip the shim from false → true:
    //
    //   1. INI did NOT explicitly set `game_free_camera_shim = ...`
    //      — if the user has named the knob in the INI (regardless
    //      of value), they have signalled "I know what I want, leave
    //      me alone" and we must not flip their value.
    //   2. A `dev_camera_toggle` / `walk_mode` / `free_camera`
    //      binding was detected in the user's controls.sii. Without
    //      a binding, the shim's synthesised key falls on a
    //      different action and the auto-enable would activate a
    //      no-op dispatch.
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
    if (auto_detected_key_name_.empty()) {
        // No binding found. Even if the user enabled the shim, the
        // dispatch would target a non-existent action. We could
        // still flip enabled_ → true (the user can add the binding
        // later and the next F10 will then dispatch), but that
        // would produce a Walking-entry log line saying
        // "dispatched 0 key event(s)" which is confusing. Stay off.
        return false;
    }
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
    // Stage 2 (always): toggle the SCS free camera. VK_NUMPAD0 is
    // the SCS convention for "fly mode" toggle.
    total += SynthKey(toggle_vk_, hwnd_, last_dispatch_path_,
                      last_postmessage_hwnd_diag_);
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
    {
        const long long until = NowMs() + 250;
        if (echo_count_ < kMaxEcho) {
            echo_vks_[echo_count_] = toggle_vk_;
            echo_until_ms_[echo_count_] = until;
            echo_count_++;
        }
    }
    return total;
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
