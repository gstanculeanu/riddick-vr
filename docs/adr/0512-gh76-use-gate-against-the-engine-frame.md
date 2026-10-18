# ADR 0512: Native stereo (epic #57 item 1) — the per-eye **use gate** is graded against the engine's own frame and judged only on a span frame; `use` goes 0→1 on every sampled in-level span and the presentation branch (GH #72) becomes the measured blocker (GH #76 / GH #90)

- Status: Accepted (scheduled autoloop wake 110, GH #76 / GH #90)
- Date: 2026-10-18
- Related: GH #76 (this), GH #90 (the scoped option-1 unit), GH #57 (epic /
  definition-of-done item 1), GH #72 (the display-branch signal that now blocks the
  presentation), ADR-0511 (**whose Next 1 this implements**, and whose own log line
  exposed the span-vs-sample flicker this fixes), ADR-0510 (the per-draw colour gate
  and its tracker), ADR-0509 (the span output-state probe), ADR-0505 (the per-eye
  pass + its original fail-safe gate)

## Context

ADR-0511 put the **engine's own presented frame** beside the eye frames and measured
that, on the in-level span, the engine frame is *as dark as* the eye frames
(`cover ~0.28`, `luma ~10.3`, `dark=both-dark`). That made ADR-0510's "the re-issue is
at fault" conclusion unsupported for that span and named the real reason `use=0`:
`kDualMinEyeCoverage = 0.5` is an **absolute** floor, and a dark level can never reach
it — the pass refuses a *faithful* pair. ADR-0511's Next 1 was therefore to **recalibrate
the gate against the engine frame** ("a relative floor — each eye carries at least a
fraction of the engine frame's coverage — plus a low absolute floor so a bright engine
frame still refuses a black eye").

ADR-0511's own live line also showed a second, structural problem with the gate as
written: the coverage verdict is sampled on a fixed 60-swap tick **regardless of whether
that swap carried the world span**, so a tick that lands on a frame with no re-issued
draw (`draws=0`) reads black eye frames and overwrites a good verdict with `ok=0`,
turning the pass off for the next 60 swaps. A gate that alternates at ~1 Hz cannot
present a stable stereo pair.

## Decision

Change **when and against what** the per-eye pair is accepted; change no draw and no
span boundary.

1. **`stereo_dual.h`** gains the relative gate:
   - `kDualEyeCoverageFraction = 0.5f` — each eye must carry at least half the engine
     frame's own non-black coverage;
   - `kDualMinEyeCoverageAbsolute = 0.05f` — and never a near-black eye, so the
     relative term alone cannot pass a black eye when the engine frame is itself
     (near) black, and a failed engine sample (`engineCoverage == 0`) degrades to this
     floor;
   - `DualEyeCoverageFloor`, `DualEyeFrameTracksEngine` (the per-eye decision) and
     `DualFrameUsableVsEngine` (both eyes).
   `kDualMinEyeCoverage` **stays** — it is still the absolute "does this frame carry an
   image at all?" threshold behind `ClassifyDualDarkSide` and `DualFrameUsable` — but it
   is no longer the gate that decides whether the pair is presented.
2. **`stereo_renderer.cpp`** uses it (`m_dualCoverOk = DualFrameUsableVsEngine(
   m_dualEngineCover, m_dualCoverL, m_dualCoverR)`) and refreshes the verdict **only on a
   frame that carried the span** (`m_dualEligible >= kDualMinEligibleDraws`); a no-span
   frame now *holds* the last span verdict and re-checks on the next swap instead of
   overwriting it. `m_dualCoverOk` starts **false** (the pair is presented only after a
   span frame has been sampled), and the `DualRender:` line gains
   `gate=[floor=… L=… R=… ok=…]` so the decision and its floor are visible per logged tick.
3. Off by default (`StereoDualRender=0`), as before: the shifted capture is untouched
   unless the opt-in is on, and even then only a frame whose sampled span verdict passes
   is presented.

## Evidence

**Offline (this wake).** `build-all.ps1 -Config Release` clean: x86 `opengl32.dll`
527872 B `b793297c376f` + x64 host. CTest **32/32 (x86)** and **30/30 (x64)** (the new
`stereo_dual` section 14 passes); `verify_reported_fixes.py` (new #76/#90/ADR-0512
invariant), `scenario_tests.py --check`, `xrsim --selftest`, `verify_hook_init.py`,
`validate_addresses.py`, `verify_test_artifacts.py` and all `tools/test_*.py` pass.
The new section pins the measured numbers: the in-level pair (`engine 0.294`, eye
`0.285`) is accepted while the old `DualFrameUsable(0.285, 0.285)` refuses it, and the
pre-level case (`engine 1.000`, eye `0.000`) is still refused.

**Live, run 1 — the gate alone (hook `13edc57d3189` = repo build).** The same command as
ADR-0511 (`RIDDICKVR_SIM=1`, read-only per-run shadow, `--shadow-install
"F:\The Chronicles of Riddick - Assault on Dark Athena" --reach-gameplay --duration 80
--advance-menu --advance-min-taps 8 --require-fresh-hook --require-parallax
--dump-branch any --downscale 4 --ini StereoDualRender=1`; `gameplay_full_bleed_reached`
fill 1.00). The recalibration does exactly what it was designed to do:

```
swap=17459 … draws=549 eligible=549 active=1 coverL=0.289 coverR=0.289
luma=[L=10.4 R=10.4] … engine=[cover=0.298 luma=10.5] dark=both-dark
gate=[floor=0.149 L=1 R=1 ok=1] … use=1
```

`use=1` on the in-level span — the first time in seven wakes the pass accepts its own
output — and, on frames with no span (`draws=0`, eye frames black, engine `cover
~0.295`), `gate=[floor=0.148 L=0 R=0 ok=0]`, `use=0`: the gate still refuses an empty
re-issue. **But `used=3840` of `frames=7593` (50.6%)** — the verdict alternates because
the 60-swap sample tick also lands on no-span frames (decision 2's measured motivation).

**Live, run 2 — final build (hook `b793297c376f` = repo build).** Same command; 80.0 s,
53 captured frames, `deployed_hook_matches_build` PASS. The flicker is gone:
**`used=7256` of `frames=7287` (99.6%)**, and every sampled in-level tick reads

```
coverL=0.286..0.290 coverR=0.286..0.290 luma=[L=10.3 R=10.3]
engine=[cover=0.295..0.299 luma=10.4..10.6] dark=both-dark
gate=[floor=0.147..0.149 L=1 R=1 ok=1] use=1
```

with `both_eyes_non_black` / `same_eye_resolution` / `no_vertical_disparity` PASS and
`gameplay_full_bleed_reached` fill 1.00.

## Verdict (honest)

- **The recalibrated, span-judged gate works and is measured**: the in-level pair is
  accepted (`use` 0→1, the state ADR-0511 named as the blocker), a no-span/black-eye
  frame is still refused (`L=0 R=0 ok=0`), and the verdict is now stable
  (`used/frames` 50.6% → 99.6%). This is the gate ADR-0511's Next 1 asked for.
- **Native stereo is still NOT presented**, and the blocker has moved for the third
  time — now to the **presentation branch**. The `DualRender:` line shows the pass
  engaged, but the renderer's own `DisplayMode:` branch is **CINEMA for the entire run**
  (one `DisplayMode:` line, **zero** `FULL-BLEED` lines, `latch=1 fill=-1.00`), so the
  `if (!isCinema && dualUsedThisFrame)` presentation branch is never taken and the
  per-eye pair is never blitted. The harness's frame labels come from that same line,
  so the run captured 53 `cinema` frames (`gameplay_captures_present` FAIL) and
  `--require-parallax` could **not** be graded (`stereo_parallax_present` FAIL,
  `stereo_kind=shift-only`, per-tile `spread=0`).
- **No regression**: `StereoDualRender=0` is the default, so the shipped shifted capture
  is what a player gets; `gameplay_full_bleed_reached` fill 1.00,
  `eye_views_scene_faithful`, `both_eyes_non_black` / `same_eye_resolution` /
  `no_vertical_disparity` all PASS on the presented frames.

## Limits (honest)

- The gate is evidence about *coverage and mean luminance* from an 8-row sample, not a
  pixel-difference proof that the two eyes carry the correct per-eye image; `use=1` says
  "the pass produced a pair as populated as the engine frame", not "the pair is a
  correct stereo view". That remains to be graded on a gameplay-labelled frame.
- The presentation branch is **diagnosed, not explained**: the DisplayMode line shows
  `fill=-1.00` (`haveFill=false`, i.e. `m_hasActiveRect` was never set) and `latch=1`,
  while `Has3DThisFrame()` is not the cause (444 `glLoadMatrixf [3D VR PROJECTION]` loads
  in the run). The latch term reduces to `!has3d || menuCursor`, so `HasMenuCursor()`
  (a cursor seen within 3 swaps) is the leading candidate — a hypothesis, not a result.
- One live run per build; both runs labelled all their frames `cinema` (GH #72 flake),
  so the stereo acceptance is ungraded. The ADR-0510 per-draw mask tracker's measured
  disagreement with the driver's `GL_COLOR_WRITEMASK` (ADR-0511) is **not** addressed
  here; its measured direction is the harmless one (tracker ON while the driver is OFF
  only spends a re-issue). The 2004 build is untouched; no headset.

## Next

1. **Fix the display-branch latch (now the presentation blocker).** Instrument the branch
   inputs on the log tick — `Has3DThisFrame()`, `HasMenuCursor()`/`s_lastCursorFrame`,
   `haveFill`/`activeFill`, `fullBleedSeen` — and log which term holds `isCinema` true,
   so the run says whether the cursor or the never-measured active rect is the cause;
   then the pair can be presented and `--require-parallax` graded (GH #72).
2. Re-run `--require-parallax` on a gameplay-labelled frame and require a non-zero
   per-tile disparity spread (the acceptance GH #76 asks for).
3. Gate on the **queried** `GL_COLOR_WRITEMASK` at the draw (ADR-0511 Next 2) or
   reconcile the tracker.
4. Keep `StereoDualRender=0` / `StereoParallaxDibr=0` / `StereoReplayProbe=0` defaults.
5. Player-visible queue GH #63/#64/#61, then the P1 bugs #45/#49/#50/#51/#65.
6. `DEFAULT_GAME_DIR` 2004-vs-2009; GH #83/#88/#89/#74.
