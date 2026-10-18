#pragma once

// GH #67 step 1 (sixth correction, ADR-0488): recover the perspective near plane
// from the projection matrix the *engine* actually submits.
//
// Why this exists: the viewmodel window's second opener (NearPlaneDrop,
// viewmodel_suppressor.h) opens when a perspective load's near plane drops well
// below the world pass's. That needs a near plane, and the classic extraction
// `near = m[14] / (m[10] - 1)` was rejecting every load the shipped engine made
// (the pass trace read `P?` on every projection, so `worldNear_` was never set
// and the opener could never fire -- a hook-side measurement gap, GH #67 /
// ADR-0487). Wake 87 logged the matrix itself (`ViewmodelProbe proj` in
// opengl_proxy.cpp) and the convention turned out to be the reason.
//
// Measured, live, read-only, on the 2009 target (wake 87; four distinct engine
// projection matrices sampled over a run that reached a live level):
//
//   element   classic GL (m[11] < 0)      measured engine (m[11] > 0)
//   m[0]      1/tan(fovx/2)  > 0          same
//   m[5]      1/tan(fovy/2)  > 0          same magnitude, negative
//   m[10]     -(f+n)/(f-n)   < 0          +(f+n)/(f-n)   > 0   (sign flipped)
//   m[11]     -1                          +1                   (sign flipped)
//   m[14]     -2fn/(f-n)     < 0          -2fn/(f-n)      < 0   (same)
//   m[15]     0                          0
//
// So the third column carries the opposite z orientation: the classic extraction
// returns **-far**, not near. Measured world pass: `m10=1.00391 m11=+1.00000
// m14=-8.01566` -> classic `m14/(m10-1) = -2048.0078` (= -far, rejected as
// negative), while the z-flipped form `-m14/(m10+1) = 4.0` and the matching
// `-m14/(m10-1) = 2048.0` -- a round near/far pair. The same run sampled
// `m10=1.00010 m14=-0.20001` -> near 0.1 / far 2000.1, and `m10=1.00005
// m14=-3.60010` -> near 1.8 / far 72002, so the two conventions are
// distinguishable on the engine's own numbers and the classic one is simply the
// wrong orientation.
//
// The discriminator is the sign of m[11]:
//   * m[11] < 0 (classic GL):        near =  m[14] / (m[10] - 1)
//   * m[11] > 0 (measured engine):   near = -m[14] / (m[10] + 1)
// Both are the same physical quantity (the host's synthetic GL matrix
// `[... m10=-1.0001 m11=-1 m14=-0.1 ...]` yields 0.05 through the classic form).
//
// Pure (no GL/Windows headers, no state) so it compiles on the x64 host and is
// unit-tested headlessly: tests/projection_near_tests.cpp.

#include <cmath>

namespace RiddickVR {

// Returned when the matrix is not a usable perspective projection (ortho, a
// degenerate near plane, a NaN/negative/non-finite extraction). Callers treat it
// as "near unknown" -- never as a near plane of zero.
constexpr float kProjectionNearUnknown = 0.0f;

// A perspective projection is distinguished from an orthographic one by m[15]
// (the homogeneous w of the z row): 0 for perspective, 1 for ortho.
inline bool IsPerspectiveProjection(const float* m) {
    return m != nullptr && std::fabs(m[15]) < 0.01f;
}

// The near plane in world units, or `kProjectionNearUnknown`. See the header
// comment for the two conventions and the measurement behind them.
inline float ExtractProjectionNear(const float* m) {
    if (!IsPerspectiveProjection(m)) return kProjectionNearUnknown;
    const bool zFlipped = m[11] >= 0.0f;
    const float denom = zFlipped ? (m[10] + 1.0f) : (m[10] - 1.0f);
    if (!(std::fabs(denom) > 1.0e-6f)) return kProjectionNearUnknown;
    float n = m[14] / denom;
    if (zFlipped) n = -n;
    // Reject NaN/inf/negative/absurd rather than return a plausible-looking wrong
    // number: a caller compares near planes to open a window, so a garbage value
    // is worse than "unknown".
    if (!(n == n) || !(n > 0.0f) || !(n < 1.0e9f)) return kProjectionNearUnknown;
    return n;
}

} // namespace RiddickVR
