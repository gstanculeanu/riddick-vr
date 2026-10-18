# ADR 0510: Native stereo (epic #57 item 1) — the per-eye pass classifies **at each draw** by the engine's **colour-write mask**; the measured in-level span is **not** colour-masked throughout (GH #76 / GH #90)

- Status: Accepted (scheduled autoloop wake 108, GH #76 / GH #90)
- Date: 2026-10-18
- Related: GH #76 (this), GH #90 (the scoped option-1 unit), GH #57 (epic /
  definition-of-done item 1), ADR-0509 (**whose Next 1 this implements**: read the mask
  once per span measured `cmask=0x0`; the next step is to classify at each draw),
  ADR-0508 (the span capture is 706/735 draws on the generic-attribute path),
  ADR-0505 (the per-eye dual-issue pass + its coverage gate), ADR-0506 (the span census)

## Context

ADR-0509 read the engine's output GL state **once per span**, at the first eligible
draw, and measured `cmask=0x0` — so the span's *opening* state cannot write colour.
That resolved ADR-0508's moved blocker to cause (1) ("the span writes no colour"), but
a single per-span read **cannot tell** the two shapes that matter:

- a span that is a depth/occlusion prepass **throughout** (a span-boundary problem), or
- a span whose mask is off for the prepass and turns **back on** for the colour pass
  (the pass is already inside the right span; the fault is in the re-issue).

Both look identical in one opening-state token and need different fixes. The step
ADR-0509 named was therefore to classify *at each draw*: a draw submitted with the
colour mask fully off re-issues correctly but can never colour an eye frame, so the
per-eye pass should not spend a per-eye draw on it.

## Decision

Track the engine's `glColorMask` state and gate the per-eye re-issue on it, per draw.
Evidence + policy; `StereoDualRender=0` stays the default.

