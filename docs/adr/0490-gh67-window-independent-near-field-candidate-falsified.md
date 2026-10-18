# ADR-0490 — GH #67 step 1: a window-independent near-field candidate, and the near-field hypothesis is falsified on the live level

- Status: accepted
- Date: 2026-10-18
- Refs: GH #67 (step 1), GH #63/#66 (the window heuristic), GH #68 (the
  iteration vehicle), ADR-0482/0483 (the submitted-vertex capture), ADR-0484
  (the planar candidate), ADR-0487 (an inoperative opener must fail),
  ADR-0488 (the near-plane convention), ADR-0489 (every sampled interval;
  whose "Next" #1 this is), `tools/xrsim.py`,
  `src/hook/vertex_submission_observe.h`, `src/hook/opengl_proxy.cpp`

## Context

ADR-0489 measured the *same* 14688-count volumetric mesh inside **both** window
openers, so the window is not what isolates the arms, and its "Next" #1 was:
"replace 'largest in-window array draw' with a discriminator that does not
depend on the window (e.g. a *near-field* test in view space, or an engine-side
identification of the viewmodel pass)".

The window-independent hypothesis is: the first-person viewmodel is submitted in
**near-field** coordinates (small, near the origin), while the world pass is
submitted far away. The observation surface already exists (the draw-time
client vertex array, ADR-0482), so the test is pure bookkeeping: for each
sampled interval, take the largest decodable array draw whose *sampled bounds*
are entirely inside a near-field box, regardless of the window.

## What was implemented

1. **`vertex_submission_observe.h`** gains `kNearFieldMaxAbs = 16.0f` (engine
   units; PROVISIONAL), `IsNearFieldSample(sample)` (pure: a *valid* sample whose
   bounds are within `±kNearFieldMaxAbs` on every axis) and a
   `VertexSubmissionSnapshot.nearSample` (+ `hasNearSample`, `nearSampleInside`).
   `NoteArrayDraw` keeps the interval's largest **decodable near-field** draw,
   window-independent, tagged with whether the window happened to be open at that
   draw. A cheap `count > nearSample_.count` guard means only a draw that could
   win is decoded, so sampling every draw is bounded.
2. **`opengl_proxy.cpp`** logs it as `ViewmodelProbe submission near: … valid=..
   inside=..` and adds a `nearSample=0|1` flag to the interval line. The flag is
   the marker that separates a #0490+ hook ("this build *can* report a
   near-field candidate, and here is whether it found one") from a pre-#0490
   build, which must be **skipped**, never failed.
3. **`tools/xrsim.py`** parses `near` (label + ` inside=`) and the `nearSample=`
   flag, mirrors the constant (`VIEWMODEL_NEAR_FIELD_MAX_ABS`,
   `viewmodel_sample_is_near_field`) so a hook whose threshold disagreed is
   visible, generalises `viewmodel_candidate_best(samples, label)` and adds
   `viewmodel_near_field_coverage(probe)`.

## Decision: grade the *claim*, not the sample

The near check is deliberately **not** "a volumetric near-field mesh exists".
That would have PASSed on the very run that falsifies the hypothesis (see
below) — the volumetric near-field mesh was a non-gameplay page element. The
hypothesis under test is "the viewmodel *is* the near-field draw", so the graded
check `viewmodel_near_field_candidate_identifies_pass` passes only when

- the hook reports the `nearSample=` marker (else SKIP),
- the window drew at least one in-window draw in the run
  (`in_window_intervals > 0`), and
- **a near-field candidate was found in at least as many intervals as the window
  had in-window draws** (`near_intervals >= in_window_intervals`), and
- the best near-field candidate is volumetric and its logged bounds really are
  near-field.

The window's in-window intervals are the best available per-interval proxy for
"the engine drew the first-person pass this interval" (ADR-0487/0488: the
near-plane-drop opener is expressed on the engine). The interval counts are
compared as counts, not positionally — an honest approximation, documented as
such.

## What was measured (two live runs, same command, two builds)

`python tools/xrsim.py --shadow-install "F:\…\Assault on Dark Athena"
--reach-gameplay --duration 45 --advance-menu --advance-min-taps 8
--require-fresh-hook --ini ViewmodelObserve=1 --ini ViewmodelTrace=1`
(no deploy; read-only per-run shadow; `deployed_hook_matches_build` PASS in both).

| build | hook sha256 | submission intervals | in-window intervals | near samples | near inside window | best near draw |
|---|---|---|---|---|---|---|
| `f80d2a9b267a` (shape-only check) | 496640 B | 21 | 18 | **3** | 0/3 | `count=44064`, bounds ±0.45, ratio 0.333 |
| `aca02a9d1c54` (coverage check) | 496640 B | 22 | 19 | **3** | 0/3 | `count=44064`, ratio 0.3158 |

Both runs reached the same live level (`gameplay_full_bleed_reached` PASS,
31/33 gameplay captures, `deployed_hook_matches_build` PASS). The `near` samples
occur at intervals `#1200`, `#1500`, `#1800` — the title/transition pages — and
**none at all** from `#2100` on, which is the level (16 consecutive intervals
with `arrayIn=3600`, i.e. ~2 million array draws, and `nearSample=0` every one).
The `fp` (in-window) sample on the level is the 12-count plane at
`first=(107.750,52.430,6.250)`; the level's first out-of-window draw is not even
decodable (`valid=0`).

Two facts follow:

1. **The near-field test does not identify the viewmodel.** On this target the
   live level submits **no** draw whose coordinates are within ±16 units of the
   origin, so a near-field/object-space test cannot separate the viewmodel from
   the world — the engine submits this game's geometry in view space with
   magnitudes in the hundreds to thousands (`world` samples at `|x| ≈ 3.5e3`,
   `z = 2032`; the level's in-window sample at `x ≈ 108..120`). The window (the
   engine's own near-plane drop, ADR-0488) remains the only measured boundary
   that isolates the first-person pass.
2. **The 44064-count mesh is not the arms.** It appears only on the
   title/transition pages (±0.45 bounds, volumetric) and never on the level; a
   graded "a volumetric near-field mesh exists" check would have PASSed on it.

`XRSIM RESULT: FAIL` in both runs — the honest verdict: the strict in-window gate
is planar *and* the window-independent near-field claim is not supported.

## Consequences

- The loop now has a graded, explicit verdict for ADR-0489's alternative: the
  near-field discriminator is measured and **falsified**, so the remaining route
  to GH #67 step 1 is the *engine-side* identification of the viewmodel pass
  (REA the RndrGL draw path / render flags), not another geometric heuristic.
- Nothing player-visible changed: the observer is inert unless
  `ViewmodelObserve=1`; no draw, state or matrix is altered.

## Honest limits

- Two runs, one install, one hook build, 45 s each; interval boundaries are
  300-swap samples, so "no near-field draw in 16 intervals" is not a proof about
  every frame.
- The near-field box (±16 units) is PROVISIONAL. It is wide relative to the
  measured candidates (±0.45) and the measured view-space draws (≳100), but a
  viewmodel drawn at, say, 20–50 units would be missed by *this* threshold — the
  harness re-checks the logged bounds so a threshold change stays visible.
- The interval coverage comparison is a count comparison, not positional: a run
  whose near-field candidate appears in intervals *different* from the window's
  in-window intervals could satisfy it. The ` inside=` tags showed 0/3 near
  samples inside the window on both runs, i.e. the two sets are disjoint here.
- The identity of the 44064-count mesh is unknown (bounds ±0.45, 3-D); it is
  measured to be a non-gameplay page element on this run, not proven to be a
  menu model.
- The level's state is still not established (the run drives no movement) and the
  branch signal still cannot tell the card from a level (GH #72 req 2).

## Validation

- Offline: `build-all.ps1 -Config Release` (no `-Deploy`) clean for both
  architectures; CTest **27/27 (x86)** and **25/25 (x64)** (the extended
  `vertex_submission_observe`); all `tools/test_*.py` exit 0, including the new
  `test_viewmodel_near_field_sample_is_parsed`,
  `test_viewmodel_near_field_check_grades_the_candidate` (which pins the sparse
  3-of-15 FAIL) and
  `test_viewmodel_near_field_check_skipped_without_a_near_sample`; `xrsim
  --selftest` +3 (`viewmodel_near_field_sample_is_parsed_and_graded`,
  `viewmodel_near_field_coverage_is_graded`,
  `viewmodel_near_field_bound_is_enforced`); `verify_reported_fixes.py`
  (extended #67 invariant), `scenario_tests.py --check`, `verify_hook_init`,
  `validate_addresses`, `verify_test_artifacts` PASS.
- Live: the table above; artifacts/reports in `%TEMP%\oc_wake88`
  (`run.txt`/`report.json` on `f80d2a9b267a`, `run2.txt`/`report2.json` on
  `aca02a9d1c54`); install untouched (no deploy), no leftover processes, live
  lock released.

## Next

1. GH #67 step 1 — **engine-side** identification of the first-person viewmodel
   pass (the geometric route is now measured to be a dead end on this target):
   REA the RndrGL draw path for the render flag / pass boundary that decides the
   viewmodel draw (GH #66 approach 2; `docs/rea/reference/RndrGL_GL_Path.txt`).
2. Keep the depth-only-clear window only as the GH #63 suppression boundary (the
   near-plane-drop opener fires but its contents are not the arms either).
3. The deploy-gated ladder on a real scene (GH #76 step 1, #79 hook half, #68
   req 5); the page-content discriminator for `gameplay_full_bleed_reached`
   (GH #71 req 2); `DEFAULT_GAME_DIR` 2004-vs-2009 retarget (GH #83).
