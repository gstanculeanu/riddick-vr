// GH #68 requirement 5 (comfort rules): unit tests for the mock host's comfort
// vignette summary (src/host/comfort_vignette.h).
//
// The harness asserts `comfort_vignette_applied`: the hook-published locomotion
// comfort level must rise above the threshold on frames where the run published
// move/turn thumbstick input. These tests pin the accumulation (clamping, the
// locomotion split, the "above threshold" count) and the exact wire line
// tools/xrsim.py parses, without a GPU or the game.
#include "comfort_vignette.h"

#include <cstdio>
#include <string>

using RiddickVR::AccumulateComfort;
using RiddickVR::ComfortMean;
using RiddickVR::ComfortVignetteStats;
using RiddickVR::FormatComfortVignetteLine;
using RiddickVR::kComfortLocomotionMag;
using RiddickVR::kComfortVignetteMin;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "comfort vignette test failed: %s\n", message);
        ++g_failures;
    }
}

static bool Near(double a, double b, double eps = 1e-6) {
    return (a - b) <= eps && (b - a) <= eps;
}

int main() {
    // At rest: a level that never rises must not read as applied.
    ComfortVignetteStats rest;
    for (int i = 0; i < 50; ++i) AccumulateComfort(rest, 0.0f, false);
    Check(rest.samples == 50, "every frame must be sampled");
    Check(rest.locomotionSamples == 0, "no locomotion frames at rest");
    Check(rest.above == 0, "a zero level is never above the threshold");
    Check(Near(rest.maxV, 0.0) && Near(rest.maxWhileLocomotion, 0.0),
          "a zero level must give a zero max");

    // Moving/turning: the hook's 0.2 lerp ramps the level to ~1.0, and every
    // frame published locomotion input.
    ComfortVignetteStats moving;
    for (int i = 0; i < 20; ++i) AccumulateComfort(moving, 1.0f, true);
    Check(moving.samples == 20 && moving.locomotionSamples == 20,
          "moving frames must all carry locomotion");
    Check(Near(moving.maxWhileLocomotion, 1.0),
          "the vignette must reach 1.0 while moving");
    Check(moving.above == 20, "every moving frame is above the threshold");

    // Clamping: out-of-range published values are clamped, not propagated.
    ComfortVignetteStats clamp;
    AccumulateComfort(clamp, 1.5f, true);
    AccumulateComfort(clamp, -0.5f, true);
    Check(Near(clamp.maxV, 1.0), "levels above 1 must clamp to 1");
    Check(clamp.above == 1, "only the positive sample is above the threshold");
    Check(Near(ComfortMean(clamp), 0.5), "the mean uses the clamped values");

    // The threshold gate is strict (>) and follows `threshold`. A level exactly
    // at the threshold is not "applied"; just above is.
    ComfortVignetteStats edge;
    AccumulateComfort(edge, kComfortVignetteMin, true);
    Check(edge.above == 0, "a level exactly at the threshold is not applied");
    AccumulateComfort(edge, kComfortVignetteMin + 0.001f, true);
    Check(edge.above == 1, "a level just above the threshold is applied");
    Check(Near(edge.maxWhileLocomotion, kComfortVignetteMin + 0.001f),
          "the locomotion max tracks the applied level");

    // A raise that happens *only* while at rest (no locomotion input) is not
    // evidence of the locomotion vignette: the locomotion max stays zero.
    ComfortVignetteStats idleRise;
    AccumulateComfort(idleRise, 0.9f, false);
    Check(idleRise.locomotionSamples == 0 && Near(idleRise.maxWhileLocomotion, 0.0),
          "a level rise without locomotion must not count as applied");

    // Locomotion magnitude boundary used by the host's classification.
    Check(Near(kComfortLocomotionMag, 0.2f), "locomotion magnitude must be 0.2");

    // Wire format the Python parser (tools/xrsim.py COMFORT_VIGNETTE_RE) matches.
    const std::string line = FormatComfortVignetteLine(moving);
    Check(line.rfind("[Mock Host] ComfortVignette samples=20 max=1.000 mean=1.000"
                     " above=20 locomotion=20 max_locomotion=1.000 threshold=0.050",
                     0) == 0,
          "line must carry every field in the parsed order + shape");
    Check(FormatComfortVignetteLine(moving) == line,
          "formatting must be deterministic");

    if (g_failures == 0) {
        std::puts("comfort vignette tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
