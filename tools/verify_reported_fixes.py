"""Guard source-level invariants for the VR issue fixes and new interaction layers."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    camera = (ROOT / "src/hook/camera_hook.cpp").read_text(encoding="utf-8")
    renderer = (ROOT / "src/hook/stereo_renderer.cpp").read_text(encoding="utf-8")
    hands = (ROOT / "src/hook/vr_hand_renderer.cpp").read_text(encoding="utf-8")
    hand_header = (ROOT / "src/hook/vr_hand_renderer.h").read_text(encoding="utf-8")
    ik_header = (ROOT / "src/hook/arm_ik.h").read_text(encoding="utf-8")
    host = (ROOT / "src/host/openxr_manager.cpp").read_text(encoding="utf-8")
    host_main = (ROOT / "src/host/main.cpp").read_text(encoding="utf-8")
    action_map = (ROOT / "src/hook/vr_action_map.h").read_text(encoding="utf-8")
    stance = (ROOT / "src/hook/combat_stance.h").read_text(encoding="utf-8")
    guard_header = (ROOT / "src/hook/combat_guard.h").read_text(encoding="utf-8")
    strike_header = (ROOT / "src/hook/combat_strike.h").read_text(encoding="utf-8")
    dialogue = (ROOT / "src/hook/dialogue_navigation.h").read_text(encoding="utf-8")
    config = (ROOT / "src/hook/vr_config.cpp").read_text(encoding="utf-8")
    pose_header = (ROOT / "src/hook/native_hand_pose.h").read_text(encoding="utf-8")
    xrsim_tool = (ROOT / "tools/xrsim.py").read_text(encoding="utf-8")

    require("vrState.cinemaMode = false" not in camera,
            "camera update must not override the renderer's cinema decision")
    require("m_sharedState.cinemaMode = isCinema;" in renderer and
            "ComputeDisplayBranch(" in renderer and
            "OpenGLProxy::Has3DThisFrame(), s_cursorMenu" in renderer,
            "renderer must select cinema for non-3D frames/menu cursor")
    require("if (isCinema)" in renderer and "ScanActiveRegion(srcX, srcY, srcW, srcH);" in renderer,
            "cinema fit and gameplay active-region crop branches must remain")

    require("OpenGLProxy::Has3DGameplayProjection())" in camera and
            "st.accumulatedLookX += mouseDeltaX;" in camera and
            "st.accumulatedLookY += mouseDeltaY;" in camera and
            "s_headlook.accumulatedLookX = 0.0f;" in camera,
            "head-look deltas must accumulate in gameplay and discard pre-gameplay menu motion")

    require("glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)" in hands and
            "s_gl.glColor4f(r, g, b, f)" in hands and "GL_CURRENT_BIT" in hands,
            "hand fade must use restored alpha blending rather than dark opaque geometry")

    require(camera.count("st.lastPullActivityTime = now;") >= 2 and
            camera.count("st.idleHintFired = false;") >= 2 and
            "constexpr DWORD MANTLE_GRACE_MS = 180;" in camera and
            "SetLadderCue(leftLadderGlow, rightLadderGlow);" in camera and
            "void VRHandRenderer::SetLadderCue" in hands,
            "ladder grab must reset its idle timer and publish the mantle visual cue")

    require(all(key in host_main for key in ("AmmoQuadCropX", "AmmoQuadCropY", "AmmoQuadCropW", "AmmoQuadCropH")) and
            all(key in host for key in ("m_ammoQuadCropX", "m_ammoQuadCropY", "m_ammoQuadCropW", "m_ammoQuadCropH")) and
            'if (!configPath.empty())' in renderer and
            'cmdLine += " --config \\"' in renderer,
            "host must load and apply all four configurable ammo crop coordinates")

    require(all(token in stance for token in ("ENTER_THRESHOLD = 0.85f", "EXIT_THRESHOLD = 0.60f",
                                               "ENTER_HOLD_MS = 100", "left.isConnected && right.isConnected")) and
            # GH #65: melee is swing-velocity driven (no both-trigger chord gate);
            # the two-hand stance still gates the guard *pose* (next token).
            "combatStance.active && !isClimbingActive" not in camera and
            "!isClimbingActive && (now - s_melee.lastPunchTime > 260)" in camera and
            "bool guardPoseDetected = !isClimbingActive && !s_twoHandedStabilizationActive" in camera and
            "chinLevel && inFrontOfFace && handsClose && handsSteady && notPunching" in camera,
            "combat stance must use two-hand hysteresis and gate strikes/guard")
    require("struct GuardEnvelope" in guard_header and "InGuardEnvelope" in guard_header and
            '#include "combat_guard.h"' in camera and
            "InGuardEnvelope(guardVolume," in camera and
            "GetGuardEnvelope()" in camera and
            all(key in config for key in ("guardYMin", "guardYMax", "guardZMin",
                                          "guardZMax", "guardMaxHandSpan")) and
            "combat_guard_tests.cpp" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"),
            "GH #62: the block guard must be a shared, INI-tunable front-of-HMD envelope")
    require("struct StrikeThresholds" in strike_header and "ClassifyStrike" in strike_header and
            '#include "combat_strike.h"' in camera and
            "ClassifyStrike(strikeThresholds," in camera and
            "GetStrikeThresholds()" in camera and
            all(f"strike{name}" in config for name in
                ("ForwardVz", "HookVx", "UppercutVy", "CrossVx")) and
            "combat_strike_tests.cpp" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"),
            "GH #65: strikes must be swing-classified by shared, INI-tunable thresholds")
    cmake_text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    send_key_body = camera.split("static void SendKey(WORD vk, bool isDown", 1)[1][:240] \
        if "static void SendKey(WORD vk, bool isDown" in camera else ""
    require("ToHeadYawLocal(" in strike_header and "ToHeadYawLocal(" in camera and
            "SendMouseTap(true)" in camera and "SendKeyHeld(" in camera and
            "FlushHeldInputs()" in camera and "MELEE STRIKE DISPATCHED" in camera and
            "class InputHoldScheduler" in (ROOT / "src/hook/input_hold.h").read_text(encoding="utf-8") and
            "input_hold_tests.cpp" in cmake_text and "aim_pull_tests.cpp" in cmake_text and
            "SimModeNoInput()" in send_key_body and
            # 18 unpaired SendMouseClick(true, true) remain: grenade cook, fire,
            # and pairs interleaved with gesture taps. Paired clicks must stay held taps.
            camera.count("SendMouseClick(true, true)") <= 18 and
            "ComputeAimPull(" in camera and "UpdateGuardPose(" in camera and
            "CombatStanceThresholds" in stance,
            "GH #65/#62/#52/#50/#51: head-local strikes, held clicks, guard hysteresis, stance and aim pull")
    require("using VRArmIK::ClampWristOrientation" in hands and "MIN_ELBOW_ANGLE" in ik_header and
            "arm_ik_tests.cpp" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8") and
            "DrawArmSegment(shoulder, elbow" in hands and "m_lastIkFrame" in hand_header and
            "ClampWristOrientation" in ik_header and "0.87266463f" in ik_header and
            "DrawArticulatedFinger(side, 0, indexCurl" in hands,
            "tracked hands must render constrained, frame-stable two-bone arms with analog fingers")
    require(all(f"VRAction::{name}" in action_map for name in
                ("Interact", "DialogueUp", "DialogueDown", "DialogueLeft", "DialogueRight")) and
            "dialogueDeadzone" in config and "interactButton" in config and
            "g_vrConfig.interactButton == 1" in camera and
            "navMagnitude < deadzone * 0.55f" in camera and
            "ChooseDialogueDirection" in camera and "ChooseDialogueDirection(" in dialogue and
            "dialogue_navigation_tests.cpp" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"),
            "dialogue/navigation must route through the action map with an analog deadzone")
    require("gripPose.linearVelocity" in host and "90.0f / frameSpan" in host and
            "ApplyScenarioFrame(state, totalFrames);" in host,
            "scripted mock trajectories must drive velocity-sensitive gestures")

    # GH #64: the native hand mesh must read as solid 3D (two-sided, real
    # per-vertex GL lighting) with a tunable orientation correction, and the
    # procedural interaction boxes must not float on the native path.
    require("RotateNativeHandPlacement" in pose_header and
            "const bool native = DrawNativeHand(side, cue);" in hands and
            "s_gl.glDisable(GL_CULL_FACE);" in hands and
            "GL_LIGHT_MODEL_TWO_SIDE" in hands and
            "GL_TRUE" in hands and
            "if (!native) {" in hands and
            all(f"nativeHand{axis}" in config for axis in ("Yaw", "Pitch", "Roll")),
            "GH #64: native hands must be two-sided + lit with a tunable orientation")

    # GH #64 / ADR-0444: the native-hand placement must use the mesh's *baked*
    # long axis (stable across the bind and posed variants) rather than the
    # largest-extent AABB guess, which flipped for the posed right arm; and the
    # record must not re-assert the (unproven) "file geometry is the bind pose"
    # claim.
    _mesh_hdr = (ROOT / "src/hook/native_hand_mesh.h").read_text(encoding="utf-8")
    _baker = (ROOT / "tools/xmd_bake.py").read_text(encoding="utf-8")
    require("ComputeNativeHandPlacementAlongAxis" in pose_header and
            "mesh->arm_axis" in hands and
            "arm_axis" in _mesh_hdr and
            "_long_axis" in _baker and "arm_axis" in _baker and
            "BIND-POSE PROVENANCE" in _mesh_hdr,
            "GH #64: the native-hand placement must use the baked mesh axis "
            "(ADR-0444), not the pose-sensitive AABB extent guess")

    # GH #61 / #64 / #46 (ADR-0445): the native hand must sample its real texture
    # despite engine GL state, show cues without stray boxes, and not fade while
    # boxing.
    _cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    _tex_hdr = (ROOT / "src/hook/native_hand_texture.h").read_text(encoding="utf-8")
    _cfg_hdr = (ROOT / "src/hook/vr_config.h").read_text(encoding="utf-8")
    require("CheckNativeTextureIntegrity" in _tex_hdr and
            "CheckNativeTextureIntegrity(" in hands and
            "0x84C0" in hands and
            "ComputeNativeHandCueTint(" in hands and
            "ComputeHandFade(" in hands and
            "hand_visuals_tests.cpp" in _cmake and
            "nativeHandUpperArm" in _cfg_hdr and "nativeHandUpperArm" in config,
            "GH #61/#64/#46: native hand GL-state sanitising, texture integrity check, "
            "cue tint, upper-arm option and boxing-safe fade must be wired (ADR-0445)")

    # GH #66 (approach 2): per-draw engine viewmodel suppression must be
    # off-by-default, intercept RndrGL's geometry draw entry points, forward to
    # the saved original when the window is closed, and expose a unit-tested
    # window state machine fed by the already-intercepted GL signals.
    suppressor = (ROOT / "src/hook/viewmodel_suppressor.h").read_text(encoding="utf-8")
    dllmain = (ROOT / "src/hook/dllmain.cpp").read_text(encoding="utf-8")
    proxy = (ROOT / "src/hook/opengl_proxy.cpp").read_text(encoding="utf-8")
    require("class ViewmodelSuppressor" in suppressor and
            "ViewmodelWindowOpener" in suppressor and
            "ShouldSuppressDraw()" in suppressor and
            "suppressEngineViewmodel = 0" in (ROOT / "src/hook/vr_config.h").read_text(encoding="utf-8") and
            "Hook_glDrawElements" in dllmain and "Hook_glCallList" in dllmain and
            '"glDrawElements"' in dllmain and '"glCallList"' in dllmain and
            "real_glDrawElements" in dllmain and "real_glCallList" in dllmain and
            "ShouldSuppressDrawCall()" in dllmain and
            "s_viewmodelSuppressor.NoteDepthOnlyClear()" in proxy and
            "s_viewmodelSuppressor.NoteProjectionLoad(" in proxy and
            "s_viewmodelSuppressor.NoteSwap()" in proxy and
            "ShouldSuppressDrawCall()" in proxy and
            "viewmodel_suppressor_tests.cpp" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"),
            "GH #66: engine viewmodel suppression must be off-by-default, intercept RndrGL draws, and be unit-tested")

    # GH #63/#66/#67: window semantics, pass trace and the opt-in controller drive.
    vm_config = (ROOT / "src/hook/vr_config.h").read_text(encoding="utf-8")
    cmake_text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    require("NoteProjectionLoad(bool" in suppressor and
            "LastFrameTrace" in suppressor and
            "driveEngineViewmodel = 0" in vm_config and
            "ComputeViewmodelCorrection" in (ROOT / "src/hook/viewmodel_drive.h").read_text(encoding="utf-8") and
            "ComputeViewmodelCorrection(" in proxy and
            "LastFrameTrace()" in proxy and
            "viewmodel_drive_tests.cpp" in cmake_text,
            "GH #63/#66/#67: viewmodel window must track the FP pass, expose a trace, and the controller drive must be opt-in and unit-tested")

    # GH #67 step 2: the engine's own first-person viewmodel pass must be
    # *identifiable* before it can be driven from the controller. The observation
    # mode reuses the GH #66 depth-only-clear boundary, records the in/out-of-
    # window draw tallies and the modelview at the first in-window draw, never
    # suppresses, defaults off, and is consumed by XRSIM (which skips, not fails,
    # when the probe was not run).
    require("bool observe = false;" in suppressor and
            "ViewmodelObservationSnapshot" in suppressor and
            "NoteObservedDraw" in suppressor and "SetObservedSample" in suppressor and
            "ComputeSnapshot" in suppressor and
            "viewmodelObserve = 0" in (ROOT / "src/hook/vr_config.h").read_text(encoding="utf-8") and
            "ViewmodelObserve" in config and
            "ViewmodelProbe" in proxy and
            "s_viewmodelSuppressor.Observing()" in proxy and
            "s_viewmodelSuppressor.NoteObservedDraw()" in proxy and
            "s_viewmodelSuppressor.SetObservedSample(s_lastView)" in proxy and
            "viewmodel_probe_stats" in xrsim_tool and
            "viewmodel_pass_identified" in xrsim_tool,
            "GH #67: the first-person viewmodel pass must be identifiable by an "
            "off-by-default, non-suppressing observation probe consumed by XRSIM")

    # GH #67 step 1: the draw-time modelview must be the *effective* one. RndrGL
    # composes transforms from glLoadMatrixf + glPushMatrix/glTranslatef/
    # glPopMatrix, so a software mirror (gl_matrix_tracker.h) must be fed by the
    # intercepted composition calls and must drive GetLastViewMatrix().
    tracker = ROOT / "src/hook/gl_matrix_tracker.h"
    require(tracker.exists() and
            "class GlMatrixTracker" in tracker.read_text(encoding="utf-8") and
            "NoteModelviewPush" in proxy and
            "NoteModelviewTranslatef" in proxy and
            "s_modelviewTracker" in proxy and
            "OPENGL32.dll" in dllmain and
            "glPushMatrix" in dllmain and
            "gl_matrix_tracker_tests.cpp" in cmake_text,
            "GH #67: the draw-time modelview must be composed (glPushMatrix/"
            "glTranslatef/glPopMatrix mirrored by gl_matrix_tracker.h), not just "
            "the last loaded glLoadMatrixf")

    # GH #58: the native hand path must be able to consume an offline-baked
    # posed variant of the mesh (FK(clip) + influence-weighted skinning,
    # ADR-0443), selected by an INI flag and defaulting to the bind geometry.
    mesh_header = (ROOT / "src/hook/native_hand_mesh.h").read_text(encoding="utf-8")
    baker = (ROOT / "tools/xmd_bake.py").read_text(encoding="utf-8")
    require("posed_vertices" in mesh_header and "pose_source" in mesh_header and
            "has_pose()" in mesh_header and
            "nativeHandPose = 0" in (ROOT / "src/hook/vr_config.h").read_text(encoding="utf-8") and
            "NativeHandPose" in config and
            "mesh->has_pose()" in hands and
            "mesh->posed_vertices" in hands and
            "mesh->posed_bounds_min" in hands and
            "load_pose" in baker and "POSE_SPEC" in baker and
            "xmd_fk.skin_positions" in baker and "pose-key" in baker,
            "GH #58: the native hand mesh must support an offline-baked posed "
            "variant selected by NativeHandPose")

    # GH #61: the native hands must ship with the real Riddick skin diffuse
    # bound. The commit that disabled it (52c2101) claimed the bake "decoded
    # white"; that is false (the committed asset is warm skin tones with no
    # near-white texels and the arm meshes' own UVs sample it), so a regression
    # test pins the fact on the committed asset and the shipped INI keeps the
    # texture enabled rather than reverting on the same false premise.
    require("NativeHandTexture=1" in (ROOT / "riddick_vr.ini").read_text(encoding="utf-8") and
            "EnsureNativeHandTexture" in hands and
            'FindNativeHandTexture("Riddick_ArmScar_D")' in hands and
            "GL_MODULATE" in hands and
            "nativeHandTexture = 1" in (ROOT / "src/hook/vr_config.h").read_text(encoding="utf-8") and
            "tests/native_hand_skin_tests.cpp" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8") and
            "native_hand_skin_mapping" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8") and
            "test_committed_asset_is_skin_toned" in
            (ROOT / "tools/test_xtc_bake.py").read_text(encoding="utf-8"),
            "GH #61: the shipped config must keep the real skin diffuse enabled "
            "and the skin-toned bake must stay pinned by a regression test")

    # GH #69: the mock host must label every capture with the display branch it
    # published (cinema vs gameplay) and persist that label beside the frames,
    # and the harness must consume the label so identical eyes are attributed
    # instead of guessed (and a cinema-only run cannot pass the stereo gates).
    xrsim_tool = (ROOT / "tools/xrsim.py").read_text(encoding="utf-8")
    require("cinema=" in host and "display_branch.log" in host and
            "stateRead && clientState.cinemaMode" in host and
            "read_display_branch_labels" in xrsim_tool and
            "gameplay_captures_present" in xrsim_tool and
            "persist_host_log" in xrsim_tool and
            'DISPLAY_LABEL_FILE = "display_branch.log"' in xrsim_tool,
            "GH #69: XRSIM must label each capture with its display branch and "
            "attribute identical eyes per frame")

    # GH #69 requirement 1: the dump window must be gated on the hook-published
    # display branch so the capture is live 3D gameplay (menu/cinema + interop
    # warm-up frames have correctly-identical eyes and cannot validate stereo).
    # The gate is a pure, unit-tested helper and scripted runs opt in.
    require("ShouldDumpFrame" in (ROOT / "src/host/dump_gate.h").read_text(encoding="utf-8") and
            "ShouldDumpFrame(dumpDue, m_dumpGameplayOnly, stateRead, cinemaMode, content)" in host and
            "--dump-gameplay-only" in host_main and
            "build_host_cmd" in xrsim_tool and
            '"--dump-gameplay-only"' in xrsim_tool and
            "dump_gate_tests.cpp" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"),
            "GH #69: XRSIM must gate the dump window on gameplay-only frames "
            "(pure helper + --dump-gameplay-only + unit test)")

    # GH #69 requirement 4: the hook truncates riddick_vr.log on every launch
    # (logging.cpp: std::ios::out|std::ios::trunc), so a pre-run byte offset is
    # not a run boundary -- the harness must recover the *current run's* section
    # by its header or markers logged early in the new run are silently dropped.
    require("std::ios::out | std::ios::trunc" in
            (ROOT / "src/hook/logging.cpp").read_text(encoding="utf-8") and
            "RUN_HEADER" in xrsim_tool and "extract_run_section" in xrsim_tool and
            "read_fresh_hook_log" in xrsim_tool and
            # The restored harness names the current-run log path `log_file`
            # (a `LOG_FILE` constant is no longer used at this call site).
            "read_fresh_hook_log(log_file, log_mark)" in xrsim_tool,
            "GH #69: XRSIM must read the current-run hook-log section (the hook "
            "truncates its log per launch), not slice by a stale byte offset")

    # GH #71 requirement 1: the mock host follows the *game's* uncapped swap rate,
    # so a frame-counted window cannot express the wall-clock the startup sequence
    # needs. Reaching live gameplay must run with an unlimited frame count bounded
    # by wall-clock, and must prove a full-bleed 3D scene was reached from the
    # hook's structural BarScan active-rect signal -- not the `cinemaMode` bit,
    # which mislabels the static 2D splash pages as gameplay (GH #72).
    require("--reach-gameplay" in xrsim_tool and
            "GAMEPLAY_DEFAULT_DURATION" in xrsim_tool and
            "active_rect_fill" in xrsim_tool and
            "BARSCAN_WINDOW_RE" in xrsim_tool and
            "gameplay_full_bleed_reached" in xrsim_tool and
            "0 if getattr(args, \"reach_gameplay\", False) else args.frames" in xrsim_tool and
            "gameplay_capture_stats" in xrsim_tool and
            "live_gameplay" in xrsim_tool,
            "GH #71: XRSIM must reach live gameplay with an unlimited, wall-clock "
            "bounded run and assert the full-bleed BarScan signal")

    # GH #72 requirement 2: the gameplay-only dump window must also be
    # *content-gated* -- a gameplay-labelled frame is skipped while it is
    # all-black (GL<->D3D11 interop warm-up) or byte-identical to the last
    # dumped frame (a frozen static 2D page), so the graded window is live,
    # advancing 3D gameplay. The verdict is a pure FrameContent helper,
    # unit-tested, and surfaced in the harness report.
    dump_gate = (ROOT / "src/host/dump_gate.h").read_text(encoding="utf-8")
    require("FrameContent" in dump_gate and
            "FrameContent::Live" in dump_gate and
            "SampleEyeContent" in host and
            "kBlankMeanLuma" in host and
            "RiddickVR::FrameContent::Frozen" in host and
            "DumpGate skip frame=" in host and
            "ShouldDumpFrame(dumpDue, m_dumpGameplayOnly, stateRead, cinemaMode, content)" in host and
            # GH #72 (both-eye rule): a frame with an empty eye is not gradable
            # (it reads as a spurious stereo failure), so the content probe
            # samples both eyes.
            "EyePairIsBlank" in dump_gate and
            "EyePairIsBlank(leftLuma, leftSampled, rightLuma, rightSampled," in host and
            "a blank right eye must mark the pair as blank" in
            (ROOT / "tests/dump_gate_tests.cpp").read_text(encoding="utf-8") and
            "FrameContent::Blank" in (ROOT / "tests/dump_gate_tests.cpp").read_text(encoding="utf-8") and
            "content_gate_stats" in xrsim_tool and
            '"content_gate"' in xrsim_tool and
            "gameplay_window_has_content" in xrsim_tool,
            "GH #72: the XRSIM gameplay window must be content-gated (blank "
            "interop warm-up / frozen repeats excluded) and report why")

    # GH #72 requirement 1: the published display branch (`cinemaMode`, which the
    # XRSIM gameplay-only dump gate and the per-frame stereo skip trust) must not
    # label a letterboxed *static 2D* startup page as 3D gameplay -- the legacy
    # verdict is `Has3DThisFrame() || menuCursor`, and the splash loads a
    # perspective projection. The hook's own BarScan active-rect fill is the
    # structural input (splash ~0.31-0.36 vs gameplay 1.00); a *cold-start* latch
    # demotes a clearly letterboxed frame until the first full-bleed frame has
    # ever been seen (so gameplay with engine-baked bars is unaffected, GH #44).
    # Opt-in: the thresholds are provisional and need one live A/B.
    display_branch = (ROOT / "src/hook/display_branch.h").read_text(encoding="utf-8")
    config_header = (ROOT / "src/hook/vr_config.h").read_text(encoding="utf-8")
    config_source = (ROOT / "src/hook/vr_config.cpp").read_text(encoding="utf-8")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    require("ComputeDisplayBranch" in display_branch and
            "DisplayBranchConfig" in display_branch and
            "coldStartLatch" in display_branch and
            "fullBleedSeen" in display_branch and
            "ComputeDisplayBranch(" in renderer and
            "displayBranchColdStartLatch" in config_header and
            'ReadInt(iniPath, "DisplayBranchColdStartLatch"' in config_source and
            "DisplayBranchColdStartLatch=0" in
            (ROOT / "riddick_vr.ini").read_text(encoding="utf-8") and
            "tests/display_branch_tests.cpp" in cmake and
            "display_branch_cold_start_latch" in cmake,
            "GH #72: the display branch must demote a letterboxed startup page "
            "behind an opt-in cold-start latch driven by the BarScan fill")

    # GH #68 requirement 1: every dumped frame must record the head/eye transform
    # and projection it was rendered with, so a capture is self-describing in 3D
    # (the input GH #67 step 3 needs). The wire format is a pure helper
    # (src/host/eye_frame.h), unit-tested, parsed back by the harness, and
    # surfaced in the report as `eye_frames`.
    eye_frame = (ROOT / "src/host/eye_frame.h").read_text(encoding="utf-8")
    require("FormatEyeFrameLine" in eye_frame and
            "EyeFrameSample" in eye_frame and
            "eye_frames.log" in host and
            "FormatEyeFrameLine(totalFrames, sample)" in host and
            "tests/eye_frame_tests.cpp" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8") and
            "eye_frame_record" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8") and
            "def read_eye_frames" in xrsim_tool and
            "def eye_frame_stats" in xrsim_tool and
            "capture_metadata_present" in xrsim_tool and
            '"eye_frames"' in xrsim_tool,
            "GH #68: the mock host must record each dumped frame's camera "
            "transform (eye pose/projection) and the harness must parse it")

    # GH #68 requirement 2 (second half): the frame budget must be asserted on a
    # *frame-pacing* statistic, not on the arithmetic mean / single worst frame.
    # The mock host follows the game's uncapped swap rate, so a one-off
    # level-load hitch (measured ~760 ms) dominates mean/max without being a
    # sustained 90 Hz regression. The summary therefore carries p50/p95 (pure
    # helper src/host/frame_time_stats.h, unit-tested) and the harness asserts
    # the budget on p50/p95 while still reporting max_ms and the drop count
    # (ADR-0455).
    frame_time = (ROOT / "src/host/frame_time_stats.h").read_text(encoding="utf-8")
    require("PercentileMs" in frame_time and
            "p50Ms" in frame_time and "p95Ms" in frame_time and
            "SummarizeFrameTimes" in frame_time and
            "FormatFrameBudgetLine" in frame_time and
            "frame_time_stats.h" in host and
            "FormatFrameBudgetLine(stats)" in host and
            "tests/frame_time_tests.cpp" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8") and
            "frame_time_percentiles" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8") and
            "p50_ms=([\\d.]+) p95_ms=([\\d.]+)" in xrsim_tool and
            "p50 <= eff and p95 <= eff" in xrsim_tool and
            '"p50_ms": p50' in xrsim_tool and
            '"p95_ms": p95' in xrsim_tool,
            "GH #68: the frame budget must be asserted on p50/p95 pacing "
            "(with max/drops still reported) and surfaced in the harness report")

    # GH #72 requirement 3: the dark-frame `dy=±4` is an alignment-estimator
    # artefact, not a stereo defect (ADR-0454). The harness must gate the
    # vertical-disparity verdict on the frame's contrast and on the optimum not
    # being pinned to the search edge (an unquantifiable offset), and surface the
    # verdict in the report -- so the graded window raises no false defect while a
    # textured frame with a real, interior offset still fails.
    _xrsim_tests = (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8")
    require("MIN_DY_CONTRAST" in xrsim_tool and
            "DY_SEARCH_BOUND" in xrsim_tool and
            "dy_at_boundary" in xrsim_tool and
            "vertical_disparity_stats" in xrsim_tool and
            '"vertical_disparity"' in xrsim_tool and
            "estimator artefact" in xrsim_tool and
            "test_boundary_dy_is_inconclusive_not_failed" in _xrsim_tests and
            "test_high_contrast_interior_dy_still_fails" in _xrsim_tests and
            "test_low_contrast_vertical_shift_is_inconclusive" in _xrsim_tests,
            "GH #72: the vertical-disparity verdict must be contrast- and "
            "search-boundary-gated (artefact on dark/flat frames, ADR-0454) and "
            "reported")

    # GH #68 requirement 5 (comfort rules): the locomotion comfort vignette must
    # be assertable without a headset, so a config/parse regression that disabled
    # it cannot hide. The mock host reads the hook-published `comfortVignette`
    # back every frame and summarises it against the locomotion input it published
    # (pure helper src/host/comfort_vignette.h, unit-tested); XRSIM asserts the
    # level actually rose while the run moved/turned, skipped -- never failed --
    # when the line or the locomotion input is absent (ADR-0456).
    comfort_hdr = (ROOT / "src/host/comfort_vignette.h").read_text(encoding="utf-8")
    ipc_bridge = (ROOT / "src/bridge/ipc_bridge.cpp").read_text(encoding="utf-8")
    require("ComfortVignetteStats" in comfort_hdr and
            "AccumulateComfort" in comfort_hdr and
            "FormatComfortVignetteLine" in comfort_hdr and
            "comfort_vignette.h" in host and
            "AccumulateComfort(s_comfort, clientState.comfortVignette" in host and
            "FormatComfortVignetteLine(s_comfort)" in host and
            # The defect this assertion found: WriteState must actually publish
            # the comfort level (it copies hook->host fields by hand).
            "m_pSharedState->comfortVignette = state.comfortVignette;" in ipc_bridge and
            "tests/comfort_vignette_tests.cpp" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8") and
            "comfort_vignette_summary" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8") and
            "COMFORT_VIGNETTE_RE" in xrsim_tool and
            "def parse_comfort" in xrsim_tool and
            "def comfort_vignette_check" in xrsim_tool and
            "comfort_vignette_applied" in xrsim_tool and
            '"comfort_vignette"' in xrsim_tool and
            "test_comfort_vignette_check_asserts_applied_during_locomotion" in _xrsim_tests and
            "test_comfort_vignette_check_skips_without_line_or_locomotion" in _xrsim_tests,
            "GH #68: the locomotion comfort vignette must be published to the host "
            "and asserted applied during scripted movement")

    # GH #75: `scenario_tests.py --check` must be green on main. An observation-only
    # profile (`hands_view`, idle framed hands with no interactive gesture) has no
    # hook marker to assert, so the validator honours an explicit "observe": true
    # opt-out instead of demanding markers -- while a profile with neither is still
    # rejected. Guarded by tools/test_scenario_tests.py and the CI check lane.
    scenario_tool = (ROOT / "tools/scenario_tests.py").read_text(encoding="utf-8")
    hands_view = (ROOT / "tools/scenarios/hands_view.json").read_text(encoding="utf-8")
    scenario_test = (ROOT / "tools/test_scenario_tests.py").read_text(encoding="utf-8")
    require('"observe": true' in hands_view and
            "observe must be a boolean" in scenario_tool and
            "or \"observe\": true for an observation-only profile" in scenario_tool and
            "test_shipped_tree_check_is_green" in scenario_test and
            "test_markerless_profile_without_opt_out_is_rejected" in scenario_test,
            "GH #75: the scenario validator must let an observation-only profile "
            "opt out of the marker requirement and the shipped manifest must pass "
            "--check")

    # GH #73 requirement 3: the large XRSIM eye-capture BMPs are evidence, not
    # source -- they stay untracked (only the diffable report.json is committed).
    # A guard makes a silent re-add loud and the CI lane runs it, so the defect
    # that #73 requirement 1 fixed (`git rm --cached`, 20b6995) cannot recur.
    artifact_guard = ROOT / "tools/verify_test_artifacts.py"
    require(artifact_guard.exists() and
            "git" in artifact_guard.read_text(encoding="utf-8") and
            "ls-files" in artifact_guard.read_text(encoding="utf-8") and
            "tests/xrsim/artifacts/" in (ROOT / ".gitignore").read_text(encoding="utf-8") and
            "tools/verify_test_artifacts.py" in
            (ROOT / ".github/workflows/vr-worker.yml").read_text(encoding="utf-8"),
            "GH #73: XRSIM eye captures must stay untracked, guarded by "
            "tools/verify_test_artifacts.py and run in the CI lane")

    # GH #76 (epic #57 definition-of-done item 1): the mod's gameplay stereo is a
    # *monoscopic* capture blitted to both eyes with one global horizontal shift
    # (stereo_renderer.cpp: "monoscopic capture has no IPD parallax"), so there is
    # no depth-dependent parallax -- yet the old gates only asserted *a* disparity
    # exists and the eyes are not byte-identical, both of which a pure shift
    # satisfies. The harness now measures the *structure* (per-tile disparity
    # spread): `stereo_kind` labels every run and `--require-parallax` grades it.
    # Without this the gap is invisible and "stereo correct" can be claimed for a
    # flat, shifted pair.
    require("def tile_disparities" in xrsim_tool and
            "def classify_stereo_kind" in xrsim_tool and
            "def stereo_kind_stats" in xrsim_tool and
            "PARALLAX_MIN_SPREAD_PX" in xrsim_tool and
            "SHIFT_ONLY_SPREAD_PX" in xrsim_tool and
            "stereo_parallax_present" in xrsim_tool and
            '"--require-parallax"' in xrsim_tool and
            '"stereo_kind"' in xrsim_tool and
            "require_parallax" in xrsim_tool and
            "test_shift_only_pair_is_classified" in _xrsim_tests and
            "test_parallax_pair_is_classified" in _xrsim_tests and
            "test_require_parallax_grades_shift_stereo" in _xrsim_tests and
            "test_scenario_can_require_parallax" in _xrsim_tests,
            "GH #76: XRSIM must measure whether the stereo disparity is one "
            "global shift (monoscopic blit) or depth-varying parallax, report it "
            "as stereo_kind, and grade it with --require-parallax")

    # GH #77 (epic #57 DoD: "all claims backed by captured evidence"): the
    # `eye_frames.log` record presented the host's *synthetic* eye baseline
    # (identity rotation, +/- half-IPD, identical projections -- mock mode never
    # calls xrLocateViews) as "the per-eye view/projection that produced the
    # dumped frame". That is false in every mock/sim run and would let GH #67
    # step 3 be built on matrices that carry no head rotation. The host tags each
    # record's provenance (`eye=runtime|sim|synthetic`) and, in mock/sim,
    # reconstructs the per-eye views a runtime would return for the frame's
    # scripted head pose (ADR-0463); XRSIM measures the view/head rotation
    # deviation and the eye separation, grading a runtime/sim capture and
    # *skipping* a placeholder one.
    eye_frame_h = (ROOT / "src/host/eye_frame.h").read_text(encoding="utf-8")
    openxr_cpp = (ROOT / "src/host/openxr_manager.cpp").read_text(encoding="utf-8")
    require("EyeViewSourceToken" in eye_frame_h and
            "ComputeEyeViewFromHeadPose" in eye_frame_h and
            'return "sim";' in eye_frame_h and
            "m_eyeViewsFromRuntime" in (ROOT / "src/host/openxr_manager.h").read_text(encoding="utf-8") and
            "m_eyeViewsFromRuntime = true;" in openxr_cpp and
            "EyeViewSource::Simulated" in openxr_cpp and
            "ComputeEyeViewFromHeadPose(sample.hmdPos" in openxr_cpp and
            "def _view_head_rotation_deviation_deg" in xrsim_tool and
            "EYE_VIEW_ROTATION_TOL_DEG" in xrsim_tool and
            "EYE_SCENE_FAITHFUL_SOURCES" in xrsim_tool and
            "runtime|sim|synthetic" in xrsim_tool and
            '"eye_views_scene_faithful"' in xrsim_tool and
            "test_eye_frame_provenance_parsed" in _xrsim_tests and
            "test_eye_view_scene_faithful_skips_synthetic" in _xrsim_tests and
            "test_eye_view_scene_faithful_grades_runtime" in _xrsim_tests and
            "test_eye_view_scene_faithful_grades_sim" in _xrsim_tests and
            "ComputeEyeViewFromHeadPose" in
            (ROOT / "tests/eye_frame_tests.cpp").read_text(encoding="utf-8"),
            "GH #77: the XRSIM eye-frame record must tag its provenance "
            "(eye=runtime|sim|synthetic), reconstruct scene-faithful views in "
            "mock/sim, and XRSIM must grade/skip the eye-view scene-faithfulness "
            "check rather than treat the placeholder baseline as a real eye view")

    # GH #78: the XRSIM frame budget must measure the game/hook's rendered-frame
    # cadence, not the harness's own artifact capture. The mock host writes two
    # 26.8 MB BMPs + sidecars every --dump-interval frames *inside the loop whose
    # delta is sampled*, so a ~100 ms capture read as a 90 Hz regression
    # (p50 3.5 ms / p95 104.8 ms) and left the committed report permanently red.
    # The host subtracts the measured capture (pure helper, unit-tested) and
    # logs the excluded cost so the exclusion is visible (ADR-0464).
    require("FrameTimeExcludingHostWork" in frame_time and
            "FrameTimeExcludingHostWork(dtMs, lastHostWorkMs)" in openxr_cpp and
            "ArtifactCapture frame=" in openxr_cpp and
            "lastHostWorkMs = std::chrono::duration<double, std::milli>" in openxr_cpp and
            "FrameTimeExcludingHostWork" in
            (ROOT / "tests/frame_time_tests.cpp").read_text(encoding="utf-8") and
            (ROOT / "docs/adr/0464-frame-budget-excludes-host-capture.md").exists(),
            "GH #78: the XRSIM frame budget must exclude the harness's own "
            "artifact capture from the sampled frame delta and report the "
            "excluded capture cost")

    print("[issue-fixes] source invariants passed (#40, #43-47, #52-54, #58, #61, "
          "#62, #64-68, #69, #71-73, #75-78)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"[issue-fixes] FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
