// GH #68 requirement 2: unit tests for the mock host's frame-time summary
// (src/host/frame_time_stats.h).
//
// The harness asserts the 90 Hz budget on the *frame-pacing* percentiles
// (p50/p95), not on the arithmetic mean or the single worst frame: the mock
// host follows the game's uncapped swap rate, so one level-load hitch dominates
// max/mean without being a sustained regression. These tests pin the percentile
// definition and the exact wire line tools/xrsim.py parses, without a GPU.
#include "frame_time_stats.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using RiddickVR::FormatFrameBudgetLine;
using RiddickVR::FrameTimeStats;
using RiddickVR::kDefaultFrameBudgetMs;
using RiddickVR::PercentileMs;
using RiddickVR::SummarizeFrameTimes;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "frame time test failed: %s\n", message);
        ++g_failures;
    }
}

static bool Near(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

int main() {
    // Nearest-rank percentile on a strictly ordered 1..100 fixture:
    // p50 -> the 50th value (50), p95 -> the 95th value (95).
    std::vector<double> ordered;
    for (int i = 1; i <= 100; ++i) ordered.push_back(static_cast<double>(i));
    Check(Near(PercentileMs(ordered, 0.5), 50.0), "p50 of 1..100 must be 50");
    Check(Near(PercentileMs(ordered, 0.95), 95.0), "p95 of 1..100 must be 95");
    Check(Near(PercentileMs(ordered, 0.0), 1.0), "q=0 must be the minimum");
    Check(Near(PercentileMs(ordered, 1.0), 100.0), "q=1 must be the maximum");
    Check(Near(PercentileMs(std::vector<double>{}, 0.95), 0.0),
          "an empty sample set must yield 0");

    // Monotonicity + the value is always a real sample (no interpolation).
    Check(PercentileMs(ordered, 0.5) <= PercentileMs(ordered, 0.95),
          "percentiles must be monotonic in q");
    Check(RiddickVR::PercentileMs(std::vector<double>{1.0, 2.0, 3.0, 4.0}, 0.5) == 2.0,
          "p50 of {1,2,3,4} must be a real sample (2)");

    // The summary must not be poisoned by one hitch: p50/p95 stay on the bulk
    // while max_ms records the stall and over_budget counts the drops.
    std::vector<double> times(1000, 7.0);          // typical frame 7 ms
    times[3] = 763.0;                              // one level-load hitch
    for (int i = 0; i < 20; ++i) times[100 + i] = 30.0;  // 2% late frames
    FrameTimeStats s = SummarizeFrameTimes(times, kDefaultFrameBudgetMs);
    Check(s.samples == 1000, "sample count must be the number of frames");
    Check(Near(s.p50Ms, 7.0), "p50 must track the typical frame, not the hitch");
    Check(s.p95Ms <= kDefaultFrameBudgetMs,
          "p95 must stay inside budget with only 2% late frames");
    Check(Near(s.maxMs, 763.0), "max_ms must still record the hitch");
    Check(s.overBudget == 21, "drops must count every frame above budget");
    Check(Near(s.meanMs, (979.0 * 7.0 + 763.0 + 20.0 * 30.0) / 1000.0),
          "mean_ms must be the arithmetic mean of the run");

    // A sustained regression fails the p95 budget even without a hitch.
    std::vector<double> slow(200, 25.0);
    FrameTimeStats s2 = SummarizeFrameTimes(slow, kDefaultFrameBudgetMs);
    Check(s2.p95Ms > kDefaultFrameBudgetMs, "a sustained slow run must blow p95");
    Check(s2.overBudget == 200, "every slow frame is a drop");

    // Empty input is reported (0 samples), not a crash.
    FrameTimeStats empty = SummarizeFrameTimes(std::vector<double>{}, 0.0);
    Check(empty.samples == 0 && Near(empty.p50Ms, 0.0) && Near(empty.p95Ms, 0.0),
          "empty summary must be all zeros");
    Check(Near(empty.budgetMs, kDefaultFrameBudgetMs),
          "a non-positive budget must fall back to the 90 Hz default");

    // Wire format the Python parser (tools/xrsim.py FRAME_BUDGET_RE) matches.
    const std::string line = FormatFrameBudgetLine(s);
    Check(line.rfind("[Mock Host] FrameBudget samples=1000 mean_ms=", 0) == 0,
          "line must open with the prefix + samples= + mean_ms=");
    const char* keys[5] = {" max_ms=", " over_budget=", " budget_ms=",
                           " p50_ms=", " p95_ms="};
    for (const char* k : keys) {
        Check(line.find(k) != std::string::npos, "line must carry every field");
    }
    // Fixed 2-decimal fields so the regex `[\d.]+` is unambiguous.
    Check(line.find(" p50_ms=7.00") != std::string::npos,
          "p50 must be formatted to 2 decimals");
    Check(FormatFrameBudgetLine(s) == line, "formatting must be deterministic");

    if (g_failures == 0) {
        std::puts("frame time tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
