#pragma once

// GH #72 requirement 1 (ADR-0459): the published display-branch bit
// (`cinemaMode`) must not label a *static 2D* startup page as 3D gameplay.
//
// The bit is derived from OpenGLProxy::Has3DThisFrame() (a perspective
// projection was loaded) plus a sustained menu cursor. The engine's startup
// splash pages (legal/attribution, ESRB) load a perspective projection, so
// Has3DThisFrame() is true and the hook logs
// `DisplayMode: FULL-BLEED gameplay (3Dfresh=1 menuCursor=0)` for a page that
// is in fact a letterboxed 2D image. Every downstream consumer (the XRSIM
// gameplay-only dump gate, the per-frame stereo skip, marker attribution)
// then trusts a wrong label.
//
// The structural counter-signal is already measured by the hook's own BarScan:
// the startup splash's active rect covers ~0.31-0.36 of the window while live
// gameplay covers 1.00 (GH #72 evidence).
//
// Fix: a *cold-start* latch. Before the first full-bleed frame has ever been
// measured, a frame whose active rect is clearly letterboxed is demoted to
// cinema. Once one full-bleed frame has been seen the latch is set and the
// demotion can never fire again, so GH #44 (real gameplay that still carries
// engine-baked bars) is unaffected after the first gameplay frame.
//
// Off by default (`DisplayBranchColdStartLatch=0`): the thresholds are
// provisional and the "a full-bleed page appears before gameplay" case needs
// one live A/B before the bit is trusted by default.
//
// Pure and dependency-free so the decision is unit-tested headlessly.

namespace RiddickVR {

struct DisplayBranchConfig {
    bool coldStartLatch = false;
    // Active-rect area / window area at or above which a frame counts as
    // full-bleed (live gameplay, measured 1.00).
    float fullBleedFill = 0.90f;
    // At or below which a frame counts as a letterboxed 2D page (the startup
    // splash, measured 0.31-0.36).
    float letterboxFill = 0.75f;
};

struct DisplayBranchState {
    // Latched true the first time a full-bleed frame is measured; never reset.
    bool fullBleedSeen = false;
    // Last *decisive* letterbox verdict (a fill inside the two thresholds
    // holds this value, so an eased/smoothed rect cannot flicker the branch).
    bool letterboxed = false;
};

// Returns true when this frame is the cinema branch. `activeFill` is the
// BarScan active-rect area / window area; `haveFill` is false until a scan has
// produced a rect, so the frames before the first measurement keep the legacy
// verdict. `state` is updated in place and must persist across frames.
inline bool ComputeDisplayBranch(bool has3dThisFrame, bool menuCursor,
                                 float activeFill, bool haveFill,
                                 const DisplayBranchConfig& cfg,
                                 DisplayBranchState& state) {
    // Only a frame the engine rendered in 3D and did not mark as a menu can be
    // evidence of gameplay; a full-bleed *2D* page must not set the latch.
    if (has3dThisFrame && !menuCursor && haveFill) {
        if (activeFill >= cfg.fullBleedFill) {
            state.fullBleedSeen = true;
            state.letterboxed = false;
        } else if (activeFill <= cfg.letterboxFill) {
            state.letterboxed = true;
        }
        // Between the thresholds: keep the previous verdict.
    }

    // The legacy (and always-on) cinema signals are unchanged.
    if (!has3dThisFrame || menuCursor) return true;

    if (!cfg.coldStartLatch) return false;
    return state.letterboxed && !state.fullBleedSeen;
}

} // namespace RiddickVR
