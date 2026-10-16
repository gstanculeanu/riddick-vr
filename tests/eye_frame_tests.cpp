// GH #68 requirement 1 (second half): unit tests for the mock host's per-dump
// eye pose/projection record (src/host/eye_frame.h).
//
// The harness already dumps both eyes and labels each frame's display branch;
// what a capture still could not say is *which camera transform* produced it.
// `eye_frames.log` answers that, and GH #67 step 3 consumes it. This test pins
// the wire format the Python parser (tools/xrsim.py) relies on, without a GPU.
#include "eye_frame.h"

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

using RiddickVR::EyeFrameSample;
using RiddickVR::FormatEyeFrameLine;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "eye frame test failed: %s\n", message);
        ++g_failures;
    }
}

// Split the line into top-level whitespace-separated fields. Spaces *inside*
// `[...]` belong to the matrix token (the format is space-separated there), so
// a bracket-depth counter keeps `viewL=[m0 .. m15]` as one field.
static std::vector<std::string> Fields(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : line) {
        if (c == '[') {
            ++depth;
            cur.push_back(c);
        } else if (c == ']') {
            if (depth > 0) --depth;
            cur.push_back(c);
        } else if (c == ' ' && depth == 0) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool StartsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

// Parse "name=[m0 m1 ... m15]" into 16 floats.
static bool ParseMatrix(const std::string& tok, float* out) {
    const size_t lb = tok.find('[');
    const size_t rb = tok.rfind(']');
    if (lb == std::string::npos || rb == std::string::npos || rb <= lb) return false;
    std::string body = tok.substr(lb + 1, rb - lb - 1);
    int n = 0;
    std::string cur;
    for (size_t i = 0; i <= body.size(); ++i) {
        if (i == body.size() || body[i] == ' ') {
            if (!cur.empty()) {
                if (n >= 16) return false;
                out[n++] = std::stof(cur);
                cur.clear();
            }
        } else {
            cur.push_back(body[i]);
        }
    }
    return n == 16;
}

static int CountChar(const std::string& s, char c) {
    int n = 0;
    for (char x : s) if (x == c) ++n;
    return n;
}

int main() {
    EyeFrameSample s{};
    s.hmdPos[0] = 0.0f; s.hmdPos[1] = 1.70f; s.hmdPos[2] = -0.25f;
    s.hmdQuat[0] = 0.0523f; s.hmdQuat[1] = 0.1045f; s.hmdQuat[2] = -0.0055f; s.hmdQuat[3] = 0.9929f;
    // Identity view with the half-IPD translation, a plausible projection.
    s.viewL[0] = 1.0f; s.viewL[5] = 1.0f; s.viewL[10] = 1.0f; s.viewL[15] = 1.0f;
    s.viewL[12] = -0.0325f;
    s.viewR[0] = 1.0f; s.viewR[5] = 1.0f; s.viewR[10] = 1.0f; s.viewR[15] = 1.0f;
    s.viewR[12] = 0.0325f;
    s.projL[0] = 1.0f; s.projL[5] = 1.0f; s.projL[10] = -1.0001f;
    s.projL[11] = -1.0f; s.projL[14] = -0.1f;
    s.projR[0] = 1.0f; s.projR[5] = 1.0f; s.projR[10] = -1.0001f;
    s.projR[11] = -1.0f; s.projR[14] = -0.1f;

    const std::string line = FormatEyeFrameLine(300, s);

    // Exact field skeleton the Python parser matches.
    Check(StartsWith(line, "frame=300 hmdpos="), "line must open with frame= + hmdpos=");
    Check(line.find(" hmdquat=") != std::string::npos, "line must carry hmdquat=");
    Check(line.find(" viewL=[") != std::string::npos, "line must carry viewL=[");
    Check(line.find(" viewR=[") != std::string::npos, "line must carry viewR=[");
    Check(line.find(" projL=[") != std::string::npos, "line must carry projL=[");
    Check(line.find(" projR=[") != std::string::npos, "line must carry projR=[");

    const auto fields = Fields(line);
    // frame=, hmdpos=, hmdquat=, viewL=, viewR=, projL=, projR=.
    Check(fields.size() == 7, "line must have exactly 7 whitespace-separated fields");

    // The pose tokens are exactly 3 / 4 comma-separated numbers.
    Check(CountChar(fields[0], ',') == 0, "frame token has no commas");
    Check(CountChar(fields[1], ',') == 2, "hmdpos= must hold 3 comma-separated values");
    Check(CountChar(fields[2], ',') == 3, "hmdquat= must hold 4 comma-separated values");

    // The four matrix tokens round-trip to the 16 floats that went in.
    const char* names[4] = {"viewL=", "viewR=", "projL=", "projR="};
    const float* expected[4] = {s.viewL, s.viewR, s.projL, s.projR};
    for (int k = 0; k < 4; ++k) {
        const std::string& tok = fields[3 + k];
        Check(StartsWith(tok, names[k]), "matrix token must be named in order");
        float parsed[16] = {};
        Check(ParseMatrix(tok, parsed), "matrix token must parse to 16 floats");
        for (int i = 0; i < 16; ++i) {
            if (std::fabs(parsed[i] - expected[k][i]) > 1e-3f) {
                std::fprintf(stderr,
                             "eye frame test failed: %s[%d] = %.4f, expected %.4f\n",
                             names[k], i, parsed[i], expected[k][i]);
                ++g_failures;
            }
        }
    }

    // The left/right eye views must be distinguishable (a record of one eye
    // duplicated for both would defeat the whole point of the capture).
    Check(line.find("viewL=[1.0000 0.0000") != std::string::npos,
          "viewL must be a full column-major matrix");
    Check(std::fabs(s.viewL[12] - s.viewR[12]) > 1e-6f,
          "test fixture must carry a non-zero IPD split");

    // Determinism: same sample -> same line.
    Check(FormatEyeFrameLine(300, s) == line, "formatting must be deterministic");

    if (g_failures == 0) {
        std::puts("eye frame tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
