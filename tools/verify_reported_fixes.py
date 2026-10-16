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

    require("vrState.cinemaMode = false" not in camera,
            "camera update must not override the renderer's cinema decision")
    require("m_sharedState.cinemaMode = isCinema;" in renderer and
            "bool isCinema = !OpenGLProxy::Has3DThisFrame() || s_cursorMenu;" in renderer,
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
            "const bool native = DrawNativeHand(side);" in hands and
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
            "s_viewmodelSuppressor.NoteProjectionLoad()" in proxy and
            "s_viewmodelSuppressor.NoteSwap()" in proxy and
            "ShouldSuppressDrawCall()" in proxy and
            "viewmodel_suppressor_tests.cpp" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"),
            "GH #66: engine viewmodel suppression must be off-by-default, intercept RndrGL draws, and be unit-tested")

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
            "read_fresh_hook_log(LOG_FILE, log_mark)" in xrsim_tool,
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
            "FrameContent::Blank" in (ROOT / "tests/dump_gate_tests.cpp").read_text(encoding="utf-8") and
            "content_gate_stats" in xrsim_tool and
            '"content_gate"' in xrsim_tool and
            "gameplay_window_has_content" in xrsim_tool,
            "GH #72: the XRSIM gameplay window must be content-gated (blank "
            "interop warm-up / frozen repeats excluded) and report why")

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
            "OPENGL32.dll" in (ROOT / "src/hook/dllmain.cpp").read_text(encoding="utf-8") and
            "glPushMatrix" in (ROOT / "src/hook/dllmain.cpp").read_text(encoding="utf-8") and
            "gl_matrix_tracker_tests.cpp" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"),
            "GH #67: the draw-time modelview must be composed (glPushMatrix/"
            "glTranslatef/glPopMatrix mirrored by gl_matrix_tracker.h), not just "
            "the last loaded glLoadMatrixf")

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

    print("[issue-fixes] source invariants passed (#40, #43-47, #52-54, #58, #61, "
          "#62, #64-68, #69, #71-72)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"[issue-fixes] FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
