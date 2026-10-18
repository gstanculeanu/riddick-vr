# ADR 0509: Native stereo (epic #57 item 1) — the per-eye pass reads the span's **output GL state** and the eye frames' **mean luminance**, so ADR-0508's moved blocker ("re-issued draws write no usable colour") becomes attributable (GH #76 / GH #90)

- Status: Accepted (scheduled autoloop wake 107, GH #76 / GH #90)
- Date: 2026-10-18
- Related: GH #76 (this), GH #90 (the scoped option-1 unit), GH #57 (epic /
  definition-of-done item 1), ADR-0508 (**whose Next 1 this implements**: re-issuing
  ~96% of the world span still produced unusable eye frames, so instrument *why*),
  ADR-0505 (the per-eye dual-issue pass + its coverage gate), ADR-0506 (the span
  census), ADR-0507 (the array-source census), ADR-0504 (the per-eye camera is a pure
  `GL_MODELVIEW` translation)

## Context

ADR-0508 moved the native-stereo blocker. It fixed the *classification* — a draw is
re-issued per eye when it used the generic-attribute path, so the span capture jumped
from ~60/735 draws to **706/735** — but the resulting per-eye frames are still
refused by the ADR-0505 fail-safe gate: `coverL=coverR=0.000..0.287` (below the 0.5
floor) and `use=0` on every sampled in-level frame. The blocker is therefore no
longer "which draws" but **"why do the re-issued draws render no usable colour"**.

Two mutually exclusive causes predict the same symptom:

1. **The span writes no colour.** If the engine's "world span" (between a colour
   clear and the mid-frame depth-only clear, ADR-0439/0483) is actually a
   depth/occlusion prepass — or a stage with `glColorMask(0,0,0,0)` — then re-issuing
   its geometry per eye is *correct* and produces a dark frame. The fix is then a
   span-boundary fix, not a re-issue fix.
2. **The re-issue is wrong** (the per-eye framebuffer, the modelview, the viewport,
   the readback). The fix is then in the pass.

Those point at different work, and ADR-0508 could not tell them apart: it never read
the output state under which the re-issued draws ran, and its "coverage" number is a
non-black *fraction* that counts a frame with mean luminance ~8/255 (measured) the
same as one that is genuinely black.

## Decision

Instrument both, evidence only, behind the existing opt-in (`StereoDualRender=0`
stays the default; the pass's draw decisions are unchanged):

1. `stereo_dual.h` (pure, headless-tested): `DualDrawState` — the engine output state
   a re-issued draw inherits — with `DualColourMaskNibble` (the 4-bit colour-write
   mask the way GL stores it; the default is `0xF`), `kDualDrawBufferNone`, and
   `DualDrawStateWritesNoColour` (a fully-masked colour write, or a `GL_NONE` draw
   buffer). Colour mask, depth mask, viewport, scissor and the enable bits are
   **global** GL state, so they carry unchanged into the per-eye frames; only
   `glDrawBuffer` is per-framebuffer, so it is recorded as the engine's own value for
   context, not blamed.
2. `stereo_renderer.cpp`: `CaptureDualDrawState()` reads that state **once per span,
   at the first eligible draw** (before the pass rebinds anything), via the system
   `glGetBooleanv`/`glGetIntegerv`/`glIsEnabled`; the state is reset each frame.
3. `DualMeanLuminance` (pure) and `SampleDualCoverage(eye, meanOut)` add the eye
   frames' mean luminance to the periodic coverage sample, so "black" and "dark" are
   distinguishable numbers rather than one fraction.
4. The `DualRender:` evidence line gains
   `luma=[L=… R=…]` and `sstate=[cap=… cmask=0x… dmask=… drawbuf=0x… vp=… sc=…
   scissor=… dt=… blend=…]`.

Nothing here changes which draws are re-issued, the eye framebuffers, or the gate.

## Evidence

Offline (this wake): `build-all.ps1` Release clean (x86 `opengl32.dll` 524800 B
`5ce012538038` + x64 host); CTest **32/32 (x86)** and **30/30 (x64)**; the extended
`stereo_dual` tests (section 11: the nibble, both no-colour forms, the unread-state
guard and the luminance mean) pass; `verify_reported_fixes`, `scenario_tests.py
--check`, `xrsim --selftest`, `verify_hook_init`, `validate_addresses`,
`verify_test_artifacts` and all `tools/test_*.py` pass.

Live (RIDDICKVR_SIM=1, read-only per-run shadow `riddickvr-shadow-15388`,
`--shadow-install "F:\The Chronicles of Riddick - Assault on Dark Athena"
--reach-gameplay --duration 70 --advance-menu --advance-min-taps 8
--require-fresh-hook --require-parallax --dump-branch any --downscale 4 --ini
StereoDualRender=1`; `deployed_hook_matches_build` PASS, hook `5ce012538038` = the
repo build): the run did **not** reach a *foreground* gameplay window (the known
launcher flake — `set_foreground_ok=False`, 4 cinema-labelled frames), so its
gameplay stereo checks are not graded; but the per-eye pass did run and its new line
names the cause. The one in-level span (swap 603) reads:

```
DualRender: swap=603 frames=219 used=31 draws=706 eligible=706 skipped=29
mirrors=384 active=1 coverL=0.000 coverR=0.000 luma=[L=0.0 R=0.0]
size=1280x605 use=0 scale=[unknown=675 near=0 mid=2 world=58]
reject=29 rc=[nonfloat=0 noarray=29 ...] gen=646 gbacked=640
gsig=[enabled=1 buf=368]
sstate=[cap=1 cmask=0x0 dmask=1 drawbuf=0x8CE0 vp=0,0,1280x605 sc=0
scissor=0,0,1280x605 dt=1 blend=0]
```

**`cmask=0x0`** — the engine's global colour-write mask is **fully off** at the first
eligible draw of the world span, with the depth test on (`dt=1`), depth writes on
(`dmask=1`) and a real colour attachment (`drawbuf=0x8CE0` = `GL_COLOR_ATTACHMENT0`),
the viewport exactly the eye-frame size and the scissor test off. So the span the pass
re-issues is a **colour-masked, depth-writing stage** (a depth/occlusion prepass): its
706 draws are real world geometry, re-issued correctly, and *cannot* colour the eye
frames. `coverL=coverR=0.000 luma=0.0` is the consistent result, and the earlier
frames show the counter-signal (`luma=113.1` when the pass captured no draws at all).

## Verdict (honest)

ADR-0508's moved blocker is **resolved to cause (1)**: the "world span" the per-eye
pass re-issues is a **colour-masked depth stage**, so re-issuing it per eye can never
produce a usable colour image — the fix is a **span-boundary fix** (find the engine's
*colour* world pass), not a change to the re-issue. Native stereo is **not** achieved
and `--require-parallax` still fails; `StereoDualRender=0` stays the default and there
is **no regression** (the pass was already inert, and the new reads change no draw).

## Limits (honest)

- This is instrumentation plus a measurement: it names the cause, it does not fix it.
- The state is sampled once per span, so a stage that changes its mask *within* the
  span is not seen; the per-span token is the span's opening state.
- `glDrawBuffer` is logged for the engine's buffer; the per-eye frames use their
  default `GL_COLOR_ATTACHMENT0` and are not affected by the engine's value.
- The live run measured one in-level span (the foreground-window flake kept the run in
  the menu thereafter); the colour-masked state was consistent within it, but the
  engine's *colour* world pass is still to be located.
- Shift-stereo remains until a live run grades a usable per-eye pair.
- The 2004 build is untouched; no headset.

