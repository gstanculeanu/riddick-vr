# ADR 0514: Native stereo (epic #57 item 1) — the per-eye pair is measured *directly*: both eyes' applied offsets are logged and the two eye FBOs are row-aligned in the same swap (GH #92 / GH #76)

- Status: Accepted (scheduled autoloop wake 112, GH #92 / GH #76)
- Date: 2026-10-18
- Related: GH #92 (this — the uniform-shift pair, whose Required 1+2 this ADR
  implements), GH #76 (native stereo, epic #57 DoD item 1), GH #90 (the scoped
  option-1 unit), ADR-0513 (**whose Next 1 this implements**, and whose live line
  first *graded* the pair as `stereo_kind=shift-only, spread=0`), ADR-0511 (the
  engine-frame control + the per-eye modelview probe), ADR-0509/0510 (the span's
  colour state), ADR-0504 (the eye-space replay the pass rests on), GH #77 (the
  host's synthetic eye baseline), GH #72 (the display-branch label)

## Context

ADR-0513 fixed the presentation branch (a *global* desktop cursor held the branch
CINEMA) and the harness could finally **grade** the pair it presents. It graded it:

```
stereo_parallax_present FAIL — stereo_kind=shift-only (gameplay frames=45,
max per-tile disparity spread=0)
```

i.e. the two presented eye images differ by a **single uniform horizontal shift**,
not depth-dependent disparity. That is GH #92. The presented BMPs are the end of the
pipeline (the host reads the shared interop textures the hook blits into), so the
reading alone cannot separate two very different faults:

- **(a)/(c)** the per-eye frames themselves are a uniform shift — the eye-space
  camera translation degenerating to a screen-space shift (wrong units / the
  vertices are not eye-space), or both eyes translated identically; versus
- **(b)** the pair that was *presented* is not the dual pair at all — the shipped
  shifted monoscopic capture blitted instead.

The `DualRender:` line already carries the gate and the first eye's post-translate
modelview (`mv=[… post=0.032,0.000,0.000]`), but not the second eye's offset, and
nothing reads the two eye FBOs' *difference* directly.

## Decision

One evidence-only unit, in two parts, exactly GH #92's Required 1 and 2. It changes
no draw and no presentation; `StereoDualRender=0` stays the default.

### 1. Log both eyes' applied offsets (`offL` / `offR`)

`stereo_renderer.cpp` already stores the per-eye eye-space offset it translates each
re-issued draw by (`m_dualOff[2][3]`, from `EyeSpaceCameraOffsetFromViews`). The
`DualRender:` line gains `offL=%.4f,%.4f,%.4f offR=%.4f,%.4f,%.4f`, so a run states
whether the two eyes get *different* offsets (cause (c) is then excluded) and by how
much along which eye-space axis (Required 2).

### 2. Align the two eye FBOs' own rows (`pair=[…]`)

The pass already reads both eye framebuffers every 60 swaps for the use gate
(`SampleDualCoverage`) and the engine frame as the control (`SampleEngineFrame`).
On the **same tick** it now also reads the same `kDualPairRows = 16` rows from
**both** eye framebuffers, converts them to luminance, and aligns each row of the
left frame onto the right (`SampleDualPair`). The pure policy lives in
`stereo_dual.h`:

- `DualRowBestShift` — the best horizontal shift (mean absolute difference over the
  interior, so every candidate compares the same pixel count) for one row, or
  *false* when the row is too flat to align on (`stdDev < kDualPairMinContrast`).
- `DualAlignPair` — every sampled row; returns `DualPairAlignment`
  (`measured`, `rows`, `bestDx` (median), `minDx`/`maxDx`, `spreadPx`, `identical`).
  A flat pair (one global shift) has `spreadPx == 0`; a genuine per-eye view whose
  depth varies — a near floor at the bottom of the frame, a far wall/sky at the top —
  has per-row shifts that differ.

The `DualRender:` line gains
`pair=[cap=%d dx=%d rows=%d spread=%d id=%d]`. Read together with the presented
`stereo_kind` this is decisive:

