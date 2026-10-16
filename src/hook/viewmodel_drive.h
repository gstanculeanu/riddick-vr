#pragma once

// GH #67 (ADR-0447): opt-in alternative to hiding the engine's first-person
// arms. Instead of suppressing them (viewmodel_suppressor.h), the hook
// pre-multiplies the modelview matrix the engine loads for the viewmodel pass
// by a correction that moves the whole viewmodel from its default screen
// anchor to the dominant controller, so the engine's arms follow the hand.
//
// All matrices are column-major (OpenGL layout). Pure and dependency-free so it
// is unit-tested headlessly.
//
// C = T(p * unitsPerMeter) * R(q) * T(-anchor * unitsPerMeter), where
//   p = R(hmd)^-1 * (ctrl.pos - hmd.pos)            controller in head space (m)
//   q = hmd^-1 * ctrl * offset(yaw, pitch, roll)    controller orientation in head space
// With the controller sitting at the anchor with the head's orientation, C is
// the identity, so the viewmodel stays where the engine drew it.

#include <cmath>

namespace RiddickVR {

struct ViewmodelDriveConfig {
    bool enabled = false;
    // Engine units per metre. 30.3 (~1 / 0.033) is PROVISIONAL, measure on a headset.
    float unitsPerMeter = 30.3f;
    // Where the engine draws the weapon/arms in head space (metres, -Z forward).
    float anchor[3] = {0.18f, -0.22f, -0.38f};
    // Extra rotation applied to the controller orientation, degrees.
    float yawDeg = 0.0f;
    float pitchDeg = 0.0f;
    float rollDeg = 0.0f;
};

struct DrivePose {
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
};

inline void MulColMajor4(const float a[16], const float b[16], float out[16]) {
    float tmp[16];
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a[k * 4 + r] * b[c * 4 + k];
            tmp[c * 4 + r] = sum;
        }
    }
    for (int i = 0; i < 16; ++i) out[i] = tmp[i];
}

namespace viewmodel_drive_detail {

inline void SetIdentity(float m[16]) {
    for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}

inline bool Finite(float v) { return std::isfinite(v); }

// Normalizes in place. False for a non-finite or (near) zero quaternion.
inline bool Normalize(float q[4]) {
    for (int i = 0; i < 4; ++i) if (!Finite(q[i])) return false;
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 1.0e-6f)) return false;
    for (int i = 0; i < 4; ++i) q[i] /= n;
    return true;
}

// Hamilton product a * b (x, y, z, w).
inline void QuatMul(const float a[4], const float b[4], float out[4]) {
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

inline void QuatConj(const float q[4], float out[4]) {
    out[0] = -q[0]; out[1] = -q[1]; out[2] = -q[2]; out[3] = q[3];
}

// Rotate v by unit quaternion q.
inline void QuatRotate(const float q[4], const float v[3], float out[3]) {
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

// Yaw about +Y, pitch about +X, roll about +Z, composed as Ry * Rx * Rz.
inline void QuatFromYawPitchRoll(float yawDeg, float pitchDeg, float rollDeg, float out[4]) {
    const float kDegToRad = 0.017453292519943295f;
    const float hy = 0.5f * yawDeg * kDegToRad;
    const float hp = 0.5f * pitchDeg * kDegToRad;
    const float hr = 0.5f * rollDeg * kDegToRad;
    const float qy[4] = {0.0f, std::sin(hy), 0.0f, std::cos(hy)};
    const float qx[4] = {std::sin(hp), 0.0f, 0.0f, std::cos(hp)};
    const float qz[4] = {0.0f, 0.0f, std::sin(hr), std::cos(hr)};
    float t[4];
    QuatMul(qy, qx, t);
    QuatMul(t, qz, out);
}

} // namespace viewmodel_drive_detail

// Writes the correction matrix to out. Returns false (and writes identity) when
// an input is non-finite or a quaternion has zero length.
inline bool ComputeViewmodelCorrection(const ViewmodelDriveConfig& cfg, const DrivePose& hmd,
                                       const DrivePose& ctrl, float out[16]) {
    using namespace viewmodel_drive_detail;
    SetIdentity(out);

    float qh[4] = {hmd.quat[0], hmd.quat[1], hmd.quat[2], hmd.quat[3]};
    float qc[4] = {ctrl.quat[0], ctrl.quat[1], ctrl.quat[2], ctrl.quat[3]};
    if (!Normalize(qh) || !Normalize(qc)) return false;
    for (int i = 0; i < 3; ++i) {
        if (!Finite(hmd.pos[i]) || !Finite(ctrl.pos[i]) || !Finite(cfg.anchor[i])) return false;
    }
    if (!Finite(cfg.unitsPerMeter) || !Finite(cfg.yawDeg) || !Finite(cfg.pitchDeg) ||
        !Finite(cfg.rollDeg)) {
        return false;
    }

    float qhInv[4];
    QuatConj(qh, qhInv);
    const float d[3] = {ctrl.pos[0] - hmd.pos[0], ctrl.pos[1] - hmd.pos[1], ctrl.pos[2] - hmd.pos[2]};
    float p[3];
    QuatRotate(qhInv, d, p);

    float qOff[4];
    QuatFromYawPitchRoll(cfg.yawDeg, cfg.pitchDeg, cfg.rollDeg, qOff);
    float t[4];
    float q[4];
    QuatMul(qhInv, qc, t);
    QuatMul(t, qOff, q);
    if (!Normalize(q)) return false;

    // Rotation matrix of q, column-major.
    const float xx = q[0] * q[0], yy = q[1] * q[1], zz = q[2] * q[2];
    const float xy = q[0] * q[1], xz = q[0] * q[2], yz = q[1] * q[2];
    const float wx = q[3] * q[0], wy = q[3] * q[1], wz = q[3] * q[2];
    const float r[9] = {
        1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy),   // column 0
        2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx),   // column 1
        2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy)};  // column 2

    // Translation = p*upm - R * (anchor*upm).
    const float upm = cfg.unitsPerMeter;
    const float a[3] = {cfg.anchor[0] * upm, cfg.anchor[1] * upm, cfg.anchor[2] * upm};
    for (int c = 0; c < 3; ++c) {
        for (int row = 0; row < 3; ++row) out[c * 4 + row] = r[c * 3 + row];
    }
    for (int row = 0; row < 3; ++row) {
        out[12 + row] = p[row] * upm - (r[0 * 3 + row] * a[0] + r[1 * 3 + row] * a[1] + r[2 * 3 + row] * a[2]);
    }
    for (int i = 0; i < 16; ++i) if (!Finite(out[i])) { SetIdentity(out); return false; }
    return true;
}

} // namespace RiddickVR
