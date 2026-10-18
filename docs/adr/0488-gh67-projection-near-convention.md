# ADR-0488 — GH #67 step 1: the engine's projection is z-flipped, so the near-plane extraction (not the engine) made `NearPlaneDrop` inert

- Status: accepted
- Date: 2026-10-18
- Refs: GH #67 (step 1), GH #63/#66 (the window heuristic), GH #68 (the
  iteration vehicle), ADR-0487 (the opener never fired because every load read
  `P?`), `src/hook/projection_near.h`, `src/hook/opengl_proxy.cpp`,
  `tools/xrsim.py`

## Context

ADR-0487 measured that the `NearPlaneDrop` opener never opened (`arrayIn=0` in
21/21 intervals) and that the pass trace read `P?` on **every** projection load,
i.e. the hook's near extraction rejected the engine's matrix. That ADR was
explicit that this was a **hook-side measurement gap, not an engine property**:
the raw matrix (`m[10]`, `m[14]`) was not logged, so *why* the classic
`m[14] / (m[10] - 1)` failed was unmeasured, and its own "Next" said to log it and
fix the extraction.

## What was measured

One live run, unchanged recipe (`--reach-gameplay --duration 45 --advance-menu
--advance-min-taps 8`, read-only shadow of the 2009 install,
`--require-fresh-hook`, `--ini ViewmodelObserve=1 --ini ViewmodelTrace=1`), with a
diagnostic added to the projection path that logs the matrix depth row and both
extractions (`ViewmodelProbe proj [#N]: m0=.. m5=.. m10=.. m11=.. m14=.. m15=..
persp=1 classic=.. near=..`).

The run reached a live level (`gameplay_full_bleed_reached` PASS, 32/32 gameplay
captures, hook `sha256=4c9744b31a34` = the repo build). **Four** distinct
projections were sampled; the world pass (143 samples) is:

```
m0=0.63021 m5=-1.33333 m10=1.00391 m11=1.00000 m14=-8.01566 m15=0.00000
persp=1 classic=-2048.007812 accepted/near=0
```

and the three others are `m10=1.00005 m14=-3.60010` (110), `m10=1.00010
m14=-0.20001` (16) and the 1-sample startup page.

The convention is **z-flipped** relative to classic GL:

| element | classic GL | measured engine |
|---|---|---|
| `m[10]` | `-(f+n)/(f-n)` < 0 | `+(f+n)/(f-n)` > 0 |
| `m[11]` | `-1` | `+1` |
| `m[14]` | `-2fn/(f-n)` | `-2fn/(f-n)` (same) |
| `m[15]` | `0` | `0` |

Consequences, exactly as measured:

- The classic extraction returns **`-far`**: `m14/(m10-1) = -2048.0078`, negative
  → rejected → `P?` on every load (the ADR-0487 symptom, now explained).
- The z-flipped extraction `-m14/(m10+1)` gives **near 4.0**, and the matching
  `-m14/(m10-1)` gives **far 2048** — a round, self-consistent pair
  (`m10 = 2052/2044`, `m14 = -16384/2044`).
- The other matrices are equally round under the same rule: near **1.8**
  (far 72002) and near **0.1** (far 2000.1). The host's synthetic GL eye
  projection (`m10=-1.0001 m11=-1 m14=-0.1`, `eye_frames.log`) yields 0.05 through
  the *classic* form — the two orientations are distinguishable, and the
  discriminator is the sign of `m[11]`.

## Decision

1. **A pure, headless extractor for both orientations.** New
   `src/hook/projection_near.h`: `IsPerspectiveProjection(m)` (`|m[15]| < 0.01`)
   and `ExtractProjectionNear(m)` →
   - `m[11] < 0` (classic GL): `near = m[14] / (m[10] - 1)`
   - `m[11] ≥ 0` (measured engine): `near = -m[14] / (m[10] + 1)`
   with `kProjectionNearUnknown = 0` for ortho, a degenerate denominator,
   NaN/inf, or a non-positive/absurd result — never a plausible wrong number,
   because the caller compares near planes to open a window. Unit-tested
   (`tests/projection_near_tests.cpp`: the host matrix → 0.05; the three measured
   engine matrices → 4.0 / 1.8 / 0.1; ortho and garbage → unknown).
