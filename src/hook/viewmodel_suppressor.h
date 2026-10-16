#pragma once

// GH #63 / GH #66 (approach 2): per-draw suppression of the engine's own
// first-person viewmodel (arms/hands).
//
// Approach 1 (forcing the RPG item's `noviewmodel` bit) was falsified in
// ADR-0438: the flag is an engine *output* (set/cleared every frame by
// `FUN_100e6aa0`) and an exhaustive raw-byte scan of every shipped x86 module
// finds no direct `[item+0x14c] & 2` reader. Approach 2 suppresses at the draw
// level instead: the hook forwards the engine's GL draw calls and this header
// decides whether a given draw belongs to the first-person viewmodel pass.
//
// Evidence for the signature candidates (docs/rea/reference/RndrGL_GL_Path.txt):
//   * RndrGL.dll is the *only* shipped module that imports OpenGL. Its geometry
//     path is `glDrawElements` (vertex arrays) and `glCallList` (display lists);
//     it also imports `glBegin`/`glEnd` but no `glDrawArrays`.
//   * It imports `glClear`, `glLoadMatrixf`, `glFrustum`, `glViewport`, `glScissor`
//     -- all already intercepted -- so a mid-frame *depth-only* clear
//     (GL_DEPTH_BUFFER_BIT with no colour bit) is observable through the entry
//     points the hook already sees.
// The classic first-person-era trick is exactly that: draw the world, clear
// depth, then draw the viewmodel so it never intersects walls. So the first
// signature is "the window opened by a depth-only clear, closed by the next
// projection load or the frame swap".
//
// This is a PROVISIONAL heuristic: it is OFF by default (SuppressEngineViewmodel
// = 0) and must be confirmed on a headset (one run with the counters logged)
// before it is trusted. When disabled, ShouldSuppressDraw() is a pure `false`
// and the draw hooks forward verbatim -- zero behaviour change.
//
// GH #67 step 2 (observation-only): the SAME depth-only-clear boundary is used
// to *identify* the first-person viewmodel pass before it is driven from the
// controller or suppressed. With `observe` on (ViewmodelObserve=1) the window
// opens and the per-frame tallies plus a representative GL_MODELVIEW are
// recorded, but ShouldSuppressDraw() remains a pure `false` -- nothing is ever
// skipped. The dispatcher formats one `ViewmodelProbe` log line per sampled
// frame (see opengl_proxy.cpp). Observation is subordinate to the suppression
// switch: enabling suppression suppresses *without* observing unless both are
// set.
//
// Pure, dependency-free state machine: it uses no Windows/GL headers so it
// compiles on the x64 host and is unit-tested headlessly (no game launch), like
// combat_guard.h / combat_strike.h.

namespace RiddickVR {

// Which observed event opens the suppression window.
enum class ViewmodelWindowOpener {
    None,             // never open: the suppressor is inert even when enabled
    DepthOnlyClear,   // GL_DEPTH_BUFFER_BIT clear with no colour bit
};

struct ViewmodelSuppressionConfig {
    // Master switch. false => ShouldSuppressDraw() is always false.
    bool enabled = false;
    // Window opener (see the header comment). Default is the classic
    // depth-only-clear boundary; PROVISIONAL, select on a headset.
    ViewmodelWindowOpener opener = ViewmodelWindowOpener::DepthOnlyClear;
    // GH #67 step 2: observation-only mode. The window opens on the same
    // boundary and the per-frame draw tallies plus a representative modelview
    // are recorded, but ShouldSuppressDraw() still returns false -- nothing is
    // ever skipped. This is how the first-person viewmodel pass is *identified*
    // before it is driven from the controller (or suppressed, GH #63). Default
    // off.
    bool observe = false;
};

// GH #67 step 2: what the observation pass saw in one frame. Pure data so the
// draw dispatcher can format one parseable log line. `sample` is the
// GL_MODELVIEW in effect at the FIRST draw observed inside the window (the pass
// usually loads one viewmodel-relative matrix and draws its submeshes), or
// nullptr when no draw was inside the window this frame.
struct ViewmodelObservationSnapshot {
    bool windowOpened = false; // a depth-only clear opened the window this frame
    unsigned drawsInside = 0;  // draws observed with the window open
    unsigned drawsOutside = 0; // draws observed with the window closed
    bool hadSample = false;    // a modelview was captured this frame
    // GH #67 step 1: matrix composition seen inside the window. RndrGL builds a
    // sub-object transform with glPushMatrix + glTranslatef (it does not import
    // glMultMatrixf/glRotatef), so a non-zero count here is direct evidence that
    // the sampled modelview must be the *composed* one (gl_matrix_tracker.h).
    unsigned pushesInside = 0;
    unsigned translatesInside = 0;
    float sample[16] = {};     // column-major, as uploaded to GL_MODELVIEW
};

// The suppressor tracks one frame at a time. Frame boundaries are the
// full (colour) clear and the buffer swap; a depth-only clear may open the
// suppression window mid-frame, and the next projection load or swap closes it.
class ViewmodelSuppressor {
public:
    void Configure(const ViewmodelSuppressionConfig& cfg) { config_ = cfg; }

