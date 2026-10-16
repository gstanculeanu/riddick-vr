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
    // GH #66 (approach 2): suppress the engine's own first-person viewmodel
    // (arms/hands) at the draw level, since approach 1 (the RPG `noviewmodel`
    // bit) was falsified in ADR-0438. Default OFF: the pass signature is a
    // PROVISIONAL depth-only-clear window (ADR-0439) and must be confirmed on a
    // headset before it is trusted. ViewmodelDepthClearWindow selects the
    // depth-only-clear opener (1) or disables the window entirely (0).
    int suppressEngineViewmodel = 0;
    int viewmodelDepthClearWindow = 1;
    // GH #67 step 2: observation-only identification of the engine's
    // first-person viewmodel pass. Opens the same depth-only-clear window as
    // suppression but never skips a draw; emits one `ViewmodelProbe` log line
    // per sampled frame with the per-frame draw tallies and the modelview in
    // effect at the first in-window draw. Default OFF.
    int viewmodelObserve = 0;
};

extern VRConfig g_vrConfig;

void LoadVRConfig();

inline float GetWorldScale() {
    return (g_vrConfig.worldScale > 0.001f) ? g_vrConfig.worldScale : 1.0f;
}

} // namespace RiddickVR
