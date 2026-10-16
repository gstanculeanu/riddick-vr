// GH #67 (ADR-0447): unit tests for the controller-driven viewmodel correction
// (viewmodel_drive.h). Column-major: translation lives in m[12..14].
#include "viewmodel_drive.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace RiddickVR;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "viewmodel drive test failed: %s\n", message);
        ++g_failures;
    }
}

static bool Near(float a, float b, float eps = 1.0e-4f) { return std::fabs(a - b) <= eps; }

static bool IsIdentity(const float m[16], float eps = 1.0e-4f) {
    for (int i = 0; i < 16; ++i) {
        if (!Near(m[i], (i % 5 == 0) ? 1.0f : 0.0f, eps)) return false;
    }
    return true;
}

static DrivePose YawPose(float x, float y, float z, float yawDeg) {
    DrivePose p;
    p.pos[0] = x; p.pos[1] = y; p.pos[2] = z;
    const float h = 0.5f * yawDeg * 0.017453292519943295f;
    p.quat[0] = 0.0f; p.quat[1] = std::sin(h); p.quat[2] = 0.0f; p.quat[3] = std::cos(h);
    return p;
}

int main() {
    ViewmodelDriveConfig cfg;
    cfg.enabled = true;
    float c[16];

    // Controller exactly at the anchor, identity rotations: identity correction.
    {
        DrivePose hmd = YawPose(1.0f, 1.7f, -2.0f, 0.0f);
        DrivePose ctrl = YawPose(1.0f + cfg.anchor[0], 1.7f + cfg.anchor[1], -2.0f + cfg.anchor[2], 0.0f);
        Check(ComputeViewmodelCorrection(cfg, hmd, ctrl, c), "a valid pose must succeed");
        Check(IsIdentity(c), "controller at the anchor must give the identity");
    }

    // 0.1 m to the right of the anchor: pure 0.1 * upm translation along +X.
    {
        DrivePose hmd = YawPose(0, 0, 0, 0);
        DrivePose ctrl = YawPose(cfg.anchor[0] + 0.1f, cfg.anchor[1], cfg.anchor[2], 0.0f);
        Check(ComputeViewmodelCorrection(cfg, hmd, ctrl, c), "offset pose must succeed");
        Check(Near(c[12], 0.1f * cfg.unitsPerMeter, 1.0e-3f) && Near(c[13], 0.0f, 1.0e-3f) &&
              Near(c[14], 0.0f, 1.0e-3f), "0.1 m offset must translate by 0.1 * unitsPerMeter");
    }

    // Rotated controller: the 3x3 block stays orthonormal (det +1).
    {
        cfg.yawDeg = 20.0f; cfg.pitchDeg = -35.0f; cfg.rollDeg = 70.0f;
        DrivePose hmd = YawPose(0, 1.7f, 0, 33.0f);
        DrivePose ctrl = YawPose(0.3f, 1.4f, -0.4f, -50.0f);
        Check(ComputeViewmodelCorrection(cfg, hmd, ctrl, c), "rotated pose must succeed");
        bool ortho = true;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                float dot = 0.0f;
                for (int k = 0; k < 3; ++k) dot += c[i * 4 + k] * c[j * 4 + k];
                if (!Near(dot, i == j ? 1.0f : 0.0f, 1.0e-4f)) ortho = false;
            }
        }
        Check(ortho, "the rotation block must be orthonormal");
        const float det = c[0] * (c[5] * c[10] - c[6] * c[9]) - c[4] * (c[1] * c[10] - c[2] * c[9]) +
                          c[8] * (c[1] * c[6] - c[2] * c[5]);
        Check(Near(det, 1.0f, 1.0e-3f), "the rotation block must have determinant +1");
        cfg.yawDeg = cfg.pitchDeg = cfg.rollDeg = 0.0f;
    }

    // Same yaw on HMD and controller (rigid body turn): the correction is unchanged.
    {
        float a[16], b[16];
        // Controller held at the anchor relative to the head, for two head yaws.
        const float yaws[2] = {0.0f, 77.0f};
        for (int i = 0; i < 2; ++i) {
            const float h = 0.5f * yaws[i] * 0.017453292519943295f;
            DrivePose hmd = YawPose(0.5f, 1.7f, 0.5f, yaws[i]);
            // anchor rotated into the world by the head yaw
            const float s = std::sin(2.0f * h), co = std::cos(2.0f * h);
            const float wx = co * (cfg.anchor[0] + 0.05f) + s * cfg.anchor[2];
            const float wz = -s * (cfg.anchor[0] + 0.05f) + co * cfg.anchor[2];
            DrivePose ctrl = YawPose(0.5f + wx, 1.7f + cfg.anchor[1], 0.5f + wz, yaws[i]);
            ComputeViewmodelCorrection(cfg, hmd, ctrl, i == 0 ? a : b);
        }
        bool same = true;
        for (int i = 0; i < 16; ++i) if (!Near(a[i], b[i], 1.0e-3f)) same = false;
        Check(same, "the same head-relative pose must give the same correction at any head yaw");
    }

    // Invalid input: false + identity.
    {
        DrivePose hmd = YawPose(0, 0, 0, 0);
        DrivePose ctrl = YawPose(0, 0, 0, 0);
        ctrl.pos[1] = std::numeric_limits<float>::quiet_NaN();
        Check(!ComputeViewmodelCorrection(cfg, hmd, ctrl, c), "NaN position must fail");
        Check(IsIdentity(c), "failure must write the identity");
        ctrl = YawPose(0, 0, 0, 0);
        ctrl.quat[0] = ctrl.quat[1] = ctrl.quat[2] = ctrl.quat[3] = 0.0f;
        Check(!ComputeViewmodelCorrection(cfg, hmd, ctrl, c), "zero quaternion must fail");
        Check(IsIdentity(c), "failure must write the identity");
    }

    // MulColMajor4: translation composes and aliasing is safe.
    {
        float t1[16], t2[16];
        for (int i = 0; i < 16; ++i) t1[i] = t2[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        t1[12] = 2.0f; t2[13] = 3.0f;
        MulColMajor4(t1, t2, t1);
        Check(Near(t1[12], 2.0f) && Near(t1[13], 3.0f), "translations must compose");
    }

    if (g_failures == 0) {
        std::puts("viewmodel drive tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
