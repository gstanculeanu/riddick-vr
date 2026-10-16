# ADR 0455: XRSIM asserts the 90 Hz frame budget on p50/p95 frame pacing

## Status
**Accepted** — GH #68 requirement 2 (second half). Host + tooling only; no game
file is touched and no binary is deployed.

## Context
GH #68 requirement 2 asks the simulator harness to log per-frame timings and
"assert a budget (e.g. p95 ≤ 11.1 ms for 90 Hz; report p50/p95/max and drops)".
The host has emitted a `FrameBudget` line since the harness landed, but it
carried only `samples/mean_ms/max_ms/over_budget/budget_ms`, and the harness
asserted `mean_ms <= budget && max_ms <= 1.5 * budget`.

That rule **fails every live mock run for a reason that is not a render cost**.
The mock host's frame loop is gated on the *game's* frame-ready signal, so its
cadence is the game's uncapped swap rate plus any load hitch. Measured on a live
`--reach-gameplay` run (ADR-0448, `run-20261016-190802`):

```
[Mock Host] FrameBudget samples=5400 mean_ms=3.32 max_ms=763.26 over_budget=123 budget_ms=11.11
```

`mean_ms ≈ 3.3 ms` (a trivial splash renders ~250–300 FPS) and `max_ms ≈ 763 ms`
is a one-off level-load stall on frame ~N. 123 of 5400 frames (2.3 %) exceed
11.11 ms. Both the mean rule and the `1.5 * budget` max rule are defeated by
that single stall, so `frame_budget` was recorded as a permanent red FAIL and
was excluded from every "all checks green" claim (ADR-0447/0448/0454).

## Decision
1. **Report the pacing statistics.** `src/host/frame_time_stats.h` (new, pure,
   dependency-free) defines `PercentileMs` (nearest-rank, so the returned value
   is always a real sample), `SummarizeFrameTimes`, and
   `FormatFrameBudgetLine`. The host emits, every 30 frames:
   `[Mock Host] FrameBudget samples=… mean_ms=… max_ms=… over_budget=…
   budget_ms=… p50_ms=… p95_ms=…`. `samples/mean_ms/max_ms/over_budget` keep
   their previous cumulative meaning; `p50_ms/p95_ms` come from the retained
   sample window (`kMaxFrameTimeSamples`, a safety bound only — a real run is
   ~1e4–1e5 samples).
2. **Assert on p50/p95, not on mean/max.** `tools/xrsim.py::parse_budget` now
   passes when **p50 ≤ budget and p95 ≤ budget**, while still surfacing
   `max_ms` (the worst frame) and `over_budget` (the drop count) in the record
   and the report. A single load hitch no longer reads as a sustained 90 Hz
   regression; a genuine sustained regression (p95 above budget) still fails.
3. **Require the percentiles.** The regex grew an optional
   `p50_ms=… p95_ms=…` tail; a line without it is reported as
   `frame_budget_reported` FAIL ("stale host build"), never silently passed, so
   a legacy `host.log` cannot be mistaken for a validated budget.
4. **Regression invariants.** `tests/frame_time_tests.cpp` (new CTest
   `frame_time_percentiles`) pins the percentile definition, the hitch-is-not-a-
   regression behaviour, the drop count and the exact wire line; `tools/test_xrsim.py`
   pins the pass/fail/legacy cases; `tools/verify_reported_fixes.py` pins the
   helper/host/harness/CMake wiring.

## Evidence (2026-10-16; `dist\riddickvr-host.exe`)
- The motivating measurement is the `FrameBudget` line above: with 2.3 % drops,
  p95 is inside the 11.11 ms budget while `max_ms` is 763 ms — the check that
  previously failed now passes on its own merits, and a 763 ms stall stays
  reported.
- Offline: `tools/test_xrsim.py` (new `test_parse_budget_asserts_p50_p95_not_max`
  + `test_parse_budget_reports_missing_line_and_stale_host`); CTest
  `frame_time_percentiles` (x86 + x64); `xrsim.py --selftest`. Live: a
  `--reach-gameplay` run end-to-end with the rebuilt host (see the wake record /
  GH #68 comment for the final numbers).

## Explicitly not claimed
- **`max_ms` is not asserted.** A one-off stall (load, desktop scheduling) is
  *reported* but does not fail the budget; only a *sustained* regression
  (p95 > budget) does. If a hard stall ceiling is ever wanted it should be a
  separate, explicitly-toleranced check.
- **The cadence is the game's swap rate, not the OpenXR compositor's.** This
  measures the host/game frame loop the simulator drives, not a headset
  present-time; the real 90 Hz pipeline still needs the headset.
- **`no_vertical_disparity` on dark frames (GH #72 req 3)** is a separate
  verdict (ADR-0454).

## Reproduce
```
python tools/test_xrsim.py
python tools/xrsim.py --selftest
python tools/xrsim.py --reach-gameplay --duration 45 --artifacts <scratch> \
    --report-file <scratch>\report.json
```

## Files
- `src/host/frame_time_stats.h` (new), `src/host/openxr_manager.cpp`
- `tests/frame_time_tests.cpp` (new), `CMakeLists.txt` (CTest `frame_time_percentiles`)
- `tools/xrsim.py` (`FRAME_BUDGET_RE`, `parse_budget`)
- `tools/test_xrsim.py`, `tools/verify_reported_fixes.py`
- `src/host/README.md`, `docs/vr-scenarios.md`
- refs: GH #68 (harness, req 2), ADR-0447 (dump gate), ADR-0448 (reach gameplay),
  ADR-0454 (vertical-disparity verdict)
