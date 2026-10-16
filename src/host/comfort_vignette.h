#pragma once

// GH #68 requirement 5 (comfort rules): "comfort vignette applied during
// scripted movement/turning".
//
// The hook derives a locomotion comfort level `comfortVignette` (0..1) from
// thumbstick move/turn activity (Track C #9, `src/hook/camera_hook.cpp`) and
// publishes it in the shared state. `StereoRenderer` consumes it: a
// motion-scaled extra eye shift plus a real peripheral dimming of each eye FBO
// while it is >= 0.01 and `ComfortEnabled=1` (`src/hook/stereo_renderer.cpp`).
// Nothing asserted that the level *reaches* the renderer, so a config/parse
// regression that silently disabled the vignette would go unnoticed without a
// headset.
//
// The mock host therefore reads the value back every frame, accumulates a
// summary alongside the per-frame locomotion input it published, and prints a
// machine-readable line the XRSIM harness parses into the `comfort` check
// `comfort_vignette_applied`. The accumulate/format logic is a pure,
// dependency-free helper so `tests/comfort_vignette_tests.cpp` can pin the wire
// format without a GPU or the game.
//
// Format (one line, printed periodically):
//   [Mock Host] ComfortVignette samples=.. max=.. mean=.. above=..
//     locomotion=.. max_locomotion=.. threshold=..
//
// `locomotion` counts frames on which a move/turn thumbstick magnitude was
// published; `max_locomotion` is the largest comfort level observed on those
// frames -- the value the check asserts against `threshold`. When no locomotion
// frame was published the vignette is not expected and the check is skipped.

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace RiddickVR {

// A comfort level at or below this is "not applied". The renderer's own gate is
// `>= 0.01` and the hook ramps the level with a 0.2 lerp, so 0.05 is a margin
// above pure noise: a value that never meaningfully rose cannot read as applied.
constexpr float kComfortVignetteMin = 0.05f;

// Thumbstick magnitude (|x|+|y|) at/above which a frame counts as locomotion.
// Matches the hook's move/turn deadzone intent without coupling to its exact
// `moveDeadzone` (the mock publishes ~0.85 for a deliberate walk).
constexpr float kComfortLocomotionMag = 0.2f;

// Running summary of a run's published comfort level + locomotion activity.
struct ComfortVignetteStats {
    std::uint64_t samples = 0;            // frames with a published level
    std::uint64_t locomotionSamples = 0;  // of which locomotion was active
    std::uint64_t above = 0;              // frames above `threshold`
    double maxV = 0.0;                    // worst-case (clamped) level
    double sumV = 0.0;                    // for the mean
    double maxWhileLocomotion = 0.0;      // what the check asserts on
    double threshold = kComfortVignetteMin;
};

// Add one frame's published level (clamped to [0, 1]) plus whether locomotion
// input was active that frame.
inline void AccumulateComfort(ComfortVignetteStats& s, float comfort,
                              bool locomotionActive) {
    float v = comfort;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    ++s.samples;
    s.sumV += v;
    if (v > s.maxV) s.maxV = v;
    if (v > s.threshold) ++s.above;
    if (locomotionActive) {
        ++s.locomotionSamples;
        if (v > s.maxWhileLocomotion) s.maxWhileLocomotion = v;
    }
}

inline double ComfortMean(const ComfortVignetteStats& s) {
    return s.samples ? s.sumV / static_cast<double>(s.samples) : 0.0;
}

// The machine-readable line `tools/xrsim.py::COMFORT_VIGNETTE_RE` parses.
// Deterministic: fixed + 3 decimals.
inline std::string FormatComfortVignetteLine(const ComfortVignetteStats& s) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(3);
    os << "[Mock Host] ComfortVignette"
       << " samples=" << s.samples
       << " max=" << s.maxV
       << " mean=" << ComfortMean(s)
       << " above=" << s.above
       << " locomotion=" << s.locomotionSamples
       << " max_locomotion=" << s.maxWhileLocomotion
       << " threshold=" << s.threshold;
    return os.str();
}

}  // namespace RiddickVR