2. **`opengl_proxy.cpp` uses it** instead of the inline classic formula, and keeps
   a diagnostic line (`ViewmodelProbe proj`) at the same cadence as the other
   summary lines recording the depth row, the classic extraction and the used
   near plane. The diagnostic is observation-only: no forwarded matrix changes.
3. **XRSIM parses and grades the record.** `viewmodel_projection_stats` reads
   `ViewmodelProbe proj` lines (`perspective`, `near_known`, `distinct`), and the
   new `projection_near_extracted` check FAILs when the record exists and no
   perspective load yielded a near plane (i.e. the extraction is still broken) —
   it is skipped only for a pre-#0488 hook, never used to hide a silent failure.
   The record is also written to the report (`viewmodel_projection`).
4. **The `NearPlaneDrop` "inert" note is retracted.** `src/hook/README.md` and the
   ADR-0487 record are updated: the opener now opens.

## Consequences

- The pass trace on a live level reads real planes:
  `P4.0 P4.0 P4.0 P4.0 P1.8 C d144 C d137 P1.8 … D d12 P4.0` — the depth-only
  clear is still visible and the near-plane drops (4.0 → 1.8 → 0.1) are now
  measurable.
- With `--ini ViewmodelWindowOpener=2` the window **opens**: 3 probe intervals,
  up to 1322 in-window draws (our first non-zero `arrayIn` for that opener), and
  `viewmodel_pass_identified` PASS. The depth-only control on the same build has
  18 intervals / up to 4564 in-window draws.
- The graded shape gate `viewmodel_candidate_is_volumetric` still FAILS on both
  openers (the last interval's largest in-window draw is planar) — see ADR-0489
  for what the per-interval record shows.

## Honest limits

- Two live runs (one per opener), one install, one hook build, 45 s each. The
  measured matrices come from the *startup + level* pages of a run that reaches
  gameplay; the arms pass is inferred (it is the `P0.1` load) and **not**
  identified here.
- The near/far pairs are inferred from the matrix algebra (they are exactly
  round: 4.0/2048, 0.1/2000.1, 1.8/72002); the engine's own near/far settings
  were not read from `RndrGL.dll`.
- `ViewmodelWindowOpener=2` opening a window does **not** mean the arms are in
  it; the candidate there is a 2520-count plane at the sampled interval, exactly
  the ADR-0484/0487 shape.

## Validation

- Offline: `tests/projection_near_tests.cpp` (CTest `projection_near`, both
  architectures) PASS; `tools/test_xrsim.py`
  `test_viewmodel_projection_lines_are_parsed` +
  `test_projection_near_check_grades_the_extraction` PASS; `xrsim --selftest`
  `viewmodel_projection_near_extracted` / `…_absent_has_no_lines` /
  `…_rejected_load_is_not_known` PASS; `verify_reported_fixes.py`,
  `scenario_tests.py --check`, `verify_test_artifacts.py`, `verify_hook_init.py`,
  `validate_addresses.py` PASS; `build-all.ps1` (x64 host + x86 hook) clean;
  CTest 25/25 (x64) and 27/27 (x86).
- Live: the two runs above; the `projection_near_extracted` check PASSes with
  `332/332 perspective load(s) with a near plane, 3 distinct matrix/matrices;
  first m10=1.00391 m11=1.000 m14=-8.01566 classic=-2048.008 near=4.0000`.

## Next

1. The near extraction is no longer the blocker; the *candidate* is. Neither the
   depth-only clear nor the near-plane drop makes the arms the largest in-window
   draw on a live level (ADR-0489 measures the same 14688-count mesh in both) —
   so step 1 needs a discriminator other than "largest in-window array draw".
