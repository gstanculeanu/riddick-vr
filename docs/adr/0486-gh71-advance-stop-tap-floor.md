# ADR-0486 — GH #71 req 1: the menu-advance stop fired on the engine's own startup transition, so the confirm taps were cut short

- Status: accepted
- Date: 2026-10-18
- Refs: GH #71 (requirement 1), GH #72 (the branch signal mislabels the card),
  GH #67 (step 1/3 — the datum this unblocks), GH #68 (the iteration vehicle),
  ADR-0485 (the scan-code fix this builds on), ADR-0484 (the card measurement),
  ADR-0466 (the route), `tools/xrsim.py`

## Context

Wake 84 fixed the *message* the harness posts to advance the game's front-end
(ADR-0485): the confirm tap now packs `VK_RETURN`'s real scan code (`0x1C`), which
the engine's window proc reads from `lParam >> 16`. Two tapping runs then left the
start card and reached a live dark level at ~the 5th-6th tap; two controls never
left it.

Wake 85 set out to take the GH #67 step-3 datum *on that live scene* (wakes 79-83
had all sampled the start card). The run used the same command as wake 84's tap
runs (`--reach-gameplay --advance-menu --ini ViewmodelObserve=1`) and reported
`menu_advance = {taps: 3, windows: 1, reached: true}` — and every dumped frame was
still the **start card** (`PRESS ENTER`, mean luma `86 ↔ 100`, byte-identical
`fp` sample to wakes 82/83). So the wake-84 "the tap reaches a level" result was
not reproducible with 3 taps.

## Root cause (measured)

The tap loop stops when `gameplay_reached_from_sequence` sees a full-bleed branch
*after* a cinema branch in the hook log. That is satisfied by the engine's **own
startup transition**, at **frame 1** of the run:

```
DisplayMode: CINEMA screen (menus/cinematics) (3Dfresh=0 menuCursor=1 ...)
3D Gameplay projection intercepted: transitioned from Menu Letterbox to Full 3D VR Immersion
DisplayMode: FULL-BLEED gameplay (3Dfresh=1 ...)
BarScan: first active rect (336,80)-(944,477) of (1280,605) win
DisplayMode: cold-start latch demoted a letterboxed 3D frame to CINEMA (fill=0.31 ...)
DisplayMode: CINEMA screen (menus/cinematics) (3Dfresh=1 fill=0.31 ...)
BarScan: snap to (0,0)-(1280,605)
DisplayMode: FULL-BLEED gameplay (3Dfresh=1 menuCursor=0 latch=1 fill=1.00 fullBleed=1)
```

All of those lines carry `Frame 1` in the log's swap counter, i.e. the pair exists
before any confirm has been delivered. What bounded wake 84's runs to 6 taps and
this run to 3 is **hook-log write buffering**: the harness can only stop once the
lines are readable, and the flush time varies run to run. The stop was a race,
not a measurement of gameplay.

