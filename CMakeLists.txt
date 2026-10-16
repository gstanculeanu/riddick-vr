cmake_minimum_required(VERSION 3.25)
project(RiddickVR VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bridge
)

# Common IPC Bridge Library
add_library(riddick_vr_bridge STATIC
    src/bridge/ipc_bridge.cpp
    src/bridge/ipc_bridge.h
)
target_include_directories(riddick_vr_bridge PUBLIC
    include
    src/bridge
)

# ---- Build Options (Follows F.E.A.R. VR multi-arch build pattern) ------------
option(RIDDICKVR_BUILD_HOST  "Build x64 OpenXR companion host (riddickvr-host.exe)" ON)
option(RIDDICKVR_BUILD_PROXY "Build x86 opengl32.dll proxy/bridge"                  ON)

# Architecture-specific targets
if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND RIDDICKVR_BUILD_PROXY)
    message(STATUS "Configuring 32-bit Hook target: opengl32.dll...")
    
    add_library(opengl32 SHARED
        src/hook/dllmain.cpp
        src/hook/opengl_proxy.cpp
        src/hook/opengl_proxy.h
        src/hook/opengl_forwarders.c
        src/hook/opengl_forwarders.h
        src/hook/iat_hook.cpp
        src/hook/iat_hook.h
        src/hook/stereo_renderer.cpp
        src/hook/stereo_renderer.h
        src/hook/vr_hand_renderer.cpp
        src/hook/vr_hand_renderer.h
        src/hook/native_hand_mesh.h
        src/hook/native_hand_mesh_data.h
        src/hook/native_hand_pose.h
        src/hook/native_hand_texture.h
        src/hook/native_hand_texture_data.h
        src/hook/hand_visuals.h
        src/hook/bc1.h
        src/hook/arm_ik.h
        src/hook/combat_stance.h
        src/hook/combat_guard.h
        src/hook/combat_strike.h
        src/hook/dialogue_navigation.h
        src/hook/viewmodel_suppressor.h
        src/hook/viewmodel_drive.h
        src/hook/camera_hook.cpp
        src/hook/camera_hook.h
        src/hook/vr_config.cpp
        src/hook/vr_config.h
        src/hook/gesture_tap.cpp
        src/hook/gesture_tap.h
        src/hook/logging.cpp
        src/hook/logging.h
        src/hook/opengl32.def
    )
    
    target_link_libraries(opengl32 PRIVATE
        riddick_vr_bridge
        d3d11
        dxgi
    )
    
    set_target_properties(opengl32 PROPERTIES
        OUTPUT_NAME "opengl32"
        PREFIX ""
    )

elseif(CMAKE_SIZEOF_VOID_P EQUAL 8 AND RIDDICKVR_BUILD_HOST)
    message(STATUS "Configuring 64-bit Companion Host target: riddickvr-host.exe...")
    
    add_executable(riddickvr-host
        src/host/main.cpp
        src/host/openxr_manager.cpp
        src/host/openxr_manager.h
    )
    
    target_link_libraries(riddickvr-host PRIVATE
        riddick_vr_bridge
        d3d11
        dxgi
    )
endif()

if(BUILD_TESTING)
    add_executable(riddick_vr_arm_ik_tests tests/arm_ik_tests.cpp)
    target_include_directories(riddick_vr_arm_ik_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME arm_ik_constraints COMMAND riddick_vr_arm_ik_tests)

    add_executable(riddick_vr_combat_stance_tests tests/combat_stance_tests.cpp)
    target_include_directories(riddick_vr_combat_stance_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME combat_stance_hysteresis COMMAND riddick_vr_combat_stance_tests)

    # GH #62: front-of-HMD block/guard envelope (shared by pose + strike gate).
    add_executable(riddick_vr_combat_guard_tests tests/combat_guard_tests.cpp)
    target_include_directories(riddick_vr_combat_guard_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME combat_guard_envelope COMMAND riddick_vr_combat_guard_tests)

    # GH #65: swing-driven strike classifier (shared, INI-tunable thresholds).
    add_executable(riddick_vr_combat_strike_tests tests/combat_strike_tests.cpp)
    target_include_directories(riddick_vr_combat_strike_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME combat_strike_thresholds COMMAND riddick_vr_combat_strike_tests)

    # GH #65: held-input scheduler (melee clicks are held, not zero-length).
    add_executable(riddick_vr_input_hold_tests tests/input_hold_tests.cpp)
    target_include_directories(riddick_vr_input_hold_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME input_hold_scheduler COMMAND riddick_vr_input_hold_tests)

    # GH #50/#51: aim steering helper (no pull while guarding or swinging).
    add_executable(riddick_vr_aim_pull_tests tests/aim_pull_tests.cpp)
    target_include_directories(riddick_vr_aim_pull_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME aim_pull COMMAND riddick_vr_aim_pull_tests)

    # GH #66 (approach 2): per-draw viewmodel suppression window state machine.
    add_executable(riddick_vr_viewmodel_suppressor_tests tests/viewmodel_suppressor_tests.cpp)
    target_include_directories(riddick_vr_viewmodel_suppressor_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME viewmodel_suppressor_window COMMAND riddick_vr_viewmodel_suppressor_tests)

    # GH #67 (ADR-0447): controller-driven engine viewmodel correction matrix.
    add_executable(riddick_vr_viewmodel_drive_tests tests/viewmodel_drive_tests.cpp)
    target_include_directories(riddick_vr_viewmodel_drive_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME viewmodel_drive_correction COMMAND riddick_vr_viewmodel_drive_tests)

    add_executable(riddick_vr_dialogue_navigation_tests tests/dialogue_navigation_tests.cpp)
    target_include_directories(riddick_vr_dialogue_navigation_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME dialogue_navigation_deadzone COMMAND riddick_vr_dialogue_navigation_tests)

    # GH #55 -> GH #46: baked native hand mesh asset (generated header + loader).
    add_executable(riddick_vr_native_hand_mesh_tests tests/native_hand_mesh_tests.cpp)
    target_include_directories(riddick_vr_native_hand_mesh_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME native_hand_mesh_asset COMMAND riddick_vr_native_hand_mesh_tests)

    # GH #46: native hand mesh model-space -> hand-local placement maths.
    add_executable(riddick_vr_native_hand_pose_tests tests/native_hand_pose_tests.cpp)
    target_include_directories(riddick_vr_native_hand_pose_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME native_hand_pose_mapping COMMAND riddick_vr_native_hand_pose_tests)

    # GH #61: baked native hand texture asset + BC1/DXT1 decoder.
    add_executable(riddick_vr_native_hand_texture_tests tests/native_hand_texture_tests.cpp)
    target_include_directories(riddick_vr_native_hand_texture_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME native_hand_texture_asset COMMAND riddick_vr_native_hand_texture_tests)

    # GH #64 / GH #46: hand cue tint and weapon fade helpers.
    add_executable(riddick_vr_hand_visuals_tests tests/hand_visuals_tests.cpp)
    target_include_directories(riddick_vr_hand_visuals_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME hand_visuals COMMAND riddick_vr_hand_visuals_tests)
endif()
