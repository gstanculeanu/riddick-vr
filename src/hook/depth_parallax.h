#pragma once

// GH #76 (epic #57 definition-of-done item 1): the *interim* depth-based
// reprojection ("DIBR-lite") for native stereo.
//
// Why this exists: the shipped renderer captures the game's backbuffer ONCE and
// blits it into both eye FBOs with a single global horizontal source-rect shift
// (`stereo_renderer.cpp`, half-IPD + comfort), so the disparity is the same for
// near and far content -- there is no depth-dependent parallax and XRSIM
// classifies every gameplay frame `stereo_kind: shift-only` (ADR-0461).
//
// True per-eye view/projection (item 1) needs the engine to render the 3D scene
// twice with the camera offset by +/-IPD/2. On this title that route is
// *architecturally* blocked: the engine applies its object/view transforms
// engine-side with an identity GL modelview (ADR-0481/ADR-0482), head-look is
// injected as mouse movement, and `CameraHook::StabilizeViewMatrix` -- the
// "intended seam" of ADR-0406 -- is measured dead code (never called). The hook
// therefore has no way to ask the engine for a second view.
//
// The interim the issue names (`GH #76` option 2) is a *depth-based*
// reprojection: keep the single capture but make each region's horizontal shift
// depend on the scene depth in that region, so the disparity finally varies with
// depth. These are the pure, headless-testable pieces of that warp:
//   * window depth -> positive view-space distance, using the *engine's own*
//     (z-flipped) projection so the convention is honoured;
//   * distance -> per-eye horizontal shift in source pixels;
//   * the inter-pupillary baseline from the host's per-eye view translations;
//   * the nearest (foreground) depth of a grid cell.
//
// No GL/Windows headers so it compiles on the x64 host and is unit-tested
// headlessly (tests/depth_parallax_tests.cpp).

#include <cmath>
#include <cstddef>

namespace RiddickVR {

// Comfort/sanity clamp on the per-eye shift, in *source* pixels. A bad depth
// sample (e.g. z just above the guard) must never smear the whole image.
constexpr float kMaxParallaxShiftPx = 128.0f;

// Guard below which a view-space distance is treated as "unknown" rather than
// "very near" (a 1 cm surface would otherwise produce a giant shift).
constexpr float kMinViewZMeters = 0.01f;

// One window depth sample (OpenGL [0,1], 0 = near plane) -> positive view-space
// distance in engine units, or 0 when the sample is not usable.
//
// The engine's projection is z-flipped relative to classic GL (ADR-0488): its
// m[11] > 0 and w_clip = +z_eye, so `z_ndc = m[10] + m[14]/z_eye`. Classic GL
// (m[11] < 0, w_clip = -z_eye) is handled too, and its sign is made positive.
inline float DepthToViewZ(float windowDepth, const float* proj) {
    if (!proj) return 0.0f;
    if (!(windowDepth >= 0.0f) || !(windowDepth <= 1.0f)) return 0.0f;
    if (!(std::fabs(proj[15]) < 0.01f)) return 0.0f; // perspective only

    const float m10 = proj[10];
    const float m14 = proj[14];
    const float m11 = proj[11];
    const float ndc = 2.0f * windowDepth - 1.0f;

    float z;
    if (m11 >= 0.0f) {
        const float denom = ndc - m10; // measured engine convention
        if (!(std::fabs(denom) > 1.0e-9f)) return 0.0f;
        z = m14 / denom;
    } else {
        const float denom = -m10 - ndc; // classic GL, yields a negative eye z
        if (!(std::fabs(denom) > 1.0e-9f)) return 0.0f;
        z = -m14 / denom;
    }
    if (!(z == z) || !(z > kMinViewZMeters)) return 0.0f;
    return z;
}

// The full inter-pupillary disparity in *source* pixels for a surface at
// `viewZ` metres: fPx * baseline / viewZ. `fPx` is the engine projection's
// focal length in source pixels (m[0] * sourceWidth / 2), `baselineM` the IPD.
// `strength` scales the whole warp (1.0 = physically correct). Clamped to
// +/-kMaxParallaxShiftPx. Returns 0 for a non-positive input.
inline float ParallaxShiftPx(float viewZ, float fPx, float baselineM, float strength) {
    if (!(viewZ > kMinViewZMeters) || !(fPx > 0.0f) || !(baselineM > 0.0f) || !(strength > 0.0f))
        return 0.0f;
    float p = fPx * baselineM / viewZ * strength;
    if (!(p == p)) return 0.0f;
    if (p > kMaxParallaxShiftPx) p = kMaxParallaxShiftPx;
    return p;
}

// The inter-pupillary distance implied by the host's per-eye view translations.
// Falls back when the pair is missing (or implausibly small/large), so a hook
// that has not been given real eye views still gets a usable baseline.
inline float EyeBaselineMeters(const float* leftView, const float* rightView, float fallback) {
    if (!leftView || !rightView) return fallback;
    const float dx = rightView[12] - leftView[12];
    const float dy = rightView[13] - leftView[13];
    const float dz = rightView[14] - leftView[14];
    const float b = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(b > 0.001f) || !(b < 0.5f)) return fallback;
    return b;
}

// The engine camera's focal length in source pixels: m[0] * (width / 2).
// 0 when the projection is not usable.
inline float FocalPixelsFromProjection(const float* proj, int sourceWidth) {
    if (!proj || sourceWidth <= 0) return 0.0f;
    if (!(std::fabs(proj[15]) < 0.01f)) return 0.0f;
    const float f = proj[0] * (static_cast<float>(sourceWidth) * 0.5f);
    if (!(f == f) || !(f > 1.0f)) return 0.0f;
    return f;
}

// The nearest (minimum) positive view-space distance over a rectangle of a
// row-major W x H window-depth buffer, or 0 when no sample in it is usable.
// Nearest is deliberate: a cell's foreground surface is what should dominate
// that cell's parallax, and it keeps background depth from bleeding through a
// thin foreground object.
inline float CellMinViewZ(const float* depth, int w, int h,
                          int x0, int y0, int x1, int y1, const float* proj) {
    if (!depth || w <= 0 || h <= 0) return 0.0f;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    float best = 0.0f;
    for (int y = y0; y < y1; ++y) {
        const float* row = depth + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        for (int x = x0; x < x1; ++x) {
            const float z = DepthToViewZ(row[x], proj);
            if (z > 0.0f && (best == 0.0f || z < best)) best = z;
        }
    }
    return best;
}

} // namespace RiddickVR
