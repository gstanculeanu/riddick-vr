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

    print("[issue-fixes] source invariants passed (#40, #43-47, #52-54)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"[issue-fixes] FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
