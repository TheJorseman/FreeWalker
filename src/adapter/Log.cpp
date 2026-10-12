// WalkerCore — logging facade implementation.
//
// M0 scope:
//  - Hold a function pointer to the game-provided log sink.
//  - Fall back to OutputDebugStringW when not bound (during DllMain or in
//    tests).
//  - Format with vsnprintf into a fixed-size stack buffer; on overflow,
//    reallocate with malloc once. Anything bigger is truncated. This is
//    intentional: we never want a misbehaving log call to cost a second
//    malloc in a frame-critical path.
//
// The public surface (Log.h) is SDK-free. This file bridges between
// walkercore_log_level_t / walkercore_log_fn_t and the SDK's
// scs_log_type_t / scs_log_t at the boundary.
//
// Threading: not thread-safe by design. The SCS SDK calls log from the
// game's main thread, which is the only place WalkerCore runs until M3
// at the earliest. If we ever need to log from elsewhere, this becomes a
// CRITICAL_SECTION-guarded path.

#include "walkercore/Log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "scssdk.h"
#include "walkercore/BuildInfo.h"

namespace {

// Initial buffer size. Most log lines are under 200 bytes; 1024 covers
// everything we emit in M0..M2 with margin.
constexpr size_t kInitialBufferSize = 1024;
// Hard cap. Past this we truncate. Keeps a buggy log call from chewing
// memory forever.
constexpr size_t kMaxBufferSize = 64 * 1024;

// The facade stores the SDK-free callback type. The signature is
// compatible with scs_log_t (both are void(*)(int, const char*)) so we
// can safely reinterpret when the adapter gives us the game pointer.
walkercore_log_fn_t g_log_sink = nullptr;
char *g_scratch = nullptr;
size_t g_scratch_capacity = 0;

void log_fallback(int level, const char *utf8_message) {
    // Convert UTF-8 to UTF-16 once for OutputDebugStringW, which is the
    // canonical Win32 sink a debugger attached to Debug will pick up. Game
    // log forwarding uses the game-provided sink so we don't double-log.
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, utf8_message, -1, nullptr, 0);
    if (wide_len <= 0) {
        return;
    }
    wchar_t *wide = static_cast<wchar_t *>(malloc(sizeof(wchar_t) * static_cast<size_t>(wide_len)));
    if (wide == nullptr) {
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8_message, -1, wide, wide_len);
    OutputDebugStringW(wide);
    free(wide);

    // Also write to stderr for tools that capture it (CI logs, etc.).
    fprintf(stderr, "[%s] %s\n",
            level == WALKERCORE_LOG_LEVEL_ERROR ? "ERR" :
            level == WALKERCORE_LOG_LEVEL_WARN  ? "WRN" : "INF",
            utf8_message);
}

char *ensure_scratch(size_t needed) {
    if (needed <= g_scratch_capacity) {
        return g_scratch;
    }
    size_t new_capacity = g_scratch_capacity == 0 ? kInitialBufferSize : g_scratch_capacity;
    while (new_capacity < needed) {
        new_capacity *= 2;
        if (new_capacity > kMaxBufferSize) {
            new_capacity = kMaxBufferSize;
            break;
        }
    }
    char *resized = static_cast<char *>(realloc(g_scratch, new_capacity));
    if (resized == nullptr) {
        return nullptr;
    }
    g_scratch = resized;
    g_scratch_capacity = new_capacity;
    return g_scratch;
}

}  // namespace

extern "C" void walkercore_log_init(walkercore_log_fn_t game_log) {
    g_log_sink = game_log;
}

extern "C" void walkercore_log_shutdown(void) {
    g_log_sink = nullptr;
    free(g_scratch);
    g_scratch = nullptr;
    g_scratch_capacity = 0;
}

extern "C" void walkercore_log(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    walkercore_log_v(level, fmt, ap);
    va_end(ap);
}

extern "C" void walkercore_log_v(int level, const char *fmt, va_list ap) {
    if (fmt == nullptr) {
        return;
    }
    // First pass: probe the size.
    va_list probe;
    va_copy(probe, ap);
    int needed = vsnprintf(nullptr, 0, fmt, probe);
    va_end(probe);
    if (needed < 0) {
        return;
    }
    // +1 for the terminator.
    size_t required = static_cast<size_t>(needed) + 1;
    char *buf = ensure_scratch(required);
    if (buf == nullptr) {
        return;
    }
    vsnprintf(buf, required, fmt, ap);

    if (g_log_sink != nullptr) {
        // The SDK signature (scs_log_t) is `void(SCSAPIFUNC *)(int, const char*)`
        // — SCSAPIFUNC is empty on GCC so the types are compatible. Reinterpret
        // at the boundary to silence any strict-aliasing lint. (We never call
        // this on MSVC; SCSAPIFUNC=__stdcall would make the types incompatible
        // there. WalkerCore targets MinGW only.)
        auto scs_sink = reinterpret_cast<scs_log_t>(g_log_sink);
        scs_sink(static_cast<scs_log_type_t>(level), buf);
    } else {
        log_fallback(level, buf);
    }
}