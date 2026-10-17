#pragma once

// GH #68 requirement 2: the frame-time budget must be asserted on a *frame
// pacing* statistic (p50/p95), not on the mean and the single worst frame.
//
// The mock host follows the game's own uncapped swap rate, so its per-frame
// cadence is the game's render cost plus any load hitch. A one-off level-load
// stall (~760 ms, measured) dominates `max_ms` and the arithmetic `mean_ms`, so
// the previous `mean <= budget && max <= 1.5*budget` rule failed every live run
// for a reason that is *not* a 90 Hz render cost (ADR-0448/0454). The honest
// questions are:
//   * p50 -- the typical frame (is the render keeping up?), and
//   * p95 -- the frame the user actually feels (do <=5% of frames blow it?),
// while `max_ms` and the drop count are still *reported* so a real stall stays
// visible without being misread as a sustained regression.
//
// The samples are the game/hook's rendered-frame cadence: the host subtracts
// its own artifact-capture time from the loop delta (GH #78), so the harness
// does not grade itself (see FrameTimeExcludingHostWork below).
//
// This is a pure, dependency-free helper so `tests/frame_time_tests.cpp` can pin
// the percentile definition and the exact wire format the Python parser
// (tools/xrsim.py) relies on, without a GPU or the game.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace RiddickVR {

// Default budget: one 90 Hz frame.
constexpr double kDefaultFrameBudgetMs = 1000.0 / 90.0;  // ~11.11 ms

// Safety bound on the retained per-frame sample window (the host runs a
// practically-unbounded frame loop, so the percentile window must be capped).
constexpr std::size_t kMaxFrameTimeSamples = 1u << 20;  // ~1.05M frames

// Nearest-rank percentile over an *ascending* sample vector. `q` in [0, 1]:
// rank = ceil(q * n), clamped to [1, n]; the rank-th smallest value. This is a
// well-defined, monotonic, interpolate-free definition (the value returned is
// always a real sample), chosen so a budget assertion reads as "p95 of the
// measured frames". Empty input is 0.0 (reported as "no samples").
inline double PercentileMs(const std::vector<double>& sortedAscending, double q) {
    const std::size_t n = sortedAscending.size();
    if (n == 0) return 0.0;
    if (q <= 0.0) return sortedAscending.front();
    if (q >= 1.0) return sortedAscending.back();
    // ceil(q*n) is in [1, n] for q in (0, 1); guard with an integer size_t.
    std::size_t rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(n)));
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return sortedAscending[rank - 1];
}

// One summary of a run's per-frame host cadence.
struct FrameTimeStats {
    std::uint64_t samples = 0;      // valid frame-time samples
    double meanMs = 0.0;            // arithmetic mean (whole run)
    double maxMs = 0.0;             // worst single frame (whole run)
    double p50Ms = 0.0;             // median frame
    double p95Ms = 0.0;             // 95th percentile frame
    std::uint64_t overBudget = 0;   // "drops": samples > budgetMs
    double budgetMs = kDefaultFrameBudgetMs;
};

// Summarise a run's frame times. Sorts a copy, so the caller's vector (the
// arrival order) is untouched. `budgetMs <= 0` falls back to the 90 Hz default.
inline FrameTimeStats SummarizeFrameTimes(const std::vector<double>& frameTimesMs,
                                          double budgetMs) {
    FrameTimeStats s;
    s.budgetMs = budgetMs > 0.0 ? budgetMs : kDefaultFrameBudgetMs;
    s.samples = static_cast<std::uint64_t>(frameTimesMs.size());
    if (frameTimesMs.empty()) return s;

    std::vector<double> sorted = frameTimesMs;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (double t : sorted) sum += t;
    s.meanMs = sum / static_cast<double>(sorted.size());
    s.maxMs = sorted.back();
    s.p50Ms = PercentileMs(sorted, 0.5);
    s.p95Ms = PercentileMs(sorted, 0.95);
    for (double t : sorted) {
        if (t > s.budgetMs) ++s.overBudget;
    }
    return s;
}

// GH #78: the mock host's artifact capture is *harness work*, not a rendered
// frame. Every `--dump-interval` frames it writes two full-resolution BMPs
// (2496x2688 = 26.8 MB each) plus the branch/eye/JSON sidecars -- on the same
// thread and inside the same loop iteration whose delta is sampled as a frame
// time. At the simulated eye size that is tens to >100 ms, so the measured
// `p95` tracked the capture cadence (16.9/104.8 ms) while `p50` stayed ~3.5 ms:
// the harness was grading itself. The budget must judge the game/hook's
// rendered-frame cadence, so the measured capture duration is subtracted.
//
// A delta that is entirely host work has no rendered frame in it; returning 0
// tells the caller to skip the sample rather than record a fake fast frame.
// Real stalls (level loads, shader compiles) are not host work and still count.
inline double FrameTimeExcludingHostWork(double loopDeltaMs, double hostWorkMs) {
    if (!(loopDeltaMs > 0.0)) return 0.0;
    if (!(hostWorkMs > 0.0)) return loopDeltaMs;
    const double netMs = loopDeltaMs - hostWorkMs;
    return netMs > 0.0 ? netMs : 0.0;
}

// Format the machine-readable `FrameBudget` line (the whole line, prefix
// included) that `tools/xrsim.py` parses. Deterministic: fixed + 2 decimals.
inline std::string FormatFrameBudgetLine(const FrameTimeStats& s) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2);
    os << "[Mock Host] FrameBudget"
       << " samples=" << s.samples
       << " mean_ms=" << s.meanMs
       << " max_ms=" << s.maxMs
       << " over_budget=" << s.overBudget
       << " budget_ms=" << s.budgetMs
       << " p50_ms=" << s.p50Ms
       << " p95_ms=" << s.p95Ms;
    return os.str();
}

}  // namespace RiddickVR
