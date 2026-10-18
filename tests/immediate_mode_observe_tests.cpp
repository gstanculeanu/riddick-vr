// GH #67 step 1 (ninth correction, ADR-0491): unit tests for the immediate-mode
// census (immediate_mode_observe.h) -- the submission path the submitted-vertex
// observer never counted.
//
// These pin the semantics the live census relies on:
//   - inert until Configure(true): every Note* is a no-op and the snapshot is all
//     zeros (so enabling observation can never change a run that did not ask),
//   - glBegin..glEnd is one block, classified by the *window state at the begin*
//     (a vertex outside a block is ignored -- it is not legal GL),
//   - 3-D (glVertex3f/glVertex3fv) and 2-D (glVertex2f/glVertex2i) vertices are
//     counted separately by side, so "the window is 2-D HUD" is distinguishable
//     from "the window is 3-D geometry",
//   - the bounds are EXACT over the whole block (first vertex + min/max over every
//     vertex), unlike the array sampler's bounded spread,
//   - the interval's in-window candidate is the LARGEST in-window block (the
//     immediate-mode analogue of ADR-0483's largest mesh), and an outside block is
//     never a candidate,
//   - a malformed begin-while-open does not merge two blocks; an unmatched glEnd is
//     not a block,
//   - ComputeSnapshot publishes + re-bases but keeps an open block pending,
//   - Configure(false) drops everything (a flag flip cannot leak a sample).
#include "immediate_mode_observe.h"

#include <cstdio>

using RiddickVR::ImmediateBlockSample;
using RiddickVR::ImmediateModeObserver;
using RiddickVR::ImmediateModeSnapshot;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "immediate mode observer test failed: %s\n", message);
        ++g_failures;
    }
}

static bool Near(float a, float b) { return (a - b) < 1e-5f && (b - a) < 1e-5f; }

static bool VecIs(const float* v, float x, float y, float z) {
    return Near(v[0], x) && Near(v[1], y) && Near(v[2], z);
}

