// GH #76 (interim depth-based reprojection, ADR-0501): unit tests for the pure
// warp math in src/hook/depth_parallax.h.
//
// These pin the four pieces the DIBR-lite path depends on:
//   - window depth -> positive view distance for BOTH projection conventions
//     (the shipped engine's z-flipped one and classic GL), using the measured
//     wake-87 world matrix (near 4.0 / far 2048.0) and the mock host's
//     synthetic classic matrix (near 0.05 / far 1000.0);
//   - distance -> per-eye shift in source pixels (monotonic, clamped);
//   - the IPD recovered from the host's per-eye view translations;
//   - the nearest depth of a grid cell (+ a fully-far cell must not read as near).
#include "depth_parallax.h"

#include <cmath>
#include <cstdio>

using RiddickVR::CellMinViewZ;
using RiddickVR::DepthToViewZ;
using RiddickVR::EyeBaselineMeters;
using RiddickVR::FocalPixelsFromProjection;
using RiddickVR::kMaxParallaxShiftPx;
using RiddickVR::ParallaxShiftPx;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "depth parallax test failed: %s\n", message);
        ++g_failures;
    }
}

static bool Near(float a, float b, float tol = 1e-3f) {
    return std::fabs(a - b) < tol;
}

// Column-major, as glLoadMatrixf takes it: m[col*4 + row].
static void BuildPerspective(float m[16], float m00, float m10, float m11, float m14) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = m00;
    m[5] = m00;
    m[10] = m10;
    m[11] = m11;
    m[14] = m14;
    m[15] = 0.0f;
}

