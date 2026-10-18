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
// This is the **absolute** form of the floor: a frame must be *mostly* content. It is
// still used as the "does this frame carry an image at all?" threshold (the
// `ClassifyDualDarkSide` tie-break and `DualFrameUsable`), but it is **no longer the
// gate that decides whether the per-eye pair is presented** -- ADR-0511 measured that
// a fixed floor refuses a *faithful* pair on a dark level (the engine's own frame
// reads the same `cover ~0.28`), so the use decision moved to
// `DualFrameUsableVsEngine` below (ADR-0512), which grades each eye against the
// engine frame's own coverage in the same swap.
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

// --- GH #76/#90 (ADR-0512): the *use* gate, read against the engine's own frame ---
//
// `kDualMinEyeCoverage` above is an *absolute* floor, and ADR-0511 measured why that
// cannot be the criterion: on the in-level span the engine's **own presented frame**
// is as dark as the eye frames (both `cover ~0.28`, `luma ~10.3`), so a fixed `0.5`
// floor refuses a *faithful* pair because the *level* is dark. The gate therefore has
// to be read against what the engine itself painted **in the same swap** (ADR-0511's
// `SampleEngineFrame`): an eye that tracks a dark engine frame is accepted, an eye
// that tracks nothing while the engine is lit is not.
//
//   * each eye must carry at least `kDualEyeCoverageFraction` of the engine frame's
//     own non-black coverage -- the measured pre-level case (`engine cover 1.000`,
//     eye `0.000`) is refused, the measured in-level case (`0.294` vs `0.285`) is
//     accepted; and
//   * each eye must clear a low `kDualMinEyeCoverageAbsolute`, so the relative term
//     alone cannot pass a black eye when the engine frame is (near) black, and a run
//     whose engine sample failed (`engineCoverage == 0`) degrades to that floor
//     rather than accepting anything.
//
// The floor is the same for both eyes (they share one engine frame), but the decision
// is taken per eye so the two eyes can never disagree about what the scene carries.
constexpr float kDualEyeCoverageFraction = 0.5f;
constexpr float kDualMinEyeCoverageAbsolute = 0.05f;

inline float DualEyeCoverageFloor(float engineCoverage,
                                  float fraction = kDualEyeCoverageFraction,
                                  float absoluteFloor = kDualMinEyeCoverageAbsolute) {
    const float engine = engineCoverage > 0.0f ? engineCoverage : 0.0f;
    const float relative = fraction * engine;
    return relative > absoluteFloor ? relative : absoluteFloor;
}

inline bool DualEyeFrameTracksEngine(float eyeCoverage, float engineCoverage,
                                     float fraction = kDualEyeCoverageFraction,
                                     float absoluteFloor = kDualMinEyeCoverageAbsolute) {
    return eyeCoverage >= DualEyeCoverageFloor(engineCoverage, fraction, absoluteFloor);
}

