# ADR 0448: XRSIM reaches live gameplay with a wall-clock window, proven by the BarScan fill

## Status
**Accepted** — GH #71 requirement 1. The iteration vehicle can now drive a mock
run all the way into live, advancing 3D gameplay and *assert* it did so, without
a headset. The mechanism is a wall-clock (`--duration`) window with an unlimited
frame count, plus a structural full-bleed acceptance signal taken from the hook's
own `BarScan` active-rect log. No game file is touched; no binary is deployed.

## Context
ADR-0447 landed the gameplay-only dump gate but recorded `#69` requirement 1 as
**not met**: "every mock run … stalls in the multi-page startup splash". That
conclusion came from a 180-frame run and a 900-frame probe. This ADR corrects the
premise with a live measurement.

**The window was the defect, not a hard stall.** The mock host's frame loop is
gated on the game's frame-ready signal (`WaitForFrameReady`), so it advances at
the **game's** swap rate — the trivial startup splash renders **~250-300 FPS**
(measured: `OnSwapBuffers: Frame 1283 (256.4 FPS)`, host `mean_ms≈3.3`). A
180-frame run is therefore ≈**0.6 s** and the 900-frame probe ≈**3 s** of game
time, far short of the title's splash sequence + level load. At **5400 frames
(≈16 s)** the run reaches live 3D gameplay.

## Decision
1. **Wall-clock, not frame-count, for startup.** `tools/xrsim.py` gains
   `--reach-gameplay`, which runs the host with an **unlimited** frame count
   (`--max-frames 0`) and is bounded by `--duration` (default 60 s); the dump
   interval defaults to 300 so a startup-length run does not fill the disk. A
   frame budget cannot express wall-clock startup time when the frame rate is the
   game's uncapped rate.
2. **Structural acceptance, not the branch bit.** `--reach-gameplay` runs add a
   graded check `gameplay_full_bleed_reached`, computed by `active_rect_fill()`
   from the hook's own `BarScan` lines. The startup splash is letterboxed
   (`(336,80)-(944,477)` of a `1280x605` window ≈ **0.36** fill); live gameplay
   fills the window (`(0,0)-(1280,605)` = **1.0**). This deliberately does **not**
   use `cinemaMode`, which mislabels the static 2D splash pages as gameplay.
3. **Diagnostics.** Every report now carries `live_gameplay`
   (`gameplay_capture_stats()`: gameplay/cinema/unknown counts, blank interop
   frames, and distinct 16x16 signatures) so the graded window's content is
   visible without guessing.
4. **Regression invariant.** `tools/verify_reported_fixes.py` pins the driver, the
   BarScan parser, the check and the `live_gameplay` report key.

## Evidence (2026-10-16, live mock runs; `dist/riddickvr-host.exe`; `RIDDICKVR_SIM=1`)
- `python tools/xrsim.py --frames 5400 --interval 300 --dump-branch any`
  (scratch artifacts) — **reached live gameplay**:
  - `scene_advanced` **PASS — 15/19 distinct frames** (wake 41's 180/900-frame
    runs were 1/5 and 1/6).
  - `gameplay_captures_present` **PASS — 18 gameplay / 1 cinema / 0 unknown of
    19**; every gameplay frame is non-black, `horizontal_disparity_present` and
    `not_mono_duplicate` PASS.
  - Rendered frames: the legal/attribution text page → ESRB "MATURE 17+" page →
    a dark interior with the **native VR hand** visible bottom-left and the HUD
    top-left (frame 4800/5400).
  - Hook `BarScan`: splash `(0,304)-(648,605)`/`(336,80)-(944,477)` of
    `(1280,605)` (≈0.36), later `snap to (0,0)-(1280,605)` (1.0) = live gameplay.
- `python tools/test_xrsim.py` **32/32 PASS** (4 new: unlimited reach command,
  BarScan fill parse, gameplay/blank/distinct stats, the full-bleed gate + report
  key); `xrsim.py --selftest` PASS; `verify_reported_fixes` PASS.

## Explicitly not claimed
- **Requirement 2 (the branch signal) is still open.** `cinemaMode` still labels
  the static 2D splash pages as gameplay; the full-bleed *structural* signal is
  used for the acceptance, not a corrected branch bit. Filed as **GH #72** with
  the decoded evidence (BarScan fill 0.36 vs 1.0).
- **Requirement 3 (warm-up content gate) is only partly addressed.** The
  `live_gameplay` stats *report* blank gameplay captures; the host gate still
  does not skip them (GH #72).
- **A scripted *interaction* run in gameplay is not yet proven.** At ~250-300 FPS
  the frame-indexed scenario clips play far faster than 90 Hz; deferring scenario
  playback until gameplay is reached is follow-up work (GH #71/#72).
- **`frame_budget` still fails** (mean≈3.3 ms but max≈763 ms from a one-off
  startup hitch) — a mock-cadence artefact, not a render cost.
- **No vertical-disparity fix.** Live gameplay frames show `dy=±4` on some
  captures (`no_vertical_disparity` FAIL); recorded as a new player-visible risk,
  not fixed here.
- **Not validated in a headset.** All evidence is the mock host.

## Reproduce
```
python tools/test_xrsim.py
python tools/xrsim.py --reach-gameplay --duration 45 --artifacts <scratch> \
    --report-file <scratch>\report.json
```

## Files
- `tools/xrsim.py` (`--reach-gameplay`/`--duration`, `active_rect_fill`,
  `gameplay_capture_stats`, `gameplay_full_bleed_reached`, `live_gameplay` report)
- `tools/test_xrsim.py` (4 new tests → 32)
- `tools/verify_reported_fixes.py` (new GH #71 invariant)
- `docs/vr-scenarios.md` (caveat)
- refs: ADR-0445/0447 (#69 branch label + gate), GH #68 (harness), #69 (req 1),
  #71 (this), #72 (branch signal), #43 (cinema routing)
