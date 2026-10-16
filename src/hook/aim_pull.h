#pragma once

// GH #50 / #51: controller aim steering ("aim pull") as a pure function.
//
// While aiming, the dominant controller's aim *movement* (a per-frame yaw and
// pitch delta, radians) nudges the camera. The constants are the ones that were
// inline in `UpdateHeadLook` (camera_hook.cpp) and are unchanged:
// deadzone 0.0015 rad, clamp 0.05 rad/frame, weight 0.12 in physical ADS and
// 0.08 otherwise.
//
// New in this header: no pull at all while the hands are in the boxer-guard
// pose (`guardPoseLike`). The fist guard sits inside the ADS corridor, and a
// left-trigger block counts as "aim held", so moving the arms while guarding
// used to steer the camera.

#include <cmath>

namespace RiddickVR {

struct AimPullInputs {
    float deltaYaw = 0.0f;    // aim yaw change this frame (rad)
    float deltaPitch = 0.0f;  // aim pitch change this frame (rad)
    bool isPhysicalADS = false;
    bool twoHanded = false;
    bool aimHeld = false;
    bool physicalBlock = false;
    bool reloading = false;
    bool meleeEquipped = false;
    bool guardPoseLike = false;
};

struct AimPullResult {
    float pullYaw = 0.0f;
    float pullPitch = 0.0f;
    float weight = 0.0f;
    bool active = false;  // actively aiming: pull may be applied
};

constexpr float kAimDeltaDeadzone = 0.0015f;  // ~0.09 deg
constexpr float kAimDeltaClamp = 0.05f;       // max rad/frame
constexpr float kAimWeightAds = 0.12f;
constexpr float kAimWeightHip = 0.08f;

inline float ShapeAimDelta(float d) {
    if (!(std::fabs(d) >= kAimDeltaDeadzone)) return 0.0f;  // also rejects NaN
    if (d > kAimDeltaClamp) return kAimDeltaClamp;
    if (d < -kAimDeltaClamp) return -kAimDeltaClamp;
    return d;
}

inline AimPullResult ComputeAimPull(const AimPullInputs& in) {
    AimPullResult out;
    out.active = (in.isPhysicalADS || in.twoHanded || in.aimHeld) &&
                 !in.physicalBlock && !in.reloading && !in.guardPoseLike;
    if (!out.active || in.meleeEquipped) return out;
    out.pullYaw = ShapeAimDelta(in.deltaYaw);
    out.pullPitch = ShapeAimDelta(in.deltaPitch);
    out.weight = in.isPhysicalADS ? kAimWeightAds : kAimWeightHip;
    return out;
}

} // namespace RiddickVR
