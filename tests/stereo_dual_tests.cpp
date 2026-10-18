// GH #76 (epic #57 DoD item 1) / GH #90: unit tests for the dual per-eye pass
// policy (stereo_dual.h). Headless, no GL, no game launch.
//
// The decisive assertions are the two decisions the pass rests on:
//   1. only *world* (non-near-field, decodable) draws are duplicated per eye, so
//      the HUD/screen-space draws and the engine's own viewmodel are never
//      translated by an eye-space offset; and
//   2. a frame with a black eye is refused, so the opt-in can never replace the
//      shipped shifted capture with an empty one.
#include "stereo_dual.h"

#include <cstdio>

using RiddickVR::DualFrameUsable;
using RiddickVR::DualNonBlackFraction;
using RiddickVR::DualRenderDrawEligible;
using RiddickVR::IsNearFieldSample;
using RiddickVR::kDualMinEyeCoverage;
using RiddickVR::kNearFieldMaxAbs;
using RiddickVR::VertexSample;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "stereo dual test failed: %s\n", message);
        ++g_failures;
    }
}

static VertexSample MakeSample(bool valid, float lo, float hi, unsigned count = 128u) {
    VertexSample s;
    s.valid = valid;
    s.count = count;
    for (int c = 0; c < 3; ++c) {
        s.min[c] = lo;
        s.max[c] = hi;
    }
    return s;
}

int main() {
    // 1. An undecodable draw is never classified as world geometry (it is left to
    //    the fail-safe gate instead of being guessed at).
    {
        VertexSample invalid = MakeSample(false, -2.0e3f, 2.0e3f, 4096u);
        Check(!DualRenderDrawEligible(invalid),
              "an undecodable draw must not be duplicated per eye");
    }

    // 2. Measured world geometry (ADR-0490: eye-space |coord| ~ 1.6e3..3.5e3) is
    //    eligible.
    {
        VertexSample world = MakeSample(true, 1600.0f, 3500.0f, 14688u);
        Check(DualRenderDrawEligible(world),
              "a decodable eye-space world mesh must be duplicated per eye");
    }

    // 3. The first-person window's flat screen-space quad (ADR-0482:
    //    min=(-0.5,-0.5,0) max=(0.5,0.5,0)) is NOT eligible: its coordinates are
    //    not metres, so an eye-space translation would be a meaningless screen
    //    slide.
    {
        VertexSample hudQuad = MakeSample(true, -0.5f, 0.5f, 4u);
        Check(!DualRenderDrawEligible(hudQuad),
              "a screen-space quad must not be eye-translated");
    }

    // 4. A draw exactly at the near-field boundary is excluded; just above it is
    //    included (the threshold is the measured separator, ADR-0490).
    {
        VertexSample atEdge = MakeSample(true, -kNearFieldMaxAbs, kNearFieldMaxAbs, 32u);
        Check(!DualRenderDrawEligible(atEdge), "the near-field boundary is excluded");
        Check(IsNearFieldSample(atEdge), "the boundary sample is near-field");
        VertexSample above = MakeSample(true, -kNearFieldMaxAbs - 0.001f, 0.0f, 32u);
        Check(DualRenderDrawEligible(above), "just beyond the near field is eligible");
    }

    // 5. The engine's own first-person viewmodel (measured near-field at ~+/-0.45
    //    units) is excluded -- it is replaced by the VR hands (#61/#64), so this is
    //    the GH #63 outcome rather than a loss.
    {
        VertexSample viewmodel = MakeSample(true, -0.45f, 0.45f, 526u);
        Check(!DualRenderDrawEligible(viewmodel),
              "the engine viewmodel must not be duplicated per eye");
    }

    // 6. Coverage: all-black is 0, a lit texel counts, alpha alone does not.
    {
        unsigned char black[4 * 4] = {0};
        Check(DualNonBlackFraction(black, 4) == 0.0f, "a black frame has zero coverage");

        unsigned char one[4 * 4] = {0};
        one[4 * 2 + 1] = 200;  // second texel, green
        Check(DualNonBlackFraction(one, 4) == 0.25f, "one lit texel of four is 25%");

        unsigned char alphaOnly[4 * 4] = {0};
        alphaOnly[4 * 3 + 3] = 255;  // alpha only: not content
        Check(DualNonBlackFraction(alphaOnly, 4) == 0.0f,
              "alpha alone is not visible content");

        unsigned char dim[4] = {14, 14, 14, 255};  // at the floor, not above it
        Check(DualNonBlackFraction(dim, 1) == 0.0f, "the 14/255 floor is not content");
        unsigned char lit[4] = {15, 0, 0, 255};
        Check(DualNonBlackFraction(lit, 1) == 1.0f, "above the floor is content");
        Check(DualNonBlackFraction(nullptr, 10) == 0.0f, "a null buffer is zero coverage");
        Check(DualNonBlackFraction(black, 0) == 0.0f, "an empty buffer is zero coverage");
    }

    // 7. The gate: both eyes must carry content.
    {
        Check(!DualFrameUsable(0.0f, 0.0f), "a black pair is refused");
        Check(!DualFrameUsable(0.9f, 0.0f), "one black eye is refused");
        Check(!DualFrameUsable(kDualMinEyeCoverage * 0.5f, 0.9f),
              "coverage below the floor is refused");
        Check(DualFrameUsable(kDualMinEyeCoverage, kDualMinEyeCoverage),
              "coverage at the floor is accepted");
        Check(DualFrameUsable(0.8f, 0.8f), "a normal pair is accepted");
    }

    if (g_failures == 0) {
        std::puts("stereo dual tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
