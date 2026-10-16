#pragma once

// GH #62: physical block/attack guard envelope, expressed in *head-local*
// coordinates (metres relative to the HMD): +X right, +Y up, -Z forward into
// the view. A block is a hand *pose* held in front of the face, never a low
// rest position.
//
// Before this header the strike gate and the guard pose used two different
// hand volumes: the pose used a tight chin-to-forehead window while the strike
// gate used a much looser volume (y down to -0.42 m / z out to -0.05 m, i.e.
// *below* and *inside* the view). Because `isMeleeStrike` is masked by
// `!inGuardVolume`, a forward/hook swing thrown from a raised guard landed
// inside that loose volume and never fired (the "raising the hands to guard can
// mask or cancel attacks" bug in GH #62). Both sites now share this envelope,
// and every number is INI-tunable (`Guard*` keys in riddick_vr.ini).
//
// The defaults reproduce the ADR-0400 / 05ae8a6 pose-detection window exactly
// (tighten/tune on a headset); the *strike gate* narrowing to this window is
// the behavioural fix.

namespace RiddickVR {

struct GuardEnvelope {
    // Head-local hand window (metres). +Y up, -Z forward.
    float yMin = -0.16f;   // no lower than just under the chin (never below the view)
    float yMax = 0.32f;    // up to roughly forehead height
    float zMin = -0.60f;   // farthest forward the guard may be held
    float zMax = -0.12f;   // nearest to the face
    float maxHandSpan = 0.50f;  // |leftX - rightX| upper bound (shield width)
};

// True when one hand is inside the head-local guard window.
inline bool GuardHandInWindow(const GuardEnvelope& env, float y, float z) {
    return y >= env.yMin && y <= env.yMax && z >= env.zMin && z <= env.zMax;
}

// True when *both* hands are inside the window and not spread wider than
// ``maxHandSpan``. ``handSpanX`` is the signed left-minus-right X difference;
// the magnitude is used, so argument order does not matter.
inline bool InGuardEnvelope(const GuardEnvelope& env,
                            float leftY, float leftZ,
                            float rightY, float rightZ,
                            float handSpanX) {
    float span = (handSpanX < 0.0f) ? -handSpanX : handSpanX;
    return GuardHandInWindow(env, leftY, leftZ) &&
           GuardHandInWindow(env, rightY, rightZ) &&
           span < env.maxHandSpan;
}

// GH #62: guard pose engage/release hysteresis. A raw pose test flickers when
// the hands jitter at the edge of the envelope, which dropped the block in the
// middle of a fight. The pose must hold `engageMs` to engage; once engaged it
// survives pose loss for `releaseMs`, except that a real punch releases it at
// once (a swing leaves the window by design). `steadySpeed` is the hand speed
// limit for "hands steady" (m/s).
struct GuardPoseTiming {
    unsigned engageMs = 40;
    unsigned releaseMs = 80;
    float steadySpeed = 1.4f;
};

// Timers use 0 as "not running", so a tick count of exactly 0 is not supported
// (same as the inline code this replaced).
struct GuardPoseState {
    unsigned long holdStart = 0;
    unsigned long releaseStart = 0;
    bool engaged = false;
};

// Returns the new engaged flag (also stored in `st.engaged`).
inline bool UpdateGuardPose(GuardPoseState& st, bool poseDetected, bool throwingPunch,
                            unsigned long now, const GuardPoseTiming& timing) {
    if (poseDetected) {
        st.releaseStart = 0;
        if (st.holdStart == 0) {
            st.holdStart = now;
        } else if (now - st.holdStart >= timing.engageMs) {
            st.engaged = true;
        }
    } else {
        st.holdStart = 0;
        if (st.engaged) {
            if (throwingPunch) {
                st.engaged = false;
                st.releaseStart = 0;
            } else if (st.releaseStart == 0) {
                st.releaseStart = now;
            } else if (now - st.releaseStart >= timing.releaseMs) {
                st.engaged = false;
                st.releaseStart = 0;
            }
        }
    }
    return st.engaged;
}

} // namespace RiddickVR