1. **`glColorMask` is IAT-hooked in `RndrGL.dll`** (`src/hook/dllmain.cpp`,
   `Hook_glColorMask` → `OpenGLProxy::NoteColourMask`). The live 2009 `RndrGL.dll`
   **statically imports** `glColorMask` (verified read-only with
   `python tools/gl_vocabulary_scan.py --imports --game-dir "<2009 shadow>"`:
   96 `OPENGL32.dll` static imports including `glColorMask`), so the hook installs; only
   `RndrGL` imports OpenGL at all, so the hook sees every engine call. It forwards
   verbatim (the engine's mask is never changed) and **counts its calls**, so a run
   states the hook is live rather than assuming it.
2. **`OpenGLProxy` holds the tracked mask** as a 4-bit nibble (`s_colourMaskNibble`,
   bit0=red..bit3=alpha; GL default `0xF`) with `ColourWriteMaskNibble()` and
   `ColourMaskCallCount()`.
3. **`stereo_dual.h`** gains the pure gate `DualColourMaskWritesColour(nibble)` —
   RGB-only (`(nibble & 0x7) != 0`); an alpha-only mask writes nothing
   `DualNonBlackFraction` reads. `SpanDrawCensus::Add` now takes the per-draw
   colour-write flag and splits its **eligible** draws into `eligibleColourOn` /
   `eligibleColourOff`.
4. **`stereo_renderer.cpp` `OnSceneDraw`** computes `colourWritesOn` at the draw, feeds
   the census, and after the ADR-0509 span-state capture **returns without re-issuing
   when `!colourWritesOn`**. `m_dualEligible` (the fail-safe gate's counter) therefore
   counts only draws actually re-issued, so a colour-masked span cannot pass the gate.
   The `DualRender:` line gains `cw=[on=… off=… calls=…]`.

The clear-based span boundary is deliberately **not** changed: whether it must move is
exactly what the new per-draw census measures, and moving it speculatively would
confound the two causes ADR-0509 separated.

## Evidence

**Offline (this wake).** `build-all.ps1 -Config Release` clean: x86 `opengl32.dll`
525312 B `7da5d4a1db98` + x64 host. CTest **32/32 (x86)** and **30/30 (x64)** (the
extended `stereo_dual` sections 12/12b pass; the updated census call sites in
`gl_entry_census_tests`/`stereo_dual_tests` build); `verify_reported_fixes.py` (new
#76/#90/ADR-0510 invariant), `scenario_tests.py --check`, `xrsim --selftest`,
`verify_hook_init.py`, `validate_addresses.py`, `verify_test_artifacts.py` and all
`tools/test_*.py` pass.

**Import table (read-only).** `gl_vocabulary_scan.py --imports` on the 2009 shadow
`RndrGL.dll` (939248 B) lists `glColorMask` among its 96 static `OPENGL32.dll`
imports — the precondition for the IAT hook.

**Live (RIDDICKVR_SIM=1, read-only per-run shadow `riddickvr-shadow-17000`,
`--shadow-install "F:\The Chronicles of Riddick - Assault on Dark Athena"
--reach-gameplay --duration 80 --advance-menu --advance-min-taps 8
--require-fresh-hook --require-parallax --dump-branch any --downscale 4
--ini StereoDualRender=1`).** `deployed_hook_matches_build` PASS, hook
`7da5d4a1db98` = the repo build; 80.1 s, 54 captured frames; `gameplay_full_bleed_reached`
fill **1.00** (the run *was* in-level — the harness labelled the frames cinema, GH #72).
The per-draw classification is decisive; one in-level span (swap=17459):

```
DualRender: swap=17459 frames=7586 used=341 draws=547 eligible=547 skipped=29
mirrors=16645 active=1 coverL=0.289 coverR=0.289 luma=[L=10.3 R=10.3] size=1280x605
use=0 scale=[unknown=675 near=0 mid=2 world=58] reject=29
rc=[nonfloat=0 noarray=29 …] gen=646 gbacked=640 gsig=[enabled=1 buf=368]
cw=[on=547 off=159 calls=162194]
sstate=[cap=1 cmask=0x0 dmask=1 drawbuf=0x8CE0 vp=0,0,1280x605 sc=0
scissor=0,0,1280x605 dt=1 blend=0]
```

- **`cw=[on=547 off=159 calls=162194]`** — of the **706** eligible draws ADR-0508
  re-issued, exactly **547 run with colour writes ON** and **159 with colour writes
  OFF**. The 159 are the span's *opening* depth prepass (the state ADR-0509 read);
  the 547 are the engine's **colour world pass**, which IS inside the span.
- `calls` rises monotonically across the run (145628 → 162194) — the `glColorMask`
  IAT hook installs and tracks the engine every frame.
- Across the 29 in-level (`active=1`) spans: 15 read `coverL 0.266..0.292`
  (`luma ~10.3`), 14 read `0.000` (pre-level/black frames); **every one `use=0`** — the
  eye frames stay below the 0.5 coverage gate and the shipped shifted capture presents.

## Verdict (honest)

- **ADR-0509's cause (1) is refuted for the in-level span.** The span is *not*
  colour-masked throughout: its opening depth prepass is (`cmask=0x0`, 159 draws) but
  its colour world pass (547 draws) is not. **The span boundary does NOT need to move**
  — ADR-0509's option 2 is closed by measurement, and the `glColorMask` knob is not the
  lever it looked like.
- The per-draw gate is a real, measured improvement: the pass now re-issues **547** per
  eye instead of ADR-0508's **706**, never spending a re-issue on a draw that cannot
  write colour.
- **But the eye-frame coverage is unchanged** (`~0.29`, luma `~10.3` — the same range
  ADR-0505/0508 saw, `0.000..0.287`), so the masked prepass was **not** the blocker.
  The remaining cause is ADR-0509's **cause (2) — the re-issue itself** (eye-FBO state /
  viewport / depth / readback), not the span.
- Native stereo is **NOT** achieved: `--require-parallax` still FAILs
  (`stereo_kind=shift-only`, `spread=0`; the harness saw 0 gameplay-labelled frames).
  `StereoDualRender=0` is the default and the pass is inert on its own gate, so there is
  **no regression** (`gameplay_full_bleed_reached` fill 1.00, `eye_views_scene_faithful`
  PASS, `both_eyes_non_black`/`same_eye_resolution`/`no_vertical_disparity` PASS on the
  presented frames).

## Limits (honest)

- One live run; its in-level spans are the flaky `cinema`-labelled ones (GH #72), so the
  stereo acceptance could not be graded on `gameplay` frames.
- `frame_budget` could not be graded (the host logged no `FrameBudget` line this run —
  the intermittent absence seen at wakes 103/105). The change *reduces* per-eye draws
  (706 → 547), so it cannot have increased frame cost.
- The gate tracks the mask only through `RndrGL`'s static `glColorMask` import;
  `calls=` makes a non-tracking run visible. The mask is the direct mechanism;
  `glDrawBuffer(GL_NONE)` is the equivalent form ADR-0509 logs (`drawbuf=`) and is not
  itself gated.
- The eye-frame coverage is sampled 8 rows every 60 swaps (an estimator). It cannot by
  itself say whether `~0.29` is "mostly-empty" or "a genuinely dark level".
- The 2004 build is untouched; no headset.

## Next

1. **Calibrate the gate, then find the re-issue fault.** Compare the eye frame's
   content in the *same span* to the engine's own presented frame: if the engine frame
   is also dark (~0.3 non-black), the fixed `0.5` floor rejects a genuine dark scene and
   the calibration — not the re-issue — is the blocker; if the engine frame is bright,
   the eye frame is genuinely mostly-empty and the fault is in the re-issue (log the
   eye FBO's own `glColorMask`/`glDrawBuffer`/`glViewport`/`GL_DEPTH_TEST` *after* the
   pass rebinds it, and the per-eye modelview actually in force).
2. Re-run `--require-parallax` and require a non-zero per-tile disparity spread.
3. Keep `StereoDualRender=0` / `StereoParallaxDibr=0` / `StereoReplayProbe=0` defaults.
4. Player-visible queue GH #63/#64/#61, then the P1 bugs #45/#49/#50/#51/#65.
5. `DEFAULT_GAME_DIR` 2004-vs-2009; GH #83/#88/#89/#74.
