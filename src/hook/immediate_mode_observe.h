#pragma once

// GH #67 step 1 (ninth correction, ADR-0491) -- census the first-person window's
// submission *mechanism*, including the one path the probe has never counted.
//
// The submitted-geometry observer (vertex_submission_observe.h) counts only the
// two draw entry points it IAT-hooks in RndrGL: `glDrawElements` (indexed client
// arrays) and `glCallList` (display lists). Immediate mode -- `glBegin`/`glEnd`
// with `glVertex3f`/`glVertex3fv` (and the 2-D `glVertex2f`/`glVertex2i`) -- is
// *bound* by the shipped module and never counted:
//
//   * `docs/rea/reference/RndrGL_GL_Path.txt` section 2 recorded `glBegin`/`glEnd`
//     as imported and explicitly NOT intercepted ("a begin/end pair gate is left
//     as a follow-up **if the first-person arms turn out to use immediate mode**").
//   * The same reference's 2009 correction measured that the live module's static
//     OPENGL32 import table is 96 entry points and describes it as a
//     "fixed-function **immediate-mode** renderer" -- and a re-run of
//     `python tools/gl_vocabulary_scan.py --imports` on the live install lists
//     `glBegin`, `glEnd`, `glVertex3f`, `glVertex3fv`, `glVertex2f`, `glVertex2i`.
//
// So after eight corrections that sampled only arrays/lists, the census is
// incomplete in a way the previous readings could not see: a mesh submitted with
// `glBegin(GL_TRIANGLE_STRIP); glVertex3f(..); glEnd();` produces **no**
// `glDrawElements` call at all, so no array sample can ever be the arms. This
// observer answers the remaining, cheap, mechanism question:
//
//   * do immediate-mode primitives fall inside the first-person window, and are
//     they 3-D (`glVertex3f`/`glVertex3fv`) or 2-D (`glVertex2f`/`glVertex2i`)?
//   * what are the bounds of the window's **largest** immediate block? A block is
//     one `glBegin`..`glEnd` pair, i.e. one submitted primitive batch -- the
//     immediate-mode analogue of "the pass's biggest mesh" (ADR-0483). Its bounds
//     are kept exactly (three compares per vertex -- cheap) so the same shape
//     question (planar vs volumetric) can be asked of it.
//
// Observation-only and inert unless `ViewmodelObserve=1`; it never suppresses a
// draw. No GL/Windows headers: plain types only, so it is unit-tested headlessly
// like vertex_submission_observe.h / vertex_program_observe.h.

namespace RiddickVR {

// One immediate-mode block (one glBegin..glEnd) sampled from the first-person
// window. `vertices` is the block's total vertex count; the bounds are exact over
// every vertex of the block (a block is one primitive batch, so this is the mesh).
struct ImmediateBlockSample {
    bool valid = false;       // at least one vertex was folded into the bounds
    unsigned vertices = 0;    // vertices submitted in the block
    float first[3] = {};      // the block's first vertex
    float min[3] = {};        // exact axis-aligned bounds over the block
    float max[3] = {};
};

// One sampled interval's immediate-mode tallies, split by window side. The
// vertex counts are classified by the *block's* side (an engine cannot legally
// emit a vertex outside glBegin..glEnd), so a block that began inside the window
// is counted inside even if the window closes mid-block.
struct ImmediateModeSnapshot {
    unsigned beganInside = 0;      // glBegin calls with the window open
    unsigned beganOutside = 0;     // glBegin calls with the window closed
    unsigned endsInside = 0;       // glEnd calls for inside blocks
    unsigned endsOutside = 0;      // glEnd calls for outside blocks
    unsigned vertex3Inside = 0;    // glVertex3f/glVertex3fv vertices, inside
    unsigned vertex3Outside = 0;
    unsigned vertex2Inside = 0;    // glVertex2f/glVertex2i vertices, inside
    unsigned vertex2Outside = 0;
    bool openInside = false;       // an in-window block was still open at the sample
    bool hasInsideSample = false;  // an in-window block was sampled
    ImmediateBlockSample insideSample; // the interval's largest in-window block
};

class ImmediateModeObserver {
public:
    void Configure(bool observe) {
        observe_ = observe;
        if (!observe) Reset();
    }
    bool Observing() const { return observe_; }

    // glBegin(mode): opens a primitive batch. `insideWindow` is the first-person
    // window state at the call, which classifies the whole block.
    void NoteBegin(bool insideWindow) {
        if (!observe_) return;
        if (insideWindow) ++beganInside_; else ++beganOutside_;
        // Defensive: a begin while a block is open (a caller-side glEnd we did not
        // see, e.g. an entry point we do not hook) must not merge two blocks.
        if (inBlock_) FinishBlock();
        inBlock_ = true;
        blockInside_ = insideWindow;
        blockVertices_ = 0;
        block_ = ImmediateBlockSample{};
    }

