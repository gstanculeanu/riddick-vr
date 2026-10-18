# ADR-0484 — GH #67 step 1: the "arms candidate" is a *plane*, so the window being identified is not the viewmodel

- Status: accepted
- Date: 2026-10-18
- Refs: GH #67 (this — step 1, fourth correction), GH #71 (the run that produced
  the sample is the **start screen**, not gameplay), GH #80 (the input bridge that
  would let a run reach gameplay), ADR-0483 (the largest-draw selector this
  qualifies), ADR-0482 (the submitted-vertex capture), ADR-0481 (the ARB
  candidate, eliminated), ADR-0439/0446 (the depth-only-clear window), GH #66/#63
  (pass identification/suppression), GH #68 (the iteration vehicle)

## Context

ADR-0483 changed the in-window sample to the window's **largest decodable array
draw** and read the live result as the first-person viewmodel signature:

> 16 sampled `fp` lines, all identical: `first=(0.400,-0.400,0.000)
> min=(-0.450,-0.450,-0.050) max=(0.450,0.450,0.000) n=24 count=2520 …`; byte-
> identical while the run's HMD yaw varied 39.5° ⇒ "camera-relative", "a real
> mesh", "the FP viewmodel signature".

Two things in that reading were assumptions, and this wake measured both:

1. **`count = 2520` was read as "a real mesh".** It is a real *draw*, but its
   **geometry is a plane**: the sample spans 0.90 × 0.90 in x/y and only 0.05 in
   z (`thickness_ratio = 0.05/0.90 = 0.056`), and it is *symmetric about the
   origin*. The engine's first-person arms are a 3-D mesh with depth comparable
   to their width; a flat, origin-centred quad-like mesh is a **screen-space
   element** (splash/post-process/HUD overlay).
2. **"Camera-relative" does not identify the arms.** A screen-space element is
   *by construction* byte-identical under any head rotation — the observation is
   equally consistent with a UI overlay. (The screen quad already measured in
   ADR-0482's `first` draw, `±0.5`, is the same family.)

The run that produced the sample also explains why: its dumped "gameplay" frames
are the game's **start screen**. The harness's `gameplay` branch is
"full-bleed (BarScan fill 1.00) + a recent perspective projection (`3Dfresh=1`)",
and the 2009 target's start screen — a full-bleed 3-D page with the title card —
satisfies both, so the sim's 45 s run is graded as gameplay while it never leaves
the splash (`--sim-confirm` was not used; the reaches-gameplay question is GH #71,
and the route to it is GH #80). The captured evidence (`right_*.bmp` /
`test_frame_*.bmp`, 29 pairs, frames 4500–12900) is the AoDA title card, pulsing
between two brightness states and otherwise unchanged.

## Decision

Carry the **shape** of the sampled draw into the harness and grade it:

- `tools/xrsim.py` gains `VIEWMODEL_CANDIDATE_MIN_THICKNESS_RATIO = 0.15` and a
  pure `viewmodel_candidate_geometry(sample)` → `{"extent", "largest_extent",
  "smallest_extent", "thickness_ratio", "volumetric"}`; `volumetric` requires a
  *valid* sample whose smallest axis extent is ≥ 0.15 of its largest.
- `viewmodel_probe_stats` attaches that geometry to each parsed sample
  (`submission.fp.geometry` / `submission.world.geometry`), so the run report
  carries it.
- `evaluate` adds the graded check **`viewmodel_candidate_is_volumetric`**
  (group `viewmodel`): FAIL when a first-person sample was captured and it is
  planar, PASS when it is volumetric, SKIP when the probe captured no sample
  (ViewmodelObserve=0 / pre-#67 host). `viewmodel_pass_identified` is unchanged —
  it answers "did the window draw anything", which it still does.

The threshold is deliberately loose (0.15 ≈ 6.7:1) so it only rejects shapes no
arm mesh can have: the measured plane is 0.056, a box-like sample is 0.5.

## Reading the result

- **FAIL** (current target): the window's largest decodable draw is planar ⇒ the
  harness must **not** treat that sample as the engine's arms. The #67 capture
  surface is therefore *not* yet identified, and the next step is to reach
  *gameplay* (GH #80/#71) before re-reading it — not to re-express a splash
  overlay in head space.
- **PASS**: the sample is a 3-D mesh and can be a viewmodel candidate; ADR-0482/
  0483's geometry reading applies.

## Consequences

- The false premise of the last three step-1 corrections is now a **machine
  check**: "the window has a large draw" can no longer be reported as "the
  viewmodel was identified". A run that is really on the splash cannot be read as
  a viewmodel measurement.
- Nothing player-visible changes; the hook, its log line and its default
  (inert unless `ViewmodelObserve=1`) are untouched, and the geometry is derived
  in the harness from bounds the hook already logged (no new wire field, no
  deploy).
- The positive case is **unverified**: no run has yet produced a volumetric
  `fp` sample, because no XRSIM run has reached in-game gameplay (GH #71/#80).
  The check is therefore a *necessary* condition, not a sufficient one; when a
  real gameplay run exists, wake-83's threshold must be re-read against it.

## Honest limits

- The sample is up to 24 vertices spread across the index list, so a mesh whose
  depth is confined to unsampled vertices would be misread as planar (unlikely
  for `count = 2520`, where the spread is even; the same caveat as ADR-0482/0483).
- "Planar" is a statement about the *sampled in-window draw*, not about the page:
  the window has ~19 draws per frame, and a different one could be volumetric.
  What is refuted is specifically "the largest in-window draw is the arms".
- The conclusion that the page is the start screen rests on the captured BMPs
  (visual) plus the hook's own `DisplayMode` chain (letterboxed logos → full-bleed
  page, `menuCursor=0`, one page for the remaining ~13.7 k frames), not on a
  game-side scene id.

## Validation

- Offline: `tools/test_xrsim.py` +3
  (`test_viewmodel_candidate_geometry_distinguishes_plane_from_mesh`,
  `test_viewmodel_candidate_check_fails_on_a_planar_sample`,
  `test_viewmodel_candidate_check_skipped_without_a_sample`); `xrsim --selftest`
  +1 (`viewmodel_candidate_geometry_planar_vs_volume`); the GH #67
  invariant in `tools/verify_reported_fixes.py` requires the check name, the pure
  helper, the threshold constant and this ADR.
- Live: one shadow run with `--ini ViewmodelObserve=1 --ini ViewmodelTrace=1
  --require-fresh-hook` — recorded in `tools/autoloop/STATE.md`; the `fp` sample is
  unchanged from ADR-0483 and the new check reports FAIL with
  `thickness_ratio=0.056 count=2520`.
