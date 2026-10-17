# ADR 0459: Cold-start full-bleed latch for the published display branch

## Status
**Accepted** — GH #72 requirement 1 (the last open requirement of the XRSIM
branch signal, the top remaining item of the #68 iteration vehicle). Hook C++ +
config + tests/tools/docs; **default off**; no deploy, no game/asset change.

## Context
The hook publishes exactly one display-branch bit, `RiddickVRSharedState::cinemaMode`,
from `StereoRenderer::OnSwapBuffers` (`src/hook/stereo_renderer.cpp`):

```cpp
bool isCinema = !OpenGLProxy::Has3DThisFrame() || s_cursorMenu;
m_sharedState.cinemaMode = isCinema;
```

The engine's **static 2D startup pages** (the legal/attribution page, the ESRB
"MATURE 17+" page) load a *perspective* projection, so `Has3DThisFrame()` is true
and the hook logs `DisplayMode: FULL-BLEED gameplay (3Dfresh=1 menuCursor=0)` for
a page that is in fact a letterboxed 2D image (GH #72 evidence, read-only mock
runs, `RIDDICKVR_SIM=1`).

That matters because every downstream consumer trusts the bit:

- the XRSIM gameplay-only dump gate (`src/host/dump_gate.h`) keeps only
  `!cinemaMode` frames, so the graded window contains the frozen splash;
- the per-frame stereo skip logic and marker attribution use the same label.

The hook's own `BarScan` (`StereoRenderer::ScanActiveRegion`) already measures the
**structural** counter-signal: the startup splash's active rect covers
**~0.31-0.36** of the window (`BarScan: snap to (336,80)-(944,477)` of
`1280x605`) while live 3D gameplay covers **1.00**
(`BarScan: snap to (0,0)-(1280,605)`). GH #71 requirement 1 already uses that
signal from the harness side (ADR-0448, `gameplay_full_bleed_reached`), but the
published branch bit itself was still wrong.

The reason it was not simply fixed: GH #44 deliberately treats gameplay with
**engine-baked** letterbox/pillarbox bars as full-bleed (the bars are cropped out
of the blit). Naively demoting on a letterboxed fill would flip real gameplay to
the cinema screen. Wake 43 recorded the shape that avoids this: a **cold-start**
latch, coupled with a default-off knob, because the "a full-bleed page appears
before gameplay" case needs one live A/B before the bit can be trusted by
default.

## Decision
Add a pure, dependency-free decision helper (`src/hook/display_branch.h`) and use
it for the published bit:

```
C = ComputeDisplayBranch(has3dThisFrame, menuCursor, activeFill, haveFill, cfg, state)
```

1. **Structural input.** `activeFill` is the BarScan active-rect area divided by
   the window area; `haveFill` is false until a scan has produced a rect (the
   first scan stores the rect and the same frame reads it back), so a frame
   before the first measurement keeps the legacy verdict.
2. **Decisive thresholds with a hold band.** A fill `>= 0.90`
   (`fullBleedFill`) is full-bleed; a fill `<= 0.75` (`letterboxFill`) is
   letterboxed; between the two the previous decisive verdict is held, so the
   smoothed/easing active rect cannot flicker the branch.
3. **Cold-start latch.** The latch (`fullBleedSeen`) is set the first time a
   *3D, non-menu* frame measures a full-bleed fill, and is never reset. A
   letterboxed frame is demoted to cinema **only while `fullBleedSeen` is false**,
   so after the first gameplay frame the demotion can never fire again — GH #44
   (baked bars in real gameplay) is unaffected.
4. **Legacy signals unchanged.** `!has3dThisFrame` and the sustained menu cursor
   are cinema exactly as before; a non-3D or menu frame can never set the latch.
5. **Opt-in.** `DisplayBranchColdStartLatch` (INI, default `0`) enables the
   latch. When it is off the verdict is byte-for-byte the legacy one and the
   BarScan stays exactly where it was (gameplay branch only); when it is on the
   rect is measured **before** the branch decision and in *every* frame, so a
   demoted page can still re-measure and clear the latch once gameplay starts
   (otherwise a demoted frame would stop scanning and latch forever).
6. The `DisplayMode` log line gains `latch=`, `fill=` and `fullBleed=` so the
   live A/B is one grep.

### Unit coverage
`tests/display_branch_tests.cpp` (CTest `display_branch_cold_start_latch`, both
architectures) pins: the latch off ⇒ legacy verdict; no fill ⇒ legacy verdict;
the splash fill demotes; the splash never sets the latch; the first full-bleed
fill latches and later letterboxed fills stay gameplay (#44); the in-between band
holds the previous verdict and does not demote without one; `!has3dThisFrame` and
the menu cursor stay cinema and never set the latch. A GH #72 invariant is added
to `tools/verify_reported_fixes.py`.

## Consequences
- With the knob on, a mock run's graded window no longer needs the wrong
  `cinemaMode` label: the startup splash is published as cinema, so the dump gate
  and the stereo/marker consumers see only real gameplay frames.
- **Honest limit:** the change is hook-side and **off by default**, so it cannot
  be live-validated by the unattended loop — the sim install's hook is a stale
  build and deploying is a hard rule of the loop. The one live A/B still owed:
  set `DisplayBranchColdStartLatch=1`, run
  `python tools/xrsim.py --reach-gameplay --duration 60`, and confirm from
  `display_branch.log` + the `DisplayMode: ... latch=1 fill=.. fullBleed=..`
  lines that the splash pages are labelled cinema and every retained frame is
  full-bleed gameplay; then check whether any pre-gameplay full-bleed page sets
  the latch early (the documented failure mode).
- The thresholds (0.90 / 0.75) are empirical from the recorded fills (splash
  0.31-0.36 vs gameplay 1.00) and are the two constants most likely to need
  tuning on a different window size; they are compile-time defaults inside the
  pure helper so a follow-up can promote them to INI values if the A/B shows it
  is needed.
- The legacy path is untouched when the knob is off, so nothing in the shipped
  behaviour changes until an interactive session opts in.
