# ADR 0502: Native stereo (epic #57 item 1) — the depth-only clear is captured, and its depth is a far-only pass (GH #76)

- Status: Accepted (scheduled autoloop wake 100, GH #76)
- Date: 2026-10-18
- Related: GH #76 (this), GH #57 (epic / definition-of-done item 1), ADR-0501
  (the interim depth-based reprojection and its swap-time depth source),
  ADR-0439/0487 (the mid-frame depth-only clear the viewmodel window opens on),
  ADR-0488 (the measured world-pass projection: z-flipped, near 4.0 / far 2048.0),
  ADR-0481/0482 (the engine's transform is applied engine-side)

## Context

ADR-0501 landed the interim depth-based per-eye reprojection ("DIBR-lite") and
measured that its *named* depth source -- the window backbuffer's depth at
`SwapBuffers` -- is **not reliably available**: across three live runs the window
(and the engine's last-bound FBO) read uniformly at the far plane, with real
world depth appearing on ~1 frame in 120.

ADR-0501's Next #1 was therefore a concrete, testable hypothesis:

> capture the world-pass depth at a deterministic point -- read it inside
> `Hook_glClear` when the mask is `GL_DEPTH_BUFFER_BIT` alone (the mid-frame
> depth-only clear that opens the viewmodel window, ADR-0439), **before** the
> engine clears it, and cache that grid for the swap.

The reasoning: the single frame with real depth, plus the known depth-only clear,
suggests the engine clears the world depth before the swap on most frames, so the
instant *before* that clear should hold the world pass.

## Decision

Implement that capture (opt-in, throttled, fail-safe) and measure whether the
pre-clear depth is the world pass; and **guarantee the change cannot make the
stereo pair worse** by keeping wake 99's no-regression invariant explicit.

### 1. Capture the currently-bound draw framebuffer's depth at the depth-only clear

- `opengl_proxy.{h,cpp}`: track the **current** draw framebuffer (including a
  rebind to `0`), `GetCurrentDrawFramebuffer()`; and expose the presented-frame
  counter `GetSwapFrameCount()`. `GetLastBoundFramebuffer()` deliberately ignores
  a rebind to 0, so it cannot say which buffer a clear targets.
- `opengl_proxy.cpp`'s `Hook_glClear`: on a depth-only mask
  (`GL_DEPTH_BUFFER_BIT` set, `GL_COLOR_BUFFER_BIT` clear) it calls
  `StereoRenderer::Instance().OnDepthOnlyClear()` **before** forwarding the clear.
- `stereo_renderer.cpp`'s `OnDepthOnlyClear()`: a no-op unless
  `StereoParallaxDibr=1`; throttled by `DibrDepthEvery`; reads the bound
  framebuffer's own viewport via `glReadPixels(GL_DEPTH_COMPONENT, GL_FLOAT)`
  into a cached snapshot (`s_dibrCapture`), stamped with the swap counter and the
  raw min/max window depth for evidence.
- `BuildDibrGrid` gains the snapshot as depth source **0** (before the window and
  the engine-FBO reads). The reduction is now rectangle-aware so a snapshot taken
  over a whole viewport can be reduced over just the active rect; for an engine
  FBO it tiles the whole viewport exactly as the existing swap-time read did.
- Every 240 captures the hook logs
  `DIBR world-depth capture: clears=N captures=M fbo=.. WxH rawDepth=[lo..hi]`;
  `rawDepth` min is the direct evidence (raw window depth 0 = the near plane).

### 2. Keep the fail-safe explicit: the warp may not *reduce* the eye separation

Wake 99's `--require-parallax` safety was "a missing depth buffer can never
flatten the image". A depth read that is *present but far* can degrade the pair
just as badly, so `StereoRenderer::OnSwapBuffers` now uses the DIBR warp only when
its maximum disparity is at least the legacy half-IPD convergence cue it replaces
(`minSepPx = 2 * halfIpdPx`); otherwise it falls back to the legacy uniform blit.
The `DIBR:` log line reports `minSep` and the final `use`.

## Measurement (live, read-only per-run shadow, `RIDDICKVR_SIM=1`)

Command shape (unchanged from wake 99): `--shadow-install <install> --reach-gameplay
--duration 120 --advance-menu --advance-min-taps 8 --require-fresh-hook
--require-parallax --downscale 4 --ini StereoParallaxDibr=1`.
`deployed_hook_matches_build` PASS in every run; the hook byte-identity is
`aa59bd760005` (the post-gate build) for the gameplay run below.

Four attempts were needed; two stalled in the launcher/menu (the known live-run
flake -- no game window, 0 frames; not a measurement). The two that reached live
gameplay are the evidence:

- **Run B (pre-gate build, gameplay reached):** every in-level frame's grid was
  `src=clear ... fbo=6 depth_z=[41.5..471.2] m fullDisp=[0.12..1.44] px
  valid=48/48 use=1` -- the capture *worked* (real, depth-varying depth from the
  depth-only clear). **But with the warp applied the eye pair collapsed**:
  `stereo_kind: mono`, `best_dx=0`, `identical=True`, so `not_mono_duplicate`
  and `horizontal_disparity_present` FAIL, and all 77 gameplay frames graded
  mono. The sub-2 px physical disparity is *smaller* than the legacy
  convergence cue, i.e. the interim made the pair worse than shift-only. This is
  the counterfactual that motivates the gate.
- **Run D (post-gate build, gameplay reached):** **573 of 589** grid builds used
  `src=clear` (the rest `src=none`, i.e. no fresh capture yet), all
  `fbo=6 1280x605 depth_z=[41.2..471.2] m fullDisp=[0.12..1.45] px valid=48/48`,
  `minSep=8.0..10.0 use=0` -- the capture is **reliably present**, and the gate
  keeps the legacy blit. `DIBR world-depth capture` lines run to
  `clears=13437 captures=3360 fbo=6 1280x605 rawDepth=[0.905..1.000]`.
  Result: **the only graded FAIL is `stereo_parallax_present`** (the honest #76
  gap); `stereo_kind=shift-only` (`spread=0`), `horizontal_disparity_present`
  PASS (`dx=-14..-24`), `not_mono_duplicate` PASS (`identical=False`),
  `gameplay_full_bleed_reached` fill 1.00, 87/87 distinct gameplay frames, and
  no frame-time regression (~28,000 frames in 120 s).

### The falsification

At the depth-only clear the bound buffer (FBO 6, 1280x605) holds **nothing nearer
than ~41 m**: its raw window depth minimum is **0.905** across the whole run
(`depth_z` minimum 41.2 m). The measured world-pass projection has **near 4.0 m**
(ADR-0488), and a first-person view necessarily includes near floor/wall geometry
(window depth ~0.1-0.3 at 5-15 m). So the depth-only clear's buffer is **not the
visible world pass** -- ADR-0501's Next-1 hypothesis ("the instant before the
engine wipes the world pass") is **not supported**, and this route is closed as
the world-depth source. Which pass FBO 6's depth belongs to is **not
established** (consistent with ADR-0501's open "FBO 6 identity").

## Consequences

- GH #76's *second* named depth source is now closed by measurement: the
  depth-only clear supplies a real but **far-only** depth, so it cannot drive
  near-field parallax. The milestone (`--require-parallax`) is **not reached**.
- The capture instrument, the rectangle-aware reduction, the current-draw-FBO
  tracking and the `minSep` gate are landed, unit-covered and **safe**: with
  `StereoParallaxDibr=0` (the default) nothing runs; with it on and a
  far/absent depth, the legacy blit is kept (measured no regression).
- The gate corrects an ADR-0501 assumption -- "a present depth is always an
  improvement": a *far* depth degrades the pair, so presence must be paired with
  a magnitude floor.
- The remaining ways to real stereo are unchanged and only one is engine-side:
  (a) find the buffer/pass that actually holds the near world depth, or a
  different deterministic capture point; (b) route (1) proper, a second engine
  view -- which ADR-0501 measured architecturally blocked (the ADR-0406 seam is
  dead code, the engine's transform is engine-side).

## Next (honest)

1. **Identify the pass behind the depth-only clear / find the near-world depth.**
   The `rawDepth` evidence shows the clear targets a far buffer; a run that
   dumps the *colour* buffer at that same instant (or scans several FBOs' depth)
   would name it. Alternatively capture at the projection load that *closes* the
   viewmodel window (ADR-0439: "the world resumes").
2. Keep the `minSep` gate: never let a depth read reduce the eye separation.
3. `StereoParallaxDibr=0` stays the default; tune `DibrStrength`/grid on a
   headset only once a near-field source exists.
4. Player-visible queue after #76: GH #63/#64/#61 (hands), then the P1 bugs
   #45/#49/#50/#51/#65.

## Validation

- `tools/build-all.ps1 -Config Release` clean (x86 hook + x64 host);
  CTest **29/29 (x86)** and **27/27 (x64)**; all 12 `tools/test_*.py` exit 0;
  `xrsim --selftest`, `verify_reported_fixes`, `scenario_tests.py --check`,
  `verify_hook_init`, `validate_addresses`, `verify_test_artifacts` PASS.
  No new headless test was needed (the change is GL state + a guard inside the
  existing, already-tested DIBR path); the pure warp math is unchanged.
- Live: run D (above) is the gameplay evidence; run B is the pre-gate
  counterfactual. Artifacts under `%TEMP%\oc_wake100{,b,c,d}\` (report.json, hook
  logs, BMPs). Install untouched (read-only per-run shadows; no deploy; no install
  write); no leftover game/host processes; live lock released.
