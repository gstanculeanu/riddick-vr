#pragma once

// Track C #10: minimal riddick_vr.ini loader (no external deps).
// Read once at CameraHook init; all fields have safe defaults so a missing
// or malformed INI never breaks the build or runtime behavior.

#include <windows.h>

namespace RiddickVR {

struct VRConfig {
    float snapAngleDeg = 45.0f;      // snap-turn step size
    float smoothTurnSpeed = 20.0f;   // smooth-turn curve gain
    float moveDeadzone = 0.22f;      // left-stick radial deadzone
    float turnDeadzone = 0.20f;      // right-stick smooth-turn deadzone
    float dialogueDeadzone = 0.50f;  // dialogue/menu analog direction threshold
    int interactButton = 0;           // 0 = face A, 1 = left Y (repurposes Eyeshine)
    float lookSensitivityYaw = 445.6f;   // GH #45: head-look mouse gain (calibrate!)
    float lookSensitivityPitch = 445.6f; // GH #45: head-look mouse gain (calibrate!)
    int comfortEnabled = 1;          // #9 comfort vignette proxy toggle
    float comfortStrength = 1.0f;    // vignette proxy gain multiplier
    float comfortMaxDim = 0.45f;     // GH #9: peripheral dimming ceiling (0=off, max alpha)
    int seated = 0;                  // #11 seated mode
    float seatedEyeOffset = 0.15f;   // #11 seated eye-height compensation
    float standingEyeHeight = 0.0f;  // optional baseline override (0 = auto)
    float worldScale = 1.0f;         // #13 world scale multiplier
    int southpaw = 0;                // #14 left-dominant hand mapping
    int gestureDebounceMs = 180;     // #15 per-action debounce (clamped 120-250)
    // GH #40: diegetic ammo quad crop rect (fraction of swapchain 0..1)
    float ammoQuadCropX = 0.75f;
    float ammoQuadCropY = 0.75f;
    float ammoQuadCropW = 0.23f;
    float ammoQuadCropH = 0.23f;
    // GH #40: wrist HUD crop rect (fraction of swapchain 0..1)
    float wristHudCropW = 0.26f;
    float wristHudCropH = 0.16f;
    // GH #46: draw the baked native Riddick arm/hand mesh (default OFF;
    // procedural overlay stays the fallback). Scale is model units -> metres
    // and the offset is hand-local metres; both are provisional and need a
    // headset-tuned value (ADR-0427).
    int nativeHands = 0;
    float nativeHandScale = 0.033f;
    float nativeHandOffsetX = 0.0f;
    float nativeHandOffsetY = 0.0f;
    float nativeHandOffsetZ = 0.0f;
    // GH #64: on-headset orientation correction for the native mesh, degrees in
    // the hand-local frame (see RotateNativeHandPlacement). Defaults are the
    // provisional, uncalibrated basis (ADR-0427) -- tune with a headset.
    float nativeHandYaw = 0.0f;
    float nativeHandPitch = 0.0f;
    float nativeHandRoll = 0.0f;
    // GH #61: bind the baked Riddick skin/scar diffuse (DXT1, baked from
    // Models.xtc) on the native hand mesh. V-flip is provisional and needs
    // on-headset validation (like the placement above); 0 = keep the UVs as
    // baked.
    int nativeHandTexture = 1;
    int nativeHandTextureVFlip = 0;
    // GH #58: draw the *posed* native mesh variant baked offline from a shipped
    // first-person .xsa clip (ADR-0443) instead of the bind T-pose geometry.
    // Default OFF: the baked pose (FPSMelee "hands at rest") is an evidence-
    // backed but unvalidated VR fit; tune/compare on a headset. Ignored when no
    // posed variant exists for a mesh.
    int nativeHandPose = 0;
    // GH #64: draw the IK upper-arm tube (shoulder to elbow) with native hands.
    // Default OFF: the native mesh is placed from its own bounds, so the tube
    // does not join it and floats as a stray block.
    int nativeHandUpperArm = 0;
    // GH #46: fade the overlay hands to 0.25 while a weapon is held (trigger
    // pulled or two-handed grip). Never applies to a boxer guard. 0 = never fade.
    int handFadeWhenWeapon = 1;
    // GH #62: front-of-HMD block/guard envelope (head-local metres; +Y up,
    // -Z forward). Block is a hand *pose* held in front of the face, never a
    // low hold. Both the guard pose and the strike-suppression gate share these
    // numbers; tune them on a headset. Defaults match the ADR-0400 window.
    float guardYMin = -0.16f;
    float guardYMax = 0.32f;
    float guardZMin = -0.60f;
    float guardZMax = -0.12f;
    float guardMaxHandSpan = 0.50f;
    // GH #65: melee strike velocity thresholds (m/s) + per-side hand positions
    // (head-local metres, +Y up, -Z forward). Strikes are swing-driven; these
    // are the classifier's sensitivities. Defaults reproduce the e858809 first
    // pass exactly (see combat_strike.h); tune them on a headset instead of
    // rebuilding.
    float strikeForwardVz = -1.20f;
    float strikeForwardMaxZ = -0.10f;
    float strikeHookVx = 1.35f;
    float strikeHookVz = -0.15f;
    float strikeUppercutVy = 1.35f;
    float strikeUppercutMaxY = -0.10f;
    float strikeUppercutVz = -0.20f;
    float strikeCrossVx = 1.10f;
    // GH #65: rotate the controller's world velocity into the HMD yaw frame
    // before classifying strikes, so a swing registers whichever way the player
    // faces. 0 = legacy (world axes; only works facing stage -Z).
    int headLocalVelocity = 1;
    // GH #65: milliseconds a melee mouse click stays held before release (0 =
    // legacy instant down+up, which the engine's polled input often misses).
    int meleeClickHoldMs = 60;
    // GH #62: guard pose hysteresis (ms) and the hand speed limit (m/s) below
    // which the hands count as steady. Defaults are the old hard-coded values.
    int guardEngageMs = 40;
    int guardReleaseMs = 80;
    float guardSteadySpeed = 1.4f;
    // GH #52: two-trigger combat stance thresholds (trigger 0..1, hold in ms).
    // Defaults are the old constants. MeleeRequiresStance=1 ignores swings
    // outside the stance (exploration mode); default 0 keeps swings always live.
    float stanceEnterTrigger = 0.85f;
    float stanceExitTrigger = 0.60f;
    int stanceHoldMs = 100;
    int meleeRequiresStance = 0;
    // GH #66 (approach 2): suppress the engine's own first-person viewmodel
    // (arms/hands) at the draw level, since approach 1 (the RPG `noviewmodel`
    // bit) was falsified in ADR-0438. Default OFF: the pass signature is a
    // PROVISIONAL depth-only-clear window (ADR-0439) and must be confirmed on a
    // headset before it is trusted. ViewmodelDepthClearWindow selects the
    // depth-only-clear opener (1) or disables the window entirely (0).
    int suppressEngineViewmodel = 0;
    int viewmodelDepthClearWindow = 1;
    // GH #63/#66: log the per-frame GL pass signature every 300 swaps (0 = off).
    int viewmodelTrace = 0;
    // GH #66: window opener. -1 = derive from viewmodelDepthClearWindow;
    // 0 none, 1 depth-only clear, 2 near-plane drop, 3 either.
    int viewmodelWindowOpener = -1;
    float viewmodelNearRatio = 0.5f;
    // GH #67 (ADR-0447): drive the engine's own viewmodel with the dominant
    // controller instead of hiding it. Opt-in; ignored while
    // suppressEngineViewmodel is on.
    int driveEngineViewmodel = 0;
    float viewmodelUnitsPerMeter = 30.3f;
    float viewmodelAnchorX = 0.18f;
    float viewmodelAnchorY = -0.22f;
    float viewmodelAnchorZ = -0.38f;
    float viewmodelDriveYaw = 0.0f;
    float viewmodelDrivePitch = 0.0f;
    float viewmodelDriveRoll = 0.0f;
};

extern VRConfig g_vrConfig;

void LoadVRConfig();

inline float GetWorldScale() {
    return (g_vrConfig.worldScale > 0.001f) ? g_vrConfig.worldScale : 1.0f;
}

} // namespace RiddickVR
