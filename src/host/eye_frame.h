#pragma once

// GH #68 requirement 1 (second half): record the eye pose / projection used for
// each captured frame.
//
// The XRSIM harness already dumps both eyes (`test_frame_####.bmp` +
// `right_####.bmp`) and labels each frame's display branch
// (`display_branch.log`, GH #69). What was still missing is the *transform*:
// which head/eye pose and projection produced that frame. Without it a capture
// cannot be interpreted in 3D -- in particular GH #67 step 3 ("re-express the
// engine's first-person viewmodel in head/controller space") needs the camera
// transform the hook injected for the sampled frame so the captured
// `GL_MODELVIEW` can be made camera-relative.
//
// The host therefore writes one machine-readable line per *dumped* frame
// to `<dump-dir>\eye_frames.log` (same truncate-then-append pattern as
// `display_branch.log`). The formatting is a pure, dependency-free helper so
// `tests/eye_frame_tests.cpp` can pin the wire format without a GPU, and
// `tools/xrsim.py` parses it back for the report.
//
// Format (one line per dumped frame):
//   frame=NNNN hmdpos=x,y,z hmdquat=x,y,z,w \
//     viewL=[m0 .. m15] viewR=[m0 .. m15] projL=[m0 .. m15] projR=[m0 .. m15] \
//     eye=<runtime|sim|synthetic>
//
// Matrices are column-major (the same layout the shared-memory protocol and
// OpenGL use) and space-separated inside `[...]`, rounded to 4 decimals.
//
// GH #77 / ADR-0462+0463: the trailing `eye=` token records *where the four
// matrices came from*, because in the headless mock host they are NOT the
// matrices the hook consumed.
//   eye=runtime    -- returned by a real `xrLocateViews` call (a headset).
//   eye=sim        -- mock/sim has no runtime, so the host *reconstructed* the
//                     per-eye views a runtime would return for this frame's
//                     scripted head pose (`ComputeEyeViewFromHeadPose`). They
//                     do track the head pose (scene-faithful in construction)
//                     but were NOT published to the hook, which still receives
//                     the placeholder baseline.
//   eye=synthetic  -- the constant placeholder baseline (identity rotation,
//                     +/- half-IPD X translation, identical projections): the
//                     hook's published camera input in mock mode, and the
//                     no-`xrLocateViews` fallback of a real session. It carries
//                     no head rotation.
// The authoritative camera *input* is always the head pose (hmdpos/hmdquat),
// which the hook applies; `eye=sim` exists so a sim run can grade the harness's
// scene-faithfulness path without a headset.

#include <cstdint>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

namespace RiddickVR {

// GH #77 / ADR-0463: provenance of a record's four matrices.
enum class EyeViewSource {
    Runtime,    // from a real xrLocateViews call
    Simulated,  // reconstructed by the mock host from the scripted head pose
    Synthetic,  // the constant placeholder baseline
};

inline const char* EyeViewSourceToken(EyeViewSource source) {
    switch (source) {
        case EyeViewSource::Runtime: return "runtime";
        case EyeViewSource::Simulated: return "sim";
        case EyeViewSource::Synthetic: return "synthetic";
    }
    return "synthetic";
}

// Row-major 3x3 rotation from an (x, y, z, w) quaternion. Mirrors the host's
// `CreateViewFromPose` and `tools/xrsim.py::_rot3_from_quat`.
inline void QuatToRowMajor3x3(const float q[4], float r[9]) {
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

// Column-major `world -> eye` view matrix for a rigid pose (inverse of the
// eye's world pose). This is exactly the maths the host's `CreateViewFromPose`
// uses; it is factored out here so it is pure and unit-testable without OpenXR.
inline void ViewMatrixFromPose(const float pos[3], const float quat[4], float out[16]) {
    float r[9];
    QuatToRowMajor3x3(quat, r);
    const float px = pos[0], py = pos[1], pz = pos[2];

    out[0] = r[0]; out[1] = r[1]; out[2] = r[2]; out[3] = 0.0f;
    out[4] = r[3]; out[5] = r[4]; out[6] = r[5]; out[7] = 0.0f;
    out[8] = r[6]; out[9] = r[7]; out[10] = r[8]; out[11] = 0.0f;

    out[12] = -(r[0] * px + r[3] * py + r[6] * pz);
    out[13] = -(r[1] * px + r[4] * py + r[7] * pz);
    out[14] = -(r[2] * px + r[5] * py + r[8] * pz);
    out[15] = 1.0f;
}

// GH #77 / ADR-0463: the per-eye view a runtime WOULD return for a head pose.
// Both eye poses share the head orientation (all supported runtimes report it);
// the eye origin is the head origin offset by `+/-halfIpd` metres along the
// head's local +X (right-eye positive), i.e. `eye = head + R_head * (+/-h, 0, 0)`.
// The output is the column-major `world -> eye` view matrix.
inline void ComputeEyeViewFromHeadPose(const float headPos[3], const float headQuat[4],
                                       float halfIpd, bool rightEye, float out[16]) {
    float r[9];
    QuatToRowMajor3x3(headQuat, r);
    const float s = rightEye ? halfIpd : -halfIpd;  // local +X offset (metres)
    const float eye[3] = {
        headPos[0] + r[0] * s,
        headPos[1] + r[3] * s,
        headPos[2] + r[6] * s,
    };
    ViewMatrixFromPose(eye, headQuat, out);
}

// One frame's camera transform: the host's published HMD pose and the
// per-eye view (world -> eye) and projection matrices.
struct EyeFrameSample {
    float hmdPos[3] = {0.0f, 0.0f, 0.0f};
    float hmdQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float viewL[16] = {};
    float viewR[16] = {};
    float projL[16] = {};
    float projR[16] = {};
    // GH #77 / ADR-0463: where the four matrices came from. See the file header.
    EyeViewSource viewSource = EyeViewSource::Synthetic;
};

inline void AppendMatrix(std::ostringstream& os, const float* m) {
    os << '[';
    for (int i = 0; i < 16; ++i) {
        if (i) os << ' ';
        os << m[i];
    }
    os << ']';
}

// Format one `eye_frames.log` line. Deterministic for a given sample (the
// stream is `fixed` + 4 decimals), so tests pin the exact shape and the Python
// parser can rely on it.
inline std::string FormatEyeFrameLine(uint32_t frame, const EyeFrameSample& s) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(4);
    os << "frame=" << frame
       << " hmdpos=" << s.hmdPos[0] << ',' << s.hmdPos[1] << ',' << s.hmdPos[2]
       << " hmdquat=" << s.hmdQuat[0] << ',' << s.hmdQuat[1] << ',' << s.hmdQuat[2]
       << ',' << s.hmdQuat[3];
    os << " viewL=";
    AppendMatrix(os, s.viewL);
    os << " viewR=";
    AppendMatrix(os, s.viewR);
    os << " projL=";
    AppendMatrix(os, s.projL);
    os << " projR=";
    AppendMatrix(os, s.projR);
    os << " eye=" << EyeViewSourceToken(s.viewSource);
    return os.str();
}

}  // namespace RiddickVR
