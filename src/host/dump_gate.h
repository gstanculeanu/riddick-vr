#pragma once

// GH #69 requirement 1: gate the XRSIM artifact dump window on the display
// branch the hook published for each frame.
//
// The mock host used to dump on a fixed frame cadence, so a scripted run's
// capture window included the menu/cinema frames -- and the all-zero
// GL<->D3D11 interop warm-up frames -- before the game reached live 3D
// gameplay. An identical left/right pair there is *correct* (the renderer's
// cinema branch deliberately blits the same source rect with no IPD shift), so
// those frames made the run look like a gameplay-stereo failure and diluted
// the capture window (#69's "early/static eye captures" complaint).
//
// With ``--dump-gameplay-only`` a frame is captured only once the hook reports
// 3D gameplay (``cinemaMode == false``), so the dump window is live gameplay
// and ``not_mono_duplicate`` / ``horizontal_disparity_present`` are meaningful.
//
// GH #72 requirement 2 (content gate): a frame can carry a *gameplay* branch
// label and still be worthless for grading -- the very first GL<->D3D11 interop
// frames are all-black, and a static 2D startup page repeats byte-identically
// across the window. ``ShouldDumpFrame`` therefore also takes the frame's
// content verdict and keeps only ``Live`` content, so the graded window is
// actually live, advancing 3D gameplay.
//
// Pure and dependency-free so it is unit-testable (tests/dump_gate_tests.cpp).

namespace RiddickVR {

// GH #72 requirement 2: the content verdict for a candidate capture.
enum class FrameContent {
    Live,    // advancing, non-blank content: worth grading
    Blank,   // all-black GL<->D3D11 interop warm-up (mean luma <= ~1)
    Frozen,  // identical to the previously dumped frame (static 2D page)
};

inline bool ShouldDumpFrame(bool dumpDue, bool dumpGameplayOnly,
                            bool stateRead, bool cinemaMode,
                            FrameContent content = FrameContent::Live) {
    if (!dumpDue) {
        return false;
    }
    if (!dumpGameplayOnly) {
        return true; // legacy behaviour: any frame the cadence selects
    }
    // Gameplay-only: the branch must be *known* (``stateRead``) and be 3D
    // gameplay. A frame whose state was not read must not be captured as if it
    // were gameplay -- it would carry no trustworthy display-branch label.
    if (!(stateRead && !cinemaMode)) {
        return false;
    }
    // GH #72 requirement 2: blank interop warm-up and frozen repeats are not
    // live gameplay, so they must not enter the graded window.
    return content == FrameContent::Live;
}

}  // namespace RiddickVR
