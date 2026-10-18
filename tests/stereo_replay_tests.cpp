// GH #76 (epic #57 DoD item 1) option 1: unit tests for the per-eye replay maths
// (stereo_replay.h). Headless, no GL, no game launch.
//
// The decisive assertions are the *cross-checks* against the host's independent
// implementation (src/host/eye_frame.h): the replay's head view must equal the
// host's view construction, and `translate(-offset) * headView` must reproduce
// the host's per-eye view to within float noise. That is the whole mechanism the
// option-1 route rests on.
#include "stereo_replay.h"
#include "eye_frame.h"

#include <cmath>
#include <cstdio>

using RiddickVR::BuildEyeReplayModelview;
using RiddickVR::EyeSpaceCameraOffset;
using RiddickVR::EyeSpaceCameraOffsetFromViews;
using RiddickVR::ReplayBaselineMeters;
using RiddickVR::ReplayEyePositionFromView;
using RiddickVR::ReplayViewFromPose;
using RiddickVR::ReplayViewResidual;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "stereo replay test failed: %s\n", message);
        ++g_failures;
    }
}

static bool Near(float a, float b, float tol = 1.0e-4f) { return std::fabs(a - b) < tol; }

static void CheckMatrixNear(const float* a, const float* b, float tol, const char* message) {
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(a[i] - b[i]) > tol) {
            std::fprintf(stderr, "stereo replay test failed: %s (entry %d: %.6f vs %.6f)\n",
                         message, i, a[i], b[i]);
            ++g_failures;
            return;
        }
    }
}

