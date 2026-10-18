// GH #72 requirement 1: unit tests for the cold-start full-bleed latch that
// keeps the published display branch from calling the static 2D startup splash
// "3D gameplay" (src/hook/display_branch.h).
//
// These pin the semantics the branch decision relies on:
//   - the compiled default is the latch ON (ADR-0478, live A/B),
//   - with the latch off the verdict is exactly the legacy one,
//   - before a fill is measured the verdict is still the legacy one,
//   - a letterboxed fill demotes a gameplay-labelled page to cinema only while
//     no full-bleed frame has ever been seen,
//   - the first full-bleed fill latches and the demotion can never fire again
//     (GH #44: real gameplay may carry engine-baked bars),
//   - a fill inside the two thresholds holds the previous decisive verdict,
//   - `!Has3DThisFrame()` / the menu cursor stay cinema unconditionally.
#include "display_branch.h"

#include <cstdio>

using RiddickVR::ComputeDisplayBranch;
using RiddickVR::DisplayBranchConfig;
using RiddickVR::DisplayBranchState;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "display branch test failed: %s\n", message);
        ++g_failures;
    }
}

// One frame: returns the verdict. `fill` <= 0 means "no scan yet".
static bool Frame(DisplayBranchState& state, const DisplayBranchConfig& cfg,
                  bool has3d, bool menuCursor, float fill) {
    const bool haveFill = fill > 0.0f;
    return ComputeDisplayBranch(has3d, menuCursor, fill, haveFill, cfg, state);
}

int main() {
    // 0. The compiled default is ON since the 2026-10-18 live A/B (ADR-0478).
    Check(DisplayBranchConfig().coldStartLatch,
          "the compiled default must be the cold-start latch on (ADR-0478)");

    // 1. Legacy behaviour is preserved when the latch is off: a perspective
    //    frame without a menu cursor is gameplay regardless of the fill.
    {
        DisplayBranchConfig cfg;
        cfg.coldStartLatch = false;
        DisplayBranchState st;
        Check(!Frame(st, cfg, true, false, 0.31f),
              "latch off: a letterboxed fill must not change the verdict");
        Check(!Frame(st, cfg, true, false, 1.00f), "latch off: full-bleed is gameplay");
        Check(Frame(st, cfg, false, false, 1.00f),
              "latch off: no 3D frame is still cinema");
        Check(Frame(st, cfg, true, true, 1.00f), "latch off: the menu cursor is still cinema");
    }

    // 2. With the latch on, a frame before the first scan (no fill yet) keeps
    //    the legacy verdict.
    {
        DisplayBranchConfig cfg;
        cfg.coldStartLatch = true;
        DisplayBranchState st;
        Check(!Frame(st, cfg, true, false, 0.0f),
              "latch on: a frame before the first scan must stay gameplay");
        Check(!st.fullBleedSeen && !st.letterboxed, "an unmeasured frame must not move the state");
    }

    // 3. The exact GH #72 defect: a splash page (perspective loaded, fill
    //    0.31-0.36) must be demoted to cinema.
    {
        DisplayBranchConfig cfg;
        cfg.coldStartLatch = true;
        DisplayBranchState st;
        Check(Frame(st, cfg, true, false, 0.31f),
              "latch on: the letterboxed startup splash must be cinema");
        Check(Frame(st, cfg, true, false, 0.36f),
              "latch on: the splash stays cinema while it is letterboxed");
        Check(!st.fullBleedSeen, "the splash must not set the full-bleed latch");
    }

    // 4. The first full-bleed frame latches and is gameplay; afterwards a
    //    letterboxed fill can never demote again (GH #44: baked bars).
    {
        DisplayBranchConfig cfg;
        cfg.coldStartLatch = true;
        DisplayBranchState st;
        Check(Frame(st, cfg, true, false, 0.31f), "splash demoted before gameplay");
        Check(!Frame(st, cfg, true, false, 1.00f),
              "the first full-bleed frame must be gameplay");
        Check(st.fullBleedSeen, "the full-bleed frame must set the latch");
        Check(!Frame(st, cfg, true, false, 0.31f),
              "after the latch, baked bars must stay gameplay (GH #44)");
        Check(!Frame(st, cfg, true, false, 0.20f),
              "after the latch, any letterbox fill stays gameplay");
    }

    // 5. A fill inside the two thresholds holds the last decisive verdict: a
    //    smoothed/easing rect cannot flicker the branch.
    {
        DisplayBranchConfig cfg;
        cfg.coldStartLatch = true;
        DisplayBranchState st;
        Check(Frame(st, cfg, true, false, 0.31f), "letterbox verdict established");
        Check(Frame(st, cfg, true, false, 0.80f),
              "an in-between fill must hold the letterboxed verdict");
        Check(!Frame(st, cfg, true, false, 0.95f),
              "a full-bleed fill clears the letterboxed verdict");
        Check(st.fullBleedSeen, "a full-bleed fill latches");
    }

    // 6. An in-between fill with no prior decisive verdict must not demote (the
    //    safe default: only a clearly letterboxed rect demotes).
    {
        DisplayBranchConfig cfg;
        cfg.coldStartLatch = true;
        DisplayBranchState st;
        Check(!Frame(st, cfg, true, false, 0.80f),
              "an in-between fill must not demote without a letterbox verdict");
    }

    // 7. `!Has3DThisFrame()` and the menu cursor are cinema unconditionally, and
    //    a menu frame does not set the full-bleed latch.
    {
        DisplayBranchConfig cfg;
        cfg.coldStartLatch = true;
        DisplayBranchState st;
        Check(Frame(st, cfg, false, false, 1.00f), "no 3D frame is cinema");
        Check(!st.fullBleedSeen, "a non-3D frame must not set the latch");
        Check(Frame(st, cfg, true, true, 1.00f), "the menu cursor is cinema");
        Check(!st.fullBleedSeen, "a menu frame must not set the latch");
    }

    if (g_failures == 0) {
        std::puts("display branch tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
