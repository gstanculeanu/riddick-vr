// GH #66 (approach 2): unit tests for the per-draw viewmodel suppression
// window (viewmodel_suppressor.h).
//
// These tests pin the *inert-by-default* guarantee (a disabled suppressor never
// skips a draw) and the window state machine the draw hooks rely on:
//   - a depth-only clear opens the window,
//   - a projection load or the frame swap closes it,
//   - only draws inside the window are suppressed,
//   - a full colour clear re-bases the per-frame counters.
#include "viewmodel_suppressor.h"

#include <cstdio>
#include <cstring>

using RiddickVR::ViewmodelSuppressionConfig;
using RiddickVR::ViewmodelSuppressor;
using RiddickVR::ViewmodelWindowOpener;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "viewmodel suppressor test failed: %s\n", message);
        ++g_failures;
    }
}

int main() {
    // Disabled: inert even inside a depth-only clear window.
    {
        ViewmodelSuppressor s;
        s.Configure(ViewmodelSuppressionConfig{}); // default: enabled == false
        s.NoteColorClear();
        s.NoteDepthOnlyClear();
        for (int i = 0; i < 5; ++i) {
            Check(!s.ShouldSuppressDraw(), "a disabled suppressor must never skip a draw");
        }
        Check(!s.WindowOpen(), "a disabled suppressor must not open a window");
        Check(s.TotalSuppressed() == 0, "a disabled suppressor must not count suppressions");
    }

    // Enabled but opener == None: still inert (no window ever opens).
    {
        ViewmodelSuppressor s;
        ViewmodelSuppressionConfig cfg;
        cfg.enabled = true;
        cfg.opener = ViewmodelWindowOpener::None;
        s.Configure(cfg);
        s.NoteColorClear();
        s.NoteDepthOnlyClear();
        Check(!s.WindowOpen(), "opener None must not open a window");
        Check(!s.ShouldSuppressDraw(), "opener None must never suppress");
        Check(s.FrameDraws() == 1 && s.FrameSuppressed() == 0,
              "opener None must count the draw but not suppress it");
    }

    // Enabled + depth-clear opener: the world (before the depth-only clear) is
    // never suppressed; the viewmodel window is.
    {
        ViewmodelSuppressor s;
        ViewmodelSuppressionConfig cfg;
        cfg.enabled = true;
        cfg.opener = ViewmodelWindowOpener::DepthOnlyClear;
        s.Configure(cfg);

        s.NoteColorClear();                 // frame start: world pass
        Check(!s.ShouldSuppressDraw(), "world geometry before the depth clear must draw");
        Check(!s.ShouldSuppressDraw(), "world geometry before the depth clear must draw");
        Check(!s.WindowOpen(), "the window must be closed before a depth-only clear");

        s.NoteDepthOnlyClear();             // first-person pass begins
        Check(s.WindowOpen(), "a depth-only clear must open the window");
        Check(s.ShouldSuppressDraw(), "the viewmodel draw must be suppressed");
        Check(s.ShouldSuppressDraw(), "every viewmodel draw must be suppressed");

        s.NoteProjectionLoad();             // world (e.g. next pass) resumes
        Check(!s.WindowOpen(), "a projection load must close the window");
        Check(!s.ShouldSuppressDraw(), "draws after the window closes must draw");

        s.NoteSwap();                       // end of frame
        Check(s.LastFrameDraws() == 5, "the frame must count every enabled draw");
        Check(s.LastFrameSuppressed() == 2, "the frame must count only windowed draws");
        Check(s.TotalSuppressed() == 2, "cumulative suppressions must accumulate");
        Check(s.FrameDraws() == 0 && s.FrameSuppressed() == 0,
              "a swap must re-base the per-frame counters");
    }

    // A swap closes an open window (frame ends before a projection load).
    {
        ViewmodelSuppressor s;
        ViewmodelSuppressionConfig cfg;
        cfg.enabled = true;
        s.Configure(cfg);
        s.NoteColorClear();
        s.NoteDepthOnlyClear();
        Check(s.ShouldSuppressDraw(), "a windowed draw must be suppressed");
        s.NoteSwap();
        Check(!s.WindowOpen(), "a swap must close an open window");
        Check(!s.ShouldSuppressDraw(), "draws after a swap must draw");
    }

    // A full colour clear also closes an open window and re-bases the counters
    // (a depth-only clear that is never followed by a projection load must not
    // leak into the next frame).
    {
        ViewmodelSuppressor s;
        ViewmodelSuppressionConfig cfg;
        cfg.enabled = true;
        s.Configure(cfg);
        s.NoteColorClear();
        s.NoteDepthOnlyClear();
        Check(s.ShouldSuppressDraw(), "a windowed draw must be suppressed");
        s.NoteColorClear();                 // next frame begins
        Check(!s.WindowOpen(), "a colour clear must close the window");
        Check(!s.ShouldSuppressDraw(), "the next frame's world must draw");
        Check(s.LastFrameDraws() == 1 && s.LastFrameSuppressed() == 1,
              "a colour clear must latch the previous frame's tallies");
    }

    // Idempotent opens: repeated depth-only clears do not double-count or
    // change the decision, and re-configuring off stops suppression at once.
    {
        ViewmodelSuppressor s;
        ViewmodelSuppressionConfig cfg;
        cfg.enabled = true;
        s.Configure(cfg);
        s.NoteColorClear();
        s.NoteDepthOnlyClear();
        s.NoteDepthOnlyClear();
        Check(s.ShouldSuppressDraw() && s.FrameSuppressed() == 1,
              "repeated depth-only clears must not double-count");
        ViewmodelSuppressionConfig off = cfg;
        off.enabled = false;
        s.Configure(off);
        Check(!s.ShouldSuppressDraw(), "re-configuring the suppressor off must stop suppression");
    }

    // GH #63/#66 (ADR-0446): the first perspective load after the depth-only
    // clear is the first-person pass's own projection and must NOT close the
    // window; a later load after draws does.
    {
        ViewmodelSuppressor s;
        ViewmodelSuppressionConfig cfg;
        cfg.enabled = true;
        s.Configure(cfg);
        s.NoteColorClear();
        s.NoteProjectionLoad(true, 4.0f);   // world projection
        s.ShouldSuppressDraw();             // world draw
        s.NoteDepthOnlyClear();
        s.NoteProjectionLoad(true, 0.5f);   // the FP pass's own projection
        Check(s.WindowOpen(), "the FP pass's own projection load must not close the window");
        Check(s.ShouldSuppressDraw() && s.ShouldSuppressDraw() && s.ShouldSuppressDraw(),
              "all three viewmodel draws must be suppressed");
        s.NoteProjectionLoad(true, 4.0f);   // the world resumes
        Check(!s.WindowOpen(), "a perspective load after draws must close the window");
        Check(!s.ShouldSuppressDraw(), "draws after the close must draw");
        Check(s.FrameSuppressed() == 3, "exactly the three windowed draws are suppressed");
    }

    // An ortho load always closes, even with no draws in the window.
    {
        ViewmodelSuppressor s;
        ViewmodelSuppressionConfig cfg;
        cfg.enabled = true;
        s.Configure(cfg);
        s.NoteColorClear();
        s.NoteDepthOnlyClear();
        s.NoteProjectionLoad(false, 0.0f);
        Check(!s.WindowOpen(), "an ortho load must close the window immediately");
    }

    // Near-plane-drop opener: world near 4.0, then a load with near 0.5 opens.
    {
        ViewmodelSuppressor s;
        ViewmodelSuppressionConfig cfg;
        cfg.enabled = true;
        cfg.opener = ViewmodelWindowOpener::NearPlaneDrop;
        s.Configure(cfg);
        s.NoteColorClear();
        s.NoteProjectionLoad(true, 4.0f);
        Check(!s.WindowOpen(), "the world projection must not open the window");
        s.NoteDepthOnlyClear();
        Check(!s.WindowOpen(), "a depth-only clear must not open with the near-plane opener");
        s.NoteProjectionLoad(true, 3.0f);
        Check(!s.WindowOpen(), "a small near drop (3.0 vs 4.0) must not open the window");
        s.NoteProjectionLoad(true, 0.5f);
        Check(s.WindowOpen(), "a near plane below world*ratio must open the window");
        Check(s.ShouldSuppressDraw(), "a draw in the near-drop window is suppressed");
        s.NoteSwap();
        s.NoteColorClear();
        s.NoteProjectionLoad(true, 0.5f);
        Check(!s.WindowOpen(), "worldNear is reset per frame, so a lone low near does not open");
    }

    // Either opener: both paths open.
    {
        ViewmodelSuppressor s;
        ViewmodelSuppressionConfig cfg;
        cfg.enabled = true;
        cfg.opener = ViewmodelWindowOpener::Either;
        s.Configure(cfg);
        s.NoteColorClear();
        s.NoteDepthOnlyClear();
        Check(s.WindowOpen(), "Either must open on a depth-only clear");
        s.NoteSwap();
        s.NoteColorClear();
        s.NoteProjectionLoad(true, 4.0f);
        s.NoteProjectionLoad(true, 1.0f);
        Check(s.WindowOpen(), "Either must open on a near-plane drop");
    }

    // track without enabled: the window opens and draws are counted, none suppressed.
    {
        ViewmodelSuppressor s;
        ViewmodelSuppressionConfig cfg;
        cfg.track = true;
        s.Configure(cfg);
        s.NoteColorClear();
        s.NoteDepthOnlyClear();
        Check(s.WindowOpen(), "track must open the window without enabled");
        Check(!s.ShouldSuppressDraw() && !s.ShouldSuppressDraw(), "track alone must never suppress");
        Check(s.FrameDraws() == 2 && s.FrameSuppressed() == 0, "track must count draws");
        Check(s.TotalSuppressed() == 0, "track must not count suppressions");
    }

    // Exact trace string for a scripted frame, latched on swap.
    {
        ViewmodelSuppressor s;
        ViewmodelSuppressionConfig cfg;
        cfg.enabled = true;
        s.Configure(cfg);
        s.NoteColorClear();
        s.NoteProjectionLoad(true, 4.0f);
        s.ShouldSuppressDraw();
        s.ShouldSuppressDraw();
        s.NoteDepthOnlyClear();
        s.NoteProjectionLoad(true, 0.5f);
        s.ShouldSuppressDraw();
        s.ShouldSuppressDraw();
        s.ShouldSuppressDraw();
        s.NoteProjectionLoad(false, 0.0f);
        s.NoteProjectionLoad();
        s.NoteSwap();
        Check(std::strcmp(s.LastFrameTrace(), "C P4.0 d2 D P0.5 d3 O P?") == 0,
              "the trace must match the scripted frame");
        s.NoteSwap();
        Check(std::strcmp(s.LastFrameTrace(), "") == 0, "an empty frame latches an empty trace");
    }

    // Trace truncation never overflows and stays NUL-terminated.
    {
        ViewmodelSuppressor s;
        ViewmodelSuppressionConfig cfg;
        cfg.track = true;
        s.Configure(cfg);
        s.NoteColorClear();
        for (int i = 0; i < 200; ++i) {
            s.NoteProjectionLoad(true, 12.5f);
            s.ShouldSuppressDraw();
        }
        s.NoteSwap();
        const size_t len = std::strlen(s.LastFrameTrace());
        Check(len > 0 && len < static_cast<size_t>(RiddickVR::kViewmodelTraceCapacity),
              "a long trace must truncate inside the fixed buffer");
    }

    if (g_failures == 0) {
        std::puts("viewmodel suppressor tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
