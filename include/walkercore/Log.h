// WalkerCore — minimal logging facade.
//
// Goals:
//  - C-only API so the boundary between adapters and the rest stays pure C.
//  - Forwards to the SCS-provided log function when available; otherwise to
//    OutputDebugStringW so we still see something during integration tests.
//  - All public functions take a UTF-8 message; we convert to UTF-16 only
//    at the very edge when the underlying sink needs wide strings.
//  - The public header does NOT depend on the SCS SDK — domain code stays
//    SDK-free. The adapter layer (Log.cpp + PluginEntry.cpp) bridges
//    between our level enum and the SDK's SCS_LOG_TYPE_* constants.
//
// This file is part of M0. The full logging system (ringbuffer for F12 dump,
// severity-based filtering, structured fields) is added in M1.

#ifndef WALKERCORE_LOG_H
#define WALKERCORE_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// Severity levels. Integer values match SCS_LOG_TYPE_* (message=0,
// warning=1, error=2) so the adapter can pass them through to the SDK
// sink without translation. We define our own enum so domain code that
// logs through this facade never needs to include scssdk.h.
typedef enum {
    WALKERCORE_LOG_LEVEL_INFO  = 0,
    WALKERCORE_LOG_LEVEL_WARN  = 1,
    WALKERCORE_LOG_LEVEL_ERROR = 2,
} walkercore_log_level_t;

// Function pointer matching the SCS scs_log_t signature
// (void(int level, const char *message)). Defined SDK-free here; the
// adapter layer upcasts to scs_log_t when calling the game sink.
typedef void (*walkercore_log_fn_t)(int level, const char *message);

// Initialize the log facade. Pass the SCS-provided log function; pass NULL
// to fall back to OutputDebugStringW (used during early DllMain and during
// tests where there is no game to forward to).
void walkercore_log_init(walkercore_log_fn_t game_log);

// Tear down. Idempotent.
void walkercore_log_shutdown(void);

// Print a formatted message. Level is one of walkercore_log_level_t.
void walkercore_log(int level, const char *fmt, ...);
void walkercore_log_v(int level, const char *fmt, va_list ap);

// Convenience tag for the plugin. Kept as a macro so it can be overridden
// from CMake (e.g. "WalkerCoreDBG" for a debug build) without touching code.
#ifndef WALKERCORE_LOG_TAG
#define WALKERCORE_LOG_TAG "WalkerCore"
#endif

// Macros for the standard log levels. Use our own enum so callers do not
// have to include the SCS SDK header.
#define WALKERCORE_LOG_INFO(fmt, ...)                                                       \
  walkercore_log(WALKERCORE_LOG_LEVEL_INFO,  "[" WALKERCORE_LOG_TAG "] " fmt, ##__VA_ARGS__)
#define WALKERCORE_LOG_WARN(fmt, ...)                                                       \
  walkercore_log(WALKERCORE_LOG_LEVEL_WARN,  "[" WALKERCORE_LOG_TAG "] " fmt, ##__VA_ARGS__)
#define WALKERCORE_LOG_ERROR(fmt, ...)                                                      \
  walkercore_log(WALKERCORE_LOG_LEVEL_ERROR, "[" WALKERCORE_LOG_TAG "] " fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // WALKERCORE_LOG_H