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
// On by default (`DisplayBranchColdStartLatch=1`) since the 2026-10-18 live
// A/B (ADR-0478): in a live XRSIM run the latch demoted the letterboxed startup
// page (BarScan fill 0.40) to cinema while real gameplay still reached
// full-bleed (fill 1.00, `fullBleed=1`), and no full-bleed page appeared before
// gameplay (the case the header above said needed the A/B). Set
// `DisplayBranchColdStartLatch=0` for the legacy verdict.
//
// Pure and dependency-free so the decision is unit-tested headlessly.

namespace RiddickVR {

struct DisplayBranchConfig {
    bool coldStartLatch = true;
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

// GH #91: *which* input holds the branch on the cinema side. The `DisplayMode:`
// line is emitted only when the verdict *changes*, so a run that never leaves
// CINEMA prints a single line and says nothing about why -- the branch had to be
// inferred (ADR-0512 measured exactly one `DisplayMode:` line and zero
// `FULL-BLEED` lines for an 80 s run that was in-level). This names the holding
// term so a live probe states it instead of requiring the inference.
//
// `CinemaHoldTerm(...) != None` is exactly `ComputeDisplayBranch(...)` for the
// same inputs (see the consistency test); `menuCursor` is the *debounced*
// cursor-menu bit the verdict uses, not the raw per-frame cursor sighting.
enum class CinemaTerm { None, No3D, MenuCursor, ColdStartLetterbox };

inline CinemaTerm CinemaHoldTerm(bool has3dThisFrame, bool menuCursor,
                                 bool coldStartLatch, bool letterboxed,
                                 bool fullBleedSeen) {
    if (!has3dThisFrame) return CinemaTerm::No3D;
    if (menuCursor) return CinemaTerm::MenuCursor;
    if (coldStartLatch && letterboxed && !fullBleedSeen)
        return CinemaTerm::ColdStartLetterbox;
    return CinemaTerm::None;
}

inline const char* CinemaTermName(CinemaTerm term) {
    switch (term) {
        case CinemaTerm::No3D: return "no3d";
        case CinemaTerm::MenuCursor: return "menuCursor";
        case CinemaTerm::ColdStartLetterbox: return "coldStartLetterbox";
        case CinemaTerm::None: return "none";
    }
    return "none";
}

// GH #91: the branch-input probe cadence (swaps). One `DisplayBranch:` line per
// interval, so a run always states the terminal inputs even with no transition.
inline constexpr unsigned kDisplayBranchProbeEverySwaps = 60;

} // namespace RiddickVR
