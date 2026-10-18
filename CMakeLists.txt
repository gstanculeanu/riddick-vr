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
        src/hook/gl_matrix_tracker.h
        src/hook/vertex_program_observe.h
        src/hook/vertex_submission_observe.h
        src/hook/immediate_mode_observe.h
        src/hook/projection_near.h
        src/hook/depth_parallax.h
        src/hook/stereo_replay.h
        src/hook/stereo_dual.h
        src/hook/gl_entry_census.h
        src/hook/display_branch.h
        src/hook/sim_input_bridge.h
        src/hook/sim_input_bridge_win32.cpp
        src/hook/sim_input_bridge_win32.h
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

    # GH #67 step 1: software GL_MODELVIEW stack (the draw-time matrix capture).
    add_executable(riddick_vr_gl_matrix_tracker_tests tests/gl_matrix_tracker_tests.cpp)
    target_include_directories(riddick_vr_gl_matrix_tracker_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME gl_matrix_tracker COMMAND riddick_vr_gl_matrix_tracker_tests)

    # GH #67 step 1 (corrected, ADR-0481): the ARB vertex-program observation --
    # the transform source the live engine actually uses.
    add_executable(riddick_vr_vertex_program_tracker_tests tests/vertex_program_tracker_tests.cpp)
    target_include_directories(riddick_vr_vertex_program_tracker_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME vertex_program_observe COMMAND riddick_vr_vertex_program_tracker_tests)

    # GH #67 step 1 (second correction, ADR-0482): the submitted-vertex sample --
    # where the transform actually is (client-array geometry) and whether the
    # first-person pass is array or display-list draws.
    add_executable(riddick_vr_vertex_submission_tests tests/vertex_submission_observe_tests.cpp)
    target_include_directories(riddick_vr_vertex_submission_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME vertex_submission_observe COMMAND riddick_vr_vertex_submission_tests)

    # GH #67 step 1 (ninth correction, ADR-0491): the immediate-mode census -- the
    # submission path (glBegin/glEnd + glVertex3f/2f) the shipped module imports and
    # no previous probe counted. Pure, headless.
    add_executable(riddick_vr_immediate_mode_tests tests/immediate_mode_observe_tests.cpp)
    target_include_directories(riddick_vr_immediate_mode_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME immediate_mode_observe COMMAND riddick_vr_immediate_mode_tests)

    # GH #67 step 1 (sixth correction, ADR-0488): the perspective near plane from
    # the engine's own (z-flipped) projection convention -- the measurement the
    # NearPlaneDrop opener depends on. Pure, headless.
    add_executable(riddick_vr_projection_near_tests tests/projection_near_tests.cpp)
    target_include_directories(riddick_vr_projection_near_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME projection_near COMMAND riddick_vr_projection_near_tests)

    # GH #76 (interim depth-based reprojection, ADR-0501): the pure warp math --
    # window depth -> view distance for both projection conventions, distance ->
    # per-eye source-pixel shift, the IPD from the eye views, and the nearest
    # depth of a grid cell. Pure, headless.
    add_executable(riddick_vr_depth_parallax_tests tests/depth_parallax_tests.cpp)
    target_include_directories(riddick_vr_depth_parallax_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME depth_parallax COMMAND riddick_vr_depth_parallax_tests)

    # GH #76 (option 1): the per-eye replay maths -- the host's per-eye views are
    # a rigid eye-space translation of the head view, so the eye offset + replay
    # modelview reproduce them exactly. Pure, headless (stereo_replay.h); also
    # cross-checks the host's independent view construction (src/host/eye_frame.h).
    add_executable(riddick_vr_stereo_replay_tests tests/stereo_replay_tests.cpp)
    target_include_directories(riddick_vr_stereo_replay_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
        ${CMAKE_CURRENT_SOURCE_DIR}/src/host
    )
    add_test(NAME stereo_replay COMMAND riddick_vr_stereo_replay_tests)

    # GH #76 / GH #90 (ADR-0505): the native per-eye pass policy -- which draws are
    # world geometry (duplicated per eye) vs HUD/screen-space/viewmodel (never
    # eye-translated), and the fail-safe gate that refuses a frame with a black eye.
    # Pure, headless (stereo_dual.h).
    add_executable(riddick_vr_stereo_dual_tests tests/stereo_dual_tests.cpp)
    target_include_directories(riddick_vr_stereo_dual_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME stereo_dual COMMAND riddick_vr_stereo_dual_tests)

    # GH #76 / GH #90 (ADR-0507): the array-source census -- the dynamic GL
    # entry-point vocabulary the engine resolves, the live buffer-object binding,
    # and the generic-attribute / glInterleavedArrays path. Pure, headless
    # (gl_entry_census.h) plus the sampler's recorded pointer tally.
    add_executable(riddick_vr_gl_entry_census_tests tests/gl_entry_census_tests.cpp)
    target_include_directories(riddick_vr_gl_entry_census_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME gl_entry_census COMMAND riddick_vr_gl_entry_census_tests)

    # GH #61: the baked diffuse + the arm mesh UVs must be skin-toned, never a
    # white / flat patch (refutes the "decodes white" premise that disabled it).
    add_executable(riddick_vr_native_hand_skin_tests tests/native_hand_skin_tests.cpp)
    target_include_directories(riddick_vr_native_hand_skin_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME native_hand_skin_mapping COMMAND riddick_vr_native_hand_skin_tests)

    # GH #69 (requirement 1): XRSIM dump gate -- capture live 3D gameplay only.
    add_executable(riddick_vr_dump_gate_tests tests/dump_gate_tests.cpp)
    target_include_directories(riddick_vr_dump_gate_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/host
    )
    add_test(NAME dump_gate_gameplay_only COMMAND riddick_vr_dump_gate_tests)

    # GH #68 (requirement 1): the mock host's per-dump eye pose/projection record
    # (src/host/eye_frame.h) -- the 3D transform that makes a capture
    # interpretable (and the input GH #67 step 3 needs).
    add_executable(riddick_vr_eye_frame_tests tests/eye_frame_tests.cpp)
    target_include_directories(riddick_vr_eye_frame_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/host
    )
    add_test(NAME eye_frame_record COMMAND riddick_vr_eye_frame_tests)

    # GH #68 (requirement 2): frame-time pacing percentiles (p50/p95) + the
    # budget wire line (src/host/frame_time_stats.h) the harness asserts on.
    add_executable(riddick_vr_frame_time_tests tests/frame_time_tests.cpp)
    target_include_directories(riddick_vr_frame_time_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/host
    )
    add_test(NAME frame_time_percentiles COMMAND riddick_vr_frame_time_tests)

    # GH #68 (requirement 5, comfort rules): the mock host must assert the
    # locomotion comfort vignette was actually applied while the scripted run was
    # moving/turning (it reads the hook-published `comfortVignette` back and
    # summarises it with the locomotion input). Pure helper + wire line in
    # src/host/comfort_vignette.h, consumed by XRSIM's `comfort_vignette_applied`.
    add_executable(riddick_vr_comfort_vignette_tests tests/comfort_vignette_tests.cpp)
    target_include_directories(riddick_vr_comfort_vignette_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/host
    )
    add_test(NAME comfort_vignette_summary COMMAND riddick_vr_comfort_vignette_tests)

    # GH #72 (requirement 1): the published display branch must not label the
    # static 2D startup splash as 3D gameplay. The hook's own BarScan active-rect
    # fill (splash ~0.31-0.36 vs gameplay 1.00) gates a *cold-start* latch
    # (src/hook/display_branch.h): a clearly letterboxed frame is cinema until
    # the first full-bleed frame is ever measured. Pure helper, unit-tested;
    # default off (provisional thresholds, needs one live A/B).
    add_executable(riddick_vr_display_branch_tests tests/display_branch_tests.cpp)
    target_include_directories(riddick_vr_display_branch_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME display_branch_cold_start_latch COMMAND riddick_vr_display_branch_tests)

    # GH #79: the XRSIM capture stages both shared eye textures into ONE private
    # copy and runs the content gate and the BMP write over it, so the pixels
    # that are graded are the pixels that are saved. The pure sampler
    # (src/host/eye_sample.h) and the BMP file layout (src/host/bmp_writer.h)
    # are pinned here without a GPU.
    add_executable(riddick_vr_eye_capture_tests tests/eye_capture_tests.cpp)
    target_include_directories(riddick_vr_eye_capture_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/host
    )
    add_test(NAME eye_capture_helpers COMMAND riddick_vr_eye_capture_tests)

    # GH #80: the simulator-only input bridge. Both input-free routes to a live
    # scene are measured dead (posted messages -- ADR-0466; the engine's command
    # line -- its wake-64 addendum), so the sim confirm must be fabricated
    # in-process on the engine's own device read path. The install point is not
    # the hook's deferred init: MSystem.dll resolves XInputGetState /
    # DirectInputCreateA once, in CInputContext_DInput's constructor (0x1010b440),
    # so the intercept must precede it (ADR-0467). This pins the pure core --
    # enable gate, tap cadence, and the DirectInput/XInput state encodings
    # (src/hook/sim_input_bridge.h) -- which is headless-testable. The Win32
    # interceptor that consumes it (early MSystem.dll GetProcAddress swap +
    # DirectInput/XInput wrappers, src/hook/sim_input_bridge_win32.cpp) is on
    # the opengl32 proxy target above; ADR-0468.
    add_executable(riddick_vr_sim_input_bridge_tests tests/sim_input_bridge_tests.cpp)
    target_include_directories(riddick_vr_sim_input_bridge_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
    )
    add_test(NAME sim_input_bridge_pure_core COMMAND riddick_vr_sim_input_bridge_tests)

    # GH #80 (requirement 1): the same bridge's *interceptor* -- the Win32 half
    # that actually routes the engine's resolution calls to our wrappers -- run
    # end-to-end offline. The test installs the bridge on its own module (the
    # mechanism is module-agnostic: production passes MSystem.dll, ADR-0467/0468),
    # drives the engine's exact path (GetProcAddress -> wrapper -> injected
    # XInput/DirectInput state), and proves the gate-off inertness control
    # (requirement 3). No game, no deploy, no OS input; SKIP (42) without the
    # input DLLs. x86 only: the hook ships x86 and the engine resolves the 32-bit
    # legacy dinput.dll named in GH #80.
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        # An independent, *unpatched* resolution path for the test: the bridge
        # patches the test executable's GetProcAddress IAT, so the baseline must
        # come from a module the test never patches (the optimizer would
        # otherwise CSE the two identical GetProcAddress calls and hide a broken
        # intercept).
        add_library(riddick_vr_sim_bridge_probe SHARED tests/sim_bridge_probe_dll.cpp)

        add_executable(riddick_vr_sim_input_bridge_win32_tests
            tests/sim_input_bridge_win32_tests.cpp
            src/hook/sim_input_bridge_win32.cpp
            src/hook/iat_hook.cpp
            src/hook/logging.cpp
        )
        target_include_directories(riddick_vr_sim_input_bridge_win32_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/hook
        )
        target_link_libraries(riddick_vr_sim_input_bridge_win32_tests PRIVATE
            riddick_vr_sim_bridge_probe
        )
        add_test(NAME sim_input_bridge_win32_intercept
                 COMMAND riddick_vr_sim_input_bridge_win32_tests gate-on)
        add_test(NAME sim_input_bridge_win32_inert
                 COMMAND riddick_vr_sim_input_bridge_win32_tests gate-off)
        set_tests_properties(sim_input_bridge_win32_intercept
                             sim_input_bridge_win32_inert
                             PROPERTIES SKIP_RETURN_CODE 42)
    endif()
endif()
