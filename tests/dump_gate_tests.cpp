// GH #69 requirement 1: unit tests for the XRSIM artifact dump gate
// (src/host/dump_gate.h).
//
// The gate decides whether the mock host captures a frame. The defect it fixes:
// the dump window was selected by frame cadence alone, so a scripted run spent
// the window on the menu/cinema branch (and all-zero interop warm-up frames),
// where identical left/right eyes are *correct* and therefore cannot validate
// gameplay stereo. With gameplay-only set, a frame is captured only once the
// hook publishes 3D gameplay (``cinemaMode == false``) for that frame.
#include "dump_gate.h"

#include <cstdio>

using RiddickVR::ShouldDumpFrame;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "dump gate test failed: %s\n", message);
        ++g_failures;
    }
}

int main() {
    // Cadence: a frame not due is never dumped, in either mode.
    Check(!ShouldDumpFrame(/*dumpDue=*/false, /*gameplayOnly=*/false, true, false),
          "a frame off the cadence must not be dumped (legacy)");
    Check(!ShouldDumpFrame(false, true, true, false),
          "a frame off the cadence must not be dumped (gameplay-only)");

    // Legacy behaviour is unchanged: any due frame is dumped regardless of
    // branch, so pre-existing tools that do not opt in keep working.
    Check(ShouldDumpFrame(true, /*gameplayOnly=*/false, /*stateRead=*/true, /*cinema=*/false),
          "legacy mode must dump a due gameplay frame");
    Check(ShouldDumpFrame(true, false, true, /*cinema=*/true),
          "legacy mode must still dump a due cinema frame (no behaviour change)");
    Check(ShouldDumpFrame(true, false, /*stateRead=*/false, false),
          "legacy mode must not require a readable state");

    // Gameplay-only: a due gameplay frame with a known state is captured...
    Check(ShouldDumpFrame(true, /*gameplayOnly=*/true, true, false),
          "gameplay-only must dump a due 3D-gameplay frame");
    // ... a cinema/menu frame is skipped (#69: identical eyes are correct there)...
    Check(!ShouldDumpFrame(true, true, true, /*cinema=*/true),
          "gameplay-only must skip a cinema frame");
    // ... and an unknown branch (state not read) is skipped rather than
    // mislabelled as gameplay.
    Check(!ShouldDumpFrame(true, true, /*stateRead=*/false, false),
          "gameplay-only must skip a frame whose display branch is unknown");

    if (g_failures == 0) {
        std::puts("dump gate tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
