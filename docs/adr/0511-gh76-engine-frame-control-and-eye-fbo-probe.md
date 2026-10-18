# ADR 0511: Native stereo (epic #57 item 1) — the eye frames are compared to the engine's **own presented frame**, the eye FBO's state and per-eye modelview are probed, and the measurement makes the **calibration** (not the re-issue) the blocker for the in-level span (GH #76 / GH #90)

- Status: Accepted (scheduled autoloop wake 109, GH #76 / GH #90)
- Date: 2026-10-18
- Related: GH #76 (this), GH #90 (the scoped option-1 unit), GH #57 (epic /
  definition-of-done item 1), ADR-0510 (**whose Next 1 this implements**: compare the
  eye frame's content to the engine's own presented frame, and log the eye FBO's own
  state + the per-eye modelview), ADR-0509 (the span output-state probe),
  ADR-0508 (the generic-attribute classification), ADR-0505 (the per-eye pass + its
  coverage gate), ADR-0504 (the identity-modelview assumption)

## Context

ADR-0510 closed the span-boundary route by measurement and named ADR-0509's cause (2)
— "the re-issue itself" — as the remaining blocker, on the strength of one reading:
`coverL=0.289`, `luma=10.3`, `use=0`. Its Next 1 was explicit and **two-sided**: read
the *engine's own presented frame* in the same span, because

- if the engine frame is also dark (~0.3 non-black), the fixed `0.5` coverage floor is
  refusing a *genuinely dark scene* and the **calibration** — not the re-issue — is the
  blocker; and
- if the engine frame is bright, the eye frame really is mostly empty and the fault is
  the re-issue (log the eye FBO's own `glColorMask`/`glDrawBuffer`/`glViewport`/
  `GL_DEPTH_TEST` *after* the pass rebinds it, and the per-eye modelview in force).

The eye-frame number alone cannot tell those apart, so ADR-0510's conclusion rested on
an unmeasured premise (that the in-level frame was bright).

## Decision

Add the control and the probes; change no draw.

1. **`stereo_renderer.cpp::SampleEngineFrame`** samples the engine's **own presented
   frame** (default read framebuffer / `GL_BACK`) with the *same* 8-row / frame-width
   estimator as the eye frames, and is called from the **same coverage tick** as
   `SampleDualCoverage`, so the two numbers describe one swap.
2. **`stereo_renderer.cpp::CaptureDualEyeState`** reads the state a re-issued draw runs
   under **while the eye FBO is bound** (its own draw buffer, viewport, depth test and
   colour mask), once per span at the first re-issued draw — the per-framebuffer twin of
   ADR-0509's `CaptureDualDrawState`.
3. **The modelview probe** reads `GL_MODELVIEW_MATRIX` at the first re-issued draw
   (before the pass pushes) and again after `translate(-offset)` on the first eye, so
   ADR-0504's *identity* premise and the applied per-eye offset are measured at the draw
   site instead of inherited.
4. **`stereo_dual.h`** gains the pure, headless-tested policy: `ClassifyDualDarkSide`
   (the two-frame verdict: both-dark → the floor is the fault; engine-lit/eye-dark → the
   re-issue produced no image), `DualEyeFrameState` + `DualEyeFrameWritesNoColour` +
   `DualEyeColourNibble` (the eye-side no-colour test), and
   `DualModelviewIsIdentity`/`DualModelviewTranslation`.
5. The `DualRender:` line gains `engine=[cover=… luma=…] dark=…`,
   `eyeFbo=[cap=… drawbuf=0x… vp=… dt=… cmask=0x…]` and
   `mv=[cap=… id=… pre=…, …, post=…, …]`.

Everything stays opt-in (`StereoDualRender=0` default) and evidence-only: the new reads
never change a draw.

## Evidence

**Offline (this wake).** `build-all.ps1 -Config Release` clean: x86 `opengl32.dll`
527360 B `9466e77fd51b` + x64 host. CTest **32/32 (x86)** and **30/30 (x64)** (the new
`stereo_dual` section 13 passes); `verify_reported_fixes.py` (new #76/#90/ADR-0511
invariant), `scenario_tests.py --check`, `xrsim --selftest`, `verify_hook_init.py`,
`validate_addresses.py`, `verify_test_artifacts.py` and all `tools/test_*.py` pass.

**Live (RIDDICKVR_SIM=1, read-only per-run shadow `riddickvr-shadow-14012`,
`--shadow-install "F:\The Chronicles of Riddick - Assault on Dark Athena"
--reach-gameplay --duration 80 --advance-menu --advance-min-taps 8
--require-fresh-hook --require-parallax --dump-branch any --downscale 4
--ini StereoDualRender=1`).** `deployed_hook_matches_build` PASS, hook
`9466e77fd51b` = the repo build; 80.1 s, 53 captured frames;
`gameplay_full_bleed_reached` fill **1.00** (the harness labelled the frames cinema,
GH #72). The new tokens are decisive. The **in-level** span (the one ADR-0510 graded —
`unknown=675 world=58 reject=29 rc=[noarray…] gen=646 gbacked=640`), swap=4817:

```
DualRender: swap=4817 … draws=549 eligible=549 skipped=29 active=1
coverL=0.285 coverR=0.285 luma=[L=10.4 R=10.4] … cw=[on=549 off=159 calls=18877]
sstate=[cap=1 cmask=0x0 dmask=1 drawbuf=0x8CE0 vp=0,0,1280x605 sc=0
scissor=0,0,1280x605 dt=1 blend=0]
engine=[cover=0.294 luma=10.6] dark=both-dark
eyeFbo=[cap=1 drawbuf=0x8CE0 vp=0,0,1280x605 dt=1 cmask=0x0]
mv=[cap=1 id=1 pre=0.000,0.000,0.000 post=0.032,0.000,0.000]
```

