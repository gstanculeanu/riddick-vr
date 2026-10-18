#pragma once

// GH #76 (epic #57 definition-of-done item 1) / GH #90: **native stereo by
// dual-issuing the engine's own eye-space draws per eye**.
//
// Why this supersedes the "record the frame, replay it per eye" plan (GH #90 as
// first written): the thing a replay has to reconstruct is the *GL state* at the
// draw site. ADR-0481/0482 + ADR-0491 measured that this title is a
// fixed-function renderer whose geometry path is `glDrawElements` over client
// arrays -- RndrGL never calls `glDrawArrays`, never binds a buffer object, never
// uses display lists (`listIn=listOut=0` in every sampled interval) and barely
// uses immediate mode (2 blocks/run, 2-D). So at the `glDrawElements` hook the
// complete geometry of a world draw is in hand *and the whole fixed-function
// state that will shade it is already current*. Re-issuing the same call while
// the camera is offset therefore needs **no state capture and no copied
// vertices**: it is the replay's result, obtained directly at the draw site.
//
// The per-eye camera itself is ADR-0504's measured mechanism: the engine submits
// eye-space vertices with an identity `GL_MODELVIEW`, so moving the eye is a pure
// translation of the modelview (`stereo_replay.h::BuildEyeReplayModelview`).
//
// This header is the pure, headless-testable policy of that pass:
//
//   * which draws belong to the 3-D world and must be duplicated per eye, and
//   * when the resulting per-eye frames are usable at all (a fail-safe gate that
//     keeps the shipped shifted-monoscopic capture whenever the pass captured
//     nothing), so the opt-in can never regress the stereo pair.
//
// No GL/Windows headers: headless, unit-tested (tests/stereo_dual_tests.cpp).

#include "vertex_submission_observe.h"

#include <cmath>
#include <cstddef>

namespace RiddickVR {

// The engine's 3-D world draws are submitted in *eye space* (ADR-0481/0482), so
// their sampled vertices are large distances (measured |x| ~ 1.6e3..3.5e3), while
// screen-space/2-D draws -- the HUD, and the first-person window's "unit quad"
// (ADR-0482/0483) -- sit within +/-0.5 of the origin. `kNearFieldMaxAbs` (16
// units, ~0.5 m) is the measured separator from ADR-0490, reused here.
//
// A draw is duplicated per eye only when it is a *decodable* client-array draw
// whose sampled bounds are NOT near-field:
//   * HUD / screen-space draws must NOT be translated: their coordinates are not
//     metres, so an eye-space translation would slide them by a meaningless
//     fraction of the screen.
//   * the engine's own first-person viewmodel geometry is near-field as well, so
//     it is excluded too -- it is replaced by the VR hands (#61/#64), so leaving
//     it out of the per-eye pass is the #63 outcome, not a loss.
//   * an *undecodable* draw (non-float array, unknown index type) is not
//     classified and is left to the failing gate rather than guessed at.
inline bool DualRenderDrawEligible(const VertexSample& sampled) {
    if (!sampled.valid) return false;
    return !IsNearFieldSample(sampled);
}

// Fail-safe gate for the per-eye frames. The dual pass writes only to its own
// framebuffers; if nothing eligible was drawn (the pass boundary missed, a menu
// frame, an unsupported state) the frames stay black and the renderer must keep
// the shipped shifted capture. `coverage` is the non-black fraction sampled from
// an eye frame.
//
// The floor is deliberately high: the first live run (wake 103) had a 4-row / 5%
// gate pass frames whose presented mean luminance was only ~3/255 (the sampled
// rows caught a little content over an otherwise black frame), which is not a
// usable eye image. A frame must be mostly content to be used.
constexpr float kDualMinEyeCoverage = 0.5f;

// Non-black fraction of an RGBA8 buffer (`pixels` 4-byte texels). A texel counts
// as content when any colour channel exceeds `threshold` (the same 14/255 floor
// ScanActiveRegion uses for "not a black bar").
inline float DualNonBlackFraction(const unsigned char* rgba, unsigned pixels, int threshold = 14) {
    if (!rgba || pixels == 0u) return 0.0f;
    unsigned lit = 0;
    for (unsigned i = 0; i < pixels; ++i) {
        const unsigned char* p = rgba + static_cast<size_t>(i) * 4u;
        if (p[0] > threshold || p[1] > threshold || p[2] > threshold) ++lit;
    }
    return static_cast<float>(lit) / static_cast<float>(pixels);
}

// Both eyes must carry content; one black eye means the pass captured nothing and
// the caller falls back (never a black/flickering eye).
inline bool DualFrameUsable(float coverageLeft, float coverageRight,
                            float minCoverage = kDualMinEyeCoverage) {
    if (!(coverageLeft >= minCoverage) || !(coverageRight >= minCoverage)) return false;
    return true;
}

// --- GH #76/#90 (ADR-0506): name what the world span actually contains ---------
//
// ADR-0505's live census measured that only ~8% of the draws inside the world span
// are world-scale, ~92% did not decode at all, and near-field draws were absent
// (`unknown=675 near=0 mid=2 world=58`). The per-eye pass therefore captured ~60
// of ~735 draws and produced unusable frames. "The pass is inert on its own gate"
// is not a diagnosis, so this names the rejection: which band each draw falls in
// and, for the undecodable ones, the *signature* (vertex type/size/stride/pointer
// and index type) of the first one. It is evidence only -- it never changes which
// draws are re-issued.

// Max |coordinate| over the sampled bounds; 0 for an invalid sample.
inline float DualSampleExtent(const VertexSample& s) {
    if (!s.valid) return 0.0f;
    float e = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float lo = std::fabs(s.min[c]);
        const float hi = std::fabs(s.max[c]);
        const float m = lo > hi ? lo : hi;
        if (m > e) e = m;
    }
    return e;
}