    const ViewmodelSuppressionConfig& Config() const { return config_; }
    bool Enabled() const { return config_.enabled; }
    bool WindowOpen() const { return windowOpen_; }

    // Frame start: a full colour clear. Closes any open window and re-bases the
    // per-frame counters.
    void NoteColorClear() {
        windowOpen_ = false;
        LatchFrameCounters();
    }
    // Mid-frame depth-only clear. Opens the window when that opener is selected.
    void NoteDepthOnlyClear() {
        if ((config_.enabled || config_.observe) &&
            config_.opener == ViewmodelWindowOpener::DepthOnlyClear) {
            windowOpen_ = true;
            if (config_.observe) {
                observeWindowOpened_ = true;
                if (observeDrawsInside_ == 0) hasObserveSample_ = false;
            }
        }
    }

    // Any projection (re)load ends the first-person pass: the world resumes.
    void NoteProjectionLoad() { windowOpen_ = false; }

    // Buffer swap: end of frame. Close the window and publish the frame tallies.
    void NoteSwap() {
        windowOpen_ = false;
        LatchFrameCounters();
    }

    // Called once per intercepted engine draw call. Returns true when this draw
    // should be skipped. Counters are only maintained while enabled so the
    // disabled path stays a single predictable branch.
    bool ShouldSuppressDraw() {
        if (!config_.enabled) return false;
        ++frameDraws_;
        if (windowOpen_) {
            ++frameSuppressed_;
            ++totalSuppressed_;
            return true;
        }
        return false;
    }

    // GH #67 step 2 (observation-only): called once per intercepted draw in
    // observe mode, before/independent of ShouldSuppressDraw. Counts the draw by
    // window state and (via the dispatcher's SetObservedSample) captures the
    // first in-window modelview. Never suppresses.
    void NoteObservedDraw() {
        if (windowOpen_) {
            ++observeDrawsInside_;
            observeWindowOpened_ = true;
        } else {
            ++observeDrawsOutside_;
        }
    }

    // Records the GL_MODELVIEW in effect at the first draw inside the window.
    // Only the first per window is kept (the first-person pass loads one matrix
    // and draws its submeshes), so later world/HUD matrices cannot overwrite it.
    void SetObservedSample(const float* matrix16) {
        if (!windowOpen_ || hasObserveSample_) return;
        for (int i = 0; i < 16; ++i) observeSample_[i] = matrix16[i];
        hasObserveSample_ = true;
    }

    // GH #67 step 1: count the matrix-composition calls seen inside the window.
    // No-ops unless observing with the window open, so the suppression path is
    // untouched. Called from the RndrGL glPushMatrix/glTranslatef hooks.
    void NoteObservedPush() {
        if (config_.observe && windowOpen_) ++observePushes_;
    }
    void NoteObservedTranslate() {
        if (config_.observe && windowOpen_) ++observeTranslates_;
    }

    bool Observing() const { return config_.observe; }

    // Publishes this frame's observation and re-bases the observation counters.
    ViewmodelObservationSnapshot ComputeSnapshot() {
        ViewmodelObservationSnapshot snap;
        snap.windowOpened = observeWindowOpened_;
        snap.drawsInside = observeDrawsInside_;
        snap.drawsOutside = observeDrawsOutside_;
        snap.hadSample = hasObserveSample_;
        snap.pushesInside = observePushes_;
        snap.translatesInside = observeTranslates_;
        if (hasObserveSample_) {
            for (int i = 0; i < 16; ++i) snap.sample[i] = observeSample_[i];
        }
        observeWindowOpened_ = false;
        observeDrawsInside_ = 0;
        observeDrawsOutside_ = 0;
        hasObserveSample_ = false;
        observePushes_ = 0;
        observeTranslates_ = 0;
        return snap;
    }

    // Per-frame tallies (draws seen / draws suppressed while enabled).
    unsigned FrameDraws() const { return frameDraws_; }
    unsigned FrameSuppressed() const { return frameSuppressed_; }
    // Tallies from the most recently completed frame.
    unsigned LastFrameDraws() const { return lastFrameDraws_; }
    unsigned LastFrameSuppressed() const { return lastFrameSuppressed_; }
    // Cumulative suppressed draws since Configure.
    unsigned long long TotalSuppressed() const { return totalSuppressed_; }

private:
    void LatchFrameCounters() {
        lastFrameDraws_ = frameDraws_;
        lastFrameSuppressed_ = frameSuppressed_;
        frameDraws_ = 0;
        frameSuppressed_ = 0;
    }

    ViewmodelSuppressionConfig config_{};
    bool windowOpen_ = false;
    unsigned frameDraws_ = 0;
    unsigned frameSuppressed_ = 0;
    unsigned lastFrameDraws_ = 0;
    unsigned lastFrameSuppressed_ = 0;
    unsigned long long totalSuppressed_ = 0;

    // GH #67 step 2 observation state (never affect suppression).
    bool observeWindowOpened_ = false;
    bool hasObserveSample_ = false;
    unsigned observeDrawsInside_ = 0;
    unsigned observeDrawsOutside_ = 0;
    unsigned observePushes_ = 0;
    unsigned observeTranslates_ = 0;
    float observeSample_[16] = {};
};

} // namespace RiddickVR
