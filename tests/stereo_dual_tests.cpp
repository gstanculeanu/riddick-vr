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
using RiddickVR::DualRejectReasonName;
using RiddickVR::DualRenderDrawEligible;
using RiddickVR::DualSampleExtent;
using RiddickVR::DualSampleScale;
using RiddickVR::DualSampleScaleOf;
using RiddickVR::IsNearFieldSample;
using RiddickVR::kDualMidMaxExtent;
using RiddickVR::kDualMinEyeCoverage;
using RiddickVR::kGlFloat;
using RiddickVR::kGlUnsignedShort;
using RiddickVR::kNearFieldMaxAbs;
using RiddickVR::SampleClientVertices;
using RiddickVR::SpanDrawCensus;
using RiddickVR::VertexSample;
using RiddickVR::VertexSampleReason;

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
    // 1. An undecodable draw with *no* generic-attribute path is never classified as
    //    world geometry (it is left to the fail-safe gate instead of being guessed at).
    {
        VertexSample invalid = MakeSample(false, -2.0e3f, 2.0e3f, 4096u);
        Check(!invalid.genericAttributeEnabled, "the default sample uses no generic attributes");
        Check(!DualRenderDrawEligible(invalid),
              "an undecodable draw with no generic attributes must not be duplicated per eye");
    }

    // 1b. GH #76/#90 (ADR-0508): an undecodable draw that used the generic-attribute
    //     path *is* the engine's 3-D geometry (ADR-0507: the fixed-function client
    //     array is null by design; the array is established by glVertexAttribPointer
    //     into buffer objects). Re-issuing it needs no decoded vertices, so the
    //     source path makes it eligible -- with or without a buffer-backed position.
    {
        VertexSample generic = MakeSample(false, 0.0f, 0.0f, 12u);
        generic.genericAttributeEnabled = true;
        generic.genericPositionBufferId = 12u;
        Check(DualRenderDrawEligible(generic),
              "an undecodable generic-attribute draw is duplicated per eye");
        generic.genericPositionBufferId = 0u;  // client memory, still the generic path
        Check(DualRenderDrawEligible(generic),
              "the generic path is eligible even without a buffer-backed position");
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

    // 8. GH #76/#90 (ADR-0506): the span census names the *band* of each draw, and
    //    the "mid" band (between the measured near-field and world bands) is now a
    //    named bucket rather than silently eligible. Invalid samples carry no
    //    extent, so they are "Unknown" and never confused with world geometry.
    {
        VertexSample invalid = MakeSample(false, -5.0f, 5.0f);
        Check(DualSampleExtent(invalid) == 0.0f, "an invalid sample has no extent");
        Check(DualSampleScaleOf(invalid) == DualSampleScale::Unknown,
              "an undecodable draw is the Unknown band");

        Check(DualSampleScaleOf(MakeSample(true, -0.45f, 0.45f)) == DualSampleScale::Near,
              "the viewmodel/HUD band is Near");
        Check(DualSampleScaleOf(MakeSample(true, -500.0f, 500.0f)) == DualSampleScale::Mid,
              "content between near-field and world is the named Mid band");
        Check(DualSampleScaleOf(MakeSample(true, 1600.0f, 3500.0f)) == DualSampleScale::World,
              "measured eye-space geometry is the World band");

        // Band boundaries: <= near-field is Near, <= mid ceiling is Mid.
        Check(DualSampleScaleOf(MakeSample(true, -kNearFieldMaxAbs, kNearFieldMaxAbs)) ==
                  DualSampleScale::Near, "the near-field boundary is Near");
        Check(DualSampleScaleOf(MakeSample(true, -kDualMidMaxExtent, kDualMidMaxExtent)) ==
                  DualSampleScale::Mid, "the mid ceiling is Mid");
        Check(DualSampleScaleOf(MakeSample(true, -kDualMidMaxExtent - 1.0f, kDualMidMaxExtent + 1.0f)) ==
                  DualSampleScale::World, "just past the mid ceiling is World");
    }

    // 9. The rejected-signature is recorded once, from the *first* undecodable draw,
    //    and the reason buckets say which decode step failed (ADR-0505 next step 1).
    {
        SpanDrawCensus census;
        census.Add(MakeSample(true, 1600.0f, 3500.0f));           // world: eligible
        Check(census.world == 1u && census.skipped == 0u, "a world draw is counted and not skipped");

        census.Add(MakeSample(true, -0.45f, 0.45f));              // near: skipped, decoded
        Check(census.nearField == 1u && census.skipped == 1u && census.decodeRejected == 0u,
              "a decoded near-field draw is skipped but not a decode rejection");

        VertexSample noArray = MakeSample(false, 0.0f, 0.0f, 526u);
        noArray.reason = VertexSampleReason::NoVertexArray;
        noArray.vertexType = kGlFloat;
        noArray.vertexSize = 3u;
        noArray.stride = 0;
        noArray.pointerSet = false;
        noArray.indexType = kGlUnsignedShort;
        census.Add(noArray);
        Check(census.unknown == 1u && census.skipped == 2u && census.decodeRejected == 1u,
              "the undecodable draw is Unknown and a decode rejection");
        Check(census.reasonNoArray == 1u, "the reason bucket names the failed decode step");
        Check(census.hasRejectSig && census.rejectReason == VertexSampleReason::NoVertexArray,
              "the first rejection's reason is kept");
        Check(census.rejectVertexType == kGlFloat && census.rejectVertexSize == 3u &&
                  census.rejectStride == 0 && !census.rejectPointerSet &&
                  census.rejectIndexType == kGlUnsignedShort && census.rejectCount == 526u,
              "the rejection signature carries type/size/stride/pointer/index/count");

        VertexSample badSize = MakeSample(false, 0.0f, 0.0f, 8u);
        badSize.reason = VertexSampleReason::UnsupportedVertexSize;
        badSize.vertexSize = 1u;
        census.Add(badSize);
        Check(census.reasonBadSize == 1u && census.decodeRejected == 2u,
              "a second rejection is bucketed");
        Check(census.rejectVertexSize == 3u,
              "the signature is the FIRST rejection, not overwritten");

        Check(DualRejectReasonName(VertexSampleReason::NoVertexArray)[0] == 'n' &&
                  DualRejectReasonName(VertexSampleReason::UnknownIndexType)[0] == 'b',
              "reject reasons have stable short names");

        census.Reset();
        Check(census.skipped == 0u && census.decodeRejected == 0u && !census.hasRejectSig,
              "Reset clears the census");
    }

    // 9b. GH #76/#90 (ADR-0508): an undecodable draw on the generic-attribute path is
    //     *eligible* (re-issued), so it is counted there -- not skipped and not a
    //     decode rejection. The buffer-backed subset and the first such draw's state
    //     are recorded so the run states the array source.
    {
        SpanDrawCensus census;
        VertexSample generic = MakeSample(false, 0.0f, 0.0f, 12u);
        generic.genericAttributeEnabled = true;
        generic.genericPositionBufferId = 42u;
        census.Add(generic);
        Check(census.genericEligible == 1u && census.genericBufferBacked == 1u,
              "a buffer-backed generic draw is counted as eligible");
        Check(census.skipped == 0u && census.decodeRejected == 0u,
              "an eligible generic draw is neither skipped nor a rejection");
        Check(census.unknown == 1u, "the undecodable draw is still the Unknown band");
        Check(census.hasGenericSig && census.genericEnabled == 1u && census.genericBufferId == 42u,
              "the first generic draw's state is recorded");

        VertexSample genericClient = MakeSample(false, 0.0f, 0.0f, 12u);
        genericClient.genericAttributeEnabled = true;   // client memory, buffer 0
        census.Add(genericClient);
        Check(census.genericEligible == 2u && census.genericBufferBacked == 1u,
              "a client-memory generic draw is eligible but not buffer backed");
        Check(census.genericBufferId == 42u, "the first generic signature is not overwritten");
    }

    // 10. The sampler records the raw GL state it was handed, so a rejection can be
    //     reported as a signature rather than a bare count (pure, no GL).
    {
        VertexSample nonFloat = SampleClientVertices(6u, kGlUnsignedShort, nullptr,
                                                     3u, 0x1403u /* GL_UNSIGNED_SHORT */, 0, nullptr);
        Check(!nonFloat.valid && nonFloat.reason == VertexSampleReason::NonFloatVertexType,
              "a non-float vertex array is reported, not decoded");
        Check(nonFloat.vertexType == 0x1403u && nonFloat.vertexSize == 3u,
              "the rejected type/size is recorded for the log");

        const float verts[3 * 3] = {1.0f, 2.0f, 3.0f, -1.0f, -2.0f, -3.0f, 0.5f, 0.5f, 0.5f};
        VertexSample ok = SampleClientVertices(3u, kGlUnsignedShort, nullptr,
                                               3u, kGlFloat, 0, verts);
        Check(ok.valid && ok.reason == VertexSampleReason::Ok,
              "a tightly packed float array decodes");
        Check(ok.min[0] == -1.0f && ok.max[0] == 1.0f && ok.pointerSet,
              "the decoded bounds are over the sampled vertices");
    }

    if (g_failures == 0) {
        std::puts("stereo dual tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
