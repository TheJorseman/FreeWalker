cmake_minimum_required(VERSION 3.20)
project(WalkerCore
    VERSION 0.0.1
    DESCRIPTION "Free, MIT-licensed walk-around-the-truck plugin for Euro Truck Simulator 2 and American Truck Simulator"
    LANGUAGES CXX)

# -----------------------------------------------------------------------------
# Project metadata injected into BuildInfo.h
# -----------------------------------------------------------------------------

# Phase tag is the M0..M12 milestone the current build corresponds to.
# Update this when a phase lands.
set(PROJECT_VERSION_PHASE "m0")

string(TIMESTAMP WALKERCORE_BUILD_ISO "%Y-%m-%dT%H:%M:%SZ" UTC)

# Best-effort: capture the current git short hash if we are in a repo.
find_package(Git QUIET)
if (GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE WALKERCORE_BUILD_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if (NOT WALKERCORE_BUILD_HASH)
    set(WALKERCORE_BUILD_HASH "nogit")
endif()

configure_file(
    "${CMAKE_SOURCE_DIR}/include/walkercore/BuildInfo.h.in"
    "${CMAKE_BINARY_DIR}/generated/walkercore/BuildInfo.h"
    @ONLY)

# -----------------------------------------------------------------------------
# Layout
# -----------------------------------------------------------------------------

# We expect the SCS SDK headers under sdk/include. CMake will throw a useful
# message if the folder is missing.
set(WALKERCORE_SDK_DIR "${CMAKE_SOURCE_DIR}/sdk"
    CACHE PATH "Path to the SCS SDK (folder with include/ and sdk_license.txt)")

if (NOT EXISTS "${WALKERCORE_SDK_DIR}/include/scssdk.h")
    message(FATAL_ERROR
        "SCS SDK headers not found at ${WALKERCORE_SDK_DIR}/include/scssdk.h. "
        "Either vendor the headers (download https://download.eurotrucksimulator2.com/scs_sdk_1_15.zip "
        "and unzip into ${WALKERCORE_SDK_DIR}) or set WALKERCORE_SDK_DIR to the right path. "
        "See sdk/README.md.")
endif()

# -----------------------------------------------------------------------------
# MinHook — vendored at third_party/minhook for the M2 D3D11 lander. We pull
# it in as a subproject so the static library is built with our visibility +
# runtime flags. The library is private to WalkerCore — we don't re-export
# its symbols.
# -----------------------------------------------------------------------------

set(WALKERCORE_MINHOOK_DIR "${CMAKE_SOURCE_DIR}/third_party/minhook" CACHE PATH
    "Path to the vendored MinHook source (folder with src/ and include/)")

if (EXISTS "${WALKERCORE_MINHOOK_DIR}/include/MinHook.h")
    message(STATUS "WalkerCore: linking MinHook from ${WALKERCORE_MINHOOK_DIR}")
    set(WALKERCORE_HAVE_MINHOOK TRUE)
    # Build as a static library with no debug suffix postfix (it makes the
    # output name match what we expect when we link it directly).
    set(BUILD_SHARED_LIBS_MINHOOK OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/minhook EXCLUDE_FROM_ALL)
else()
    message(STATUS
        "WalkerCore: MinHook not vendored at ${WALKERCORE_MINHOOK_DIR}. "
        "The M2 D3D11 lander requires it; until then the stub D3D11Hook "
        "compiles and links but does nothing at runtime.")
    set(WALKERCORE_HAVE_MINHOOK FALSE)
endif()

# -----------------------------------------------------------------------------
# WalkerCore library — all phases link against this
# -----------------------------------------------------------------------------

add_library(WalkerCore SHARED
    src/Main.cpp
    src/adapter/DiagnosticDump.cpp
    src/adapter/Locale.cpp
    src/adapter/Log.cpp
    src/adapter/Settings.cpp
    src/adapter/Time.cpp
    src/adapter/PluginEntry.cpp
    src/domain/CameraState.cpp
    src/domain/GameVersion.cpp
    src/domain/VersionTable.cpp
    src/provider/D3D11Hook.cpp
    src/provider/D3D12Hook.cpp
    src/provider/FuelPump.cpp
    src/provider/InputHook.cpp
    src/provider/Knockout.cpp
    src/provider/PerFrameThread.cpp
    src/provider/PhysXAdapter.cpp
    src/provider/PolishProviders.cpp
    src/provider/OpentrackReceiver.cpp
    src/provider/ScsTelemetryProvider.cpp
    src/provider/TrailerCoupling.cpp
    src/workflow/FrameLoop.cpp
    src/workflow/SettingsMenu.cpp
    src/workflow/WalkerFsm.cpp)

target_include_directories(WalkerCore
    PUBLIC
        "${CMAKE_BINARY_DIR}/generated"  # for the generated BuildInfo.h
        "${CMAKE_SOURCE_DIR}/include"
    PRIVATE
        "${WALKERCORE_SDK_DIR}/include")

set_target_properties(WalkerCore PROPERTIES
    OUTPUT_NAME "WalkerCore"
    POSITION_INDEPENDENT_CODE ON
    WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# -----------------------------------------------------------------------------
# Compile and link flags
# -----------------------------------------------------------------------------

# C++17 — enough for std::optional, std::string_view, structured bindings.
# M2+ may need C++20; promote when we get there.
target_compile_features(WalkerCore PUBLIC cxx_std_17)

# Warnings as errors to keep the codebase tight. Add new warnings rather than
# relaxing this; if a third-party header breaks them, use SYSTEM include dirs.
if (MSVC)
    target_compile_options(WalkerCore PRIVATE /W4 /WX /permissive- /utf-8)
    target_compile_definitions(WalkerCore PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(WalkerCore PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -Wshadow -Wnon-virtual-dtor
        -Wold-style-cast -Wcast-align
        -Wunused -Woverloaded-virtual
        -Wconversion -Wsign-conversion
        -Wnull-dereference -Wdouble-promotion
        -Wformat=2 -Wformat-security)
endif()

# Static runtime so the DLL does not depend on libgcc_s_seh-1.dll /
# msvcp140.dll / vcruntime140.dll being on the PATH of the game folder.
if (MINGW)
    target_link_options(WalkerCore PRIVATE
        -static-libgcc -static-libstdc++
        -Wl,-Bstatic -lstdc++ -lpthread
        -Wl,-Bdynamic)
endif()

# Windows: link to the system DLLs we actually use. Done by including the
# headers (Log.cpp includes <windows.h>); CMake's modules handle the rest.

# Hide symbols we don't export. The SDK call is extern "C" __declspec(dllexport)
# so it ends up in the .def-equivalent table; everything else stays hidden.
if (MSVC)
    target_compile_definitions(WalkerCore PRIVATE WALKERCORE_BUILD_DLL)
    target_link_options(WalkerCore PRIVATE /DEF:${CMAKE_SOURCE_DIR}/src/WalkerCore.def)
endif()

if (MINGW)
    # MinGW auto-generates the export table from -fvisibility=hidden + the
    # explicit __declspec(dllexport) on the two SCS entry points. No .def
    # needed.
    target_compile_options(WalkerCore PRIVATE
        -fvisibility=hidden
        -fvisibility-inlines-hidden)
endif()

# Link MinHook when it's available. The library target name is `minhook`
# (set by the upstream CMakeLists.txt) and we use it privately.
if (WALKERCORE_HAVE_MINHOOK)
    target_link_libraries(WalkerCore PRIVATE minhook)
    target_compile_definitions(WalkerCore PRIVATE WALKERCORE_HAVE_MINHOOK=1)
endif()

# M7 lander: opentrack UDP receiver uses Winsock2 (Windows-only).
# We link ws2_32 unconditionally; on non-Windows the receiver
# .cpp guards the Winsock calls with `#if defined(_WIN32)`, so the
# link line is dead weight on Linux but does not error.
if (WIN32)
    target_link_libraries(WalkerCore PRIVATE ws2_32)
endif()

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

option(WALKERCORE_BUILD_TESTS "Build WalkerCore unit tests" OFF)

if (WALKERCORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------------
# Examples and documentation
# -----------------------------------------------------------------------------

# The SDK ships with examples under sdk/examples/. They reference the SDK
# layout directly. We do not compile them; they exist as a reference for
# plugin authors. CMake intentionally leaves them alone.

# -----------------------------------------------------------------------------
# Architectural lint — enforce "no SCS SDK in domain headers"
# -----------------------------------------------------------------------------
#
# WalkerCore's domain + application + workflow layers must NOT depend on
# the SCS SDK. The SDK types are a private implementation detail of the
# adapter layer (PluginEntry.cpp, Log.cpp, ScsTelemetryProvider.cpp). Any
# #include of scssdk*.h inside include/walkercore/*.h or
# src/{domain,application,workflow}/*.h is a violation and fails the
# configure step with a clear message.
#
# This catches regressions early: a contributor who adds a new domain
# header that needs scs_u32_t will hit this check instead of silently
# leaking the SDK into the public surface.
if (NOT CMAKE_SCRIPT_MODE_FILE)
    set(WALKERCORE_LINT_LAYERS
        "${CMAKE_SOURCE_DIR}/include/walkercore"
        "${CMAKE_SOURCE_DIR}/src/domain"
        "${CMAKE_SOURCE_DIR}/src/workflow")

    set(WALKERCORE_LINT_VIOLATIONS "")
    foreach (layer_dir IN LISTS WALKERCORE_LINT_LAYERS)
        file(GLOB_RECURSE _walkercore_layer_headers
             "${layer_dir}/*.h"
             "${layer_dir}/*.hpp")
        foreach (_hd IN LISTS _walkercore_layer_headers)
            file(READ "${_hd}" _hd_content)
            # CMake regex is BRE-ish: no \s, no \r, no \n literals. We match
            # `#` + optional spaces + `include` + optional spaces + opening
            # quote/angle + scssdk. The exact include form `# include <...>`
            # is also covered.
            string(REGEX MATCHALL
                   "#include[ \t]*[<\"]scssdk[^\">]+[>\"]"
                   _matches "${_hd_content}")
            if (_matches)
                foreach (_m IN LISTS _matches)
                    list(APPEND WALKERCORE_LINT_VIOLATIONS
                         "${_hd}: ${_m}")
                endforeach()
            endif()
        endforeach()
    endforeach()

    if (WALKERCORE_LINT_VIOLATIONS)
        message(FATAL_ERROR
            "WalkerCore architectural lint failed: SCS SDK headers leaked into domain/application/workflow headers.\n"
            "Only src/adapter/ and src/provider/ may include the SDK.\n"
            "Violations:\n  ${WALKERCORE_LINT_VIOLATIONS}")
    else()
        message(STATUS
            "WalkerCore lint OK: no SCS SDK headers in domain/application/workflow headers.")
    endif()
endif()