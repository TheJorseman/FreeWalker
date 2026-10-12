cmake_minimum_required(VERSION 3.20)

# WalkerCore unit tests. Enable from the root with -DWALKERCORE_BUILD_TESTS=ON.
# We link against the same domain + provider sources the DLL uses, so any
# code path the DLL exercises we can exercise here. The test harness is
# header-only and lives in tests/TestHarness.h.

# Build a STATIC library of the same sources the DLL uses. Avoids duplicating
# CMake target definitions and keeps the test build's #include paths identical
# to the DLL's.
add_library(WalkerCoreTestLib STATIC
    ${CMAKE_SOURCE_DIR}/src/adapter/DiagnosticDump.cpp
    ${CMAKE_SOURCE_DIR}/src/adapter/Locale.cpp
    ${CMAKE_SOURCE_DIR}/src/adapter/Log.cpp
    ${CMAKE_SOURCE_DIR}/src/adapter/Settings.cpp
    ${CMAKE_SOURCE_DIR}/src/adapter/Time.cpp
    ${CMAKE_SOURCE_DIR}/src/domain/CameraState.cpp
    ${CMAKE_SOURCE_DIR}/src/domain/GameVersion.cpp
    ${CMAKE_SOURCE_DIR}/src/domain/VersionTable.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/D3D11Hook.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/D3D12Hook.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/FuelPump.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/InputHook.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/Knockout.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/PerFrameThread.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/PhysXAdapter.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/PolishProviders.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/OpentrackReceiver.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/PerTruckDoorTable.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/ScsTelemetryProvider.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/TrailerCoupling.cpp
    ${CMAKE_SOURCE_DIR}/src/workflow/FrameLoop.cpp
    ${CMAKE_SOURCE_DIR}/src/workflow/SettingsMenu.cpp
    ${CMAKE_SOURCE_DIR}/src/workflow/WalkerFsm.cpp)

target_include_directories(WalkerCoreTestLib
    PUBLIC
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_BINARY_DIR}/generated"
    PRIVATE
        "${WALKERCORE_SDK_DIR}/include")

target_compile_features(WalkerCoreTestLib PUBLIC cxx_std_17)

# Tests turn off -Werror because some SDK headers don't pass the warning
# flags. We still keep -Wall -Wextra so regressions show up.
if (MSVC)
    target_compile_options(WalkerCoreTestLib PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(WalkerCoreTestLib PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(WalkerCoreTestLib PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-sign-conversion -Wno-conversion
        -Wno-double-promotion)
endif()

# The test executable.
add_executable(WalkerCoreTests
    test_main.cpp
    test_camera_state.cpp
    test_movement_config.cpp
    test_truck_door_config.cpp
    test_settings.cpp
    test_walker_fsm.cpp
    test_frame_loop.cpp
    test_knockout.cpp
    test_door_anim.cpp
    test_door_anim_wiring.cpp
    test_input_hook.cpp
    test_head_tracker.cpp
    test_head_tracker_wiring.cpp
    test_flashlight_provider.cpp
    test_shadow_wiring.cpp
    test_game_version.cpp
    test_log_facade.cpp
    test_fuel_provider.cpp
    test_diagnostic_dump.cpp
    test_settings_menu.cpp
    test_stub_warnings.cpp
    test_version_table.cpp
    test_abi_contract.cpp
    test_frame_loop_perf.cpp
    test_movement_config_probes.cpp
    test_settings_check_file_changed.cpp
    test_d3d11_hook.cpp
    test_d3d12_hook.cpp
    test_scstelemetry_provider.cpp
    test_coupling_provider.cpp
    test_opentrack_receiver.cpp
    test_truck_door_config_from_state.cpp
    test_time.cpp
    test_per_frame_thread.cpp
    test_physx_adapter.cpp
    test_per_truck_door_table.cpp)

target_link_libraries(WalkerCoreTests PRIVATE WalkerCoreTestLib)
if (WIN32)
    # M7 OpentrackReceiver uses Winsock2 (Windows-only). The test
    # static lib compiles the .cpp; the test exe needs ws2_32 on
    # the link line.
    target_link_libraries(WalkerCoreTests PRIVATE ws2_32)
endif()

# We don't need the SCS SDK at runtime for the domain tests; the SDK is
# only consulted in the SDK callback path which we don't exercise. Stub
# the symbols out so we don't link them in for tests. Same goes for
# the per-frame tick (walker_per_frame_tick lives in the DLL adapter
# layer, which is not part of the test static library); the ticker
# exercises the lock + cadence in tests, not the full conductor.
if (MSVC)
    target_compile_definitions(WalkerCoreTestLib PRIVATE
        WALKERCORE_TEST_NO_SDK=1
        WALKERCORE_TEST_NO_PER_FRAME_TICK=1)
else()
    target_compile_definitions(WalkerCoreTestLib PRIVATE
        WALKERCORE_TEST_NO_SDK=1
        WALKERCORE_TEST_NO_PER_FRAME_TICK=1)
endif()

# Real-binary integration test: when WALKERCORE_REAL_BINARY_TEST_PATH
# is set (typically by the root CMakeLists.txt auto-detect), gate in
# the TestPhysXRealBinaryMatchesCatalog test that reads amtrucks.exe
# from disk and runs the catalog through it. Both WalkerCoreTestLib
# and WalkerCoreTests need the define; the test code is in
# test_physx_adapter.cpp and the registration is in test_main.cpp.
if (DEFINED WALKERCORE_REAL_BINARY_TEST_PATH)
    target_compile_definitions(WalkerCoreTestLib PRIVATE
        WALKERCORE_REAL_BINARY_TEST_PATH="${WALKERCORE_REAL_BINARY_TEST_PATH}")
    target_compile_definitions(WalkerCoreTests PRIVATE
        WALKERCORE_REAL_BINARY_TEST_PATH="${WALKERCORE_REAL_BINARY_TEST_PATH}")
endif()

# Register with CTest. Each RUN_TEST macro is a sub-check; we register the
# binary itself so CTest returns nonzero on failure.
add_test(NAME WalkerCoreTests COMMAND WalkerCoreTests)