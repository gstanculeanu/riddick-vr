# ADR-0498 — XRSIM: an attempt that stays live but measures nothing is also not a measurement (`no_evidence`, retry once)

- Status: accepted
- Date: 2026-10-18
- Refs: GH #87 (this, extension), GH #71 (reach live gameplay), GH #80 (sim input
  bridge), GH #68 (iteration vehicle), ADR-0479 (the early-exit fix this extends),
  ADR-0497 (the wake-96 reach that measured this signature)

## Context

ADR-0479 made one **no-measurement** signature of a live run decidable: the game
process *ends on its own before* `--min-live-seconds` (default 5) with no fresh
hook log and no captured frame — an **early exit** — which is retried once and,
if it repeats, reported as one distinct `game_started` failure instead of a wall
of capture FAILs.

Its own "Honest limits" left the **other** way to measure nothing explicitly
unhandled:

> A game that stays alive the full window but never attaches the hook is **not**
> an early exit — it grades as before (`frames_captured` FAIL) because it is a
> real, reproducible defect, not the measured flake.

Autoloop wake 96 measured exactly that signature as a **flake**, not a defect.
While establishing the GH #71 reach (ADR-0497), its *first* live attempt ran with
the game alive **47 s** and produced **0 captured frames and no hook log at all**
— the mock host logged `Game Connected: WAITING` for the whole window — and yet
the harness graded `game_started` **PASS**, because the game had "stayed live".
The zero frames then produced the usual capture FAILs (`frames_captured`,
`gameplay_full_bleed_reached`, …): a **false red** that reads as "the fix is
broken". The **identical re-run** worked (62 distinct gameplay frames), so the
attempt was a flake the retry machinery never saw because the game had not
*exited*. Wake 96 recorded it as the "GH #87 extension lead".

The distinction the harness needs is therefore not "did the process exit?" but
"**did this attempt produce any evidence at all?**". Evidence is a captured frame
or a fresh hook log; without either, the hook never attached and no capture check
is meaningful.

## Decision

Add the second no-measurement state and treat both the same way.

### 1. `no_evidence` in the pure `game_start_decision`

`game_start_decision(game_exited, live_seconds, frames, hook_fresh,
min_live_seconds)` now returns, in order:

- `started` — there is **evidence** (>= 1 captured frame or a *fresh* hook log;
  "fresh" is an **mtime change**, per ADR-0479). A `started` attempt is graded
  normally and **never retried**.
- `no_evidence` — **no** evidence, and the attempt ran for at least
  `min_live_seconds` (whether the game *exited* at that point or was still live
  when the harness terminated it). The hook never attached, so the run measured
  nothing. This is the wake-96 signature.
- `early_exit` — **no** evidence, and the game process ended *before*
  `min_live_seconds` (the ADR-0479 signature).
- A harness-terminated run *shorter* than `min_live_seconds` with no evidence is
  still `started`: a `--frames` run is bounded by frame count, not wall clock, so
  such a game may simply be too short to prove anything yet (the pre-extension
  behaviour for genuinely short runs is preserved).

### 2. Retry both no-measurement states

`launch_retry_decision(state, attempt, max_attempts)` retries a not-yet-final
`early_exit` **or** `no_evidence`; a real result is never retried, and the bound
(`--max-launch-attempts`, default 2) is unchanged. The `launch` report key and the
one-line summary now carry the `state`, and the summary labels the attempt `NO
EVIDENCE` (`early_exit` stays `DID NOT START`).

### 3. Same distinct failure

`evaluate`'s single `game_started` check is unchanged in name and semantics
(`ok = launch["game_started"]`); when it fails, the capture checks are demoted to
reported-not-graded (`demote_checks_for_no_game_start`), and the check now also
carries the `state`. The demotion detail reads "the attempt was not a measurement
-- the hook never attached" regardless of which of the two states caused it.

## Consequences

- The wake-96 false red becomes attributable: the attempt is retried, and a
  transient no-hook launch no longer produces a red at all. If a *persistent*
  failure to attach the hook repeats, it is reported as one distinct
  `game_started` failure (`state=no_evidence`) with the capture FAILs demoted —
  still visible, no longer noisy.
- A run with any evidence (a frame or a fresh hook log) is never retried,
  including a short host-bounded `--frames` run where the game is still alive at
  the break. `--evaluate-only` (no launch record) is unchanged.
- This **corrects ADR-0479's** "Honest limits" claim that a live-but-hook-less
  window "grades as before because it is a real, reproducible defect": it was
  measured as a flake (wake 96), and the harness now distinguishes a *repeated*
  no-evidence failure from a one-off.

## Honest limits

- `no_evidence` is detected from the harness's *outcome* signals (captured frames
  / hook-log mtime) alone. It does not diagnose *why* the hook did not attach
  (shadow-install race, DLL load failure, a launch that never created a window).
  It makes the failure cheap and attributable, not explained.
- The retry can double the wall-clock cost of a run that will fail (2 ×
  `--duration`); the bound is one extra attempt, as for early exits.
- Harness-only. No C++, no hook/game change, no deploy, no install write. The
  `no_evidence` **live** path is covered by pure unit tests and the evaluator;
  this wake did not reproduce the wake-96 flake on demand (it is intermittent).

## Validation (offline)

- `tools/xrsim.py --selftest` — the `game_start_decision_states` and
  `launch_retry_bounds_early_exit` cases extended with the `no_evidence` states.
- `tools/test_xrsim.py` — `test_game_start_decision_states`,
  `test_launch_retry_is_bounded_to_one_early_exit` updated, and a new
  `test_no_evidence_run_fails_once_and_demotes_capture_checks`.
- `tools/verify_reported_fixes.py` — the GH #87 source invariant extended
  (`"no_evidence"`, the new test, and this ADR).
- No C++ changed, so no `build-all.ps1`/CTest.