int main() {
    const float kHalfIpd = 0.0325f;  // 65 mm IPD

    // 1. A zero offset is an identity modelview.
    {
        const float zero[3] = {0.0f, 0.0f, 0.0f};
        float m[16];
        BuildEyeReplayModelview(zero, m);
        const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        CheckMatrixNear(m, identity, 1.0e-6f, "zero offset must be the identity");
    }

    // 2. The replay modelview is the *negated* eye-space offset in its last column.
    {
        const float off[3] = {0.02f, -0.01f, 0.005f};
        float m[16];
        BuildEyeReplayModelview(off, m);
        Check(Near(m[0], 1.0f) && Near(m[5], 1.0f) && Near(m[10], 1.0f) && Near(m[15], 1.0f),
              "replay modelview keeps the identity diagonal");
        Check(Near(m[12], -off[0]) && Near(m[13], -off[1]) && Near(m[14], -off[2]),
              "replay modelview translates by the negative offset");
    }

    // 3. The replay's view construction is the host's view construction.
    {
        const float pos[3] = {0.3f, 1.7f, -0.4f};
        const float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float a[16], b[16];
        ReplayViewFromPose(pos, quat, a);
        RiddickVR::ViewMatrixFromPose(pos, quat, b);
        CheckMatrixNear(a, b, 1.0e-6f, "replay view must equal the host's view");
    }

    // 4. The eye position recovered from a view matrix round-trips.
    {
        const float pos[3] = {-0.15f, 1.62f, 0.9f};
        const float quat[4] = {0.1f, 0.2f, -0.3f, 0.9273618f};  // unit-ish
        float view[16];
        ReplayViewFromPose(pos, quat, view);
        float eye[3];
        Check(ReplayEyePositionFromView(view, eye), "eye position must be recoverable");
        Check(Near(eye[0], pos[0]) && Near(eye[1], pos[1]) && Near(eye[2], pos[2]),
              "recovered eye position must match the pose");
    }

    // 5. THE MECHANISM. For a head pose, the host's per-eye views are exactly a
    //    rigid eye-space translation of the head view: the recovered offset is
    //    +/-halfIpd along the head's local +X and the residual is ~0.
    for (float yawDeg : {0.0f, 35.0f, 90.0f, -140.0f}) {
        const float yaw = yawDeg * 3.14159265f / 180.0f;
        const float headPos[3] = {0.4f, 1.68f, -0.2f};
        const float headQuat[4] = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};

        float headView[16];
        ReplayViewFromPose(headPos, headQuat, headView);

        float viewL[16], viewR[16];
        RiddickVR::ComputeEyeViewFromHeadPose(headPos, headQuat, kHalfIpd, false, viewL);
        RiddickVR::ComputeEyeViewFromHeadPose(headPos, headQuat, kHalfIpd, true, viewR);

        float offL[3], offR[3];
        bool usedL = false, usedR = false;
        Check(EyeSpaceCameraOffsetFromViews(viewL, viewR, headPos, headQuat, false, 0.065f, offL, &usedL),
              "left offset must be derivable from the measured view");
        Check(EyeSpaceCameraOffsetFromViews(viewL, viewR, headPos, headQuat, true, 0.065f, offR, &usedR),
              "right offset must be derivable from the measured view");
        Check(usedL && usedR, "a scene-faithful view pair must be used, not the fallback");

        // The offset is in the head's own eye-space axes, so it is +/-halfIpd on X.
        Check(Near(offL[0], -kHalfIpd) && Near(offL[1], 0.0f) && Near(offL[2], 0.0f),
              "left eye offset is -halfIpd along eye-space X");
        Check(Near(offR[0], kHalfIpd) && Near(offR[1], 0.0f) && Near(offR[2], 0.0f),
              "right eye offset is +halfIpd along eye-space X");

        const float resL = ReplayViewResidual(headView, viewL, offL);
        const float resR = ReplayViewResidual(headView, viewR, offR);
        Check(resL < 1.0e-5f, "translate(-offL) * headView must equal the left eye view");
        Check(resR < 1.0e-5f, "translate(-offR) * headView must equal the right eye view");
    }

    // 6. A point straight ahead lands LEFT of centre in the right eye (sign sanity):
    //    after the replay modelview, its eye-space X is -halfIpd.
    {
        const float off[3] = {kHalfIpd, 0.0f, 0.0f};
        float m[16];
        BuildEyeReplayModelview(off, m);
        const float straightAhead[4] = {0.0f, 0.0f, -3.0f, 1.0f};  // column vector
        float x = m[0] * straightAhead[0] + m[4] * straightAhead[1] +
                  m[8] * straightAhead[2] + m[12] * straightAhead[3];
        Check(Near(x, -kHalfIpd), "a straight-ahead point appears to the left in the right eye");
    }

    // 7. The fallback (no views) is +/-halfIpd along the head's local +X.
    {
        const float headPos[3] = {0, 0, 0};
        const float headQuat[4] = {0, 0, 0, 1};
        float off[3];
        Check(EyeSpaceCameraOffsetFromViews(nullptr, nullptr, headPos, headQuat, true, 0.06f, off),
              "the IPD fallback must be usable without views");
        Check(Near(off[0], 0.06f) && Near(off[1], 0.0f) && Near(off[2], 0.0f),
              "the right-eye fallback is +halfIpd on X");
        EyeSpaceCameraOffsetFromViews(nullptr, nullptr, headPos, headQuat, false, 0.06f, off);
        Check(Near(off[0], -0.06f), "the left-eye fallback is -halfIpd on X");
    }

    // 8. A degenerate quaternion is rejected, not silently zeroed.
    {
        const float headPos[3] = {0, 1.6f, 0};
        const float badQuat[4] = {0, 0, 0, 0};
        const float eyePosA[3] = {0.03f, 1.6f, 0};
        float off[3];
        Check(!EyeSpaceCameraOffset(headPos, badQuat, eyePosA, off),
              "a zero quaternion must be rejected");
    }

    // 9. The measured baseline is the distance between the eye-view translations.
    {
        const float headPos[3] = {0.1f, 1.7f, 0.2f};
        const float headQuat[4] = {0, 0, 0, 1};
        float viewL[16], viewR[16];
        RiddickVR::ComputeEyeViewFromHeadPose(headPos, headQuat, kHalfIpd, false, viewL);
        RiddickVR::ComputeEyeViewFromHeadPose(headPos, headQuat, kHalfIpd, true, viewR);
        Check(Near(ReplayBaselineMeters(viewL, viewR, 0.065f), 2.0f * kHalfIpd, 1.0e-5f),
              "the measured baseline is the eye separation");
        Check(Near(ReplayBaselineMeters(nullptr, viewR, 0.064f), 0.064f),
              "a missing pair falls back");
    }

    // 10. A published *placeholder* baseline (identity rotation, origin at the
    //     world origin -- the mock host's synthetic eye view, ADR-0463 / GH #77)
    //     is NOT a valid replay source: the helper falls back to the head-local
    //     IPD and the residual against the placeholder is large.
    {
        const float headPos[3] = {0.0f, 1.68f, 0.0f};
        const float headQuat[4] = {0, 0, 0, 1};
        float headView[16];
        ReplayViewFromPose(headPos, headQuat, headView);

        float placeholderL[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -kHalfIpd, 0, 0, 1};
        float placeholderR[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, kHalfIpd, 0, 0, 1};

        float off[3];
        bool usedViews = true;
        Check(EyeSpaceCameraOffsetFromViews(placeholderL, placeholderR, headPos, headQuat,
                                            false, kHalfIpd, off, &usedViews),
              "the helper still yields an offset with a placeholder pair");
        Check(!usedViews, "a placeholder pair must not be used as the replay source");
        Check(Near(off[0], -kHalfIpd) && Near(off[1], 0.0f) && Near(off[2], 0.0f),
              "a placeholder pair falls back to the head-local IPD");
        Check(ReplayViewResidual(headView, placeholderL, off) > 0.1f,
              "the residual names the placeholder mismatch");
    }

    if (g_failures == 0) {
        std::puts("stereo replay tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