int main() {
    // Inert by default: observation is opt-in (ViewmodelObserve=1).
    {
        ImmediateModeObserver o;
        Check(!o.Observing(), "a fresh observer must not be observing");
        o.NoteBegin(true);
        o.NoteVertex3(1.0f, 2.0f, 3.0f);
        o.NoteVertex2(4.0f, 5.0f);
        o.NoteEnd();
        const ImmediateModeSnapshot s = o.ComputeSnapshot();
        Check(s.beganInside == 0 && s.beganOutside == 0 && s.vertex3Inside == 0 &&
              s.vertex2Inside == 0 && !s.hasInsideSample,
              "every Note must be a no-op while observation is off");
    }

    // One block: exact bounds over every vertex, 3-D vertices counted inside.
    {
        ImmediateModeObserver o;
        o.Configure(true);
        o.NoteBegin(true);
        o.NoteVertex3(0.0f, 0.0f, 0.0f);
        o.NoteVertex3(1.0f, 2.0f, 3.0f);
        o.NoteVertex3(10.0f, -5.0f, 4.0f);
        o.NoteEnd();
        const ImmediateModeSnapshot s = o.ComputeSnapshot();
        Check(s.beganInside == 1 && s.endsInside == 1 && s.beganOutside == 0,
              "one in-window block must be counted inside only");
        Check(s.vertex3Inside == 3 && s.vertex3Outside == 0 && s.vertex2Inside == 0,
              "the block's 3-D vertices must be counted inside");
        Check(s.hasInsideSample && s.insideSample.valid && s.insideSample.vertices == 3,
              "the in-window block must be the interval's candidate");
        Check(VecIs(s.insideSample.first, 0.0f, 0.0f, 0.0f),
              "the candidate's first vertex must be the block's first vertex");
        Check(VecIs(s.insideSample.min, 0.0f, -5.0f, 0.0f) &&
              VecIs(s.insideSample.max, 10.0f, 2.0f, 4.0f),
              "the candidate's bounds must be exact over the whole block");
    }

    // 2-D and 3-D are counted separately, and outside blocks never become candidates.
    {
        ImmediateModeObserver o;
        o.Configure(true);
        o.NoteBegin(false);                 // the world / HUD, outside the window
        o.NoteVertex3(100.0f, 50.0f, 6.0f);
        o.NoteVertex3(140.0f, 60.0f, 8.0f);
        o.NoteEnd();
        o.NoteBegin(true);                  // an in-window 2-D block (z = 0)
        o.NoteVertex2(0.0f, 0.0f);
        o.NoteVertex2(100.0f, 50.0f);
        o.NoteVertex2(100.0f, 0.0f);
        o.NoteEnd();
        const ImmediateModeSnapshot s = o.ComputeSnapshot();
        Check(s.beganOutside == 1 && s.vertex3Outside == 2 && s.vertex3Inside == 0,
              "an outside block's 3-D vertices must be counted outside");
        Check(s.vertex2Inside == 3 && s.vertex2Outside == 0,
              "2-D vertices must be counted separately from 3-D, by side");
        Check(s.hasInsideSample && s.insideSample.vertices == 3,
              "the in-window 2-D block is still an in-window block");
        Check(Near(s.insideSample.min[2], 0.0f) && Near(s.insideSample.max[2], 0.0f),
              "a 2-D block's z must be 0, i.e. planar -- the shape check can see it");
    }

    // The interval's candidate is the LARGEST in-window block, not the first.
    {
        ImmediateModeObserver o;
        o.Configure(true);
        o.NoteBegin(true);
        o.NoteVertex3(0.5f, 0.5f, 0.0f);
        o.NoteVertex3(-0.5f, -0.5f, 0.0f);
        o.NoteEnd();
        o.NoteBegin(true);
        for (int i = 0; i < 120; ++i) o.NoteVertex3(0.1f * i, 0.2f * i, 0.05f * i);
        o.NoteEnd();
        const ImmediateModeSnapshot s = o.ComputeSnapshot();
        Check(s.beganInside == 2 && s.vertex3Inside == 122,
              "both in-window blocks must be counted");
        Check(s.hasInsideSample && s.insideSample.vertices == 120,
              "the candidate must be the largest in-window block (ADR-0483 analogue)");
        Check(VecIs(s.insideSample.min, 0.0f, 0.0f, 0.0f) &&
              Near(s.insideSample.max[0], 11.9f) && Near(s.insideSample.max[2], 5.95f),
              "the candidate's bounds must be the largest block's, exact");
    }

    // A malformed begin-while-open does not merge blocks; an unmatched glEnd is not one.
    {
        ImmediateModeObserver o;
        o.Configure(true);
        o.NoteBegin(true);
        o.NoteVertex3(1.0f, 1.0f, 1.0f);
        o.NoteBegin(true);                  // missing glEnd from the caller
        o.NoteVertex3(2.0f, 2.0f, 2.0f);
        o.NoteEnd();
        o.NoteEnd();                        // unmatched: must be ignored
        const ImmediateModeSnapshot s = o.ComputeSnapshot();
        Check(s.beganInside == 2 && s.endsInside == 1 && s.vertex3Inside == 2,
              "a begin-while-open must close the previous block, not merge them "
              "(the unmatched glEnd must not be counted)");
        Check(s.hasInsideSample && s.insideSample.vertices == 1,
              "the closed 1-vertex block must be the candidate (strictly larger wins)");
    }

    // ComputeSnapshot re-bases the interval but keeps an open block pending.
    {
        ImmediateModeObserver o;
        o.Configure(true);
        o.NoteBegin(true);
        o.NoteVertex3(1.0f, 2.0f, 3.0f);
        const ImmediateModeSnapshot first = o.ComputeSnapshot();
        Check(first.beganInside == 1 && first.vertex3Inside == 1 && first.openInside &&
              !first.hasInsideSample,
              "an open in-window block must be reported, not dropped");
        o.NoteVertex3(4.0f, 5.0f, 6.0f);
        o.NoteEnd();
        const ImmediateModeSnapshot second = o.ComputeSnapshot();
        Check(second.beganInside == 0 && second.vertex3Inside == 1 &&
              second.hasInsideSample && second.insideSample.vertices == 2,
              "the reopened interval must hold only the block's remaining vertices");
    }

    // Configure(false) drops everything (a flag flip cannot leak a sample).
    {
        ImmediateModeObserver o;
        o.Configure(true);
        o.NoteBegin(true);
        o.NoteVertex3(1.0f, 1.0f, 1.0f);
        o.NoteEnd();
        o.Configure(false);
        o.Configure(true);
        const ImmediateModeSnapshot s = o.ComputeSnapshot();
        Check(s.beganInside == 0 && !s.hasInsideSample && s.vertex3Inside == 0,
              "disabling observation must drop any accumulated block");
    }

    if (g_failures == 0) {
        std::printf("immediate mode observer: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "immediate mode observer: %d failure(s)\n", g_failures);
    return 1;
}