- **`engine=[cover=0.294 luma=10.6]` beside `coverL=0.285 luma=[L=10.4 …]`** — the
  engine's **own** presented frame in the in-level span is as dark as the eye frames
  (across the run's 20 in-level ticks the engine frame reads `cover 0.276..0.300`,
  `luma 10.3..10.6`, the eye frame `0.267..0.294`, `10.1..10.5`). The measured
  `~0.28 / ~10.3` is the **scene**, not an empty eye frame.
- **`eyeFbo=[cap=1 drawbuf=0x8CE0 vp=0,0,1280x605 dt=1 cmask=0x0]`** — the eye
  framebuffer's own draw buffer is `GL_COLOR_ATTACHMENT0` (not `GL_NONE`) and its
  viewport is exactly the frame size, so the pass's own FBO state is **correct** and
  cannot be what discards the re-issued colour.
- **`mv=[cap=1 id=1 pre=0.000,0.000,0.000 post=0.032,0.000,0.000]`** — the engine's
  modelview at the draw is **identity** (ADR-0504 confirmed at the draw site) and the
  per-eye translate lands (`post = -offset = +halfIpd`), so the per-eye camera is
  applied as designed.
- **`cw=[on=549 off=159]` beside `sstate=[… cmask=0x0 …]` on the *bright* pre-level
  spans** (`engine=[cover=1.000 luma=107.6] dark=eye-only-dark`, `eyeC=0.000`, e.g.
  swap=2409/3011/3613): the ADR-0510 tracker reports colour-**ON** for the span's first
  eligible draw while the driver's actual `GL_COLOR_WRITEMASK` is `0x0`. Those spans'
  eye frames are black with a bright engine frame.

## Verdict (honest)

- **ADR-0510's conclusion is refuted for the in-level span.** It read `coverL=0.289` as
  "the eye frame is mostly-empty" and named the re-issue; the control shows the
  engine's own frame is the *same* dark reading. With `kDualMinEyeCoverage = 0.5` the
  pass therefore **cannot pass its own gate on this level even when it produces a
  faithful frame**: `use=0` is forced by the gate's absolute floor. **The calibration is
  a blocker** (ADR-0510's Next-1 first branch, not its second).
- **The re-issue's own state is not the fault**: the eye FBO's draw buffer and viewport
  are correct and the per-eye modelview is identity with the expected translate.
- **The ADR-0510 per-draw mask gate is not a reliable discriminator** (new, measured):
  it reports colour-ON for draws whose *driver* mask is `0x0`. `s_colourMaskNibble` is
  updated only by `Hook_glColorMask`; the mask is also changed by the hook's own
  `OpenGLProxy::glColorMask` calls (`stereo_renderer.cpp:2116-2126`) and by any
  `glPushAttrib`/`glPopAttrib` restore, neither of which the tracker observes.
- Native stereo is **NOT** achieved: `--require-parallax` still FAILs
  (`stereo_kind=shift-only`, `spread=0`; 0 gameplay-labelled frames — the GH #72 label
  flake). `StereoDualRender=0` is the default, so there is **no regression**
  (`gameplay_full_bleed_reached` fill 1.00, `eye_views_scene_faithful` PASS,
  `both_eyes_non_black`/`same_eye_resolution`/`no_vertical_disparity` PASS on the
  presented frames).

## Limits (honest)

- The engine-frame sample is the same 8-row / frame-width estimator of the *presented
  backbuffer*; the verdict rests on its order of magnitude and its mean luminance (both
  ~0.28 / ~10.3, matching the eye frames and ADR-0505/0508/0510), **not** on exact
  per-tick equality. On ticks with no re-issued draw the eye reading can coincide with
  the engine's exactly, which the estimator alone cannot attribute; that is why the
  comparison is reported as a pair with the verdict token rather than graded.
- The eye frame reads black on some in-level ticks and faithful on others; this ADR
  records the intermittency but does not yet explain it (the eye-FBO state is captured
  once per span, at the *first* re-issued draw, which is the span's masked opening).
- One live run; its frames are the flaky `cinema`-labelled ones (GH #72), so the stereo
  acceptance could not be graded on `gameplay` frames.
- The modelview probe reads `GL_MODELVIEW_MATRIX` once per span; a mid-span modelview
  change is not seen. The 2004 build is untouched; no headset.

## Next

1. **Recalibrate the gate against the engine frame.** Grade each eye against the
   engine's own frame (a relative floor — each eye carries at least a fraction of the
   engine frame's coverage — plus a low absolute floor so a bright engine frame still
   refuses a black eye), so a dark level is accepted while an empty re-issue is not.
2. **Gate on the *queried* mask.** Use `glGetBooleanv(GL_COLOR_WRITEMASK)` at the draw
   (or reconcile the tracker: observe the hook's own mask writes and attrib restores),
   since the ADR-0510 nibble was measured to disagree with the driver.
3. Re-run `--require-parallax` and require a non-zero per-tile disparity spread.
4. Keep `StereoDualRender=0` / `StereoParallaxDibr=0` / `StereoReplayProbe=0` defaults.
5. Player-visible queue GH #63/#64/#61, then the P1 bugs #45/#49/#50/#51/#65.
6. `DEFAULT_GAME_DIR` 2004-vs-2009; GH #83/#88/#89/#74.
