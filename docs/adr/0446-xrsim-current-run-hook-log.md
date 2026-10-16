# ADR 0446: XRSIM reads the current hook-log run, so scenario markers are reproducible

## Status
**Accepted** — the XRSIM harness recovers the hook log's *current run* section by
its run header instead of a pre-run byte offset, so markers logged early in the
new run are no longer silently dropped; the `melee` scenario is re-scripted so
its declared markers (`COMBAT MODE entered`, `RIOT BATON ARC JAB`) actually fire
under the mock. No game file is modified; no binary is deployed.

## Context
Issue #69 asks for a scripted XRSIM run whose interaction markers are
reproducible. Both wake 38 and wake 39 recorded that the `melee` scenario's
markers "did not fire under the mock" and blamed the scripted controller state.

Two independent defects were found this wake:

1. **The harness read the wrong bytes.** `tools/xrsim.py` recorded the hook
   log's size *before* launching the game (`log_mark`) and then evaluated
   `LOG_FILE.read_bytes()[log_mark:]`. But `Logger::Initialize`
   (`src/hook/logging.cpp:24`) opens `riddick_vr.log` with
   `std::ios::out | std::ios::trunc`: **every launch rewrites the file from
   offset 0**. A pre-run byte mark is therefore not a run boundary — when the
   previous run's log was longer than the new one, the slice is empty (or starts
   mid-run) and any marker logged before the old length vanishes.
   *Measured:* the wake-39 log was **4108 B**; the validation run's truncated log
   is **3404 B** with `COMBAT MODE entered` at byte **2292** and
   `RIOT BATON ARC JAB` at byte **2673** → the old slice `[4108:]` is **empty**,
   so both markers would be reported missing even though the hook logged them.
2. **The scenario expected the wrong classification.** The previous `melee.json`
   held **both** triggers (0.90) through the punch. Both triggers engage the
   block (`Combat State: AIM / BLOCK engaged (Left Trigger)`), and while
   `st.currentBlockEngaged` the forward thrust is dispatched as the *parry*
   branch (`isForwardParry`, logged `Physical Gesture: PARRY / COUNTER-STRIKE
   THRUST`) before the trajectory jab — so `RIOT BATON ARC JAB` could never be
   emitted. The scenario also let `ApplyScenarioFrame` overwrite the mock's
   built-in forward walk (`thumbstickY` defaults to 0 in a keyframe).

## Decision
1. **Run-scoped log extraction.** `tools/xrsim.py` gains `RUN_HEADER`
   (`"=== Riddick VR Log Started at"`), `extract_run_section(text)` (text after
   the **last** run header; whole text when absent) and
   `read_fresh_hook_log(path, log_mark=0)` (prefers the header; falls back to the
   byte mark only for a header-less/appending producer). `run_sim` and
   `--evaluate-only` both use it; the report records `hook_log_scope`
   (`current-run section` | `whole file`) so the evidence is attributable.
2. **Marker verdicts in the report.** `marker_summary(checks)` records each
   scenario marker's pass/fail in the report (`markers`), and the marker check's
   detail states the run-section length.
3. **The `melee` scenario drives what it asserts.** Walk forward into the level
   (`stickLY 0.85`), hold both triggers to enter the combat stance, **release the
   block** (`ltrig 0.10`, `rtrig` stays 0.90 for `isBatonArcJab`), then a fast
   forward right-hand jab (`rz −0.26 → −0.42` over 3 frames ≈ **4.8 m/s**). Grips
   are 0.50 so the guard-baton *discharge* branches cannot co-opt the log.
4. **Regression invariant.** `tools/verify_reported_fixes.py` pins the
   truncating writer (`logging.cpp`) and the header-based extraction, so the
   harness cannot silently revert to a byte offset.

## Evidence (2026-10-16, live mock run; `dist/riddickvr-host.exe` 18:21)
- `python tools/xrsim.py --scenario tools/scenarios/melee.json --timeout 150`
  (live, `RIDDICKVR_SIM=1`, scratch artifacts) →
  `marker:COMBAT MODE entered` **PASS**, `marker:RIOT BATON ARC JAB` **PASS**
  (both read from `hook log run section (3404 chars)` → `hook_log_scope =
  current-run section`); `gameplay_captures_present` **PASS — 8 gameplay /
  5 cinema / 0 unknown of 13**; the 8 gameplay frames register `dx=-14, dy=0,
  identical=False`.
- The hook log shows the intended chain: `Combat State: AIM / BLOCK engaged
  (Left Trigger)` → `Combat Stance: COMBAT MODE entered (both triggers held)` →
  `3D Gameplay projection intercepted` → `Combat Stance: EXPLORATION MODE
  restored` (block released) → `Physical Gesture: RIOT BATON ARC JAB detected
  (Vz: 4.80 m/s, Speed: 4.80 m/s)`.
- `python tools/test_xrsim.py` **25/25 PASS** (4 new: last-run section chosen;
  a larger pre-run byte mark is shown to drop the markers while
  `read_fresh_hook_log` keeps them; header-less fallback; `--evaluate-only` with
  `--hook-log`/`--scenario` reports the marker map and still fails an un-fired
  marker); `xrsim.py --selftest` PASS; `scenario_tests.py --check` PASS;
  `verify_reported_fixes.py` (extended #69 invariant) PASS. No C++/asset changed
  → no `build-all.ps1`/CTest rerun.

## Explicitly not claimed
- **Requirement 1 (gameplay earlier) is still open.** The dump window is still
  5 cinema frames (1–60) + 8 gameplay (75–180); this wake did not move the
  gameplay transition. The now-available mechanism is host-side: the mock host
  already reads `cinemaMode` per frame, so it can gate dumps on gameplay (which
  would also drop the all-zero interop-warmup frames 1/15/30). That is a
  separate host-C++ unit.
- **Not validated in-headset.** The marker path is exercised with the mock host.
- **The same byte-offset pattern is unchanged elsewhere.**
  `tools/semi_play.py`, `tools/self_test_pipeline.py`,
  `tools/scenario_tests.py --validate --since-byte` and the recipe in
  `docs/vr-scenarios.md` still slice by a pre-run byte mark and are vulnerable to
  the same truncation. They are outside the loop's vehicle (semi_play injects
  desktop input) and should adopt `read_fresh_hook_log` in a follow-up.
- **The residual live failures are unchanged and unattributed to this change:**
  frames 1/15/30 are all-zero (`interop_produced_content`, interop warmup), frame
  15 has `dy=-4`, and the mock host's `FrameBudget` line fails
  (`mean=15.92 max=755.38 @11.11 ms` — a mock cadence artefact).

## Reproduce
```
python tools/test_xrsim.py
python tools/scenario_tests.py --check
python tools/xrsim.py --selftest
python tools/xrsim.py --scenario tools/scenarios/melee.json --timeout 150 \
    --artifacts <scratch> --report-file <scratch>\report.json
```

## Files
- `tools/xrsim.py` (`RUN_HEADER`, `extract_run_section`, `read_fresh_hook_log`,
  `marker_summary`, report `markers`/`hook_log_scope`)
- `tools/test_xrsim.py` (4 new tests → 25)
- `tools/scenarios/melee.json` (reproducible markers: unblock, then jab)
- `tools/verify_reported_fixes.py` (extended GH #69 invariant)
- cross-refs: ADR-0445 (#69 per-frame branch), `src/hook/logging.cpp`,
  `src/hook/camera_hook.cpp` (`isForwardParry` @4400, trajectory jab @8177),
  #68/#69, #62/#65
