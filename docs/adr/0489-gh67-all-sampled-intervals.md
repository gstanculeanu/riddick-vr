# ADR-0489 — GH #67 step 1: keep every sampled interval, and report the volumetric in-window draw separately from the strict gate

- Status: accepted
- Date: 2026-10-18
- Refs: GH #67 (step 1), GH #63/#66 (the window heuristic), GH #68 (the
  iteration vehicle), ADR-0484 (the sample's shape is graded), ADR-0487 (the
  opener), ADR-0488 (the near extraction, same wake), `tools/xrsim.py`,
  `src/hook/vertex_submission_observe.h`

## Context

The observation probe logs **one** in-window submission sample per 300-swap
interval: `ViewmodelProbe submission [#N]: … inSample=1` followed by
`ViewmodelProbe submission fp: first=… min=… max=… n=… count=… valid=…`.
`viewmodel_probe_stats` stored those per **label**, so a run with N intervals
kept only the *last* `fp` sample, and `viewmodel_candidate_is_volumetric`
(ADR-0484) graded that one. Which interval a run happens to end on therefore
decided the check — an arbitrary input to a graded verdict.

## What was measured

Two live runs on the same build and install (ADR-0488's recipe; the only
difference is `--ini ViewmodelWindowOpener`), both reaching the same live level
(`gameplay_full_bleed_reached` PASS, 32/32 gameplay captures,
`deployed_hook_matches_build` PASS):

| opener | probe intervals | max in-window draws | sampled `fp` candidates | graded (last) |
|---|---|---|---|---|
| `1` depth-only clear | 18 | 4564 | **1 of 18** volumetric, `count=14688`, extent `[0.80,0.88,0.30]`, ratio 0.341; all others planar | `count=12`, extent `[12.5,12.5,0.0]`, ratio 0.0 |
| `2` near-plane drop | 3 | 1322 | **1 of 3** volumetric, `count=14688`, extent `[0.85,0.845,0.30]`, ratio 0.353; others planar | `count=2520`, extent `[0.9,0.9,0.05]`, ratio 0.056 |

Two facts follow directly:

1. The strict gate's FAIL on both runs is decided by the **last** interval
   (the 12-count and the 2520-count planes), while an earlier interval sampled a
   genuinely volumetric 14688-count mesh — the verdict was arbitrary in the
   sense that it does not represent the run.
2. The *same* 14688-count mesh is sampled inside **both** windows (1/18
   intervals with the depth-only clear, 1/3 with the near-plane drop). So "a
   volumetric draw is inside the window" is **not** opener-specific and is
   **not** evidence that the window isolates the arms. It is a property of the
   window's contents on this level.

## Decision

1. **Retain every sample.** `viewmodel_probe_stats` keeps a `submission.samples`
   list in log order (each with `label`, `count`, `geometry`); the per-label
   `fp`/`world` slots still hold the **last** sample, so existing semantics —
   including the strict ADR-0484 gate — are unchanged.
2. **Do not weaken the graded gate.** `viewmodel_candidate_is_volumetric` stays a
   strict verdict on the last sampled interval (FAIL for a planar sample). Making
   "any interval was volumetric" a PASS would have turned the check into a
   near-vacuous one that passes for *both* openers — measured, not hypothetical.
3. **Report the observation as its own graded check.**
   `viewmodel_volumetric_candidate_sampled` PASSes when at least one sampled
   interval's largest in-window draw is volumetric, FAILs when samples exist but
   none is, and is skipped only when there is no sample. Its detail names the
   ratio (`N/M sampled interval(s) had a volumetric in-window draw; best
   count=… extent=… ratio=…; last interval count=… ratio=…`) so the loop can
   never read "no volumetric sample" as silence, and never mistake "a mesh was
   seen once" for "the pass was identified".
4. **The pure helper** `viewmodel_candidate_best(samples)` returns
   `{sampled, volumetric, best, geometry, last}` (the largest-count volumetric
   sample, else the last) — used for the report/checks and unit-tested
   (`xrsim --selftest` `viewmodel_candidate_best_is_volumetric` /
   `…_falls_back_to_last`, `test_viewmodel_volumetric_sample_is_reported_apart_from_the_strict_gate`).

## Consequences

- Both wake-87 runs now grade honestly and identically in structure: strict
  `viewmodel_candidate_is_volumetric` **FAIL**, `viewmodel_volumetric_candidate_sampled`
  **PASS** (1/18 and 1/3), `projection_near_extracted` **PASS**, `XRSIM RESULT:
  FAIL`.
- The loop has an explicit signal that the in-window candidate selection — not
  the opener, and not the near extraction — is what remains for GH #67 step 1.

## Honest limits

- One run per opener, 45 s each; the "1/3 vs 1/18" interval fractions are single
  observations, not rates, and the interval boundaries are 300-swap samples.
- The 14688-count mesh's identity is **unknown**: object-space bounds of
  `~0.85 × 0.85 × 0.30` say nothing about what geometry it is, and the same mesh
  appearing in both windows argues against it being the viewmodel.
- The level's state is still not established (the mock controller never reaches
  the game's own input), so the sampled intervals may include menu/overlay pages.

## Validation

- Offline: `tools/test_xrsim.py` +
  `tools/test_xrsim.py::test_viewmodel_volumetric_sample_is_reported_apart_from_the_strict_gate`
  and all existing viewmodel tests PASS; `xrsim --selftest` PASS (new
  `viewmodel_candidate_best_*` entries); `verify_reported_fixes.py`,
  `scenario_tests.py --check`, `verify_test_artifacts.py` PASS.
- Live: the table above; both reports re-evaluated from their persisted hook logs
  (`report_near2_final.json`, `report_d1_final.json`).

## Next

1. Replace "largest in-window array draw" with a discriminator that does not
   depend on the window (e.g. a *near-field* test in view space, or an engine-side
   identification of the viewmodel pass), because the volumetric mesh is present
   in both windows.
2. Keep the depth-only-clear window only as the GH #63 suppression boundary.
