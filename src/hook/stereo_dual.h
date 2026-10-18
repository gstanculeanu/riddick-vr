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
// A draw is duplicated per eye when it belongs to the 3-D world:
//   * a *decodable* client-array draw whose sampled bounds are NOT near-field. HUD
//     / screen-space draws must NOT be translated (their coordinates are not
//     metres), and the engine's own first-person viewmodel geometry is near-field
//     as well -- it is replaced by the VR hands (#61/#64), so leaving it out of the
//     per-eye pass is the #63 outcome, not a loss.
//   * an *undecodable* draw that used the **generic-attribute path** (ADR-0508).
//     ADR-0507 measured why the world draws do not decode: the engine establishes
//     the array through `glVertexAttribPointer[ARB]` (called ~695x/frame) into
//     buffer objects, and leaves the fixed-function `glVertexPointer` the sampler
//     reads **null by design** (83% of its calls). Re-issuing such a draw needs no
//     decoded vertices at all -- the arrays and buffers are current GL state at the
//     draw site -- so the source *path* is the discriminator, not the sampled
//     bounds. The near-field exclusion above still applies to every decodable draw,
//     so a screen-space/HUD draw (which is decodable) is never translated.
//   * any other undecodable draw (no vertex array and no generic attributes, a
//     non-float fixed-function type, an unknown index type) stays excluded rather
//     than guessed at.
inline bool DualRenderDrawEligible(const VertexSample& sampled) {
    if (sampled.valid) return !IsNearFieldSample(sampled);
    return sampled.genericAttributeEnabled;
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

// Mean luminance (0..255) of an RGBA8 sample. The non-black *fraction* alone cannot
// tell "black" from "dark", and the live runs measured the eye frames dark rather
// than empty (mean ~3/255 at wake 103, ~8/255 at wake 106), so the evidence needs
// both numbers to say whether a re-issued span writes anything usable.
inline float DualMeanLuminance(const unsigned char* rgba, unsigned pixels) {
    if (!rgba || pixels == 0u) return 0.0f;
    double sum = 0.0;
    for (unsigned i = 0; i < pixels; ++i) {
        const unsigned char* p = rgba + static_cast<size_t>(i) * 4u;
        sum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
    }
    return static_cast<float>(sum / static_cast<double>(pixels));
}

// --- GH #76/#90 (ADR-0509): can a re-issued draw write colour at all? -----------
//
// ADR-0508 re-issued ~96% of the world span's draws per eye (706/735) and the eye
// frames were still unusable (coverage below the gate, mean ~8/255). The pass now
// captured the *geometry* but never the *output state*, and one class of state
// decides whether real geometry renders any colour: `glColorMask`. A span that is a
// depth/shadow stage -- the scene's occlusion prepass -- re-issues correctly but
// writes no colour, which is exactly what a high re-issue count with dark eye frames
// looks like. This is the engine state read **once per span, at the first eligible
// draw, before the pass rebinds anything** (the pass only changes the modelview and
// the draw framebuffer, so everything else here is the engine's).
//
// Colour mask, depth mask, viewport, scissor and the enable bits are *global* state,
// so they carry over unchanged into the per-eye frames; only `glDrawBuffer` is
// per-framebuffer, so the recorded value is the engine's own buffer (the eye frames
// use their default GL_COLOR_ATTACHMENT0) and is logged for context, not blamed.
struct DualDrawState {
    bool captured = false;
    bool colourWrite[4] = {true, true, true, true};  // glColorMask (GL default: all on)
    bool depthWrite = true;                          // glDepthMask
    unsigned drawBuffer = 0;                         // glDrawBuffer of the engine's buffer
    int viewport[4] = {0, 0, 0, 0};                  // x, y, w, h
    bool scissorEnabled = false;                     // GL_SCISSOR_TEST
    int scissorBox[4] = {0, 0, 0, 0};                // x, y, w, h (only when enabled)
    bool depthTestEnabled = false;                   // GL_DEPTH_TEST
    bool blendEnabled = false;                       // GL_BLEND

    bool ColourWriteEnabled() const {
        return colourWrite[0] || colourWrite[1] || colourWrite[2] || colourWrite[3];
    }
};

// The colour-write mask as a 4-bit nibble (bit0=red, bit1=green, bit2=blue,
// bit3=alpha), so one log token states all four flags; the GL default is 0xF.
inline unsigned DualColourMaskNibble(const DualDrawState& s) {
    return (s.colourWrite[0] ? 1u : 0u) | (s.colourWrite[1] ? 2u : 0u) |
           (s.colourWrite[2] ? 4u : 0u) | (s.colourWrite[3] ? 8u : 0u);
}

constexpr unsigned kDualDrawBufferNone = 0x0404u;  // GL_NONE

// True when the captured state means a re-issued draw cannot colour the eye frame:
// the colour mask is fully off, or the span's own draw buffer is GL_NONE. This is
// the measured form of "the span is a depth/colour-masked stage", as opposed to "the
// re-issue is broken" -- the two answers point at different fixes.
inline bool DualDrawStateWritesNoColour(const DualDrawState& s) {
    return s.captured && (!s.ColourWriteEnabled() || s.drawBuffer == kDualDrawBufferNone);
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
    // GH #76/#90 (ADR-0507): whether a GL_ARRAY_BUFFER was bound at that first
    // rejection, i.e. whether the null vertex pointer is the buffer-object idiom
    // (offset 0 into a bound buffer) or genuinely unset.
    bool rejectArrayBufferBound = false;
    unsigned rejectArrayBufferId = 0;
    // GH #76/#90 (ADR-0508): the draws the pass re-issues *without* a decodable
    // client array, i.e. on the generic-attribute path. `genericBufferBacked` is
    // the subset whose position array's data lives in a buffer object (the
    // bind-time GL_ARRAY_BUFFER binding was non-zero); `genericEnabled` is the
    // generic-attribute arrays enabled at the first such draw.
    unsigned genericEligible = 0;
    unsigned genericBufferBacked = 0;
    bool hasGenericSig = false;
    unsigned genericEnabled = 0;
    unsigned genericBufferId = 0;

    void Add(const VertexSample& s) {
        switch (DualSampleScaleOf(s)) {
            case DualSampleScale::Near:  ++nearField; break;
            case DualSampleScale::Mid:   ++mid;       break;
            case DualSampleScale::World: ++world;     break;
            default:                     ++unknown;   break;
        }
        if (DualRenderDrawEligible(s)) {        // a world draw the pass re-issues
            if (!s.valid) {                     // ... via the generic-attribute path
                ++genericEligible;
                if (s.genericPositionBufferId != 0u) ++genericBufferBacked;
                if (!hasGenericSig) {
                    hasGenericSig = true;
                    genericEnabled = s.genericAttributeEnabled ? 1u : 0u;
                    genericBufferId = s.genericPositionBufferId;
                }
            }
            return;
        }
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
            rejectArrayBufferBound = s.arrayBufferBound;
            rejectArrayBufferId = s.arrayBufferId;
            rejectIndexType = s.indexType;
            rejectCount = s.count;
        }
    }

    void Reset() { *this = SpanDrawCensus{}; }
};

} // namespace RiddickVR
