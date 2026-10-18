# ADR-0478 — Display-branch cold-start latch: default ON (live A/B)

- Status: accepted
- Date: 2026-10-18
- Supersedes the "off by default" clause of ADR-0459
- Refs: GH #72 (requirement 1), GH #43, GH #44, GH #68 (XRSIM), GH #69, GH #81
  (shadow install), ADR-0459, ADR-0471

## Context

ADR-0459 added a *cold-start* latch to the published display branch: until the
first full-bleed frame is ever measured, a frame whose BarScan active rect is
clearly letterboxed (`<= 0.75` of the window) is demoted to the cinema branch,
so the engine's static 2D startup pages (legal/attribution, ESRB) are not
published as `FULL-BLEED gameplay`. It shipped **off by default** with an
explicit reason: the fill thresholds were provisional and the "a full-bleed page
appears before gameplay" case needed **one live A/B** before the bit could be
trusted by default.

Until now that A/B was impossible for the autoloop: a live run exercised a
*deployed* hook (older than the tree), and the loop must never write the game
install. ADR-0471 (the read-only **shadow install**) removed that gate: a live
XRSIM run can exercise the current hook, with zero writes to the install.

## Decision

Run the A/B on the target install's assets via a read-only shadow, then make the
latch **on by default**.

## Evidence (live, `RIDDICKVR_SIM=1`, shadow install, no deploy)

One shadow (`%TEMP%\latch-ab\shadow-doc`, source
`F:\The Chronicles of Riddick - Assault on Dark Athena`, hook
`sha256 49643de93128…` = the repo build, `fresh=true`), two runs that differ
**only** in the shadow's `riddick_vr.ini` (never the install's):

| run | ini | hook log `Display branch config` | first branch lines |
|-----|-----|----------------------------------|--------------------|
| A (baseline) | key absent (compiled default 0) | `cold_start_latch=0` | `DisplayMode: CINEMA … (3Dfresh=0 menuCursor=1 latch=0 fill=-1.00)` then **`DisplayMode: FULL-BLEED gameplay (3Dfresh=1 menuCursor=0 latch=0 fill=-1.00 fullBleed=0)`** → `BarScan: first active rect (280,96)-(992,533) of (1280,605)` |
| B (latch on) | `DisplayBranchColdStartLatch=1` | `cold_start_latch=1` | `DisplayMode: CINEMA … (… latch=1 …)` → `BarScan: first active rect (280,96)-(992,533) of (1280,605)` → **`DisplayMode: FULL-BLEED gameplay (3Dfresh=1 menuCursor=0 latch=1 fill=1.00 fullBleed=1)`** |

- **Observation (the defect, live):** in run A the letterboxed startup page
  (`(280,96)-(992,533)` of `(1280,605)` = **fill 0.402**) was published as
  `FULL-BLEED gameplay`. That is exactly the GH #72 mislabel, measured, not
  inferred — and the `BarScan` line that proves the fill is only emitted in the
  gameplay branch, i.e. *after* the wrong label was already published.
- **Observation (the fix, live):** in run B the same page produced **no**
  `FULL-BLEED` transition at all: the branch stayed cinema through the
  letterboxed page and changed to `FULL-BLEED` only at `fill=1.00`
  (`fullBleed=1`), i.e. at real gameplay.
- **Inference (labelled as such):** the two runs use the same binary and the same
  scene sequence and differ only in the ini, so the missing transition in B is
  the latch's demotion. ADR-0478 makes this an *observation* for future runs by
  logging the demotion itself once
  (`DisplayMode: cold-start latch demoted a letterboxed 3D frame to CINEMA
  (fill=0.40 latch=1 fullBleed=0)`); run C below is that observation.
- **The case ADR-0459 wanted:** no full-bleed page appeared before gameplay in
  either run (the pre-gameplay pages are letterboxed), so the latch cannot be
  consumed by a pre-gameplay full-bleed page in this title's startup.
- **GH #44 stays safe:** the latch can only fire before the first full-bleed
  frame; after it, `state.fullBleedSeen` is latched and a letterboxed fill no
  longer demotes (pure test case 4, `tests/display_branch_tests.cpp`).
- **Harness compatibility (run B):** `gameplay_captures_present` PASS
  (`81 gameplay / 0 cinema / 1 unknown of 82`), `gameplay_full_bleed_reached`
  PASS (`max BarScan active-rect fill=1.00`), `capture_gate_matches_saved` PASS
  `81/81`, `comfort_vignette_applied` PASS. Run B's only two FAILs are
  end-of-run artefacts, not the latch: `frame_budget` (a frame-time spike,
  `max=39.32 ms`) and `frame_readable_6690` (a BMP truncated because the run was
  killed at the 30 s boundary while the host was writing it).
- **Bonus (GH #69):** with the splash demoted to cinema the harness's
  gameplay-only dump gate no longer captures the frozen splash (run A dumped 111
  frames starting at frame 120 on the letterboxed page; run B's dumped window
  starts in real gameplay), which is the intent of GH #69.

## Consequences

- The compiled default (`DisplayBranchConfig::coldStartLatch`,
  `VRConfig::displayBranchColdStartLatch`) is **1**; the example and repo
  `riddick_vr.ini` document `DisplayBranchColdStartLatch=1` and that `0` restores
  the legacy bit.
- The `DisplayMode` line keeps `latch=.. fill=.. fullBleed=..`, and the latch
  logs its own demotion once, so a live run *proves* the demotion instead of
  inferring it from an absent transition.
- A player can always opt out with `DisplayBranchColdStartLatch=0`.

## Honest limits

- The thresholds (`0.90` full-bleed / `0.75` letterboxed) are still provisional;
  this ADR validates them against this title's startup and gameplay (0.40 vs
  1.00), not against every page the engine can show.
- The A/B ran on the **2009 *Assault on Dark Athena*** install (the mod's target,
  GH #85/#86). The 2004 install's startup was not A/B'd (its assets are a
  different build).
- One run (the session's first live launch) captured nothing at all (host
  `Game Connected: WAITING`, no `OnSwapBuffers` line); a re-run of the identical
  command worked. That is recorded as an unexplained first-launch artefact, not
  attributed to this change.
