#pragma once

// GH #76 (epic #57 definition-of-done item 1) **option 1**: native stereo by
// replaying the engine's own eye-space geometry per eye.
//
// Where this comes from (all read-only + live, no install write):
//   * The shipped renderer captures ONE backbuffer and blits it into both eyes
//     with a single global half-IPD source shift, so every gameplay frame is
//     `stereo_kind: shift-only` (ADR-0461). That is the top gap of the port.
//   * The issue's interim option 2 (a depth-based reprojection) was implemented
//     and then had its depth source measured *absent* on this title: no
//     hook-readable framebuffer holds the near world pass (ADR-0501/0502/0503).
//     So the depth route is closed and option 1 is the only route left.
//   * Option 1 was previously called "architecturally blocked" because the
//     engine applies its object/view transform engine-side with an identity
//     `GL_MODELVIEW` (ADR-0481/0482), and `CameraHook::StabilizeViewMatrix` is
//     dead code. But that same measurement is exactly what makes a *replay*
//     possible: the engine's geometry reaches GL as **eye-space client arrays**
//     (`glDrawElements`; RndrGL binds no `glDrawArrays`, no `glMultMatrixf`),
//     so the whole camera transform is a *rigid translation of every eye-space
//     vertex*. Moving the eye from the head origin to the left/right eye is
//     therefore a pure `glTranslatef` on the modelview at replay time --
//     mathematically identical to a real per-eye camera.
//
// This header is the pure, headless-testable first slice of that route: it
// converts the host's per-eye views (or the IPD fallback) into the eye-space
// camera offset the replay must apply, builds the replay `GL_MODELVIEW`, and
// provides an independent residual check so the underlying assumption can be
// *measured* rather than assumed.
//
// It changes no rendering; it is the maths the (separately scoped) draw-recorder
// + replay will consume, and `StereoReplayProbe=1` logs it live.
//
// No GL/Windows headers, so it compiles on the x64 host and is unit-tested
// headlessly (tests/stereo_replay_tests.cpp).

#include <cmath>

