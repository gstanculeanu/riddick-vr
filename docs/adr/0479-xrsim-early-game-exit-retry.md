# ADR-0479 — XRSIM: an early game exit is not a measurement (retry once, report `game_started`)

- Status: accepted
- Date: 2026-10-18
- Refs: GH #87 (this), GH #68 (iteration vehicle), GH #81 (hook identity / the
  shadow install), GH #83 (per-run isolation), ADR-0471, ADR-0459, ADR-0478

## Context

Every deploy-gated unit is now validated *live* through a read-only shadow
install (ADR-0471), so the loop depends on a live run being a measurement of the
change under test. On 2026-10-18 (wake 77, the GH #72 latch A/B) that dependency
failed intermittently: **2 of 3 fresh-shadow first launches** ended with
`DarkAthena.exe` exiting within ~2 s of launch — no hook log at all (or one that
stopped at `Stereo Eye Framebuffers successfully created`, before
`OnSwapBuffers: Frame 1`), the mock host logging `Game Connected: WAITING` for
the whole (short) run, and 0 captured frames. The harness then reported
real-looking FAILs (`frames_captured`, `gameplay_full_bleed_reached`,
`frame_budget_reported`) — indistinguishable from "the fix is broken" — and the
**identical re-run passed** (run C2: 603 host-log lines, 6030 frames, 59
gameplay frames, `fill=1.00`).

The signature was already in the artifacts (the host heartbeat and the
missing/partial hook log), but nothing in the harness *acted* on it, and the
report could not distinguish "the game never started" from "the game started and
the feature is broken". Filed as GH #87 with the run C / C2 pair as evidence.

## Decision

Treat an attempt whose game process ends before it produced any evidence as a
**non-measurement**, and make that decidable and attributable.

### 1. A pure `game_start_decision` (offline-testable)

`game_start_decision(game_exited, live_seconds, frames, hook_fresh,
min_live_seconds)` returns `started` or `early_exit`:

- `started` — the game produced **evidence** (>= 1 captured frame, or a *fresh*
  hook log) or stayed live at least `min_live_seconds` (default `5.0`), or the
  harness terminated it (the game never exited on its own — a host-bounded /
  `--duration` / timeout break leaves `game_exited` False).
- `early_exit` — the game process ended before `min_live_seconds` with **no**
  captured frame and **no** fresh hook log. This is the measured GH #87
  signature.

"Hook log fresh" is an **mtime change**, not mere existence: the log lives in the
(game/shadow) directory and outlives an attempt, so a dead second attempt must
not be made to look live by a first attempt's log.

### 2. Retry once, then fail distinctly

`run_sim` runs up to `--max-launch-attempts` (default `2`) attempts; only a
not-yet-final `early_exit` is retried (`launch_retry_decision`, pure). Each
attempt records `live_seconds`, `frames`, `hook_fresh`, `game_exited`,
`game_exit_code`; the run report gains a `launch` key with the whole list and a
one-line `summary`.

`evaluate` appends a single `game_started` check. When the game never started,
the checks that would otherwise fail on the missing captures
(`frames_captured`, `frame_budget_reported`, `gameplay_full_bleed_reached`, …)
are **demoted to reported-not-graded** (`demote_checks_for_no_game_start`), and
`game_started` is the one failure — labelled with the attempt count, the last
attempt's lived seconds, frames and hook-log freshness. The hook identity
(`deployed_hook_matches_build`) stays graded: it is a fact about what *would*
have been exercised and is independent of whether this attempt reached a live
scene.

## Consequences

- A startup flake can no longer be read as a broken fix: the report says
  `game_started FAIL … launch attempts=2/2 game DID NOT START`, and the capture
  FAILs carry `(not graded: the game did not start -- GH #87)`.
- The measured cure (re-run the identical command) is now automatic, and the
  retry is bounded (one), so a run can never loop on a persistently broken
  launch.
- A run *with* evidence is never retried, including a short host-bounded
  `--frames` run where the game is still alive at the break.
- `--evaluate-only` passes no launch record, so its grading (and every existing
  report) is unchanged; a launch-less evaluation records `"launch": null`.

## Honest limits

- Early exit is detected as "the game process ended on its own before
  `min_live_seconds` with no evidence". A game that stays alive the full window
  but never attaches the hook is **not** an early exit — it grades as before
  (`frames_captured` FAIL) because it is a real, reproducible defect, not the
  measured flake.
- The 5 s threshold is calibrated to the measured event (~2 s) versus the
  documented startup time (~15 s to gameplay); it is a harness heuristic, not a
  property of the engine.
- This is harness-only. It does not explain *why* `DarkAthena.exe` occasionally
  exits in ~2 s (no hook log, renderer init incomplete); it makes that failure
  attributable and cheap instead of a false red. No game or hook change, no
  deploy, no install write — every run in scope used a `%TEMP%` shadow.

## Validation (offline)

- `tools/xrsim.py --selftest` — +2 cases: `game_start_decision_states`,
  `launch_retry_bounds_early_exit` (now 13 checks) PASS.
- `tools/test_xrsim.py` — +5 cases: `test_game_start_decision_states`,
  `test_launch_retry_is_bounded_to_one_early_exit`,
  `test_no_game_start_fails_once_and_demotes_capture_checks`,
  `test_no_launch_record_keeps_capture_grading`,
  `test_launch_reliability_flags_parse` PASS.
- `tools/verify_reported_fixes.py` — a GH #87 source invariant (summary now
  `#40 … #83, #85, #86, #87`).
- No C++ changed, so no `build-all.ps1`/CTest; no live run (the change is harness
  bookkeeping and is validated by the pure decisions and the evaluator).
