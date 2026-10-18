# ADR 0505: Native stereo (epic #57 item 1) — the per-eye camera is applied at the draw site: dual-issue the engine's eye-space draws into per-eye frames (GH #76 / GH #90)

- Status: Accepted (scheduled autoloop wake 103, GH #76 / GH #90)
- Date: 2026-10-18
- Related: GH #76 (this), GH #90 (the scoped option-1 unit this implements),
  GH #57 (epic / definition-of-done item 1), ADR-0504 (the measured mechanism:
  the engine submits eye-space client-array geometry with an identity
  `GL_MODELVIEW`, so a per-eye camera is a pure modelview translation),
  ADR-0481/0482 (that measurement), ADR-0491 (display lists + `glDrawArrays` and
  the world-pass immediate mode are all unused — the array path is complete),
  ADR-0490 (the measured near-field/world vertex separation this reuses),
  ADR-0439/0483 (the depth-only clear opens the first-person window),
  ADR-0461 (the shift-only stereo classification), GH #63/#61/#64 (the VR hands
  vs the engine's own viewmodel), ADR-0463 / GH #77 (why the hook must not trust
  the published per-eye views in mock/sim)

## Context

GH #76 is epic #57's definition-of-done item 1: the renderer captures **one**
backbuffer and blits it into both eyes with a single global half-IPD source
shift, so every gameplay frame classifies as `stereo_kind: shift-only` with zero
depth-dependent parallax.

Wake 99–101 closed the issue's interim option (2) by measurement (ADR-0501/0502/
0503: no hook-readable buffer holds the near world pass). Wake 102 (ADR-0504)
opened option (1) and landed its replay **maths**, scoping the remaining work as
"record the frame's `glDrawElements` draws and replay them per eye" — GH #90.

That plan was written as a *record-and-replay*: capture each draw's mode/count/
type/indices and the client arrays, then re-issue them after the frame. Writing it
revealed a simpler and strictly better form, from measurements already in hand:

- ADR-0481/0482: the engine's world geometry reaches GL as **eye-space client
  arrays with an identity `GL_MODELVIEW`**. The camera transform is a translation
  of the vertices; moving the eye is `translate(-offset_eye)` (ADR-0504).
- ADR-0491: `listIn=listOut=0` in every sampled interval (no display lists),
  `glDrawArrays` is never bound, and the world pass uses no immediate mode. The
  `glDrawElements` hook therefore sees the **complete** geometry path.
- Consequently, at the moment of the draw hook the geometry is in hand *and the
  entire fixed-function state that will shade it is already current* (textures,
  lighting, blending, depth, matrices). Re-issuing the identical call while the
  camera is offset reproduces exactly what the record-and-replay would have
  reconstructed — with **no state capture and no copied vertices**.

So the per-eye camera can be applied *at the draw site*, once per eye, instead of
being replayed after the fact. What a replay would have to reconstruct is the
reason not to replay.

## Decision

Land the native per-eye render as an **opt-in dual-issue pass** (`StereoDualRender`,
default `0`), with a fail-safe fallback to the shipped shifted capture.

### 1. Per-eye frames (`stereo_renderer.{h,cpp}`)

Two colour+depth framebuffers (`m_dualFbo[0/1]`, RGBA8 + `DEPTH24_STENCIL8`) are
created at the **engine's viewport size**, so the engine's own `glViewport` state
applies unchanged to a re-issued draw and no viewport juggling is needed. They are
recreated when the viewport changes and destroyed with the eye buffers.

### 2. Mirroring the scene boundary and the world span (`StereoRenderer::OnSceneClear`)

`Hook_glClear` mirrors a colour and/or depth clear into both per-eye frames, using
the **engine's own clear colour/depth state** (the same GL state applies to any
bound draw framebuffer). Mirrors happen only while no eligible draw has been
re-issued this frame, so a later clear can never wipe a frame that was just built.
A colour clear re-bases the frame's counters and refreshes the per-eye offsets.

The same hook maintains the **world span**: the measured frame order
(ADR-0439/0483) is the 3-D world pass, then a mid-frame **depth-only** clear that
opens the first-person window, then the viewmodel/HUD pass, then the swap. So a
colour clear (or the previous swap) starts the span and the depth-only clear ends
it, and only draws inside the span are candidates. The per-eye frames are cleared
at the **swap** (after they were used) with the engine's own current clear state —
clearing at the mid-frame depth-only clear would wipe the world pass just built.