namespace RiddickVR {

// Column-major 3x3 rotation from an (x, y, z, w) quaternion. Mirrors the host's
// `CreateViewFromPose` and `eye_frame.h::QuatToRowMajor3x3`; the unit test
// cross-checks the two independent implementations agree.
inline void ReplayQuatToRowMajor3x3(const float q[4], float r[9]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1.0f - 2.0f * (y * y + z * z);
    r[1] = 2.0f * (x * y - z * w);
    r[2] = 2.0f * (x * z + y * w);
    r[3] = 2.0f * (x * y + z * w);
    r[4] = 1.0f - 2.0f * (x * x + z * z);
    r[5] = 2.0f * (y * z - x * w);
    r[6] = 2.0f * (x * z - y * w);
    r[7] = 2.0f * (y * z + x * w);
    r[8] = 1.0f - 2.0f * (x * x + y * y);
}

// Column-major `world -> eye` view matrix for a rigid pose (the inverse of the
// eye's world pose). Identical maths to `eye_frame.h::ViewMatrixFromPose`.
inline void ReplayViewFromPose(const float pos[3], const float quat[4], float out[16]) {
    float r[9];
    ReplayQuatToRowMajor3x3(quat, r);
    const float px = pos[0], py = pos[1], pz = pos[2];

    out[0] = r[0]; out[1] = r[1]; out[2] = r[2]; out[3] = 0.0f;
    out[4] = r[3]; out[5] = r[4]; out[6] = r[5]; out[7] = 0.0f;
    out[8] = r[6]; out[9] = r[7]; out[10] = r[8]; out[11] = 0.0f;

    out[12] = -(r[0] * px + r[3] * py + r[6] * pz);
    out[13] = -(r[1] * px + r[4] * py + r[7] * pz);
    out[14] = -(r[2] * px + r[5] * py + r[8] * pz);
    out[15] = 1.0f;
}

// A real per-eye offset is a fraction of an IPD; anything larger means the view
// matrix is not a head-relative eye (e.g. the mock host's placeholder baseline,
// whose origin is the world origin). ADR-0463 / GH #77.
constexpr float kReplayMaxEyeOffsetMeters = 0.25f;
// Element tolerance when comparing two view rotations (unitless matrix entries).
constexpr float kReplayRotationTol = 5.0e-3f;

// True when `eyeView`'s rotation matches `headView`'s (the translation is
// ignored). A head-aligned eye shares the head view's rotation, so a mismatch
// means the view was not built from this head pose.
inline bool ReplayViewRotationMatchesHead(const float* eyeView, const float* headView, float tol) {
    if (!eyeView || !headView) return false;
    const int idx[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};  // 3x3 rotation, column-major
    for (int k = 0; k < 9; ++k) {
        if (std::fabs(eyeView[idx[k]] - headView[idx[k]]) > tol) return false;
    }
    return true;
}

// The eye's world position recovered from a `world -> eye` view matrix:
// `t = -R_eye^T * eyePos`, so `eyePos = -R_eye^T * t` (R is orthonormal, so its
// transpose is its inverse). Returns false for a null/degenerate matrix.
inline bool ReplayEyePositionFromView(const float view[16], float out[3]) {
    if (!view) return false;
    const float t0 = view[12], t1 = view[13], t2 = view[14];
    if (!(t0 == t0) || !(t1 == t1) || !(t2 == t2)) return false;
    // eyePos = -R^T t with R = the view's upper-left 3x3, stored column-major, so
    // row i of R is (view[i*4+0], view[i*4+1], view[i*4+2]).
    out[0] = -(view[0] * t0 + view[1] * t1 + view[2] * t2);
    out[1] = -(view[4] * t0 + view[5] * t1 + view[6] * t2);
    out[2] = -(view[8] * t0 + view[9] * t1 + view[10] * t2);
    return true;
}

// The camera translation from `headPos` to `eyePos`, expressed in the engine's
// (head-centred) eye space: `d_eye = R_head^T (eyePos - headPos)`. Applying
// `-d_eye` as a modelview translation renders the engine's eye-space vertices
// as seen from that eye. Returns false for a null/garbage quaternion.
inline bool EyeSpaceCameraOffset(const float headPos[3], const float headQuat[4],
                                 const float eyePos[3], float out[3]) {
    if (!headPos || !headQuat || !eyePos || !out) return false;
    const float qn = headQuat[0] * headQuat[0] + headQuat[1] * headQuat[1] +
                     headQuat[2] * headQuat[2] + headQuat[3] * headQuat[3];
    if (!(qn == qn) || qn < 1.0e-8f) return false;
    float r[9];
    ReplayQuatToRowMajor3x3(headQuat, r);
    const float dx = eyePos[0] - headPos[0];
    const float dy = eyePos[1] - headPos[1];
    const float dz = eyePos[2] - headPos[2];
    // R^T: row i of R is the i-th basis vector; eye-space = R^T * world-delta.
    out[0] = r[0] * dx + r[3] * dy + r[6] * dz;
    out[1] = r[1] * dx + r[4] * dy + r[7] * dz;
    out[2] = r[2] * dx + r[5] * dy + r[8] * dz;
    return true;
}

// The per-eye camera offset in eye space, preferring the host's *measured*
// per-eye views and falling back to `+/-halfIpd` along the head's local +X
// (right eye positive). `rightEye` selects which view/fallback side to use.
//
// The views are only trusted when they are *scene-faithful*: the view's rotation
// must match the head pose's and its recovered eye origin must lie within
// `kReplayMaxEyeOffsetMeters` of the head. The mock host publishes the synthetic
// placeholder baseline to the hook (its origin is the world origin, not the head;
// ADR-0463 / GH #77), so without this gate the recovered offset is nonsense
// (`~ -headPos`). When the gate fails the helper falls back to the head-local
// IPD, which is exactly what a runtime that reports head-aligned eyes gives.
// `usedMeasuredViewsOut`, when non-null, reports which source was used.
// Returns false only when no usable input exists at all.
inline bool EyeSpaceCameraOffsetFromViews(const float* viewL, const float* viewR,
                                          const float headPos[3], const float headQuat[4],
                                          bool rightEye, float halfIpdFallbackM, float out[3],
                                          bool* usedMeasuredViewsOut = nullptr) {
    if (usedMeasuredViewsOut) *usedMeasuredViewsOut = false;
    if (!headPos || !headQuat || !out) return false;
    const float qn = headQuat[0] * headQuat[0] + headQuat[1] * headQuat[1] +
                     headQuat[2] * headQuat[2] + headQuat[3] * headQuat[3];
    if (!(qn == qn) || qn < 1.0e-8f) return false;

    const float* view = rightEye ? viewR : viewL;
    if (view) {
        float headView[16];
        ReplayViewFromPose(headPos, headQuat, headView);
        float eyePos[3];
        if (ReplayEyePositionFromView(view, eyePos) &&
            ReplayViewRotationMatchesHead(view, headView, kReplayRotationTol)) {
            float off[3];
            if (EyeSpaceCameraOffset(headPos, headQuat, eyePos, off)) {
                const float m = std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
                if (m < kReplayMaxEyeOffsetMeters) {
                    out[0] = off[0];
                    out[1] = off[1];
                    out[2] = off[2];
                    if (usedMeasuredViewsOut) *usedMeasuredViewsOut = true;
                    return true;
                }
            }
        }
    }
    out[0] = rightEye ? halfIpdFallbackM : -halfIpdFallbackM;
    out[1] = 0.0f;
    out[2] = 0.0f;
    return true;
}

// The GL_MODELVIEW to load during the replay so the engine's head-eye-space
// vertices are rendered from the offset eye: a pure translation by `-offset`
// (column-major identity with the negated offset in the last column).
inline void BuildEyeReplayModelview(const float eyeSpaceOffset[3], float out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    if (!eyeSpaceOffset) return;
    out[12] = -eyeSpaceOffset[0];
    out[13] = -eyeSpaceOffset[1];
    out[14] = -eyeSpaceOffset[2];
}

// The IPD implied by the two eye-view translations (`|eyeR - eyeL|`), or
// `fallback` when the pair is missing or implausible. Mirrors
// `depth_parallax.h::EyeBaselineMeters`; kept local so the replay's probe is
// self-contained.
inline float ReplayBaselineMeters(const float* viewL, const float* viewR, float fallback) {
    if (!viewL || !viewR) return fallback;
    const float dx = viewR[12] - viewL[12];
    const float dy = viewR[13] - viewL[13];
    const float dz = viewR[14] - viewL[14];
    const float b = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(b > 0.001f) || !(b < 0.5f)) return fallback;
    return b;
}

// Independent check of the mechanism: the maximum absolute entry difference
// between `translate(-offset) * headView` and `eyeView`. A ~0 residual means the
// per-eye view is exactly a rigid eye-space translation of the head view -- the
// assumption the replay rests on. Used by the unit tests and measured live by
// `StereoReplayProbe=1`.
inline float ReplayViewResidual(const float* headView, const float* eyeView,
                                const float eyeSpaceOffset[3]) {
    if (!headView || !eyeView || !eyeSpaceOffset) return 1.0e9f;
    float m[16];
    BuildEyeReplayModelview(eyeSpaceOffset, m);
    float worst = 0.0f;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            // Column-major product `m * headView`.
            const float a = m[0 * 4 + row] * headView[col * 4 + 0] +
                            m[1 * 4 + row] * headView[col * 4 + 1] +
                            m[2 * 4 + row] * headView[col * 4 + 2] +
                            m[3 * 4 + row] * headView[col * 4 + 3];
            const float d = std::fabs(a - eyeView[col * 4 + row]);
            if (d > worst) worst = d;
        }
    }
    return worst;
}

} // namespace RiddickVR