The branch signal genuinely cannot separate the card from a level here: this
run's `display_branch.log` marks every frame `cinema=0` on the card *and* on the
level (GH #72), and the reached level reported the same
`gameplay_full_bleed_reached fill=1.00`.

## Decision

Require the taps to have actually been **delivered** before the branch stop may
fire:

- `ADVANCE_STOP_MIN_TAPS = 8` and the pure
  `advance_stop_decision(branch_seq, taps, min_taps=ADVANCE_STOP_MIN_TAPS)` =
  `taps >= min_taps and gameplay_reached_from_sequence(branch_seq)`.
- `--advance-min-taps N` (default 8, `0` restores the pre-wake-85 branch-only
  stop), recorded in the report as `menu_advance.min_taps`.
- 8 is chosen from the wake-84 measurement (the card is left at the 5th-6th tap),
  so the floor can never fire before the advance has been attempted.

## Evidence (live, `RIDDICKVR_SIM=1`, `--require-fresh-hook`, no deploy)

Read-only per-run shadow of the 2009 install, hook `sha256=e82a2dee526e` 495616 B
= the repo build (`deployed_hook_matches_build` PASS), `--ini ViewmodelObserve=1
--ini ViewmodelTrace=1`:

| command | taps | `menu_advance` | page of the dumped frames |
|---|---|---|---|
| pre-wake-85 default (branch-only stop) | 3 | `reached: true` | the start card for the whole run (mean 86 ↔ 100) |
| **wake-85 default** (`--advance-min-taps 8`) | 8 | `taps: 8, min_taps: 8, reached: true` | card (frames 2400-3000) → transition (3300-3600) → **dark level** (3900-15600; mean 8-10) |

The per-frame mean luma of the second run's 51 captures: 85.7 / 86.5 / 84.8
(frames 2400-3000, the card), 47.7 / 41.6 (3300-3600, the transition), then 8-10
for the remaining 47 frames. The late frame (15600) shows interior geometry, a
lit hatch and the HUD blocks — the wake-84 "live scene, not a plate" page.

## GH #67 step-3 datum (the reason for this unit)

The `fp` submission sample on a **real scene** (largest decodable in-window array
draw):

```
pre:  first=(0.400,-0.400,0.000) min=(-0.450,-0.450,-0.050) max=(0.450,0.450,0.000)
      read=24 count=2520 indexType=5123 vertexType=5126   -> extent [0.9,0.9,0.05]
post: first=(107.750,52.430,6.250) min=(107.750,39.930,6.250) max=(120.250,52.430,6.250)
      read=12 count=12 indexType=5123 vertexType=5126    -> extent [12.5,12.5,0.0]
```

The sample is now **scene-dependent** (2520 vertices on the card, a 12-vertex
draw in the level) rather than a frozen card artefact, but on the live scene the
window's largest in-window draw is still **planar** (thickness ratio `0.0`, a
constant `z = 6.25`), i.e. a screen-space element, so the depth-only-clear opener
still does **not** isolate the engine's first-person viewmodel. The run's
`viewmodel_candidate_is_volumetric` is consequently **FAIL** — the honest verdict,
now measured on gameplay instead of on the start screen.

## Consequences

- The GH #67/#76/#79/#68-req5 deploy-gated ladder can be validated on a real
  scene with the default harness flags (no per-run `--advance-max-taps` tuning
  needed).
- A run that reaches the level can now grade gameplay effects truthfully; the
  card is no longer silently substituted.
- The branch signal is still not a card/level discriminator (GH #72 req 2 stays
  open); the tap floor is a bound on the *tap loop*, not a fix for the label.

## Honest limits

- The floor (8) is calibrated to one install and one title's startup (wake 84:
  card left at tap 5-6, at the default 2 s interval); it is a safety margin, not
  a measured constant.
- Extra confirms after the level loads are posted (up to the floor/`--advance-max-taps`);
  `VK_RETURN` is the engine's own "PRESS ENTER"/interact key, and the harness
  terminates the game at the wall-clock cap, but the exact in-game effect of a
  surplus confirm is not measured.
- The write-buffer race is *explained*, not eliminated: the tap count is now
  deterministic (the floor), but a future run could still differ if the engine
  needs more than 8 taps on a slower cold start.

## Validation

- Offline: `tools/test_xrsim.py` `test_advance_stop_waits_for_delivered_taps`;
  `xrsim --selftest` `advance_stop_requires_delivered_taps`; the GH #71 invariant
  in `tools/verify_reported_fixes.py` now also requires `advance_stop_decision`,
  `ADVANCE_STOP_MIN_TAPS = 8`, `--advance-min-taps`, the test, and this ADR.
- Live: the two rows of the table above (recorded in `tools/autoloop/STATE.md`);
  the harness's own report keys `menu_advance.{taps,min_taps,reached}` and
  `viewmodel_probe.submission.fp` carry the measurements.
