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
// GH #63 / GH #66 window semantics (ADR-0446): the first perspective projection
// load after the depth-only clear is the first-person pass's OWN projection, not
// the end of that pass, so a perspective load closes the window only once at
// least one draw was seen inside it. An orthographic load (HUD) always closes.
// A second opener, NearPlaneDrop, opens the window when a perspective load has a
// near plane well below the world pass's near plane (viewmodels use a short
// near plane). `track` keeps the window and counters live without suppressing,
// so the pass signature can be logged (FrameTrace) or the engine arms can be
// driven by the controller (viewmodel_drive.h) with suppression off.
//
// GH #67 step 2 (observation-only, union with the above): the SAME window is
// used to *identify* the first-person viewmodel pass before it is driven from
// the controller. With `observe` on (ViewmodelObserve=1) the window opens and
// the per-frame tallies plus a representative GL_MODELVIEW are recorded, but a
// draw is never skipped -- ShouldSuppressDraw() stays false unless `enabled` is
// also set. The dispatcher formats one `ViewmodelProbe` log line per sampled
// frame (see opengl_proxy.cpp). Observation is independent of the suppression
// and track switches; enabling suppression suppresses *without* observing
// unless both are set.
//
// It uses no Windows/GL headers so it
// compiles on the x64 host and is unit-tested headlessly (no game launch), like
// combat_guard.h / combat_strike.h.

#include <cstring>

namespace RiddickVR {

// Which observed event opens the suppression window.
enum class ViewmodelWindowOpener {
    None,             // never open: the suppressor is inert even when enabled
    DepthOnlyClear,   // GL_DEPTH_BUFFER_BIT clear with no colour bit
    NearPlaneDrop,    // perspective load whose near plane drops below the world's
    Either,           // depth-only clear or near-plane drop
};

struct ViewmodelSuppressionConfig {
    // Master switch. false => ShouldSuppressDraw() is always false.
    bool enabled = false;
    // GH #66: keep the window and counters live without suppressing draws.
    bool track = false;
    // GH #67 step 2: observation-only mode. The window opens on the same
    // boundary and the per-frame draw tallies plus a representative modelview
    // are recorded, but ShouldSuppressDraw() still returns false -- nothing is
    // ever skipped. This is how the first-person viewmodel pass is *identified*
    // before it is driven from the controller (or suppressed, GH #63). Default
    // off.
    bool observe = false;
    // Window opener (see the header comment). Default is the classic
    // depth-only-clear boundary; PROVISIONAL, select on a headset.
    ViewmodelWindowOpener opener = ViewmodelWindowOpener::DepthOnlyClear;
    // NearPlaneDrop: open when near < worldNear * nearRatio.
    float nearRatio = 0.5f;
};

// GH #67 step 2: what the observation pass saw in one frame. Pure data so the
// draw dispatcher can format one parseable log line. `sample` is the
// GL_MODELVIEW in effect at the FIRST draw observed inside the window (the pass
// usually loads one viewmodel-relative matrix and draws its submeshes), or
// all-zero when no draw was inside the window this frame.
struct ViewmodelObservationSnapshot {
    bool windowOpened = false; // the window opened this frame
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

// One frame's pass signature as tokens: C (colour clear), D (depth-only clear),
// P<near> (perspective load, 1 decimal, P? when unknown), O (ortho load),
// d<n> (run of n draws). Fixed buffer, no heap; tokens that do not fit are dropped.
constexpr int kViewmodelTraceCapacity = 192;

class ViewmodelSuppressor {
public:
    void Configure(const ViewmodelSuppressionConfig& cfg) { config_ = cfg; }

    const ViewmodelSuppressionConfig& Config() const { return config_; }
    bool Enabled() const { return config_.enabled; }
    bool Active() const { return config_.enabled || config_.track; }
    // GH #67 step 2: observation-only mode (window state maintained, never
    // suppresses on its own).
    bool Observing() const { return config_.observe; }
    bool WindowOpen() const { return windowOpen_; }

    // Frame start: a full colour clear. Closes any open window and re-bases the
    // per-frame counters.
    void NoteColorClear() {
        windowOpen_ = false;
        windowDraws_ = 0;
        worldNear_ = 0.0f;
        LatchFrameCounters();
        if (Active()) AppendToken("C");
    }

    // Mid-frame depth-only clear. Opens the window when that opener is selected.
    void NoteDepthOnlyClear() {
        if (!Tracking()) return;
        if (Active()) AppendToken("D");
        if (config_.opener == ViewmodelWindowOpener::DepthOnlyClear ||
            config_.opener == ViewmodelWindowOpener::Either) {
            Open();
        }
    }

    // A projection (re)load. No-arg form: perspective with unknown near plane.
    void NoteProjectionLoad() { NoteProjectionLoad(true, 0.0f); }

    void NoteProjectionLoad(bool perspective, float nearPlane) {
        if (!Tracking()) return;
        const bool nearKnown = nearPlane > 0.0f && nearPlane < 1.0e9f;
        if (!perspective) {
            if (Active()) AppendToken("O");
            windowOpen_ = false;
            return;
        }
        if (nearKnown) {
            char tok[24];
            FormatNear(tok, sizeof(tok), nearPlane);
            if (Active()) AppendToken(tok);
        } else if (Active()) {
            AppendToken("P?");
        }
        const bool hadWorldNear = worldNear_ > 0.0f;
        if (!hadWorldNear && nearKnown) worldNear_ = nearPlane;
        if (windowOpen_) {
            // The first load after the opener is the first-person pass's own
            // projection; the pass is over only once it has drawn something.
            if (windowDraws_ > 0) windowOpen_ = false;
        } else if (hadWorldNear && nearKnown &&
                   (config_.opener == ViewmodelWindowOpener::NearPlaneDrop ||
                    config_.opener == ViewmodelWindowOpener::Either) &&
                   nearPlane < worldNear_ * config_.nearRatio) {
            Open();
        }
    }