### 3. Dual-issue (`StereoRenderer::OnSceneDraw`, called from `Hook_glDrawElements`)

Before the engine's own draw is forwarded:

- the draw is accepted only when it falls inside the **world span** and its
  **sampled client-array bounds are not near-field** (`stereo_dual.h::
  DualRenderDrawEligible`): the measured world/eye-space meshes sit at
  `|coord| ~ 1.6e3..3.5e3` while HUD/screen-space quads and the engine's own
  viewmodel sit at `±0.45..0.5` (ADR-0482/0483/0490). An undecodable array is left
  to the gate rather than guessed at;
- for each eye, the real `glBindFramebuffer` + `glPushMatrix` +
  `glTranslatef(-offset_eye)` + `glDrawElements` + `glPopMatrix` are issued
  through the **system opengl32** (bypassing the hook layer, so no recursion and
  the modelview tracker is not disturbed), then the engine's binding and matrix
  mode are restored.

The offset comes from `EyeSpaceCameraOffsetFromViews` (stereo_replay.h), i.e. the
live per-eye views when they are scene-faithful and the head-local `±halfIpd`
otherwise — the same gate ADR-0504 measured (the mock host publishes the
placeholder baseline, GH #77).

### 4. Fail-safe gate (`stereo_dual.h`, swap in `OnSwapBuffers`)

A frame is used only when at least `kDualMinEligibleDraws` draws were re-issued
and a periodically sampled non-black coverage of **both** eye frames passes
`DualFrameUsable` (`kDualMinEyeCoverage`). Otherwise the shipped shifted capture
is kept. HUD/screen-space draws and the engine's own viewmodel are never
translated, so the HUD stays screen-aligned and the engine arms stay out of the
per-eye frames (the GH #63 outcome). Everything is off by default; the pass is
inert and allocates nothing when `StereoDualRender=0`.

## Measurement (live, read-only per-run shadow, `RIDDICKVR_SIM=1`)

Command shape: `--shadow-install "F:\...Assault on Dark Athena" --reach-gameplay
--duration 70 --advance-menu --advance-min-taps 8 --require-fresh-hook
--require-parallax --dump-branch any --downscale 4 --ini StereoDualRender=1`.
`deployed_hook_matches_build` PASS every run; no deploy, no install write.

Four runs. The first two exposed two implementation defects that only a live run
could; the last two are the measured result of the landed build.

- **Run 1 (no `use`, no log lines at all).** The original acceptance filter also
  required the *first-person window* to be open. That window is opened by the
  **mid-frame depth-only clear** and holds the viewmodel/HUD pass (ADR-0439/0483),
  i.e. it is *after* the world pass — so the filter excluded the world entirely and
  the pass never engaged. Fixed by the world span (colour clear / previous swap
  starts it, the depth-only clear ends it) in `OnSceneClear`.
- **Run 2 (`--dump-branch any`, 4 frames captured).** The pass engaged for the
  first time (`eligible=60` draws, `use=1` on two sampled frames) — but the frames'
  presented mean luminance was only **~3/255**: the sampled rows caught a little
  content over an otherwise black frame. That is not a usable eye image, so the
  gate's floor was raised to 8 rows / 50% (`kDualMinEyeCoverage`), with the
  measurement as its stated reason.
- **Run 3 (scale census added).** The census made the blocker explicit. Every
  sampled frame in the world span reads
  `scale=[unknown=675..677 near=0 mid=2 world=58]`: of ~735 draws in the span,
  **~92% are undecodable** by the client-array sampler and only **58-60** are
  decodable world-scale geometry. `near=0` refutes the earlier reading that the
  span's draws were the near-field/viewmodel pass.
- **Run 4 (final build, hook `sha256 ba9b1f4e566e`, `519680 B`).** With the raised
  gate: `draws=60 eligible=60 skipped=675..677 mirrors≈1/frame
  coverL=coverR=0.000..0.281 use=0` on every sampled frame — so the tightened gate
  refuses the pass's frames and **gameplay still presents the shipped shifted
  capture** (no regression). The run reached the level (BarScan rects, `ViewmodelProbe
  proj` lines, 9000+ swaps in 70 s) but the harness captured only **cinema-labelled**
  frames (`gameplay_captures_present`: 0 gameplay / 5 cinema), so
  `stereo_parallax_present` reports `stereo_kind=shift-only (gameplay frames=0)` and
  the per-tile disparity acceptance could **not** be graded this wake.

**Verdict (honest):** the native per-eye pass is implemented, opt-in, gated,
off by default and **measured inert on its own gate** — it captures ~60 world-scale
draws per frame and its frames are not yet a usable image, so nothing about the
presented stereo changed. Native stereo is **not** achieved: `--require-parallax`
still fails, and the remaining work is now specific rather than speculative:

1. **Why are ~92% of the span's draws undecodable?** The sampler rejects non-`GL_FLOAT`
   vertex arrays, unknown index types, null pointers and sizes outside 2..4
   (`vertex_submission_observe.h`). Log the rejected draws' vertex
   type/size/stride/pointer and extend the decode (the world pass is clearly not
   being captured by the float-array path that ADR-0482 sampled).
2. **The span itself is not yet proven to be the visible world pass**: only 60
   draws of ~735 are world-scale, and the ~2 mid-scale ones are unexplained.
   Log the per-frame draw total and the projection/ortho state per draw.
3. **Grading needs gameplay-labelled captures**: the harness's dump gate (GH
   #72/#69) labelled all captured frames CINEMA this run, so the parallax check ran
   on 0 gameplay frames. Either fix that gate or add an in-hook per-eye disparity
   measurement (the eye pair is available to the hook itself).
4. The eye frames use the engine's projection (parallel-axis stereo) and need the
   runtime's per-eye projections plus HUD handling before any headset run.

## Consequences

- The opt-in implements epic #57 DoD item 1's **render** (two real per-eye views
  of the world) rather than a shifted blit, without the draw recorder/state
  capture GH #90 originally scoped — the measurement in Context is what makes the
  simpler form correct.
- **Off by default and gated**: `StereoDualRender=0` is inert, and an empty pass
  falls back to the shipped capture, so the shift-stereo path and its gates cannot
  regress.
- Cost: the world geometry is submitted twice more per frame (the engine's own
  draw plus two re-issues) plus two framebuffer binds per draw. The harness did not
  report a `FrameBudget` line in the wake-103 runs (a host-side gap), so the
  frame-time effect is **not** measured yet — it must be before any run with the
  pass enabled on a headset.
- Limits: the eye frames use the engine's projection (parallel-axis stereo), so a
  runtime-provided per-eye projection is a later refinement; the HUD is
  screen-aligned (not stereo-placed); the world span is a *heuristic* boundary
  (only ~8% of its draws decode today); nothing here addresses the 2004 build; and
  the orientation/sign of the offset needs one headset run to confirm.

## Next (honest)

1. **Make the span's draws decodable**: log the vertex type/size/stride/pointer of
   the ~675 undecodable draws per frame and extend the sampler (see Measurement 1).
2. Prove (or replace) the world-span boundary from evidence: per-frame draw totals
   and the per-draw projection state (see Measurement 2).
3. Grade parallax: fix the harness dump gate's gameplay label (GH #72/#69) or
   measure the eye pair's per-tile disparity in the hook.
4. On a headset: use the runtime's per-eye projections, place the HUD, and tune
   comfort (the vignette applies to the eye frames as before).
5. Keep `StereoDualRender=0` the default until 1-4 land; `StereoParallaxDibr=0` and
   `StereoReplayProbe=0` stay the defaults too.
6. Player-visible queue GH #63/#64/#61 and the P1 bugs remain headset/deploy-gated
   (#64's latest diagnosis routes them through the capped #67).

## Validation

- `tools/build-all.ps1 -Config Release` clean (x86 hook, x64 host); CTest
  **31/31 (x86)** and **29/29 (x64)** (+1 each: `stereo_dual`); all 12
  `tools/test_*.py` exit 0; `xrsim --selftest`, `verify_reported_fixes`,
  `scenario_tests.py --check`, `verify_hook_init`, `validate_addresses`,
  `verify_test_artifacts` PASS.
- Live: four read-only per-run shadow runs (`%TEMP%\oc_wake103*`, plus the shadows
  `riddickvr-shadow-416/16696/2636/14312`), `deployed_hook_matches_build` PASS each;
  the final build's lines are quoted above. Install untouched (read-only shadows;
  no deploy, no install write); no leftover game/host processes; live lock released.
