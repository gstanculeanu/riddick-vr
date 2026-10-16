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
// The mock host therefore writes one machine-readable line per *dumped* frame
// to `<dump-dir>\eye_frames.log` (same truncate-then-append pattern as
// `display_branch.log`). The formatting is a pure, dependency-free helper so
// `tests/eye_frame_tests.cpp` can pin the wire format without a GPU, and
// `tools/xrsim.py` parses it back for the report.
//
// Format (one line per dumped frame):
//   frame=NNNN hmdpos=x,y,z hmdquat=x,y,z,w \
//     viewL=[m0 .. m15] viewR=[m0 .. m15] projL=[m0 .. m15] projR=[m0 .. m15]
//
// Matrices are column-major (the same layout the shared-memory protocol and
// OpenGL use) and space-separated inside `[...]`, rounded to 4 decimals.

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace RiddickVR {

// One frame's camera transform: the mock host's published HMD pose and the
// per-eye view (world -> eye) and projection matrices.
struct EyeFrameSample {
    float hmdPos[3] = {0.0f, 0.0f, 0.0f};
    float hmdQuat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float viewL[16] = {};
    float viewR[16] = {};
    float projL[16] = {};
    float projR[16] = {};
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
    return os.str();
}

}  // namespace RiddickVR