inline bool DualFrameUsableVsEngine(float engineCoverage, float coverageLeft, float coverageRight,
                                    float fraction = kDualEyeCoverageFraction,
                                    float absoluteFloor = kDualMinEyeCoverageAbsolute) {
    return DualEyeFrameTracksEngine(coverageLeft, engineCoverage, fraction, absoluteFloor) &&
           DualEyeFrameTracksEngine(coverageRight, engineCoverage, fraction, absoluteFloor);
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

// --- GH #76/#90 (ADR-0510): per-draw colour-write gate --------------------------
//
// ADR-0509 read the span's output state *once per span* and measured `cmask=0x0` at
// the first eligible draw -- so it could not tell a span that is a depth/occlusion
// prepass throughout from one whose mask turns back on later (the colour pass). The
// next step it named is to classify **at each draw**: a draw the engine submits with
// the colour mask fully off re-issues correctly but can never colour an eye frame, so
// the pass must not spend a per-eye draw on it.
//
// The gate is on the RGB channels only: an alpha-only mask (`0x8`) writes nothing
// `DualNonBlackFraction` reads, so it counts as no colour. `colourNibble` is the
// 4-bit mask `DualColourMaskNibble` reports (bit0=red..bit3=alpha; default `0xF`).
inline bool DualColourMaskWritesColour(unsigned colourNibble) {
    return (colourNibble & 0x7u) != 0u;
}

// --- GH #76/#90 (ADR-0511): who is dark, and under what eye-frame state? ------
//
// ADR-0510 closed the span-boundary question by measurement: the in-level span's
// *colour* world pass is inside the span (col 547/706 draws colour-on), so moving the
// boundary is not the fix and the remaining cause is ADR-0509's cause (2) -- the
// re-issue itself. Two readings separate the two ways a re-issue can leave the eye
// frames unusable, and neither needs a new hypothesis:
//
//   * the **calibration**: the fixed `kDualMinEyeCoverage` floor refusing a scene that
//     is genuinely dark (the level measured mean luma ~10/255) -- the *engine's* own
//     frame is dark in the same swap, so the pass is not the blocker; and
//   * the **re-issue**: the engine frame is lit but the eye frame mostly empty, which
//     narrows to the eye framebuffer's own state (its draw buffer, viewport, ...) and
//     the modelview actually in force.
//
// The first is read by sampling the engine's own presented frame in the *same swap*
// with the same sampler (otherwise a "dark eye frame" cannot be told from "a dark
// scene"); the second by reading the state *while the eye framebuffer is bound*.
constexpr float kDualDarkLumaCeiling = 16.0f;

enum class DualDarkSide : unsigned {
    Unknown = 0,     // no sample taken (or the pass did not run this swap)
    BothDark,        // both frames dark: the scene is dark, so the floor is at fault
    EyeOnlyDark,     // engine frame lit, eye frame not: the re-issue produced no image
    EyeLit,          // eye frame carries as much content as the gate asks of it
};

inline const char* DualDarkSideName(DualDarkSide side) {
    switch (side) {
        case DualDarkSide::BothDark:    return "both-dark";
        case DualDarkSide::EyeOnlyDark: return "eye-only-dark";
        case DualDarkSide::EyeLit:      return "eye-lit";
        default:                        return "unknown";
    }
}

// The verdict compares what each frame *carries*: coverage at the gate's own floor,
// or a mean luminance above the darkness ceiling (the tie-break for a dim-but-real
// scene whose sampled rows sit under the 14/255 content floor yet are clearly not
// black). Both frames dark -> the floor is rejecting a dark scene; engine lit and eye
// dark -> the eye pass produced no image. Symmetric in the two frames on purpose: the
// same helper grades either side, so a mis-sampled engine frame cannot silently become
// "the pass is fine".
inline DualDarkSide ClassifyDualDarkSide(float engineCoverage, float engineLuma,
                                         float eyeCoverage, float eyeLuma,
                                         float minCoverage = kDualMinEyeCoverage,
                                         float lumaCeiling = kDualDarkLumaCeiling) {
    const bool eyeHasImage = eyeCoverage >= minCoverage || eyeLuma > lumaCeiling;
    if (eyeHasImage) return DualDarkSide::EyeLit;
    const bool engineHasImage = engineCoverage >= minCoverage || engineLuma > lumaCeiling;
    return engineHasImage ? DualDarkSide::EyeOnlyDark : DualDarkSide::BothDark;
}

// The state a re-issued draw actually ran under *inside the eye framebuffer*, read
// while that framebuffer is bound (so it is the pass's own per-framebuffer state, not
// the engine's). The two are the ones that can discard every re-issued fragment
// without touching the geometry: the draw buffer (a user FBO defaults to
// GL_COLOR_ATTACHMENT0, but a `GL_NONE` draw buffer or a fully-off colour mask renders
// nothing) and the viewport (a re-issued world draw is clipped to it).
struct DualEyeFrameState {
    bool captured = false;
    unsigned drawBuffer = 0;                    // glDrawBuffer while the eye FBO is bound
    int viewport[4] = {0, 0, 0, 0};             // x, y, w, h
    bool depthTestEnabled = false;              // GL_DEPTH_TEST (a depth-only stage)
    bool colourWrite[4] = {true, true, true, true};
    bool ColourWriteEnabled() const {
        return colourWrite[0] || colourWrite[1] || colourWrite[2] || colourWrite[3];
    }
};

// True when the eye framebuffer's own state cannot accept colour: no colour
// attachment named, or the colour mask fully off. This is the eye-side twin of
// `DualDrawStateWritesNoColour` and the first thing "engine lit / eye dark" narrows to.
inline bool DualEyeFrameWritesNoColour(const DualEyeFrameState& s) {
    return s.captured && (s.drawBuffer == kDualDrawBufferNone || !s.ColourWriteEnabled());
}

// The eye frame's colour mask as the same 4-bit nibble `DualColourMaskNibble` reports,
// so the engine's and the eye frame's masks are comparable in one log line.
inline unsigned DualEyeColourNibble(const DualEyeFrameState& s) {
    return (s.colourWrite[0] ? 1u : 0u) | (s.colourWrite[1] ? 2u : 0u) |
           (s.colourWrite[2] ? 4u : 0u) | (s.colourWrite[3] ? 8u : 0u);
}

// The per-eye modelview in force at the draw. ADR-0504 measured the engine's world
// draws arriving with an *identity* `GL_MODELVIEW` in eye space, and the pass applies
// `translate(-offset)` on exactly that assumption; probing the matrix at the draw site
// turns the assumption into a measurement. Column-major, so the translation is
// elements 12..14; an identity modelview means the engine left no view transform on the
// stack this frame (a non-identity one would make `translate(-offset)` an offset in the
// *engine's* space, not eye space -- a concrete, checkable fault).
inline bool DualModelviewIsIdentity(const float m[16], float tolerance = 1.0e-4f) {
    if (!m) return false;
    for (int i = 0; i < 16; ++i) {
        if (!(m[i] == m[i])) return false;  // NaN is not identity
        const float expect = (i % 5 == 0) ? 1.0f : 0.0f;
        const float d = m[i] - expect;
        if (d > tolerance || d < -tolerance) return false;
    }
    return true;
}

inline void DualModelviewTranslation(const float m[16], float out[3]) {
    if (!out) return;
    out[0] = m ? m[12] : 0.0f;
    out[1] = m ? m[13] : 0.0f;
    out[2] = m ? m[14] : 0.0f;
}

// --- GH #92/#76 (ADR-0514): does the two-eye pair carry *structured* disparity? --
//
// GH #92 measured the *presented* pair as one uniform horizontal shift
// (`stereo_kind=shift-only`, per-tile spread 0). The presented BMPs alone cannot say
// which of two very different faults that is: the per-eye frames themselves may be a
// uniform shift (the eye-space camera translation degenerating to a screen shift, or
// two eyes translated identically), or the pair that was presented may not be the
// dual pair at all (the shipped shifted capture blitted instead). So the pass measures
// its **own** output: at the same 60-swap tick it already samples for the gate, it
// reads the same rows from *both* eye framebuffers and aligns each row of the left
// frame onto the right. A physically correct per-eye pair whose depth varies -- a
// floor near the bottom of the frame, a wall/sky far from it -- yields per-row shifts
// that **differ**; a single global shift (or two identical frames) does not. Evidence
// only: nothing here changes a draw.
constexpr int kDualPairRows = 16;         // rows sampled from each eye frame
constexpr int kDualPairSearchPx = 48;     // +/- horizontal search, engine px
constexpr int kDualPairMinContrast = 6;   // skip a row with no texture to align on
constexpr int kDualPairMaxRows = 64;      // fixed cap on the per-row shift buffer
constexpr float kDualPairIdenticalErr = 0.5f;  // mean abs diff at dx=0 for "identical"

struct DualPairAlignment {
    bool measured = false;   // at least one row produced a usable shift
    int rows = 0;            // rows that contributed
    int bestDx = 0;          // median per-row shift (a flat pair's single dx)
    int minDx = 0, maxDx = 0;
    int spreadPx = 0;        // maxDx - minDx: >0 means the disparity varies with depth
    bool identical = false;  // every measured row matched at dx=0 with ~zero residual
};

// Best horizontal shift aligning one row of `a` onto the same row of `b` (mean
// absolute difference over the interior, so every candidate compares the same pixel
// count). Returns false when the row is too flat to align on, or no candidate window
// fits. `dx` is in engine (full-resolution) pixels and has the harness's sign
// convention (`b[x + dx] == a[x]`).
inline bool DualRowBestShift(const unsigned char* a, const unsigned char* b, int width,
                             int maxShift, int minContrast,
                             int* dxOut, float* errOut) {
    if (dxOut) *dxOut = 0;
    if (errOut) *errOut = 0.0f;
    if (!a || !b || !dxOut || width <= 2 * maxShift + 8) return false;
    double mean = 0.0;
    for (int x = 0; x < width; ++x) mean += a[x];
    mean /= static_cast<double>(width);
    double var = 0.0;
    for (int x = 0; x < width; ++x) { const double d = a[x] - mean; var += d * d; }
    if (std::sqrt(var / static_cast<double>(width)) < static_cast<double>(minContrast)) {
        return false;
    }
    int best = 0;
    double bestErr = 0.0;
    bool have = false;
    for (int dx = -maxShift; dx <= maxShift; ++dx) {
        const int x0 = dx < 0 ? -dx : 0;
        const int x1 = dx > 0 ? width - dx : width;
        if (x1 - x0 < 16) continue;
        double sum = 0.0;
        for (int x = x0; x < x1; ++x) {
            const double d = static_cast<double>(a[x]) - static_cast<double>(b[x + dx]);
            sum += d < 0 ? -d : d;
        }
        const double err = sum / static_cast<double>(x1 - x0);
        if (!have || err < bestErr) { have = true; bestErr = err; best = dx; }
    }
    if (!have) return false;
    *dxOut = best;
    if (errOut) *errOut = static_cast<float>(bestErr);
    return true;
}

// Align every sampled row of the two eye frames. `lumLeft`/`lumRight` are `rows`
// contiguous rows of one luminance byte per pixel, each `width` wide, at the *same*
// frame rows. The verdict: a flat pair (a single global shift, or identical frames)
// has `spreadPx == 0`; a pair with depth-dependent disparity does not.
inline DualPairAlignment DualAlignPair(const unsigned char* lumLeft,
                                       const unsigned char* lumRight,
                                       int width, int rows,
                                       int maxShift = kDualPairSearchPx,
                                       int minContrast = kDualPairMinContrast) {
    DualPairAlignment out;
    if (!lumLeft || !lumRight || width <= 0 || rows <= 0) return out;
    if (rows > kDualPairMaxRows) rows = kDualPairMaxRows;
    int shifts[kDualPairMaxRows];
    int n = 0;
    int zeroRows = 0;
    float worstZeroErr = 0.0f;
    for (int r = 0; r < rows; ++r) {
        int dx = 0;
        float err = 0.0f;
        if (!DualRowBestShift(lumLeft + static_cast<size_t>(r) * width,
                              lumRight + static_cast<size_t>(r) * width,
                              width, maxShift, minContrast, &dx, &err)) {
            continue;
        }
        shifts[n++] = dx;
        if (dx == 0) {
            ++zeroRows;
            if (err > worstZeroErr) worstZeroErr = err;
        }
    }
    if (n == 0) return out;
    // Insertion sort: `n` <= 64.
    for (int i = 1; i < n; ++i) {
        const int v = shifts[i];
        int j = i - 1;
        while (j >= 0 && shifts[j] > v) { shifts[j + 1] = shifts[j]; --j; }
        shifts[j + 1] = v;
    }
    out.measured = true;
    out.rows = n;
    out.minDx = shifts[0];
    out.maxDx = shifts[n - 1];
    out.bestDx = shifts[(n - 1) / 2];       // median of the per-row shifts
    out.spreadPx = out.maxDx - out.minDx;
    out.identical = (zeroRows == n) && (worstZeroErr <= kDualPairIdenticalErr);
    return out;
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
// invisible to the array sampler. `Add` also takes the engine's per-draw colour-write
// state (ADR-0510) and splits the *eligible* draws into colour-on/off, so the same
// span says whether it ever turns colour back on.
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
    // GH #76/#90 (ADR-0510): the eligible draws split by the engine's colour-write
    // mask *at the draw*. `colourOff` draws were re-issued by ADR-0508 but cannot
    // write colour (the measured `cmask=0x0` depth stage); `colourOn` is the subset
    // that can colour an eye frame. The pass now re-issues only `colourOn`.
    unsigned eligibleColourOn = 0;
    unsigned eligibleColourOff = 0;

    void Add(const VertexSample& s, bool colourWritesOn) {
        switch (DualSampleScaleOf(s)) {
            case DualSampleScale::Near:  ++nearField; break;
            case DualSampleScale::Mid:   ++mid;       break;
            case DualSampleScale::World: ++world;     break;
            default:                     ++unknown;   break;
        }
        if (DualRenderDrawEligible(s)) {        // a world draw the pass would re-issue
            if (colourWritesOn) ++eligibleColourOn; else ++eligibleColourOff;
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
