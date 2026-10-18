// GH #67 step 1 (sixth correction, ADR-0488): unit tests for the projection
// near-plane extraction (src/hook/projection_near.h).
//
// These pin the two conventions the live capture is graded on:
//   - the classic GL orientation (m[11] < 0): near = m[14] / (m[10] - 1), which
//     is what the mock host's synthetic eye projection uses (measured 0.05);
//   - the z-flipped orientation the shipped 2009 engine actually submits
//     (m[11] > 0): near = -m[14] / (m[10] + 1). The measured wake-87 world-pass
//     matrix (m[10]=1.00391389 m[11]=+1 m[14]=-8.01566) must read 4.0 -- not the
//     previous code's -2048.0078 (= -far, which rejected every load and left the
//     pass trace at `P?`), and the sampled near-plate pass
//     (m[10]=1.00010 m[14]=-0.20001) must read 0.1;
//   - anything not a usable perspective projection (ortho, degenerate near plane,
//     NaN/inf/negative extraction) is reported UNKNOWN (0), never a plausible
//     wrong number -- the opener compares near planes.
#include "projection_near.h"

#include <cmath>
#include <cstdio>

using RiddickVR::ExtractProjectionNear;
using RiddickVR::IsPerspectiveProjection;
using RiddickVR::kProjectionNearUnknown;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "projection near test failed: %s\n", message);
        ++g_failures;
    }
}

// Column-major, as glLoadMatrixf takes it: m[col*4 + row].
static void BuildPerspective(float m[16], float m10, float m11, float m14) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = m10;
    m[11] = m11;
    m[14] = m14;
    m[15] = 0.0f;
}

static bool Near(float a, float b, float tol = 1e-3f) {
    return std::fabs(a - b) < tol;
}

int main() {
    // The mock host's synthetic GL projection (tests/xrsim puzzle seed, also the
    // wake-87 eye_frames.log record): m10=-1.0001 m11=-1 m14=-0.1 -> near 0.05.
    {
        float m[16];
        BuildPerspective(m, -1.0001f, -1.0f, -0.1f);
        Check(IsPerspectiveProjection(m), "the synthetic host projection must be perspective");
        Check(Near(ExtractProjectionNear(m), 0.05f, 1e-4f),
              "the classic GL orientation must extract near = m14/(m10-1) = 0.05");
    }

    // The measured engine WORLD pass: near 4.0 / far 2048.0 in the z-flipped
    // convention. The old formula read -2048 here.
    {
        float m[16];
        BuildPerspective(m, 1.00391389f, 1.0f, -8.01566284f);
        Check(IsPerspectiveProjection(m), "the engine world projection must be perspective");
        const float near = ExtractProjectionNear(m);
        Check(Near(near, 4.0f, 1e-3f),
              "the z-flipped engine world pass must extract near 4.0");
        // The classic form returns -far: a negative near is never a near plane.
        Check(Near(m[14] / (m[10] - 1.0f), -2048.0078f, 1.0f),
              "the classic extraction on the engine matrix must be -far (the bug)");
    }

    // The sampled near-plane-drop pass: near 0.1 / far 2000.1.
    {
        float m[16];
        BuildPerspective(m, 1.00010f, 1.0f, -0.20001f);
        Check(Near(ExtractProjectionNear(m), 0.1f, 1e-3f),
              "the z-flipped near-plane drop pass must extract near 0.1");
    }

    // The third measured matrix: near 1.8 / far 72002.
    {
        float m[16];
        BuildPerspective(m, 1.00005f, 1.0f, -3.60010f);
        Check(Near(ExtractProjectionNear(m), 1.8f, 1e-3f),
              "the z-flipped wide-depth pass must extract near 1.8");
    }

    // Both conventions agree that a bigger f/n ratio means a smaller near plane:
    // the NearPlaneDrop opener's comparison must be on real planes.
    {
        float world[16];
        float fp[16];
        BuildPerspective(world, 1.00391389f, 1.0f, -8.01566284f);
        BuildPerspective(fp, 1.00010f, 1.0f, -0.20001f);
        Check(ExtractProjectionNear(fp) < ExtractProjectionNear(world),
              "the near-plane drop pass must read a nearer plane than the world pass");
    }

    // An orthographic projection is not a perspective one (m15 == 1).
    {
        float m[16];
        BuildPerspective(m, -1.0f, -1.0f, -0.1f);
        m[15] = 1.0f;
        Check(!IsPerspectiveProjection(m), "an ortho matrix must not be perspective");
        Check(ExtractProjectionNear(m) == kProjectionNearUnknown,
              "an ortho matrix must extract no near plane");
    }

    // Degenerate / garbage input is UNKNOWN, never a number.
    {
        Check(ExtractProjectionNear(nullptr) == kProjectionNearUnknown,
              "a null matrix must extract no near plane");

        float degenerate[16];
        BuildPerspective(degenerate, 1.0f, 1.0f, 0.0f);   // m14 == 0 -> near 0
        Check(ExtractProjectionNear(degenerate) == kProjectionNearUnknown,
              "a zero m14 must extract no near plane");

        float nan[16];
        BuildPerspective(nan, 1.0039f, 1.0f, -8.0f);
        nan[14] = std::nanf("");
        Check(ExtractProjectionNear(nan) == kProjectionNearUnknown,
              "a NaN m14 must extract no near plane");

        float inf[16];
        BuildPerspective(inf, 1.0039f, 1.0f, -8.0f);
        inf[14] = INFINITY;
        Check(ExtractProjectionNear(inf) == kProjectionNearUnknown,
              "an infinite m14 must extract no near plane");

        // A classic-orientation matrix whose extraction is negative (a sign error
        // in the engine's own matrix) must not be reported as a near plane.
        float negative[16];
        BuildPerspective(negative, -1.0039f, -1.0f, 8.0f);   // near = 8/-2.0039 < 0
        Check(ExtractProjectionNear(negative) == kProjectionNearUnknown,
              "a negative extraction must be rejected, not returned");
    }

    if (g_failures == 0) {
        std::printf("projection near: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "projection near: %d failure(s)\n", g_failures);
    return 1;
}