// The measured bands (ADR-0490/0505): near-field is <= kNearFieldMaxAbs and is the
// HUD/viewmodel band; world is the eye-space geometry at |coord| ~1.6e3..3.5e3.
// What sits *between* them was never named -- wake 103 saw ~2 such draws per span
// -- so this band exists to make it countable rather than silently "eligible".
enum class DualSampleScale : unsigned { Unknown = 0, Near, Mid, World };

constexpr float kDualMidMaxExtent = 1000.0f;  // > near-field and <= this is "mid"

inline DualSampleScale DualSampleScaleOf(const VertexSample& s) {
    if (!s.valid) return DualSampleScale::Unknown;
    const float e = DualSampleExtent(s);
    if (e <= kNearFieldMaxAbs) return DualSampleScale::Near;
    if (e <= kDualMidMaxExtent) return DualSampleScale::Mid;
    return DualSampleScale::World;
}

// Reason names for the evidence log (stable tokens, asserted by the unit test).
inline const char* DualRejectReasonName(VertexSampleReason r) {
    switch (r) {
        case VertexSampleReason::Ok:                   return "ok";
        case VertexSampleReason::NonFloatVertexType:   return "nonfloat";
        case VertexSampleReason::NoVertexArray:        return "noarray";
        case VertexSampleReason::ZeroCount:            return "zerocount";
        case VertexSampleReason::UnsupportedVertexSize:return "badsize";
        case VertexSampleReason::UnknownIndexType:     return "badindex";
    }
    return "?";
}

// One span's draw census. `skipped` counts the draws the pass refused: the
// undecodable ones plus the decoded-but-near-field ones (the viewmodel/HUD band).
// `decodeRejected` is the undecodable subset, bucketed by reason; the first such
// draw's signature is kept, so a single log line states *why* the world pass is
// invisible to the array sampler.
struct SpanDrawCensus {
    // `nearField` not `near`: windef.h defines a bare `near` macro (hence the
    // capitalised m_dualScaleNear of ADR-0505).
    unsigned unknown = 0, nearField = 0, mid = 0, world = 0;
    unsigned skipped = 0;
    unsigned decodeRejected = 0;
    unsigned reasonNonFloat = 0, reasonNoArray = 0, reasonZeroCount = 0;
    unsigned reasonBadSize = 0, reasonBadIndex = 0;
    bool hasRejectSig = false;
    VertexSampleReason rejectReason = VertexSampleReason::Ok;
    unsigned rejectVertexType = 0, rejectVertexSize = 0, rejectIndexType = 0, rejectCount = 0;
    int rejectStride = 0;
    bool rejectPointerSet = false;

    void Add(const VertexSample& s) {
        switch (DualSampleScaleOf(s)) {
            case DualSampleScale::Near:  ++nearField; break;
            case DualSampleScale::Mid:   ++mid;       break;
            case DualSampleScale::World: ++world;     break;
            default:                     ++unknown;   break;
        }
        if (DualRenderDrawEligible(s)) return;  // a world draw the pass re-issues
        ++skipped;
        if (s.valid) return;                    // decoded, but near-field (viewmodel/HUD)
        ++decodeRejected;
        switch (s.reason) {
            case VertexSampleReason::NonFloatVertexType:    ++reasonNonFloat; break;
            case VertexSampleReason::NoVertexArray:         ++reasonNoArray;  break;
            case VertexSampleReason::ZeroCount:             ++reasonZeroCount;break;
            case VertexSampleReason::UnsupportedVertexSize: ++reasonBadSize;  break;
            case VertexSampleReason::UnknownIndexType:      ++reasonBadIndex; break;
            default: break;
        }
        if (!hasRejectSig) {
            hasRejectSig = true;
            rejectReason = s.reason;
            rejectVertexType = s.vertexType;
            rejectVertexSize = s.vertexSize;
            rejectStride = s.stride;
            rejectPointerSet = s.pointerSet;
            rejectIndexType = s.indexType;
            rejectCount = s.count;
        }
    }

    void Reset() { *this = SpanDrawCensus{}; }
};

} // namespace RiddickVR
