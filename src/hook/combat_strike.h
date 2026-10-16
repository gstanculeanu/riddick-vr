#pragma once

// GH #65: controller-swing strike classifier.
//
// Before this header the punch/hook/uppercut classifiers lived inline in
// `UpdateMeleeCounters` (`camera_hook.cpp`) with hard-coded velocity thresholds
// (`forwardVz < -1.20`, `hookVx > 1.35`, `uppercutVy > 1.35`, per-side hand
// positions). e858809 dropped the both-trigger `combatStance.active` gate and
// the `!inGuardVolume` mask so a swing now fires, but the numbers were fixed in
// code and could only be tuned by rebuilding. They are now a dependency-free,
// unit-testable struct (`StrikeThresholds`) fed from the INI (`Strike*` keys in
// `riddick_vr.ini`), so a headset pass can tune sensitivity without a rebuild.
//
// Semantics (head-local hand velocity in m/s, position in metres, +Y up,
// -Z forward) reproduce the e858809 first pass exactly; the defaults are those
// numbers. ADR-0403 intent is preserved: raising the hands into guard has
// `Vz ~ 0`, so it matches no strike even with the forward-punch / hook gates
// relaxed.

#include <cmath>

namespace RiddickVR {

struct StrikeThresholds {
    // Forward jab / straight: hand driven forward past `forwardVz` and ahead of
    // the HMD plane (`z < forwardMaxZ`).
    float forwardVz = -1.20f;
    float forwardMaxZ = -0.10f;
    // Hook: fast lateral swing (`|Vx| > hookVx`) that also travels forward.
    float hookVx = 1.35f;
    float hookVz = -0.15f;
    // Uppercut: fast upward drive (`Vy > uppercutVy`) starting from below the
    // view (`y < uppercutMaxY`) with some forward travel.
    float uppercutVy = 1.35f;
    float uppercutMaxY = -0.10f;
    float uppercutVz = -0.20f;
    // Dual cross slash: the hands sweep in opposite horizontal directions.
    float crossVx = 1.10f;
};

// A hand's velocity in m/s (world/head-local axes).
struct StrikeVelocity {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct StrikeIntent {
    bool dualCrossSlash = false;
    bool forwardPunch = false;
    bool hookPunch = false;
    bool uppercutPunch = false;
    bool strike = false;  // any of the above (the combined melee-strike gate)
};

// GH #65: rotate a world-space velocity into the HMD *yaw-only* frame
// (+X right, +Y up, -Z body-forward). The classifier thresholds are head-local
// (-Z = punching away from the body), but the controller velocity arrives in
// world/stage space, so a punch only registered while the player faced stage
// -Z. Yaw only (no pitch/roll) so looking up or tilting the head does not turn
// a forward punch into an upward one. A degenerate (looking straight up/down or
// zero) orientation passes the velocity through unchanged.
inline StrikeVelocity ToHeadYawLocal(float vx, float vy, float vz,
                                     float qx, float qy, float qz, float qw) {
    float fx = -2.0f * (qx * qz + qw * qy);
    float fz = -(1.0f - 2.0f * (qx * qx + qy * qy));
    const float n = std::sqrt(fx * fx + fz * fz);
    if (!(n > 1e-4f)) return StrikeVelocity{vx, vy, vz};
    fx /= n;
    fz /= n;
    // right = (-fz, 0, fx)
    return StrikeVelocity{vx * (-fz) + vz * fx, vy, -(vx * fx + vz * fz)};
}

// Classify a frame's two controller samples into strike intents. Both velocity
// and hand-position thresholds are strict (`<` / `>`), matching the original
// inline classifier; `leftLocalY/Z` and `rightLocalY/Z` are head-local metres.
inline StrikeIntent ClassifyStrike(const StrikeThresholds& t,
                                   const StrikeVelocity& leftVel,
                                   const StrikeVelocity& rightVel,
                                   float leftLocalY, float leftLocalZ,
                                   float rightLocalY, float rightLocalZ) {
    StrikeIntent out;

    out.dualCrossSlash = ((rightVel.x < -t.crossVx && leftVel.x > t.crossVx) ||
                          (rightVel.x > t.crossVx && leftVel.x < -t.crossVx));

    out.forwardPunch = ((rightVel.z < t.forwardVz && rightLocalZ < t.forwardMaxZ) ||
                        (leftVel.z < t.forwardVz && leftLocalZ < t.forwardMaxZ));

    out.hookPunch = ((std::fabs(rightVel.x) > t.hookVx && rightVel.z < t.hookVz) ||
                     (std::fabs(leftVel.x) > t.hookVx && leftVel.z < t.hookVz));

    out.uppercutPunch = ((rightVel.y > t.uppercutVy && rightLocalY < t.uppercutMaxY) ||
                         (leftVel.y > t.uppercutVy && leftLocalY < t.uppercutMaxY)) &&
                        (rightVel.z < t.uppercutVz || leftVel.z < t.uppercutVz);

    out.strike = out.dualCrossSlash || out.forwardPunch || out.hookPunch || out.uppercutPunch;
    return out;
}

} // namespace RiddickVR