    // glVertex3f / glVertex3fv.
    void NoteVertex3(float x, float y, float z) { NoteVertex(x, y, z, true); }
    // glVertex2f / glVertex2i (the 2-D path; z is 0).
    void NoteVertex2(float x, float y) { NoteVertex(x, y, 0.0f, false); }

    // glEnd: closes the batch and, for an in-window block, keeps it when it is the
    // interval's largest (the immediate-mode analogue of ADR-0483's largest mesh).
    void NoteEnd() {
        if (!observe_) return;
        if (!inBlock_) return;               // an unmatched glEnd: not a block
        if (blockInside_) ++endsInside_; else ++endsOutside_;
        FinishBlock();
    }

    // Publishes this interval's tallies/sample and re-bases the interval counters.
    // A block open at sample time is *not* discarded (its vertices are still
    // pending); it is only reported via `openInside`.
    ImmediateModeSnapshot ComputeSnapshot() {
        ImmediateModeSnapshot snap;
        snap.beganInside = beganInside_;
        snap.beganOutside = beganOutside_;
        snap.endsInside = endsInside_;
        snap.endsOutside = endsOutside_;
        snap.vertex3Inside = vertex3Inside_;
        snap.vertex3Outside = vertex3Outside_;
        snap.vertex2Inside = vertex2Inside_;
        snap.vertex2Outside = vertex2Outside_;
        snap.openInside = inBlock_ && blockInside_;
        snap.hasInsideSample = hasInsideSample_;
        snap.insideSample = insideSample_;
        beganInside_ = beganOutside_ = endsInside_ = endsOutside_ = 0u;
        vertex3Inside_ = vertex3Outside_ = vertex2Inside_ = vertex2Outside_ = 0u;
        hasInsideSample_ = false;
        insideSample_ = ImmediateBlockSample{};
        return snap;
    }

    void Reset() {
        beganInside_ = beganOutside_ = endsInside_ = endsOutside_ = 0u;
        vertex3Inside_ = vertex3Outside_ = vertex2Inside_ = vertex2Outside_ = 0u;
        inBlock_ = false;
        blockInside_ = false;
        blockVertices_ = 0u;
        block_ = ImmediateBlockSample{};
        hasInsideSample_ = false;
        insideSample_ = ImmediateBlockSample{};
    }

private:
    void NoteVertex(float x, float y, float z, bool is3d) {
        if (!observe_ || !inBlock_) return;
        ++blockVertices_;
        if (blockInside_) { if (is3d) ++vertex3Inside_; else ++vertex2Inside_; }
        else              { if (is3d) ++vertex3Outside_; else ++vertex2Outside_; }
        if (blockVertices_ == 1u) {
            block_.first[0] = x; block_.first[1] = y; block_.first[2] = z;
            block_.min[0] = x; block_.min[1] = y; block_.min[2] = z;
            block_.max[0] = x; block_.max[1] = y; block_.max[2] = z;
        } else {
            if (x < block_.min[0]) block_.min[0] = x;
            if (y < block_.min[1]) block_.min[1] = y;
            if (z < block_.min[2]) block_.min[2] = z;
            if (x > block_.max[0]) block_.max[0] = x;
            if (y > block_.max[1]) block_.max[1] = y;
            if (z > block_.max[2]) block_.max[2] = z;
        }
    }

    void FinishBlock() {
        inBlock_ = false;
        if (!blockInside_ || blockVertices_ == 0u) return; // never a candidate empty/outside
        if (hasInsideSample_ && blockVertices_ <= insideSample_.vertices) return;
        block_.vertices = blockVertices_;
        block_.valid = blockVertices_ > 0u;
        insideSample_ = block_;
        hasInsideSample_ = true;
    }

    bool observe_ = false;
    unsigned beganInside_ = 0, beganOutside_ = 0;
    unsigned endsInside_ = 0, endsOutside_ = 0;
    unsigned vertex3Inside_ = 0, vertex3Outside_ = 0;
    unsigned vertex2Inside_ = 0, vertex2Outside_ = 0;
    bool inBlock_ = false;
    bool blockInside_ = false;
    unsigned blockVertices_ = 0;
    ImmediateBlockSample block_;
    bool hasInsideSample_ = false;
    ImmediateBlockSample insideSample_;
};

} // namespace RiddickVR
