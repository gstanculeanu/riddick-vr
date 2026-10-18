# ADR-0487 — GH #67 step 1: neither window opener isolates the first-person viewmodel, and "the opener never fired" must not read as "the probe was off"

- Status: accepted
- Date: 2026-10-18
- Refs: GH #67 (step 1), GH #63/#66 (the window heuristic), GH #68 (the
  iteration vehicle), ADR-0439 (the depth-only-clear signature), ADR-0450 (the
  observation probe), ADR-0483/0484 (the in-window sample and its shape),
  ADR-0486 (the run that first reached a live level), `tools/xrsim.py`,
  `src/hook/viewmodel_suppressor.h`

## Context

Wake 85 took the GH #67 step-3 datum on a **live level** for the first time
(ADR-0486). Its finding, and the explicit next step, was: the depth-only-clear
window's largest decodable array draw on the level is a **12-vertex planar
draw at constant `z = 6.25`** (`extent [12.5, 12.5, 0.0]`), so the window does
not contain the engine's arms; the opener must be replaced or the candidate
chosen by submitted geometry instead of by the window.

The suppressor already supports two openers (`ViewmodelWindowOpener`: `1` =
depth-only clear, `2` = near-plane drop, `3` = either) and the harness can set
either per run via `--ini` on a read-only shadow. That made the "drop the
depth-only-clear opener" branch testable **without a rebuild**.

## What was measured

Two live runs, identical recipe (`--reach-gameplay --duration 45
--advance-menu --advance-min-taps 8`, read-only shadow of the 2009 install,
`--require-fresh-hook`, hook `sha256=e82a2dee526e` 495616 B = the repo build,
`--ini ViewmodelObserve=1 --ini ViewmodelTrace=1`, `RIDDICKVR_SIM=1`). The only
difference is the opener:

| opener | menu | `viewmodel_probe.lines` | in-window draws | `fp` sample | graded |
|---|---|---|---|---|---|
| `1` depth-only clear (control) | 8 taps → gameplay | **18** intervals | 4356 max/interval | 12-vertex planar, `extent [12.5,12.5,0.0]` | `viewmodel_candidate_is_volumetric` **FAIL** |
| `2` near-plane drop | 8 taps → gameplay | **0** | `arrayIn=0` in **21/21** intervals | none | (pre-fix: both checks **SKIPPED**) |

Both runs reached the same live level (`gameplay_full_bleed_reached` PASS,
`gameplay_captures_present` 32/32, deployed hook = current build). So the
result is a property of the engine/opener pair, not of a run that failed to
reach gameplay.

### Why the near-plane opener never opened

The pass trace (`--ini ViewmodelTrace=1`) is identical on every sampled frame
of both runs:

```
P? P? P? P? P? C d144 C d137 P? d7 P? d249 P? d1 P? d1 P? d150 P? P? d26
P? P? d1 P? P? d19 D d12 P?
```

Every projection load is `P?` — **near plane unknown**. The trace also shows
the depth-only clear `D` is followed by `d12` and then another `P?`: exactly the
12 tiny draws the submission observer tags in-window. The window is real; it
simply is not the arms.

`NearPlaneDrop` opens only when a **known** near plane is below
`worldNear * nearRatio`, and `worldNear_` is itself only set from a known near
plane (`viewmodel_suppressor.h`). With every load `P?`, the opener can never
fire. The projection is loaded through `glLoadMatrixf` (`glFrustum called`
count = **0**), and the hook derives near as `m[14] / (m[10] - 1)`
(`opengl_proxy.cpp`); for this engine's matrix that formula yields no positive
finite value. **This is a measurement gap in the hook, not a measured property
of the engine**: the engine may well use a short near plane for the viewmodel,
but the hook cannot currently see it.

## Decision

1. **Report an inoperative opener honestly.** `tools/xrsim.py` gains the pure
   helper `viewmodel_window_ran(probe)` →
   `{ran, intervals, in_window_draws}`. A non-empty `submission_lines` list
   proves `ViewmodelObserve=1` ran on a #0482+ hook even when the window never
   opened. When the probe ran and **`in_window_draws == 0`**:
   - `viewmodel_pass_identified` **FAILS** with "the probe ran (N interval(s))
     but no draw fell inside the window (arrayIn=0): the configured
     ViewmodelWindowOpener never opened it";
   - `viewmodel_candidate_is_volumetric` **FAILS** with "…no in-window
     candidate to grade".
   A run with no submission tallies at all is still **skipped** (probe not
   run), and a run whose tallies show in-window draws but no decodable sample is
   still skipped (nothing to measure).
2. **Do not add a third heuristic yet.** Both available openers are now measured:
   the depth-only clear opens on the level but holds only tiny planar draws;
   the near-plane drop cannot fire because the near plane is unknown. The next
   discriminator must come from the **submitted geometry** (a whole-frame mesh /
   near-field candidate), or the near plane must be extracted from the
   projection matrix first — see Next.
3. **Do not change C++ in this unit.** No rebuild/deploy was needed to learn
   this, and the hook's behaviour is unchanged.

## Consequences

- An inoperative opener can no longer masquerade as "the probe was not run": the
  `NearPlaneDrop` run above is `XRSIM RESULT: FAIL` with explicit opener details,
  not a silent skip (re-evaluated from its persisted hook log).
- The #67 status is now precise: the *window* is identified (18 intervals, 4356
  in-window draws) but the *viewmodel* is not — on a real level, not the card.
- `ViewmodelWindowOpener=2` is known to be inert on this engine for now; the
  INI comment/README says so instead of implying the engine has no short near
  plane.

## Honest limits

- Two runs, one install, 45 s each, one hook build; the opener comparison is a
  single A/B, not a replicated campaign (wake 85 supplies a third, 60 s,
  default-opener observation with the same result).
- The `P?` classification is the **hook's** verdict. The raw projection matrix
  (`m[10]`, `m[14]`) is not logged, so the exact reason the formula rejects is
  not yet measured — only that it does, on every load of both runs.
- The level's *state* is still not established (the mock controller never
  reaches the game's own input), and the branch signal still cannot tell the
  card from a level (GH #72 req 2).

## Next

1. Either (a) log `m[10]`/`m[14]` (or the full projection) once per run and fix
   the near extraction so `NearPlaneDrop` becomes testable, or (b) add a
   whole-frame **geometry-classified** candidate (largest volumetric
   near-field array draw) that does not depend on any window opener.
   (b) is the more direct route to "identify the arms" and is preferred unless
   the near extraction turns out to be a one-line fix.
2. Keep the depth-only-clear window only as the suppression boundary (#63), not
   as the arms-identification boundary.

## Validation

- Offline: `tools/test_xrsim.py` `test_viewmodel_probe_check_fails_when_the_opener_never_fires`
  (2 intervals, `arrayIn=0` → both checks fail, not skip); `xrsim --selftest`
  `viewmodel_opener_never_fired_is_visible`; the existing skip tests still pass.
- Live: the two rows above; the `NearPlaneDrop` report re-evaluated from its
  persisted hook log gives `viewmodel_pass_identified` FAIL with
  "21 interval(s)".
