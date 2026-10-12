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
    ${CMAKE_SOURCE_DIR}/src/provider/GameFreeCameraShim.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/InputHook.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/Knockout.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/PerFrameThread.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/PhysXAdapter.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/PolishProviders.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/OpentrackReceiver.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/PxSceneHook.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/ImGuiOverlay.cpp
    ${CMAKE_SOURCE_DIR}/src/provider/WindowSubclass.cpp
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
    test_per_truck_door_table.cpp
    test_game_free_camera_shim.cpp
    test_px_scene_hook.cpp
    test_imgui_overlay.cpp
    test_window_subclass.cpp)

target_link_libraries(WalkerCoreTests PRIVATE WalkerCoreTestLib)
if (WIN32)
    # M7 OpentrackReceiver uses Winsock2 (Windows-only). The test
    # static lib compiles the .cpp; the test exe needs ws2_32 on
    # the link line.
    target_link_libraries(WalkerCoreTests PRIVATE ws2_32)
endif()

# Stub out the DLL-adapter functions referenced by D3D11Hook and
# D3D12Hook when the test build defines WALKERCORE_HAVE_MINHOOK=1.
# The real implementations live in src/Main.cpp of the DLL adapter
# layer; tests don't link against that layer. Without the stubs,
# enabling MinHook in tests breaks the link with undefined
# references to walker_per_frame_tick / walker_set_swapchain_aspect.
# See tests/TestD3DHookStubs.cpp for the implementation.
if (WALKERCORE_HAVE_MINHOOK)
    target_sources(WalkerCoreTestLib PRIVATE TestD3DHookStubs.cpp)
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
        WALKERCORE_TEST_NO_PER_FRAME_TICK=1
        WALKERCORE_GAME_FREE_CAMERA_SHIM_DISABLED=1)
else()
    target_compile_definitions(WalkerCoreTestLib PRIVATE
        WALKERCORE_TEST_NO_SDK=1
        WALKERCORE_TEST_NO_PER_FRAME_TICK=1
        WALKERCORE_GAME_FREE_CAMERA_SHIM_DISABLED=1)
endif()

# Mirror the root CMakeLists.txt: when WALKERCORE_HAVE_MINHOOK is set,
# the test build also defines it on WalkerCoreTestLib so D3D11Hook::Install
# and D3D12Hook::Install take the real-minhook code path. The test then
# exercises the actual MinHookInstall/MH_CreateHook/etc. flow rather than
# the stub. The root sets this cache var on the parent scope.
if (WALKERCORE_HAVE_MINHOOK)
    target_compile_definitions(WalkerCoreTestLib PRIVATE
        WALKERCORE_HAVE_MINHOOK=1)
    target_link_libraries(WalkerCoreTestLib PUBLIC minhook)
endif()

# Mirror the root CMakeLists.txt ImGui vendoring. When the ImGui sources
# are present, the test build links them into WalkerCoreTestLib so
# ImGuiOverlay's WALKERCORE_HAVE_IMGUI-gated init path actually runs.
# When ImGui is missing, the overlay stays in scaffolding state and the
# new lifecycle tests are stubbed out via #if defined() in test_imgui_overlay.cpp.
# The same define + include dir must be on WalkerCoreTests itself
# (not just WalkerCoreTestLib) so the test source files in
# test_imgui_overlay.cpp see the same #define value and the
# WALKERCORE_HAVE_IMGUI-gated test functions are compiled in.
if (WALKERCORE_HAVE_IMGUI)
    # Mirror the DLL build's ImGui static lib. The test build
    # already builds the same ImGui sources (we do not depend
    # on the DLL build's libimgui_vendor because tests need
    # their own copy with WALKERCORE_TEST_NO_SDK and the
    # WALKERCORE_GAME_FREE_CAMERA_SHIM_DISABLED defines applied
    # to the test-only sources). The two builds share the same
    # flag surface so the lifecycle tests exercise the same
    # code path the DLL exercises.
    add_library(imgui_vendor_test STATIC
        "${WALKERCORE_IMGUI_DIR}/imgui.cpp"
        "${WALKERCORE_IMGUI_DIR}/imgui_draw.cpp"
        "${WALKERCORE_IMGUI_DIR}/imgui_tables.cpp"
        "${WALKERCORE_IMGUI_DIR}/imgui_widgets.cpp"
        "${WALKERCORE_IMGUI_DIR}/imgui_impl_dx11.cpp"
        "${WALKERCORE_IMGUI_DIR}/imgui_impl_win32.cpp")
    target_include_directories(imgui_vendor_test SYSTEM PUBLIC
        "${WALKERCORE_IMGUI_DIR}")
    target_compile_options(imgui_vendor_test PRIVATE -w)
    if (WIN32)
        target_link_libraries(imgui_vendor_test PUBLIC d3dcompiler dwmapi)
    endif()
    target_compile_definitions(WalkerCoreTestLib PRIVATE
        WALKERCORE_HAVE_IMGUI=1)
    target_compile_definitions(WalkerCoreTests PRIVATE
        WALKERCORE_HAVE_IMGUI=1)
    target_include_directories(WalkerCoreTestLib PRIVATE
        "${WALKERCORE_IMGUI_DIR}")
    target_include_directories(WalkerCoreTests PRIVATE
        "${WALKERCORE_IMGUI_DIR}")
    target_link_libraries(WalkerCoreTestLib PUBLIC imgui_vendor_test)
    if (WIN32)
        target_link_libraries(WalkerCoreTests PRIVATE d3dcompiler dwmapi)
    endif()
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