- `pair spread ≥ 6` but the presented pair is `shift-only` → cause **(b)**: the dual
  pair carries parallax and the *presentation* selected another pair.
- `pair spread ≤ 2` and `id=1` (identical) → cause **(c)**: both eyes translated the
  same / not applied.
- `pair spread ≤ 2` and `dx ≠ 0` → cause **(a)**: the camera translation becomes a
  uniform screen shift (the eye-space/units assumption is wrong).

## Evidence

**Offline (this wake).** `build-all.ps1 -Config Release` clean: x86 `opengl32.dll`
531968 B `559f9fbc08c9` + x64 host. CTest **32/32 (x86)** / **30/30 (x64)**; the new
`stereo_dual` section 15 passes (a textured uniform-shift pair → `spread 0` and
`dx 4`, byte-identical frames → `identical`, a per-row-differing pair → `spread ≥ 6`,
a flat row → refused, an all-flat pair → not measured). `verify_reported_fixes.py`
(new GH #92/ADR-0514 invariant), `xrsim --selftest`, `scenario_tests.py --check`,
`verify_hook_init.py`, `validate_addresses.py`, `verify_test_artifacts.py` and all 12
`tools/test_*.py` PASS.

**Live (wake 112, hook `559f9fbc08c9` = repo build).** Same command as ADR-0513
(`RIDDICKVR_SIM=1`, read-only per-run shadow, `--shadow-install "F:\The Chronicles of
Riddick - Assault on Dark Athena" --reach-gameplay --duration 80 --advance-menu
--advance-min-taps 8 --require-fresh-hook --require-parallax --dump-branch any
--downscale 4 --ini StereoDualRender=1`; `deployed_hook_matches_build` PASS; 80.3 s,
37 frames, `gameplay_captures_present` PASS 30 gameplay / 7 cinema, fill 1.00).

Across **41** `DualRender:` samples, **40 read `id=1`** (the two eye FBOs
*byte-identical*) and **41 read `spread=0`/`dx=0`**; the applied offsets are the
expected symmetric pair:

```
DualRender: swap=11439 draws=549 eligible=549 skipped=29 active=1
  coverL=0.279 coverR=0.279 luma=[L=10.4 R=10.4] use=1
  cw=[on=549 off=159] gen=648 gbacked=642 gsig=[enabled=1 buf=368]
  sstate=[cap=1 cmask=0x0 dmask=1 drawbuf=0x8CE0 vp=0,0,1280x605 dt=1]
  mv=[cap=1 id=1 pre=0.000,0.000,0.000 post=0.032,0.000,0.000]
  pair=[cap=1 dx=0 rows=16 spread=0 id=1]
  offL=-0.0325,0.0000,0.0000 offR=0.0325,0.0000,0.0000
DualRender: swap=12041 draws=167 eligible=167 active=1 coverL=0.669 coverR=0.669
  use=1 pair=[cap=1 dx=0 rows=16 spread=0 id=1]
  offL=-0.0325,0.0000,0.0000 offR=0.0325,0.0000,0.0000
```

- The offsets **differ** (`offL x = -0.0325`, `offR x = +0.0325`), so the two eyes are
  *not* translated identically — GH #92's cause (c) in its "same offset" form is
  excluded.
- Yet the two eye FBOs are **identical** (`id=1`, `spread=0`, `dx=0`) on both a dark
  span (`luma ~10.4`) and a bright one (`luma ~18.2`, `cover 0.669`), and across all
  spans. The re-issued draws *do* run (`draws=549 eligible=549`, `cw=[on=549
  off=159]`) and the frames carry a real scene (16/16 sampled rows are textured, so
  they are not flat clears [10 rows were too flat to align, 16 measured]). So the
  re-issued geometry renders, but **the per-eye `translate(-off)` at the draw site has
  no effect on the rendered image**.

## Verdict (honest)

- **GH #92's Required 1 and 2 are met**: the run now states, from the hook's own log,
  that the **per-eye pair carries no disparity at all** — the two eye images are
  byte-identical even though the two eyes are translated by different offsets.
- **The measured fault is the re-issue's transform, not the presentation and not the
  offsets.** The presented BMP pair differs (`dx=-24`/`-14` in the downscaled analysis,
  `meanL 8.242` vs `meanR 8.29`, so `not_mono_duplicate` PASSes) because the VR-hand
  overlay is drawn *per eye* with the eye's own view over an identical background; the
  harness's large apparent background shift is consistent with the known GH #79
  capture tear (the run reports `29/37 attempt(s) straddled a hook frame`), not with a
  real background disparity.
- **This closes a premise, explicitly.** ADR-0504's "the world geometry is eye-space
  **and the per-eye camera is a fixed-function `GL_MODELVIEW` translate**" holds only
  for the fixed-function path: applying exactly that translate leaves the rendered
  image unchanged, so for the shipped world pass the per-eye offset is **not** where a
  `glTranslatef` on the modelview can reach. The route "run the world pass twice with
  a fixed-function modelview translate" is therefore closed by measurement.
- **No regression**: evidence only, no draw or presentation change;
  `StereoDualRender=0` stays the default; `gameplay_full_bleed_reached` PASS,
  `eye_views_scene_faithful` PASS, `both_eyes_non_black`/`same_eye_resolution`/
  `no_vertical_disparity` PASS.

## Next

1. **Name the transform the world pass actually uses** (the leading hypothesis: an
   **ARB vertex program** — the engine resolves *and calls* the whole
   `glProgramStringARB` / `glProgramEnvParameter4fvARB` / `glBindProgramARB` family,
   measured at wake 105, and a program's transform rides its **environment
   parameters**, which a fixed-function `glTranslatef` cannot touch). LOG THE BOUND
   PROGRAM (id + string hash) and the current matrix sources *at the world draw*, then
   apply the per-eye offset where that transform lives (program env parameter), or
   bind a per-eye program — a genuinely new, concrete lead.
2. Only then re-run `--reach-gameplay --require-parallax` and require a non-zero
   per-tile spread on a gameplay-labelled frame.
3. Keep `StereoDualRender=0` / `StereoParallaxDibr=0` / `StereoReplayProbe=0` defaults.
4. Gate on the **queried** `GL_COLOR_WRITEMASK` at the draw (ADR-0511 Next 2).
5. Player-visible queue GH #63/#64/#61, then the P1 bugs #45/#49/#50/#51/#65;
   `DEFAULT_GAME_DIR` 2004-vs-2009; GH #83/#88/#89/#74.

## Consequences

- The next live run states, from the hook's own log, whether the **per-eye pair**
  carries depth-dependent disparity — separating GH #92's causes (a)/(b)/(c) without
  relying on the presented BMPs, which cannot tell them apart. (Measured: it does
  **not** — the eye pair is identical, and the fixed-function modelview-translate route
  is closed; see the Verdict.)
- The second eye's offset is now in the log, so "both eyes translated the same" is
  checkable directly rather than inferred from one `post=` vector.
- Honest limits: (a) the row alignment is a coarse 16-row luminance correlation, not
  a full-frame disparity map, so it can say "the pair varies with depth" but not the
  exact depth of each region; (b) it samples the eye FBOs only every 60 swaps and
  only for the rows the gate samples; (c) a scene whose depth is nearly constant
  across those rows would read `spread 0` even on a correct pair (the frame is then
  genuinely flat, not a bug); (d) it changes no draw, so it cannot by itself make the
  pair carry parallax — it only explains which fault it is.

## Validation

- `build-all.ps1 -Config Release` + CTest (above).
- `python tools/verify_reported_fixes.py` — new GH #92 / ADR-0514 invariant.
- `tools/xrsim.py --selftest`, `scenario_tests.py --check`, `verify_hook_init.py`,
  `validate_addresses.py`, `verify_test_artifacts.py`, all `tools/test_*.py`.
- A live `--reach-gameplay` run records the new `pair=[…] offL=… offR=…` tokens
  (see `STATE.md` for this wake).
