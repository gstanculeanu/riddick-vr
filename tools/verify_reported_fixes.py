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

    # GH #67 step 3 (live tuning): the probe must be *reachable live* without a
    # deploy. `--ini` writes a hook flag into the read-only shadow's own
    # program-dir riddick_vr.ini -- the file the hook resolves beside its module
    # (vr_config.cpp) -- and the run records the override, so "this run used
    # ViewmodelObserve=1" is attributable without ever writing the game install.
    _shadow_tool = (ROOT / "tools/shadow_install.py").read_text(encoding="utf-8")
    require("def merge_ini" in _shadow_tool and
            "def parse_ini_overrides" in _shadow_tool and
            "def apply_ini_overrides" in _shadow_tool and
            '"--ini"' in xrsim_tool and
            "ini_overrides=" in xrsim_tool and
            '"ini_overrides"' in xrsim_tool and
            "test_merge_ini_replaces_inserts_and_is_idempotent" in
            (ROOT / "tools/test_shadow_install.py").read_text(encoding="utf-8") and
            "test_ini_flag_parses_and_refuses_without_a_shadow" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8"),
            "GH #67 step 3: the viewmodel probe must be reachable live from the "
            "read-only shadow (--ini writes the shadow's riddick_vr.ini), so the "
            "observation can be taken without deploying (ADR-0480)")

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

    # GH #67 step 1 (corrected, ADR-0481): the ARB vertex-program path is the first
    # *candidate* for the live engine's object/view transforms (the fixed-function
    # GL_MODELVIEW is measured to hold one identity load per run, with zero
    # pushes/translates). The capture must intercept the ARB path the engine
    # resolves through wglGetProcAddress, keep the observation off unless
    # ViewmodelObserve=1, and be unit-tested; the vocabulary scan that established
    # it (and then measured it silent) must stay reproducible.
    vp_observer = ROOT / "src/hook/vertex_program_observe.h"
    vp_scan = ROOT / "tools/gl_vocabulary_scan.py"
    require(vp_observer.exists() and
            "class VertexProgramObserver" in vp_observer.read_text(encoding="utf-8") and
            "NoteEnvParameter" in vp_observer.read_text(encoding="utf-8") and
            "Hook_glProgramStringARB" in proxy and
            "Hook_glBindProgramARB" in proxy and
            "Hook_glProgramEnvParameter4fvARB" in proxy and
            "Hook_glProgramEnvParameter4fARB" in proxy and
            '"glProgramStringARB"' in proxy and
            '"glBindProgramARB"' in proxy and
            '"glProgramEnvParameter4fvARB"' in proxy and
            '"glProgramEnvParameter4fARB"' in proxy and
            "s_vertexProgramObserver.Configure(g_vrConfig.viewmodelObserve != 0)" in proxy and
            "ViewmodelProbe program" in proxy and
            "VIEWMODEL_PROGRAM_RE" in xrsim_tool and
            "test_viewmodel_probe_program_lines_are_parsed" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8") and
            "vertex_program_observe.h" in cmake_text and
            "vertex_program_observe" in cmake_text and
            "vertex_program_tracker_tests.cpp" in cmake_text and
            (ROOT / "docs/adr/0481-gh67-arb-vertex-program-transform-source.md").exists() and
            vp_scan.exists() and
            "def scan_names" in vp_scan.read_text(encoding="utf-8") and
            (ROOT / "tools/test_gl_vocabulary_scan.py").exists(),
            "GH #67: the ARB vertex-program path (glProgramStringARB/"
            "glBindProgramARB/glProgramEnvParameter4fvARB via wglGetProcAddress) "
            "must stay instrumented; ADR-0481 measured it silent, so it is a "
            "candidate eliminated by measurement, not the transform source")

    # GH #67 step 1 (second correction, ADR-0482): the transform is applied
    # engine-side to the *submitted vertices*, so the capture must sample the
    # client vertex array at draw time and split array vs display-list draws by
    # the first-person window side. The observer must be off unless
    # ViewmodelObserve=1, the harness must parse the sample, and both must be
    # unit-tested; the import scan that established "no matrix call, no
    # glDrawArrays, client arrays only" must stay reproducible. ADR-0483 sharpens
    # the in-window sample to the window's largest decodable draw (the arms mesh,
    # not the window's first flat 2-D draw) and logs the draw's index count.
    vs_observer = ROOT / "src/hook/vertex_submission_observe.h"
    require(vs_observer.exists() and
            "class VertexSubmissionObserver" in vs_observer.read_text(encoding="utf-8") and
            "SampleClientVertices" in vs_observer.read_text(encoding="utf-8") and
            "NoteVertexPointer" in vs_observer.read_text(encoding="utf-8") and
            "NoteArrayDraw" in vs_observer.read_text(encoding="utf-8") and
            "NoteListDraw" in vs_observer.read_text(encoding="utf-8") and
            "s.count > insideSample_.count" in vs_observer.read_text(encoding="utf-8") and
            "NoteDrawElementsSubmission" in proxy and
            "NoteDrawListSubmission" in proxy and
            "NoteVertexPointer" in proxy and
            "s_vertexSubmissionObserver.Configure(g_vrConfig.viewmodelObserve != 0)" in proxy and
            "ViewmodelProbe submission" in proxy and
            "n=%u count=%u indexType=%u" in proxy and
            "Hook_glVertexPointer" in dllmain and
            '"glVertexPointer"' in dllmain and
            "NoteDrawElementsSubmission(mode, count, type, indices)" in dllmain and
            "VIEWMODEL_SUBMISSION_RE" in xrsim_tool and
            "VIEWMODEL_SUBMISSION_SAMPLE_RE" in xrsim_tool and
            "(?: count=(\\d+))?" in xrsim_tool and
            "test_viewmodel_probe_submission_lines_are_parsed" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8") and
            "viewmodel_submission_parsed" in xrsim_tool and
            "vertex_submission_observe.h" in cmake_text and
            "vertex_submission_observe" in cmake_text and
            "vertex_submission_observe_tests.cpp" in cmake_text and
            (ROOT / "docs/adr/0482-gh67-submitted-vertex-transform-capture.md").exists() and
            (ROOT / "docs/adr/0483-gh67-in-window-mesh-sample.md").exists() and
            "viewmodel_candidate_geometry" in xrsim_tool and
            "VIEWMODEL_CANDIDATE_MIN_THICKNESS_RATIO" in xrsim_tool and
            '"viewmodel_candidate_is_volumetric"' in xrsim_tool and
            "viewmodel_candidate_geometry_planar_vs_volume" in xrsim_tool and
            "test_viewmodel_candidate_geometry_distinguishes_plane_from_mesh" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8") and
            "test_viewmodel_candidate_check_fails_on_a_planar_sample" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8") and
            (ROOT / "docs/adr/0484-gh67-planar-candidate-is-not-the-viewmodel.md").exists() and
            # GH #67 step 1 (fifth correction, ADR-0487): an operational probe
            # whose opener never fired must FAIL, not read as "probe disabled".
            "viewmodel_window_ran" in xrsim_tool and
            "test_viewmodel_probe_check_fails_when_the_opener_never_fires" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8") and
            "viewmodel_opener_never_fired_is_visible" in xrsim_tool and
            "ADR-0487" in (ROOT / "src/hook/README.md").read_text(encoding="utf-8") and
            (ROOT / "docs/adr/0487-gh67-window-opener-does-not-isolate-the-viewmodel.md").exists() and
            # GH #67 step 1 (sixth correction, ADR-0488): the near-plane extraction
            # is the measurement the NearPlaneDrop opener depends on. The engine's
            # z-flipped convention (m[11] > 0) must be handled by a pure, headless
            # helper the hook uses, the raw matrix + both extractions must be
            # logged, and XRSIM must grade the record (a still-rejecting
            # extraction FAILs rather than skipping).
            "ExtractProjectionNear" in
            (ROOT / "src/hook/projection_near.h").read_text(encoding="utf-8") and
            "kProjectionNearUnknown" in
            (ROOT / "src/hook/projection_near.h").read_text(encoding="utf-8") and
            (ROOT / "src/hook/projection_near.h").exists() and
            "ExtractProjectionNear(m)" in proxy and
            "ViewmodelProbe proj" in proxy and
            "classic=%.6f near=%.6f" in proxy and
            "projection_near.h" in cmake_text and
            "projection_near_tests.cpp" in cmake_text and
            "add_test(NAME projection_near" in cmake_text and
            "VIEWMODEL_PROJECTION_RE" in xrsim_tool and
            "viewmodel_projection_stats" in xrsim_tool and
            '"projection_near_extracted"' in xrsim_tool and
            "viewmodel_projection_near_extracted" in xrsim_tool and
            "viewmodel_projection_rejected_load_is_not_known" in xrsim_tool and
            "test_viewmodel_projection_lines_are_parsed" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8") and
            "test_projection_near_check_grades_the_extraction" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8") and
            (ROOT / "docs/adr/0488-gh67-projection-near-convention.md").exists() and
            # GH #67 step 1 (seventh correction, ADR-0489): every sampled interval
            # must be retained (the last one is still the strict gate's sample) and
            # the run must report the volumetric in-window observation separately,
            # so a mesh seen once is neither discarded nor mistaken for an
            # identified pass.
            "viewmodel_candidate_best" in xrsim_tool and
            '"samples": submission_sample_list' in xrsim_tool and
            '"viewmodel_volumetric_candidate_sampled"' in xrsim_tool and
            "viewmodel_candidate_best_is_volumetric" in xrsim_tool and
            "viewmodel_candidate_best_falls_back_to_last" in xrsim_tool and
            "test_viewmodel_volumetric_sample_is_reported_apart_from_the_strict_gate" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8") and
            (ROOT / "docs/adr/0489-gh67-all-sampled-intervals.md").exists() and
            # GH #67 step 1 (eighth correction, ADR-0490): the window-independent
            # near-field candidate. The observer must keep it (window-independent,
            # tagged with whether the window was open), the interval line must carry
            # the `nearSample=` marker that separates a #0490+ hook from a pre-0490
            # one, and XRSIM must parse the `near` label, mirror the near-field
            # constant, and grade the *claim* (coverage of the window's in-window
            # intervals + shape) so a menu-page mesh cannot pass as the arms.
            "kNearFieldMaxAbs" in vs_observer.read_text(encoding="utf-8") and
            "IsNearFieldSample" in vs_observer.read_text(encoding="utf-8") and
            "hasNearSample" in vs_observer.read_text(encoding="utf-8") and
            "nearSampleInside" in vs_observer.read_text(encoding="utf-8") and
            "LogVertexSubmissionCandidate" in proxy and
            "nearSample=%u" in proxy and
            "inside=%u" in proxy and
            "VIEWMODEL_NEAR_FIELD_MAX_ABS" in xrsim_tool and
            "viewmodel_sample_is_near_field" in xrsim_tool and
            "viewmodel_near_field_coverage" in xrsim_tool and
            "(fp|world|near|fpimm)" in xrsim_tool and
            "nearSample=" in xrsim_tool and
            '"viewmodel_near_field_candidate_identifies_pass"' in xrsim_tool and
            "viewmodel_near_field_sample_is_parsed_and_graded" in xrsim_tool and
            "viewmodel_near_field_coverage_is_graded" in xrsim_tool and
            "viewmodel_near_field_bound_is_enforced" in xrsim_tool and
            "test_viewmodel_near_field_sample_is_parsed" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8") and
            "test_viewmodel_near_field_check_grades_the_candidate" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8") and
            "test_viewmodel_near_field_check_skipped_without_a_near_sample" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8") and
            "ADR-0490" in (ROOT / "src/hook/README.md").read_text(encoding="utf-8") and
            (ROOT / "docs/adr/0490-gh67-window-independent-near-field-candidate-falsified.md").exists() and
            # GH #67 step 1 (ninth correction, ADR-0491): the mechanism census. Every
            # prior correction sampled only glDrawElements/glCallList, but the shipped
            # RndrGL.dll *imports* the immediate-mode path (glBegin/glEnd/glVertex3f/
            # glVertex3fv/glVertex2f/glVertex2i), so a mesh submitted with
            # glBegin..glEnd emitted no draw call and could never appear in a sample.
            # The census must be a pure, unit-tested observer, IAT-hooked in RndrGL and
            # forwarded verbatim, logged in the existing `submission <label>:` form
            # (label `fpimm`) so the same planar/volumetric shape gate applies, and
            # graded by XRSIM (a pre-#0491 hook has no census line and is skipped).
            (ROOT / "src/hook/immediate_mode_observe.h").exists() and
            "ImmediateModeObserver" in (ROOT / "src/hook/immediate_mode_observe.h").read_text(encoding="utf-8") and
            "ImmediateBlockSample" in (ROOT / "src/hook/immediate_mode_observe.h").read_text(encoding="utf-8") and
            "NoteImmediateBegin" in dllmain and "Hook_glBegin" in dllmain and
            '"glBegin"' in dllmain and '"glEnd"' in dllmain and
            '"glVertex3f"' in dllmain and '"glVertex3fv"' in dllmain and
            '"glVertex2f"' in dllmain and '"glVertex2i"' in dllmain and
            "real_glVertex3fv" in dllmain and
            "NoteImmediateBegin" in proxy and "NoteImmediateVertex3" in proxy and
            "NoteImmediateVertex2" in proxy and "NoteImmediateEnd" in proxy and
            "ViewmodelProbe immediate" in proxy and
            "ViewmodelProbe submission fpimm" in proxy and
            "immediate_mode_observe.h" in cmake_text and
            "immediate_mode_observe_tests.cpp" in cmake_text and
            "add_test(NAME immediate_mode_observe" in cmake_text and
            "VIEWMODEL_IMMEDIATE_RE" in xrsim_tool and
            "viewmodel_immediate_census" in xrsim_tool and
            '"viewmodel_in_window_immediate_geometry"' in xrsim_tool and
            '"viewmodel_immediate_candidate_is_volumetric"' in xrsim_tool and
            "viewmodel_immediate_census_is_parsed" in xrsim_tool and
            "test_immediate_mode_census_is_parsed_and_graded" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8") and
            "ADR-0491" in (ROOT / "src/hook/README.md").read_text(encoding="utf-8") and
            (ROOT / "docs/adr/0491-gh67-immediate-mode-submission-census.md").exists(),
            "GH #67: the submitted-vertex capture (client array sampled at "
            "glDrawElements, array vs display-list split by the FP window, the "
            "window's largest decodable draw as the arms candidate) must be "
            "inert by default, parsed by XRSIM and unit-tested (ADR-0482/0483), "
            "and the sampled candidate's *shape* must be graded -- a planar "
            "in-window draw is a screen-space element, not the viewmodel "
            "(ADR-0484); the perspective near plane must come from the engine's "
            "own z-flipped convention so the NearPlaneDrop opener is measurable "
            "(ADR-0488)")

    # GH #67 step 1 (tenth correction, ADR-0492): every geometric route is now
    # measured dead, so the remaining route is engine-side. The engine's own
    # first-person viewmodel is an item *property* (`VIEWMODEL`) with an attach
    # configuration (`ATTACHPOINT`/`ATTACHROTTRACK`) on `CRPG_Object_Inventory`
    # -- read-only, from the LIVE (2009) GameClasses module -- not a GL pass. Pin
    # the reference artifact so the finding (module hash, the bit-ordered flag
    # table with `noviewmodel` at bit 1, the three property names and their single
    # code xrefs, the provider-timeout limit) cannot silently rot.
    vm_props = ROOT / "docs/rea/reference/GameClasses2009_ViewmodelProperties.txt"
    _vm_props_text = vm_props.read_text(encoding="utf-8") if vm_props.exists() else ""
    require(vm_props.exists() and
            "d7e81d7d3726d8f75e320c76e237b5fae69a0bea2f64ef729c53e3c1ddd72b79" in _vm_props_text and
            "0x1082d728" in _vm_props_text and "noviewmodel" in _vm_props_text and
            "0x1075a144" in _vm_props_text and "0x1075a150" in _vm_props_text and
            "0x1075a15c" in _vm_props_text and
            "0x10265a92" in _vm_props_text and "0x10265b7a" in _vm_props_text and
            "0x10265b9d" in _vm_props_text and
            "CRPG_Object_Inventory" in _vm_props_text and
            "provider_timeout" in _vm_props_text and
            "FirstPerson" in _vm_props_text and
            (ROOT / "docs/adr/0492-gh67-engine-side-viewmodel-mechanism.md").exists() and
            "ADR-0492" in (ROOT / "src/hook/README.md").read_text(encoding="utf-8"),
            "GH #67: the engine-side identification of the first-person viewmodel "
            "(an item property VIEWMODEL + ATTACHPOINT/ATTACHROTTRACK on "
            "CRPG_Object_Inventory, recorded from the live 2009 GameClasses module) "
            "must stay recorded (ADR-0492)")

    # GH #67 step 1 (eleventh correction, ADR-0493): the ADR-0492 limit was
    # tested and corrected -- the 330 s Ghidra deadline is the *default*
    # (REA_GHIDRA_STARTUP_TIMEOUT_MS), and with it raised the 9.0 MB 2009
    # GameClasses decompiles (exit 0, ~382 s / ~380 s wall); --snapshot is NOT a
    # cross-call cache (the second call still re-imported); and the callee
    # 0x100162e0 is CStr::CStr(const char*), not a "name-keyed helper". Pin the
    # decompiled artifact + the ADR so the correction cannot silently rot.
    vm_dec = ROOT / "docs/rea/reference/GameClasses2009_ViewmodelDecompiled.txt"
    _vm_dec_text = vm_dec.read_text(encoding="utf-8") if vm_dec.exists() else ""
    require(vm_dec.exists() and
            "d7e81d7d3726d8f75e320c76e237b5fae69a0bea2f64ef729c53e3c1ddd72b79" in _vm_dec_text and
            "REA_GHIDRA_STARTUP_TIMEOUT_MS" in _vm_dec_text and
            "1500000" in _vm_dec_text and
            "FUN_10265a70" in _vm_dec_text and
            "CStr::vftable" in _vm_dec_text and
            "ephemeral-source-immutable" in _vm_dec_text and
            (ROOT / "docs/adr/0493-gh67-2009-gameclasses-decompiles-and-the-budget-is-a-deadline.md").exists() and
            "ADR-0493" in (ROOT / "src/hook/README.md").read_text(encoding="utf-8"),
            "GH #67: the 2009 GameClasses module must stay recorded as *decompilable* "
            "once REA_GHIDRA_STARTUP_TIMEOUT_MS is raised above the 330 s default, with "
            "--snapshot measured NOT to be a cross-call cache and 0x100162e0 identified "
            "as CStr::CStr (ADR-0493)")

    # GH #67 step 1 (twelfth correction, ADR-0494): the two questions ADR-0493 named
    # are answered -- the VIEWMODEL site is a READ (FUN_1026d790 is the named-child
    # copy helper, used identically by the item model loader FUN_10260940 for
    # MODEL/MODEL0..3/ATTACHMODEL/...), and the owning class is
    # CRPG_Object_Collectible (vtable 0x10759eb4, slot 8 = 0x10759ed4, RTTI
    # 0x1082d888), NOT CRPG_Object_Inventory. No weapon-class vtable holds the site,
    # so "the player's FP arms are this property" is not supported. Pin the artifact
    # + the ADR so the correction cannot silently rot.
    vm_cons = ROOT / "docs/rea/reference/GameClasses2009_ViewmodelConsumer.txt"
    _vm_cons_text = vm_cons.read_text(encoding="utf-8") if vm_cons.exists() else ""
    require(vm_cons.exists() and
            "d7e81d7d3726d8f75e320c76e237b5fae69a0bea2f64ef729c53e3c1ddd72b79" in _vm_cons_text and
            "CRPG_Object_Collectible" in _vm_cons_text and
            ".?AVCRPG_Object_Collectible@@" in _vm_cons_text and
            "0x10759eb4" in _vm_cons_text and
            "0x10759ed4" in _vm_cons_text and
            "0x1082d888" in _vm_cons_text and
            "FUN_1026d790" in _vm_cons_text and
            "FUN_10260940" in _vm_cons_text and
            "ATTACHMODEL" in _vm_cons_text and
            "PICKUPMSG" in _vm_cons_text and
            "0x1075aa0c" in _vm_cons_text and
            "0x1078957c" in _vm_cons_text and
            (ROOT / "docs/adr/0494-gh67-viewmodel-is-the-collectible-model-load-override.md").exists() and
            "ADR-0494" in (ROOT / "src/hook/README.md").read_text(encoding="utf-8"),
            "GH #67: the VIEWMODEL property must stay recorded as a *read* of the item "
            "definition by CRPG_Object_Collectible's model-load override (slot 8 of "
            "vtable 0x10759eb4), with the base loader FUN_10260940's key list and the "
            "measured absence from every weapon-class vtable (ADR-0494)")

    # GH #67 step 1 (thirteenth correction, ADR-0495): ADR-0494's "Next 1b" --
    # chase the FirstPerson/ThirdPerson "view-mode table" to the FP body/arms --
    # is closed by measurement. The block at 0x1083ff68 ("FirstPerson"/
    # "ThirdPerson"/"KeepEndFade"/"NoSkip") has exactly ONE code reference
    # (`push 0x1083ff68` at 0x105ec068), inside FUN_105ebf40, which is slot 15 of
    # the vtable 0x1078c0e4 whose RTTI is "?AVCWObject_Matinee@@"
    # (.\WObj_Misc\WObj_Matinee.cpp). So the block is a CWObject_Matinee script
    # key, not the player FP body/arms. Pin the artifact + the ADR.
    vm_vmode = ROOT / "docs/rea/reference/GameClasses2009_ViewmodeConsumer.txt"
    _vm_vmode_text = vm_vmode.read_text(encoding="utf-8") if vm_vmode.exists() else ""
    require(vm_vmode.exists() and
            "d7e81d7d3726d8f75e320c76e237b5fae69a0bea2f64ef729c53e3c1ddd72b79" in _vm_vmode_text and
            "0x1083ff68" in _vm_vmode_text and
            "0x1083ff6c" in _vm_vmode_text and
            "0x105ec068" in _vm_vmode_text and
            "0x105ebf40" in _vm_vmode_text and
            "0x1078c0e4" in _vm_vmode_text and
            "0x1078c120" in _vm_vmode_text and
            ".?AVCWObject_Matinee@@" in _vm_vmode_text and
            "WObj_Matinee.cpp" in _vm_vmode_text and
            "FirstPerson" in _vm_vmode_text and
            "ThirdPerson" in _vm_vmode_text and
            (ROOT / "docs/adr/0495-gh67-viewmode-table-is-a-matinee-property-not-the-fp-body.md").exists() and
            "ADR-0495" in (ROOT / "src/hook/README.md").read_text(encoding="utf-8"),
            "GH #67: the FirstPerson/ThirdPerson .data block must stay recorded as a "
            "CWObject_Matinee script key with exactly one code reference (push "
            "0x1083ff68 at 0x105ec068) into FUN_105ebf40 = slot 15 of vtable "
            "0x1078c0e4 (RTTI ?AVCWObject_Matinee@@), so it is NOT chased as the "
            "player FP body/arms (ADR-0495)")

    # GH #67 step 1 (fourteenth correction, ADR-0496): ADR-0494's "Next 1a" --
    # the destination vtable slot 0x68 -- is measured to be a per-class virtual
    # (a census finds 1,319 RTTI vtables / 874 distinct slot-0x68 targets), and on
    # the item class it is a SECOND property loader (CRPG_Object_Item slot 0x68 =
    # FUN_10252d70, which calls the slot-8 loader FUN_10260940 and then loads
    # SPAWN/PROJECTILE/SPAWNEFFECT/SUMMON_PROJECTILECLASS[_PLAYER]/GIVEONPICKUP/
    # MELEE_HITSOUND). So the slot is not the model-instance creation. Pin the
    # artifact + the ADR.
    slot68 = ROOT / "docs/rea/reference/GameClasses2009_ModelSetterSlot68.txt"
    _slot68_text = slot68.read_text(encoding="utf-8") if slot68.exists() else ""
    require(slot68.exists() and
            "d7e81d7d3726d8f75e320c76e237b5fae69a0bea2f64ef729c53e3c1ddd72b79" in _slot68_text and
            "0x10252d70" in _slot68_text and
            "FUN_10260940" in _slot68_text and
            "SPAWNEFFECT" in _slot68_text and
            "SUMMON_PROJECTILECLASS_PLAYER" in _slot68_text and
            "GIVEONPICKUP" in _slot68_text and
            "MELEE_HITSOUND" in _slot68_text and
            "0x104b17d0" in _slot68_text and
            (ROOT / "docs/adr/0496-gh67-slot68-is-a-per-class-virtual-not-the-model-instance-creation.md").exists() and
            "ADR-0496" in (ROOT / "src/hook/README.md").read_text(encoding="utf-8"),
            "GH #67: the item-model destination vtable[0x68] must stay recorded as a "
            "per-class virtual (874 distinct targets) that on CRPG_Object_Item is a "
            "second property loader FUN_10252d70, NOT the model-instance creation "
            "(ADR-0496)")

    # GH #71/#80 (ADR-0497): a scripted run reached live gameplay only once the
    # harness activated the game's own top-level window before posting the
    # confirm taps -- the engine's message pump is focus-gated, so "posted
    # messages are dead" was an artefact of an inactive window. Pin the mechanism,
    # the report key and the ADR so the reach fix cannot silently regress.
    _xrsim = (ROOT / "tools/xrsim.py").read_text(encoding="utf-8")
    require("def activate_game_window(" in _xrsim and
            "WINDOW_ACTIVATION_TIMEOUT_S" in _xrsim and
            "activate_game_window(game.pid)" in _xrsim and
            '"window_activation": window_stats' in _xrsim and
            (ROOT / "docs/adr/0497-xrsim-activates-the-game-window-to-reach-gameplay.md").exists() and
            "ADR-0497" in (ROOT / "docs/vr-scenarios.md").read_text(encoding="utf-8"),
            "GH #71/#80: XRSIM must activate the game window before the "
            "posted-confirm taps (the engine's pump is focus-gated) and report "
            "window_activation (ADR-0497)")

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

    # GH #71 requirement 1 (input-blocker DIAGNOSIS, corrected in wake 84): in
    # simulator mode the hook never injects desktop input, so nothing advances the
    # game's front-end start card. The harness carries the *sim-safe* attempt -- a
    # targeted window-message confirm tap to the game's own HWND (never OS-global
    # input) -- OPT-IN, and it packs the key's real scan code, which is what
    # RndrGL.dll's window proc reads from lParam (`lParam >> 16`); `lParam = 1`
    # forwarded key code 0 and looked like "the engine ignores posted messages".
    # See ADR-0485 and docs/rea/reference/RndrGL_Input_Path.txt. The schedule is
    # pure/unit-tested and its outcome is recorded in the report as `menu_advance`.
    require("class GameMenuAdvancer" in xrsim_tool and
            "def post_confirm_to_game_windows" in xrsim_tool and
            "def confirm_key_messages" in xrsim_tool and
            "def vk_to_scancode" in xrsim_tool and
            "KEY_SCANCODE_FALLBACK" in xrsim_tool and
            "KEY_LPARAM_NO_SCANCODE = 0x100" in xrsim_tool and
            "def gameplay_reached_from_sequence" in xrsim_tool and
            "def advance_stop_decision" in xrsim_tool and
            "ADVANCE_STOP_MIN_TAPS = 8" in xrsim_tool and
            '"--advance-min-taps"' in xrsim_tool and
            '"--advance-menu"' in xrsim_tool and
            "set_defaults(advance_menu=False)" in xrsim_tool and
            '"menu_advance": menu_stats' in xrsim_tool and
            "test_menu_advancer_fires_on_cadence_and_honours_max_taps" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8") and
            "test_confirm_scancode_fallback_is_pure" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8") and
            "test_advance_stop_waits_for_delivered_taps" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8") and
            (ROOT / "docs/adr/0485-gh71-confirm-tap-scan-code-and-live-level-reach.md").exists() and
            (ROOT / "docs/adr/0486-gh71-advance-stop-tap-floor.md").exists() and
            (ROOT / "docs/rea/reference/RndrGL_Input_Path.txt").exists(),
            "GH #71 req 1: XRSIM must carry the sim-safe (opt-in) menu-advance "
            "attempt, pack the confirm key's real scan code (lParam >> 16), and "
            "record its outcome; see ADR-0485")

    # GH #71 requirement 1 (second input-free route, measured): the engine's
    # console surface (loadgame/changemap/cg_newgame in the Content game DLLs) is
    # real and savegames exist, but the engine does NOT execute `+`-prefixed
    # console commands from its command line (nor a COMMANDLINE env override), so
    # `--game-arg` alone cannot reach gameplay. The harness keeps the repeatable
    # `--game-arg` pass-through as the exact handle for the hook-side sim bridge
    # (ADR-0466 route (a)); `game_command` is pure/unit-tested and the tokens are
    # recorded in the report as `game_args`.
    require("def game_command" in xrsim_tool and
            '"--game-arg"' in xrsim_tool and
            'action="append"' in xrsim_tool and
            'game_command(game_exe, getattr(args, "game_arg", None))' in xrsim_tool and
            '"game_args": list(getattr(args, "game_arg", None) or [])' in xrsim_tool and
            "test_game_command_passes_tokens_verbatim" in
            (ROOT / "tools/test_xrsim.py").read_text(encoding="utf-8"),
            "GH #71 req 1: XRSIM must expose a repeatable, unit-tested --game-arg "
            "pass-through (the handle for the hook-side sim input bridge) and "
            "record the tokens in the report; the engine ignores +console commands")

    # GH #72 requirement 2: the gameplay-only dump window must also be
    # *content-gated* -- a gameplay-labelled frame is skipped while it is
    # all-black (GL<->D3D11 interop warm-up) or byte-identical to the last
    # dumped frame (a frozen static 2D page), so the graded window is live,
    # advancing 3D gameplay. The verdict is a pure FrameContent helper,
    # unit-tested, and surfaced in the harness report.
    dump_gate = (ROOT / "src/host/dump_gate.h").read_text(encoding="utf-8")
    require("FrameContent" in dump_gate and
            "FrameContent::Live" in dump_gate and
            # GH #79: the probe samples the private staged copy of both eyes
            # (the pure sampler), not the shared textures directly.
            "StageEyePair" in host and
            "SampleEyeImage" in host and
            "kBlankMeanLuma" in host and
            "RiddickVR::FrameContent::Frozen" in host and
            "DumpGate skip frame=" in host and
            "ShouldDumpFrame(dumpDue, m_dumpGameplayOnly, stateRead, cinemaMode, content)" in host and
            # GH #72 (both-eye rule): a frame with an empty eye is not gradable
            # (it reads as a spurious stereo failure), so the content probe
            # samples both eyes.
            "EyePairIsBlank" in dump_gate and
            "EyePairIsBlank(left.meanLuma, true, right.meanLuma, true," in host and
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
    # Default ON since the 2026-10-18 live A/B (ADR-0478): a live run demoted the
    # letterboxed startup page and still reached full-bleed gameplay.
    display_branch = (ROOT / "src/hook/display_branch.h").read_text(encoding="utf-8")
    config_header = (ROOT / "src/hook/vr_config.h").read_text(encoding="utf-8")
    config_source = (ROOT / "src/hook/vr_config.cpp").read_text(encoding="utf-8")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    require("ComputeDisplayBranch" in display_branch and
            "DisplayBranchConfig" in display_branch and
            "coldStartLatch" in display_branch and
            "bool coldStartLatch = true;" in display_branch and
            "fullBleedSeen" in display_branch and
            "ComputeDisplayBranch(" in renderer and
            "cold-start latch demoted" in renderer and
            "displayBranchColdStartLatch" in config_header and
            "int displayBranchColdStartLatch = 1;" in config_header and
            'ReadInt(iniPath, "DisplayBranchColdStartLatch"' in config_source and
            "DisplayBranchColdStartLatch=1" in
            (ROOT / "riddick_vr.ini").read_text(encoding="utf-8") and
            "tests/display_branch_tests.cpp" in cmake and
            "display_branch_cold_start_latch" in cmake,
            "GH #72: the display branch must demote a letterboxed startup page "
            "behind a cold-start latch (default on, ADR-0478) driven by the "
            "BarScan fill, and report the demotion it makes")

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

    # GH #76 (interim depth-based reprojection, ADR-0501): the issue's option 2 --
    # keep the single capture but make each region's horizontal shift depend on
    # the scene depth -- is implemented behind an opt-in flag with a safe
    # fallback, and its depth source is measured. The guard pins the pure warp
    # math, the renderer wiring (grid blits + the engine-FBO depth source), the
    # config keys, the headless test, and the ADR, so the interim cannot silently
    # disappear or be reverted to a single global shift without a red.
    depth_parallax_h = (ROOT / "src/hook/depth_parallax.h").read_text(encoding="utf-8")
    stereo_renderer_cpp = (ROOT / "src/hook/stereo_renderer.cpp").read_text(encoding="utf-8")
    opengl_proxy_h = (ROOT / "src/hook/opengl_proxy.h").read_text(encoding="utf-8")
    opengl_proxy_cpp = (ROOT / "src/hook/opengl_proxy.cpp").read_text(encoding="utf-8")
    vr_config_h = (ROOT / "src/hook/vr_config.h").read_text(encoding="utf-8")
    require("inline float DepthToViewZ" in depth_parallax_h and
            "inline float ParallaxShiftPx" in depth_parallax_h and
            "inline float EyeBaselineMeters" in depth_parallax_h and
            "inline float CellMinViewZ" in depth_parallax_h and
            "kMaxParallaxShiftPx" in depth_parallax_h and
            "BuildDibrGrid" in stereo_renderer_cpp and
            "s_dibrGrid.fullDispPx" in stereo_renderer_cpp and
            "GL_DEPTH_COMPONENT" in stereo_renderer_cpp and
            "GetLastBoundFramebuffer" in stereo_renderer_cpp and
            "GetLastBoundFramebuffer" in opengl_proxy_h and
            "s_lastBoundFramebuffer" in opengl_proxy_cpp and
            "stereoParallaxDibr" in vr_config_h and
            "dibrStrength" in vr_config_h and
            "tests/depth_parallax_tests.cpp" in
            (ROOT / "CMakeLists.txt").read_text(encoding="utf-8") and
            (ROOT / "tests/depth_parallax_tests.cpp").exists() and
            (ROOT / "docs/adr/0501-gh76-interim-depth-reprojection-and-its-depth-source.md").exists(),
            "GH #76 / ADR-0501: the interim depth-based per-eye reprojection must "
            "exist (pure warp math + the depth-varying grid blits + the "
            "engine-FBO depth source), be opt-in via StereoParallaxDibr, and stay "
            "covered by the headless depth_parallax test")

    # GH #76 / ADR-0502: the wake-99 "capture the world-pass depth at the
    # depth-only clear" route is implemented (the capture instrument survives) and
    # its no-regression guarantee is explicit -- a present-but-far depth must
    # never reduce the eye separation below the legacy half-IPD cue. The guard
    # pins the mechanism (the depth-only-clear hook, the current-draw-framebuffer
    # tracking, the cached snapshot, the evidence line) and the `minSep` gate, so
    # neither can be silently removed without a red.
    require("OnDepthOnlyClear" in stereo_renderer_cpp and
            "Instance().OnDepthOnlyClear" in opengl_proxy_cpp and
            "GetCurrentDrawFramebuffer" in opengl_proxy_h and
            "GetCurrentDrawFramebuffer" in opengl_proxy_cpp and
            "s_currentDrawFramebuffer" in opengl_proxy_cpp and
            "s_dibrCapture" in stereo_renderer_cpp and
            "DIBR world-depth capture" in stereo_renderer_cpp and
            "minSepPx" in stereo_renderer_cpp and
            (ROOT / "docs/adr/0502-gh76-depth-only-clear-depth-is-a-far-pass.md").exists(),
            "GH #76 / ADR-0502: the depth-only-clear depth capture and its "
            "no-regression `minSep` gate must exist (the depth-only clear hooks "
            "OnDepthOnlyClear, the current draw framebuffer is tracked, the "
            "snapshot is cached and logged, and a warp below the legacy half-IPD "
            "separation falls back)")

    # GH #76 / ADR-0503: ADR-0502's Next 1 ("find the buffer that actually holds
    # the near world depth") is measured -- no framebuffer object the hook can read
    # holds the visible world pass, so the interim has no usable depth source on
    # this title. The guard pins the instrument that produced that measurement (the
    # engine-FBO id recorder, the per-FBO depth scan run at the depth-only clear,
    # and its evidence lines) plus the ADR, so the negative cannot be asserted
    # without the mechanism behind it, and it pins that the scan is
    # evidence-gathering only (it must not rewrite the snapshot).
    require("Hook_glGenFramebuffers" in opengl_proxy_h and
            "GetKnownFramebufferIds" in opengl_proxy_h and
            "real_glGenFramebuffers" in opengl_proxy_h and
            "Hook_glGenFramebuffers" in opengl_proxy_cpp and
            "s_knownFramebuffers" in opengl_proxy_cpp and
            "GetKnownFramebufferIds" in opengl_proxy_cpp and
            "ScanFramebufferDepths" in
            (ROOT / "src/hook/stereo_renderer.h").read_text(encoding="utf-8") and
            "ScanFramebufferDepths" in stereo_renderer_cpp and
            "DIBR fbo scan" in stereo_renderer_cpp and
            "kDibrNearDepthMax" in stereo_renderer_cpp and
            "kDibrFboScanEveryClears" in stereo_renderer_cpp and
            (ROOT / "docs/adr/0503-gh76-no-readable-near-world-depth-buffer.md").exists(),
            "GH #76 / ADR-0503: the FBO depth scan (engine FBO ids recorded by "
            "Hook_glGenFramebuffers, a per-FBO depth read at the depth-only clear, "
            "its evidence lines) must exist as the instrument behind the measured "
            "negative 'no readable FBO holds the near world pass'")

    # GH #76 / ADR-0504: the interim depth route is closed (ADR-0501/0502/0503),
    # so option 1 opens. The engine submits *eye-space* client-array geometry with
    # an identity modelview (ADR-0481/0482), so a per-eye camera is a pure
    # modelview translation: the per-eye replay maths must exist (headless,
    # tested), the scene-faithfulness gate must refuse the mock host's placeholder
    # per-eye baseline (GH #77 / ADR-0463) instead of trusting it, and the live
    # provenance probe must stay opt-in (evidence only -- no render change).
    stereo_replay_h = (ROOT / "src/hook/stereo_replay.h").read_text(encoding="utf-8")
    require("inline void BuildEyeReplayModelview" in stereo_replay_h and
            "inline bool EyeSpaceCameraOffsetFromViews" in stereo_replay_h and
            "kReplayMaxEyeOffsetMeters" in stereo_replay_h and
            "ReplayViewRotationMatchesHead" in stereo_replay_h and
            "inline float ReplayViewResidual" in stereo_replay_h and
            "stereo_replay.h" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8") and
            (ROOT / "tests/stereo_replay_tests.cpp").exists() and
            "stereo_replay" in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8") and
            "StereoReplayProbe" in (ROOT / "src/hook/vr_config.cpp").read_text(encoding="utf-8") and
            "stereoReplayProbe" in (ROOT / "src/hook/vr_config.h").read_text(encoding="utf-8") and
            '"StereoReplay: swap=' in stereo_renderer_cpp and
            "srcL=" in stereo_renderer_cpp and
            "stereoReplayProbe" in stereo_renderer_cpp and
            (ROOT / "docs/adr/0504-gh76-option1-eye-space-replay-mechanism.md").exists(),
            "GH #76 / ADR-0504: the option-1 per-eye replay maths (stereo_replay.h "
            "+ its headless test), the scene-faithfulness gate that refuses the "
            "mock host's placeholder per-eye baseline, and the opt-in live "
            "provenance probe must exist -- option 1 rests on them")

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

    # GH #79: the XRSIM capture must read each shared eye texture ONCE, as a
    # back-to-back pair, and grade/save from that private copy. The old path
    # sampled both eyes (`SampleEyeContent`, its own staging copy each) and then
    # re-read both textures in `SaveTextureToBMP`, so the hook -- which blits
    # both eyes on every swap and never waits on the consumed event -- could
    # overwrite the shared textures between the gate probe and the save
    # (measured: frame 540, a lit probe with an all-black saved right eye;
    # frame 1770, an intra-image tear). The hook-side wait is deploy-gated; the
    # host half is the single staged pair (ADR-0465).
    eye_sample_hdr = (ROOT / "src/host/eye_sample.h").read_text(encoding="utf-8")
    bmp_writer_hdr = (ROOT / "src/host/bmp_writer.h").read_text(encoding="utf-8")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    require("SampleEyeImage" in eye_sample_hdr and
            "MakeBmpHeader" in bmp_writer_hdr and
            "ConvertRgbaRowToBgra" in bmp_writer_hdr and
            "StageEyePair" in openxr_cpp and
            "context->CopyResource(out.leftTex, sharedL);" in openxr_cpp and
            "context->CopyResource(out.rightTex, sharedR);" in openxr_cpp and
            "WriteBmpFromMapped" in openxr_cpp and
            "SampleEyeContent(" not in openxr_cpp and
            "SaveTextureToBMP(" not in openxr_cpp and
            "SaveTextureToBMP" not in
            (ROOT / "src/host/openxr_manager.h").read_text(encoding="utf-8") and
            "tests/eye_capture_tests.cpp" in cmake and
            "eye_capture_helpers" in cmake and
            (ROOT / "docs/adr/0465-eye-capture-single-staged-pair.md").exists(),
            "GH #79: the XRSIM capture must stage both eyes back-to-back once "
            "and sample/save from that private copy, not re-read the shared "
            "textures (the hook half is deploy-gated)")

    # GH #79 host-half capstone (ADR-0469): the host must *record* the gate's own
    # sample and the hook-publish counter either side of the staging copies, and
    # the harness must assert the saved BMP matches the gate sample. This is the
    # machine-checked form of GH #79's XRSIM acceptance ("the gate sample and the
    # saved BMP must agree") -- deploy-free, so a regression to two reads is
    # caught without the hook half.
    require("GateSample frame=" in openxr_cpp and
            "clientFrameCount" in openxr_cpp and
            "straddled=" in openxr_cpp and
            "hookFramesBefore" in openxr_cpp and
            "def parse_gate_samples" in xrsim_tool and
            "def capture_gate_check" in xrsim_tool and
            "def saved_left_lumas" in xrsim_tool and
            "capture_gate_matches_saved" in xrsim_tool and
            '"capture_gate"' in xrsim_tool and
            "test_capture_gate_matches_saved_passes_when_agreeing" in _xrsim_tests and
            "test_capture_gate_fails_on_saved_mismatch" in _xrsim_tests and
            (ROOT / "docs/adr/0469-capture-gate-integrity-record.md").exists(),
            "GH #79: the host must record the capture gate's sample + the "
            "hook-publish straddle, and XRSIM must assert the saved BMP matches "
            "the gate sample (the deploy-free half of #79's acceptance)")

    # GH #80: the simulator-only input bridge. Both input-free routes to a live
    # scene are measured dead (posted window messages -- ADR-0466; the engine's
    # command line -- its wake-64 addendum), so the sim confirm must be
    # fabricated in-process on the engine's own device read path. The install
    # point is *not* the hook's deferred init: MSystem.dll resolves
    # XInputGetState / DirectInputCreateA once, inside
    # CInputContext_DInput's constructor (0x1010b440), so the intercept must
    # precede that constructor (ADR-0467). This lands the pure core -- the
    # two-token enable gate, the bounded tap cadence, and the DirectInput /
    # XInput state encodings -- with headless tests; the interception is next.
    sim_bridge_hdr = (ROOT / "src/hook/sim_input_bridge.h").read_text(encoding="utf-8")
    sim_bridge_cpp = (ROOT / "src/hook/sim_input_bridge_win32.cpp").read_text(encoding="utf-8")
    dllmain = (ROOT / "src/hook/dllmain.cpp").read_text(encoding="utf-8")
    require("SimInputBridgeEnabled" in sim_bridge_hdr and
            "SimConfirmRoute" in sim_bridge_hdr and
            "SimConfirmTick" in sim_bridge_hdr and
            "SimConfirmPeriod" in sim_bridge_hdr and
            "SimBridgeOwnsProcName" in sim_bridge_hdr and
            "kDirectInputKeyboardBytes" in sim_bridge_hdr and
            "SetDirectInputKey" in sim_bridge_hdr and
            "SimBridgeIsKeyboardGuid" in sim_bridge_hdr and
            "ApplySimConfirmToGamepad" in sim_bridge_hdr and
            "BumpSimPacketNumber" in sim_bridge_hdr and
            "RIDDICKVR_SIM_CONFIRM" in sim_bridge_hdr and
            "SimRouteFromToken" in sim_bridge_hdr and
            "SimConfirmScheduleFromTokens" in sim_bridge_hdr and
            "kDirectInputCreateDeviceSlot = 3" in sim_bridge_hdr and
            "kDirectInputDeviceGetDeviceStateSlot = 9" in sim_bridge_hdr and
            # GH #80 req 1 (wake 66): the interceptor itself -- the early
            # GetProcAddress IAT swap on MSystem.dll and the DirectInput/XInput
            # wrappers -- installed from DllMain (ADR-0467/0468).
            "KERNEL32.dll\", \"GetProcAddress" in sim_bridge_cpp and
            "Hook_GetProcAddress" in sim_bridge_cpp and
            "Hook_XInputGetState" in sim_bridge_cpp and
            "Hook_DirectInputCreateA" in sim_bridge_cpp and
            "kDirectInputCreateDeviceSlot" in sim_bridge_cpp and
            "kDirectInputDeviceGetDeviceStateSlot" in sim_bridge_cpp and
            "SimInputBridgeInstallOnAttach" in sim_bridge_cpp and
            "SimInputBridgeInstallOnAttach();" in dllmain and
            "SimInputBridgeLogStartupReport();" in dllmain and
            "src/hook/sim_input_bridge_win32.cpp" in cmake and
            "sim_confirm_env" in
            (ROOT / "tools/xrsim.py").read_text(encoding="utf-8") and
            "--sim-confirm" in
            (ROOT / "tools/xrsim.py").read_text(encoding="utf-8") and
            "tests/sim_input_bridge_tests.cpp" in cmake and
            "sim_input_bridge_pure_core" in cmake and
            # GH #80 req 1 (wake 67): the interceptor is now *executed* offline.
            # The bridge installs through a module-parameterized seam, and a
            # headless test installs it on its own module and drives the engine's
            # exact resolution path -- gate-on interception (GetProcAddress ->
            # wrapper -> injected XInput/DirectInput state) plus a gate-off
            # inertness control. No game, no deploy.
            "SimInputBridgeInstallOnModule" in
            (ROOT / "src/hook/sim_input_bridge_win32.h").read_text(encoding="utf-8") and
            "SimInputBridgeInstallOnModule" in sim_bridge_cpp and
            "tests/sim_input_bridge_win32_tests.cpp" in cmake and
            "sim_input_bridge_win32_intercept" in cmake and
            "sim_input_bridge_win32_inert" in cmake and
            "0x1010b440" in
            (ROOT / "docs/adr/0467-sim-input-bridge-install-point.md").read_text(encoding="utf-8") and
            (ROOT / "docs/adr/0468-sim-input-bridge-interception-mechanism.md").exists(),
            "GH #80: the simulator-only input bridge's pure core (two-token "
            "enable gate, bounded tap cadence, DirectInput/XInput state "
            "encodings), its install point (the input context's constructor, "
            "0x1010b440), and the interceptor (the early GetProcAddress IAT swap "
            "on MSystem.dll + the DirectInput/XInput wrappers wired from DllMain) "
            "must exist, be unit-tested, and be recorded")

    # GH #81: a live XRSIM run must identify the *hook build* it exercised. The
    # game loads the deployed `<game-dir>\opengl32.dll`, not the repo build the
    # mock host is launched from (measured 2026-10-18: deployed 479232 B
    # sha256 8514d35a…@2026-10-17T12:47 vs dist build 488448 B sha256 14867cf3…),
    # so the loop's live results were grading an ~11 h older hook than the tree
    # they were reported against -- which makes a deploy-gated claim (#71/#72/
    # #76/#79/#80) impossible to attribute. The identity is recorded (path/
    # sha256/bytes/mtime) and the graded form is opt-in (`--require-fresh-hook`),
    # so a normal deploy-gated run stays green while a deploy can be asserted.
    require("def file_identity" in xrsim_tool and
            "def deployed_hook_identity" in xrsim_tool and
            "def format_hook_identity" in xrsim_tool and
            "def deployed_hook_check" in xrsim_tool and
            "deployed_hook_matches_build" in xrsim_tool and
            "--require-fresh-hook" in xrsim_tool and
            '"deployed_hook"' in xrsim_tool and
            "hook under test: " in xrsim_tool and
            "DEPLOYED_HOOK_NAME" in xrsim_tool and
            "test_file_identity_reports_hash_and_tracks_change" in _xrsim_tests and
            "test_deployed_hook_identity_matches_and_differs" in _xrsim_tests and
            "test_deployed_hook_check_skips_by_default_and_grades_on_request"
            in _xrsim_tests and
            "test_evaluate_records_hook_identity_and_grades_only_when_asked"
            in _xrsim_tests and
            (ROOT / "docs/adr/0470-xrsim-hook-build-identity.md").exists(),
            "GH #81: a live XRSIM run must record the hook build under test "
            "(deployed `<game-dir>\\opengl32.dll` vs the repo build) and provide "
            "the opt-in `--require-fresh-hook` gate so a deploy-gated result "
            "cannot be attributed to the wrong build")

    # GH #81 follow-up (wake 70): identifying the hook is not enough -- the loop
    # still could not make a live run exercise current source without the
    # forbidden deploy. The read-only *shadow install* mirrors a real install
    # (junctions for the bulk, real copies for the program directory) and drops
    # the repo build into the hook slot, so `--require-fresh-hook` is now
    # satisfiable offline and nothing is written to the install.
    shadow_tool = (ROOT / "tools/shadow_install.py").read_text(encoding="utf-8")
    shadow_tests = (ROOT / "tools/test_shadow_install.py").read_text(encoding="utf-8")
    require("def shadow_plan" in shadow_tool and
            "def build_shadow" in shadow_tool and
            "def shadow_manifest" in shadow_tool and
            "def format_shadow" in shadow_tool and
            "mklink" in shadow_tool and
            "opengl32.dll" in shadow_tool and
            "--shadow-install" in xrsim_tool and
            "--shadow-dir" in xrsim_tool and
            '"shadow"' in xrsim_tool and
            "test_build_shadow_routes_hook_and_mirrors_assets" in shadow_tests and
            "test_build_is_idempotent_and_never_writes_the_source" in shadow_tests and
            "test_manifest_flags_a_stale_hook" in shadow_tests and
            (ROOT / "docs/adr/0471-xrsim-shadow-install-read-only-hook-"
                    "validation.md").exists(),
            "GH #81: a live XRSIM run must be able to exercise the current hook "
            "build without deploying -- the read-only shadow install (junctions + "
            "real program dir + the repo hook) and its `--shadow-install` route "
            "must exist, be unit-tested, and be recorded")

    # GH #71 req 1 / #80 follow-up (wake 71): the input bridge must *report what
    # it observed*, so the reach question is decidable from a run instead of
    # inferred from prose. The bridge names every input proc the engine resolved
    # through the swapped GetProcAddress (owned or merely observed), counts its
    # polls and the confirms it fabricated, and records the real XInput API's
    # return code; the harness parses those lines into the `input_bridge` report
    # key and prints one summary line. A killed game (the harness's
    # TerminateProcess) skips DLL_PROCESS_DETACH, so the summary is also emitted
    # on a poll cadence from the wrappers themselves -- and once from the
    # deferred-init report, so a poll whose own line preceded the logger is still
    # reported by value (`SimShouldLogPoll` is the doubling cadence that keeps a
    # small count visible).
    require("SimInputProcNameIndex" in sim_bridge_hdr and
            "SimBridgeIsInputProcName" in sim_bridge_hdr and
            "SimInputProcNameAt" in sim_bridge_hdr and
            "SimShouldLogPoll" in sim_bridge_hdr and
            "SimShouldLogPoll" in sim_bridge_cpp and
            "SimInputBridgeLogSummary" in
            (ROOT / "src/hook/sim_input_bridge_win32.h").read_text(encoding="utf-8") and
            "SimInputBridgeLogSummary" in sim_bridge_cpp and
            "SimInputBridgeLogSummary();" in dllmain and
            "engine requested" in sim_bridge_cpp and
            "XInputGetState first poll" in sim_bridge_cpp and
            "summary route=" in sim_bridge_cpp and
            "def parse_input_bridge" in xrsim_tool and
            "def input_bridge_stats" in xrsim_tool and
            '"input_bridge"' in xrsim_tool and
            "test_parse_input_bridge_is_none_without_lines" in _xrsim_tests and
            "test_parse_input_bridge_reads_gate_procs_and_summary" in _xrsim_tests and
            "test_input_bridge_report_key_written_by_evaluate_only" in _xrsim_tests and
            "TestInputProcNames" in
            (ROOT / "tests/sim_input_bridge_tests.cpp").read_text(encoding="utf-8") and
            "TestPollLogCadence" in
            (ROOT / "tests/sim_input_bridge_tests.cpp").read_text(encoding="utf-8") and
            (ROOT / "docs/adr/0472-sim-input-bridge-observability.md").exists(),
            "GH #71 req 1 / #80: the sim input bridge must record the input procs "
            "the engine resolved, its poll/tap counts and the real API's return "
            "code, and the harness must parse them into the `input_bridge` report "
            "key (the datum that makes the reach decidable)")

    # GH #83: the autoloop must not run two agents in one checkout. The runner
    # guards on *liveness* -- a wake whose runner was killed leaves its
    # `opencode run` child working the same tree while the runner's mutex is free
    # -- records one attributable run id per wake, and the live harness takes a
    # single-live-run lock plus a per-run shadow directory so a parallel or
    # leftover run can never rebuild another run's tree or race the shared
    # `dist\` binaries. The guard's pure decisions have their own self-test
    # (`loop_guard.ps1 -SelfTest`), the harness lock has unit tests, and the
    # harness lock is also exercised by `xrsim --selftest`.
    loop_guard = (ROOT / "tools/autoloop/loop_guard.ps1").read_text(encoding="utf-8")
    runner = (ROOT / "tools/autoloop/run-autoloop.ps1").read_text(encoding="utf-8")
    require("function Test-LiveWakeProcess" in loop_guard and
            "function Get-AutoloopProcesses" in loop_guard and
            "function Get-AutoloopRunId" in loop_guard and
            "function Write-AutoloopLock" in loop_guard and
            "function Read-AutoloopLock" in loop_guard and
            "function Remove-AutoloopLock" in loop_guard and
            "Test-AutoloopGuard" in loop_guard and
            "loop_guard.ps1" in runner and
            "Test-LiveWakeProcess" in runner and
            "Get-AutoloopProcesses" in runner and
            "Write-AutoloopLock" in runner and
            "Remove-AutoloopLock" in runner and
            "start run id=" in runner and
            "def default_shadow_dir" in xrsim_tool and
            "def live_lock_decision" in xrsim_tool and
            "def acquire_live_lock" in xrsim_tool and
            "def release_live_lock" in xrsim_tool and
            "--ignore-live-lock" in xrsim_tool and
            '"live_lock"' in xrsim_tool and
            "live_lock_round_trip" in xrsim_tool and
            "shadow_dir_is_per_run" in xrsim_tool and
            "test_default_shadow_dir_is_per_run" in _xrsim_tests and
            "test_live_lock_refuses_while_held_and_takes_over_stale" in _xrsim_tests and
            (ROOT / "docs/adr/0475-autoloop-single-instance-guard.md").exists(),
            "GH #83: the autoloop runner must refuse to start a second agent in "
            "one checkout (liveness scan over live processes + an attributable "
            "run id) and the live harness must take a single-live-run lock and a "
            "per-run shadow directory, both unit-tested")

    # GH #85: the 2009 *Assault on Dark Athena* models store a version-0x205
    # VERTEXFRAMES frame -- the 2004 52-byte header + quantized u16 positions +
    # s8 normals, then **two more s8 unit-vector arrays** (tangent + bitangent),
    # i.e. 52 + 15n bytes and no leading frame_count word. Undecoded, the corpus
    # check on the target install reported 4566 problems and the whole 2009
    # geometry (the native-hand mesh path, GH #55) was unreadable.
    mos_xmd = (ROOT / "tools/mos_xmd.py").read_text(encoding="utf-8")
    mos_xmd_tests = (ROOT / "tools/test_mos_xmd.py").read_text(encoding="utf-8")
    require("VF205_VERSION = 0x205" in mos_xmd and
            "VF205_EXTRA_BASIS = 2" in mos_xmd and
            "0x200, 0x201, 0x202, 0x203, 0x204, 0x205" in mos_xmd and
            "_decode_i8_unit_vectors" in mos_xmd and
            "extra_basis" in mos_xmd and
            "test_geometry_roundtrip_205" in mos_xmd_tests and
            "test_geometry_204_has_no_extra_basis" in mos_xmd_tests and
            "test_geometry_205_truncated_basis_is_rejected" in mos_xmd_tests and
            (ROOT / "docs/adr/0476-xmd-vertex-frame-205-layout.md").exists(),
            "GH #85: the 2009 VERTEXFRAMES version 0x205 (52-byte header + u16 "
            "positions + s8 normals + two s8 unit-vector arrays, no leading frame "
            "count) must be decoded drop-in compatible with 0x204, unit-tested "
            "and recorded")

    # GH #86: `TRIANGLES` is not exclusive to the mesh either. The 2009 models
    # also carry a TRIANGLES under a BOXTREE bounding-volume node (1411 across
    # the install) in a different `u16 tag; u16 count; u16[count]` run layout;
    # by-index pairing picked it and either failed to decode (41 red on `F:`) or
    # silently paired ~1938 clusters with the wrong list. The engine resolves the
    # mesh chunk with CDataFile::GetNext(cluster, "TRIANGLES") -- a sibling of
    # the cluster's VERTEXFRAMES -- so a BOXTREE child can never be one.
    xmd_export_src = (ROOT / "tools/xmd_export.py").read_text(encoding="utf-8")
    require("def triangle_nodes(self)" in mos_xmd and
            "def triangle_node(self)" in mos_xmd and
            "self.triangle_nodes()" in mos_xmd and
            "def triangle_nodes(x: Xmd)" in xmd_export_src and
            "tri_nodes = triangle_nodes(x)" in xmd_export_src and
            "test_triangle_nodes_skips_boxtree_leaf" in mos_xmd_tests and
            "test_boxtree_only_triangles_means_no_mesh" in mos_xmd_tests and
            "test_check_pairs_boxtree_triangles" in (
                ROOT / "tools/test_xmd_pose.py").read_text(encoding="utf-8") and
            "test_decode_cluster_skips_boxtree_triangles" in (
                ROOT / "tools/test_xmd_export.py").read_text(encoding="utf-8") and
            (ROOT / "docs/adr/0477-xmd-triangles-boxtree-leaf-selection.md"
             ).exists(),
            "GH #86: the mesh TRIANGLES must be selected (not a BOXTREE "
            "bounding-volume leaf), the cluster->triangle pairing must be exact "
            "on both installs, and the selection must be unit-tested and "
            "recorded")

    # GH #87 (+ autoloop wake-97 extension): a live run can measure nothing two
    # ways -- the game *ends* within ~2 s before the hook attaches (an early exit),
    # or it stays **live** the whole window with the hook never attaching (measured
    # wake 96: 47 s alive, 0 frames, no hook log, identical re-run passes). Both are
    # retried once and, if they repeat, reported as one distinct `game_started`
    # failure instead of grading the capture checks; any evidence is never retried.
    require("def game_start_decision" in xrsim_tool and
            "def launch_retry_decision" in xrsim_tool and
            "def demote_checks_for_no_game_start" in xrsim_tool and
            "def format_launch_summary" in xrsim_tool and
            "EARLY_EXIT_MIN_LIVE_SECONDS" in xrsim_tool and
            "--min-live-seconds" in xrsim_tool and
            "--max-launch-attempts" in xrsim_tool and
            '"game_started"' in xrsim_tool and
            '"no_evidence"' in xrsim_tool and
            '"launch": launch' in xrsim_tool and
            "game_start_decision_states" in xrsim_tool and
            "launch_retry_bounds_early_exit" in xrsim_tool and
            "test_game_start_decision_states" in _xrsim_tests and
            "test_no_game_start_fails_once_and_demotes_capture_checks"
            in _xrsim_tests and
            "test_no_evidence_run_fails_once_and_demotes_capture_checks"
            in _xrsim_tests and
            (ROOT / "docs/adr/0479-xrsim-early-game-exit-retry.md").exists() and
            (ROOT / "docs/adr/0498-xrsim-no-evidence-attempt-retry.md").exists(),
            "GH #87: a live run that measures nothing -- the game exits in ~2 s "
            "with no hook log and no frames, OR stays live with the hook never "
            "attached -- must be retried once and, if it repeats, reported as one "
            "distinct game_started failure rather than as broken captures")

    # GH #67 (+ #80) state question, wake 98: step 1 has no engine-side route left
    # (ADR-0496), so the only way to learn whether the engine draws its
    # first-person arms is to observe it in the state where it does -- armed /
    # fighting. The engine's window proc keys on the message's lParam scan code
    # and the *player profile* maps scan codes to actions (`*ACTION_FORWARD 17`,
    # `*ACTION_PRIMARYFIRE 240`, `*ACTION_WEAPON_SHIV 3`, ...), so XRSIM carries a
    # sim-safe in-level key drive: post the profile's own scan codes to the game's
    # own window once gameplay is reached. Pure parser + schedule, opt-in, outcome
    # recorded in the report as `key_drive`.
    require("class KeyScriptDriver" in xrsim_tool and
            "def parse_drive_keys" in xrsim_tool and
            "def parse_action_profile" in xrsim_tool and
            "def profile_action_path" in xrsim_tool and
            "def scan_key_message" in xrsim_tool and
            "def post_scan_to_game_windows" in xrsim_tool and
            "def post_key_to_game_windows" in xrsim_tool and
            "PROFILE_ACTION_PREFIX = \"*ACTION_\"" in xrsim_tool and
            '"--drive-keys"' in xrsim_tool and
            '"--action-profile"' in xrsim_tool and
            '"key_drive": getattr(args, "drive_stats", None)' in xrsim_tool and
            "test_drive_keys_parse_and_resolve_the_action_profile" in _xrsim_tests and
            "test_key_drive_orders_down_hold_up_gap_without_a_thread" in _xrsim_tests and
            "drive_keys_parse_profile" in xrsim_tool and
            (ROOT / "docs/adr/0499-xrsim-drives-the-engine-in-level-with-profile-scan-codes.md").exists(),
            "GH #67/#80: XRSIM must be able to drive the engine in-level with the "
            "profile's own scan codes (weapon select / fire / move) so a scripted "
            "run can reach the armed/fighting state the #67 state question needs")

    # GH #88: the autoloop run trace (tools/autoloop/logs/STATE.log) is
    # append-only by contract and now TRACKED, because a replace-style edit
    # destroyed ~107 KB of wake history with no git copy (wake 84). The fix is
    # enforced by a tracked append helper that refuses to shrink the file, a
    # header + never-shrink check (`log_state.ps1 -Verify`), .gitignore
    # un-ignoring the single traced file, and the standing prompt pointing wakes
    # at the helper instead of the file.
    log_state = (ROOT / "tools/autoloop/log_state.ps1").read_text(encoding="utf-8")
    autoloop_ignore = (ROOT / "tools/autoloop/.gitignore").read_text(encoding="utf-8")
    autoloop_prompt = (ROOT / "tools/autoloop/prompt.md").read_text(encoding="utf-8")
    require("function Invoke-Append" in log_state and
            "function Invoke-Verify" in log_state and
            "function Invoke-SelfTest" in log_state and
            "SHRUNK" in log_state and
            "Add-Content" in log_state and
            "log_state.ps1" in autoloop_prompt and
            "logs/*" in autoloop_ignore and
            "!logs/STATE.log" in autoloop_ignore,
            "GH #88: the autoloop run trace must be append-only and recoverable -- "
            "the tracked helper (log_state.ps1) refuses to shrink it, -Verify pins "
            "the header and never-shrink check, .gitignore must un-ignore "
            "logs/STATE.log, and the standing prompt must point wakes at the "
            "helper rather than at the file")

    # ADR hygiene: numbers must be unique, so the next ADR is max + 1 and no two
    # units share a file prefix. The five GH #74 collisions (the PR #70 merge
    # landed two parallel implementations) are allow-listed until that issue
    # reconciles them; see docs/adr/README.md.
    adr_dir = ROOT / "docs/adr"
    known_duplicates = {"0445", "0446", "0447", "0448", "0449"}  # GH #74
    counts = {}
    for adr in adr_dir.glob("[0-9][0-9][0-9][0-9]-*.md"):
        prefix = adr.name[:4]
        counts[prefix] = counts.get(prefix, 0) + 1
    unexpected = sorted(n for n, c in counts.items() if c > 1 and n not in known_duplicates)
    require(not unexpected,
            "ADR numbers must be unique -- new collision(s): {0}; renumber the new "
            "file to max + 1 and update docs/adr/README.md".format(", ".join(unexpected)))
    require(all(counts.get(n, 0) == 2 for n in known_duplicates),
            "GH #74: the allow-listed ADR collisions changed -- update the guard and "
            "docs/adr/README.md together")

    print("[issue-fixes] source invariants passed (#40, #43-47, #52-54, #58, #61, "
          "#62, #64-74, #69, #71-73, #75-81, #83, #85-#88)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"[issue-fixes] FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