int main() {
    // The measured engine WORLD pass (wake 87): z-flipped, near 4.0 / far 2048.0.
    float engine[16];
    BuildPerspective(engine, 1.0f, 1.00391389f, 1.0f, -8.01566284f);
    {
        Check(Near(DepthToViewZ(0.0f, engine), 4.0f, 1e-2f),
              "window depth 0 must read the z-flipped near plane (4.0)");
        Check(Near(DepthToViewZ(1.0f, engine), 2048.0f, 1.0f),
              "window depth 1 must read the z-flipped far plane (2048.0)");
        // Mid-buffer must be between the planes and monotonically increasing.
        const float mid = DepthToViewZ(0.5f, engine);
        Check(mid > 4.0f && mid < 2048.0f, "mid depth must lie between near and far");
        Check(DepthToViewZ(0.25f, engine) < mid, "smaller depth must be nearer");
    }

    // The mock host's synthetic classic GL projection: near 0.05 / far 1000.0.
    float classic[16];
    BuildPerspective(classic, 1.0f, -1.0001f, -1.0f, -0.1f);
    {
        Check(Near(DepthToViewZ(0.0f, classic), 0.05f, 1e-3f),
              "classic GL depth 0 must read near 0.05");
        Check(DepthToViewZ(0.5f, classic) > DepthToViewZ(0.0f, classic),
              "classic GL depth must increase with window depth");
        Check(DepthToViewZ(1.0f, classic) > 100.0f,
              "classic GL depth 1 must be far");
    }

    // Unusable depth is UNKNOWN (0), never a plausible wrong number.
    {
        Check(DepthToViewZ(-0.1f, engine) == 0.0f, "negative depth must be unknown");
        Check(DepthToViewZ(1.5f, engine) == 0.0f, "depth > 1 must be unknown");
        Check(DepthToViewZ(0.5f, nullptr) == 0.0f, "a null projection must be unknown");
        float ortho[16];
        BuildPerspective(ortho, 1.0f, 1.0f, 1.0f, -8.0f);
        ortho[15] = 1.0f;
        Check(DepthToViewZ(0.5f, ortho) == 0.0f, "an ortho matrix must be unknown");
        float nan[16];
        BuildPerspective(nan, 1.0f, 1.0039f, 1.0f, -8.0f);
        nan[14] = std::nanf("");
        Check(DepthToViewZ(0.5f, nan) == 0.0f, "a NaN m14 must be unknown");
    }

    // The per-eye shift: fPx * baseline / z, monotonic, clamped, clamped away
    // from nonsense inputs.
    {
        const float fPx = 213.0f;     // 426 px source, 90 deg -> 1.0 * 426/2
        const float ipd = 0.065f;
        const float nearShift = ParallaxShiftPx(2.0f, fPx, ipd, 1.0f);   // ~6.9 px
        const float farShift = ParallaxShiftPx(64.0f, fPx, ipd, 1.0f);   // ~0.2 px
        Check(Near(nearShift, fPx * ipd / 2.0f, 1e-3f), "near shift must be fPx*b/z");
        Check(nearShift > farShift, "a nearer surface must shift more");
        Check(ParallaxShiftPx(0.0f, fPx, ipd, 1.0f) == 0.0f, "z=0 must shift 0");
        Check(ParallaxShiftPx(2.0f, 0.0f, ipd, 1.0f) == 0.0f, "fPx=0 must shift 0");
        Check(ParallaxShiftPx(2.0f, fPx, 0.0f, 1.0f) == 0.0f, "baseline=0 must shift 0");
        // Extremely near surface: clamped, not a giant smear.
        Check(ParallaxShiftPx(0.02f, 2000.0f, ipd, 1.0f) == kMaxParallaxShiftPx,
              "an extreme near surface must clamp to kMaxParallaxShiftPx");
        // Strength scales the warp.
        Check(Near(ParallaxShiftPx(2.0f, fPx, ipd, 2.0f), 2.0f * nearShift, 1e-2f),
              "strength 2 must double the shift");
    }

    // The IPD is read from the host's per-eye view translations.
    {
        float lv[16], rv[16];
        BuildPerspective(lv, 1.0f, -1.0f, -1.0f, -0.1f);
        BuildPerspective(rv, 1.0f, -1.0f, -1.0f, -0.1f);
        lv[12] = -0.0325f;
        rv[12] = +0.0325f;
        Check(Near(EyeBaselineMeters(lv, rv, 0.06f), 0.065f, 1e-4f),
              "the baseline must be the eye-view translation difference");
        Check(Near(EyeBaselineMeters(nullptr, rv, 0.06f), 0.06f, 1e-6f),
              "a missing pair must use the fallback");
        // Identical views (no real IPD) fall back rather than report 0.
        Check(Near(EyeBaselineMeters(lv, lv, 0.06f), 0.06f, 1e-6f),
              "coincident eyes must use the fallback, not a zero baseline");
    }

    // The focal length in source pixels comes from m[0] * width / 2.
    {
        Check(Near(FocalPixelsFromProjection(engine, 426), 213.0f, 1e-3f),
              "fPx must be m00 * width / 2");
        Check(FocalPixelsFromProjection(nullptr, 426) == 0.0f, "null projection -> fPx 0");
        Check(FocalPixelsFromProjection(engine, 0) == 0.0f, "zero width -> fPx 0");
    }

    // The nearest depth of a cell: a foreground corner wins, a far-only cell is
    // far (never mis-read as near), an all-clear cell is unknown.
    {
        const int w = 4, h = 2;
        // Two row-major rows of window depth. Row 0 (bottom) is far (1.0),
        // row 1 (top) has one near sample at x=2.
        float depth[w * h] = { 1.0f, 1.0f, 1.0f, 1.0f,
                               1.0f, 1.0f, 0.10f, 1.0f };
        const float farZ = DepthToViewZ(1.0f, engine);
        const float nearZ = DepthToViewZ(0.10f, engine);
        Check(nearZ < farZ, "the synthetic near sample must be nearer than far");
        Check(Near(CellMinViewZ(depth, w, h, 0, 0, 4, 2, engine), nearZ, 1e-2f),
              "a cell containing a near sample must read the nearest depth");
        Check(Near(CellMinViewZ(depth, w, h, 0, 0, 2, 1, engine), farZ, 1.0f),
              "a cell with only far samples must read far, not near");
        float allFar[w * h];
        for (int i = 0; i < w * h; ++i) allFar[i] = 1.0f;
        Check(Near(CellMinViewZ(allFar, w, h, 0, 0, w, h, engine), farZ, 1.0f),
              "an all-far cell must read the far plane");
        Check(CellMinViewZ(nullptr, w, h, 0, 0, 1, 1, engine) == 0.0f,
              "a null buffer must be unknown");
        // Out-of-range rectangles are clamped, not read out of bounds.
        Check(CellMinViewZ(depth, w, h, -5, -5, 99, 99, engine) > 0.0f,
              "an oversized rect must clamp to the buffer");
    }

    if (g_failures == 0) {
        std::printf("depth parallax: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "depth parallax: %d failure(s)\n", g_failures);
    return 1;
}