    // Buffer swap: end of frame. Close the window and publish the frame tallies.
    void NoteSwap() {
        windowOpen_ = false;
        windowDraws_ = 0;
        worldNear_ = 0.0f;
        LatchFrameCounters();
        FlushRun();
        std::memcpy(lastTrace_, trace_, sizeof(trace_));
        trace_[0] = '\0';
        traceLen_ = 0;
    }

    // Called once per intercepted engine draw call. Returns true when this draw
    // should be skipped (only when enabled; track/observe alone never suppress).
    bool ShouldSuppressDraw() {
        if (!Tracking()) return false;
        ++frameDraws_;
        ++drawRun_;
        if (windowOpen_) {
            ++windowDraws_;
            if (config_.enabled) {
                ++frameSuppressed_;
                ++totalSuppressed_;
                return true;
            }
        }
        return false;
    }

    // GH #67 step 2 (observation-only): called once per intercepted draw in
    // observe mode, before/independent of ShouldSuppressDraw. Counts the draw by
    // window state and (via the dispatcher's SetObservedSample) captures the
    // first in-window modelview. Never suppresses.
    void NoteObservedDraw() {
        if (!config_.observe) return;
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
        if (!config_.observe || !windowOpen_ || hasObserveSample_) return;
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

    // Per-frame tallies (draws seen / draws suppressed while active).
    unsigned FrameDraws() const { return frameDraws_; }
    unsigned FrameSuppressed() const { return frameSuppressed_; }
    // Tallies from the most recently completed frame.
    unsigned LastFrameDraws() const { return lastFrameDraws_; }
    unsigned LastFrameSuppressed() const { return lastFrameSuppressed_; }
    // Cumulative suppressed draws since Configure.
    unsigned long long TotalSuppressed() const { return totalSuppressed_; }
    // Pass signature of the most recently completed frame (latched on swap).
    const char* LastFrameTrace() const { return lastTrace_; }

private:
    // The window state machine runs whenever any consumer needs it: suppression
    // (`enabled`), pass tracing / controller drive (`track`), or the GH #67
    // step-2 probe (`observe`). Only `enabled` can skip a draw.
    bool Tracking() const { return Active() || config_.observe; }

    void Open() {
        windowOpen_ = true;
        windowDraws_ = 0;
    }

    void LatchFrameCounters() {
        lastFrameDraws_ = frameDraws_;
        lastFrameSuppressed_ = frameSuppressed_;
        frameDraws_ = 0;
        frameSuppressed_ = 0;
    }

    static void FormatNear(char* out, int cap, float v) {
        // "P" + one decimal, e.g. P0.5 / P4.0 / P120.0 (hand-rolled: no stdio in the hot path).
        if (!(v < 100000.0f)) v = 99999.9f;
        int tenths = static_cast<int>(v * 10.0f + 0.5f);
        int whole = tenths / 10;
        int frac = tenths % 10;
        char digits[12];
        int n = 0;
        do { digits[n++] = static_cast<char>('0' + whole % 10); whole /= 10; } while (whole > 0 && n < 11);
        int w = 0;
        if (cap < n + 4) { if (cap > 0) out[0] = '\0'; return; }
        out[w++] = 'P';
        while (n > 0) out[w++] = digits[--n];
        out[w++] = '.';
        out[w++] = static_cast<char>('0' + frac);
        out[w] = '\0';
    }

    void FlushRun() {
        if (drawRun_ == 0) return;
        char tok[16];
        unsigned v = drawRun_ > 99999u ? 99999u : drawRun_;
        char digits[8];
        int n = 0;
        do { digits[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v > 0 && n < 7);
        int w = 0;
        tok[w++] = 'd';
        while (n > 0) tok[w++] = digits[--n];
        tok[w] = '\0';
        drawRun_ = 0;
        AppendRaw(tok);
    }

    void AppendToken(const char* tok) {
        FlushRun();
        AppendRaw(tok);
    }

    void AppendRaw(const char* tok) {
        int len = 0;
        while (tok[len] != '\0') ++len;
        const int sep = traceLen_ > 0 ? 1 : 0;
        if (traceLen_ + sep + len + 1 > kViewmodelTraceCapacity) return; // truncate: drop the token
        if (sep) trace_[traceLen_++] = ' ';
        for (int i = 0; i < len; ++i) trace_[traceLen_++] = tok[i];
        trace_[traceLen_] = '\0';
    }

    ViewmodelSuppressionConfig config_{};
    bool windowOpen_ = false;
    unsigned windowDraws_ = 0;
    float worldNear_ = 0.0f;
    unsigned drawRun_ = 0;
    unsigned frameDraws_ = 0;
    unsigned frameSuppressed_ = 0;
    unsigned lastFrameDraws_ = 0;
    unsigned lastFrameSuppressed_ = 0;
    unsigned long long totalSuppressed_ = 0;
    int traceLen_ = 0;
    char trace_[kViewmodelTraceCapacity] = {};
    char lastTrace_[kViewmodelTraceCapacity] = {};

    // GH #67 step 2 observation state (never affects suppression).
    bool observeWindowOpened_ = false;
    bool hasObserveSample_ = false;
    unsigned observeDrawsInside_ = 0;
    unsigned observeDrawsOutside_ = 0;
    unsigned observePushes_ = 0;
    unsigned observeTranslates_ = 0;
    float observeSample_[16] = {};
};

} // namespace RiddickVR
