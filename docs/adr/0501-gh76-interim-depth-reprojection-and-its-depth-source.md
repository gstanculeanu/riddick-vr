# ADR-0501: Native stereo (epic #57 item 1) — the interim depth-based reprojection is implemented, and its depth source is measured unavailable at swap time (GH #76)

- Status: Accepted (scheduled autoloop wake 99, GH #76)
- Date: 2026-10-18
- Related: GH #76 (this), GH #57 (epic / definition-of-done item 1), ADR-0461
  (the shifted-monoscopic measurement and the `--require-parallax` gate),
  ADR-0406 (the "intended seam" `StabilizeViewMatrix`, measured dead here),
  ADR-0481/0482 (the engine applies its transform engine-side with an identity
  modelview), ADR-0488 (the engine's z-flipped projection convention),
  ADR-0439/0487 (the mid-frame depth-only clear the viewmodel window opens on)

## Context

GH #76 is the top player-visible gap: epic #57 definition-of-done item 1. Today
the renderer captures the game's backbuffer **once** and blits it into both eye
FBOs with a single global horizontal source-rect shift
(`halfIpdPx = max(1, spanW/240)` plus a comfort term), so every gameplay frame is
`stereo_kind: shift-only` with a per-tile disparity spread of **0 px**
(ADR-0461). The depth cue that makes stereo stereo is absent.

The issue names two routes:

1. **Per-eye view/projection** — render the 3D scene twice with the camera offset
   by +/-IPD/2. The issue points at `CameraHook::StabilizeViewMatrix` as "the
   intended seam (ADR-0406)".
2. **Interim: depth-based reprojection (DIBR)** of the single capture, using the
   game's own depth buffer, so the disparity at least varies with depth.

This wake is the interim. Before coding it, the seam was checked (measure first):

- `CameraHook::StabilizeViewMatrix` is **dead code**: it is declared in
  `camera_hook.h:18`, defined at `camera_hook.cpp:327`, and has **zero call
  sites** in the tree (`grep -rn StabilizeViewMatrix src/ tests/` → the header,
  the definition, and ADR prose only). `CameraHook::Update` drives head-look by
  **injecting mouse movement** (`SendMouseMove`), and `ScanForCameraSignature()`
  is `return false; // Dynamic matrix intercept active` (`camera_hook.cpp:111`).
  So the hook never touches the engine's view matrix, and route (1) is
  architecturally blocked: the engine renders once and the hook cannot ask for a
  second view (consistent with ADR-0481/0482 — the engine applies its own
  transform with an identity GL modelview).

Route (2) is therefore the only implementable option this wake.

## Decision

Implement the interim depth-based per-eye reprojection, **opt-in and default
off**, with a safe fallback to the existing uniform shift; and measure whether
its depth source exists on this title.

### 1. Pure warp math (`src/hook/depth_parallax.h`, new)

Header-only, no GL/Windows headers, unit-tested headlessly:

- `DepthToViewZ(windowDepth, proj)` — window depth [0,1] -> positive view
  distance, honouring **both** projection conventions (the engine's z-flipped one
  measured in ADR-0488, and classic GL); 0 (unknown) for ortho/NaN/out-of-range.
- `ParallaxShiftPx(viewZ, fPx, baselineM, strength)` — `fPx * baseline / z`,
  clamped to `kMaxParallaxShiftPx` (128).
- `FocalPixelsFromProjection(proj, w)` — `m[0] * w / 2`.
- `EyeBaselineMeters(leftView, rightView, fallback)` — the IPD from the host's
  own per-eye translations (the sim's `kHalfIpdMeters = 0.0325` -> 0.065 m).
- `CellMinViewZ(depth, w, h, x0, y0, x1, y1, proj)` — a grid cell's nearest depth.

### 2. The renderer warp (`stereo_renderer.cpp`)

Opt-in via `StereoParallaxDibr=1`. When on and the engine's last projection is a
usable perspective matrix, the single uniform-shift blit is replaced by a
`DibrCols x DibrRows` grid of blits, each cell shifted by its own depth-derived
disparity (`-disp/2` left, `+disp/2` right) — so disparity varies with depth.
`DibrStrength` scales the physical warp, `DibrDepthEvery` throttles the depth
readback (default every 3rd swap; the grid is reused in between).

Depth is read from two sources in order: (1) the **window backbuffer**'s depth
(`glReadPixels(GL_DEPTH_COMPONENT, GL_FLOAT)`), and (2) the **engine's last
non-zero draw FBO** (now tracked in `OpenGLProxy::Hook_glBindFramebuffer`),
resolved against its own viewport. `OpenGLProxy::GetLastBoundFramebuffer()`
exposes it.

**Fail-safe:** the warp is only used when the reduced grid actually *varies*
(`maxDisp - minDisp >= 0.5 px`). A uniform read falls back to the legacy uniform
shift, so a missing/cleared depth buffer can never flatten the image or regress
the existing stereo checks. Disabled by default, so the shipped path is
unchanged.

## Measurement (live, read-only per-run shadow, `RIDDICKVR_SIM=1`)

Three `--reach-gameplay --advance-menu --advance-min-taps 8 --require-fresh-hook
--require-parallax --downscale 4 --ini StereoParallaxDibr=1` runs reached live
full-bleed gameplay (`gameplay_full_bleed_reached` fill 1.00; 18-50 gameplay
frames; `deployed_hook_matches_build` PASS each time).

- **The window depth buffer does not carry the scene.** Across **347** grid
  rebuilds in run A, `depth_z = [2048.01 .. 2048.01] m` — i.e. the window
  backbuffer's depth is uniformly at the far plane (`fullDisp = 0.03 px`), so the
  grid is flat and the code falls back (`use=0`). The *colour* backbuffer is
  populated in the same frames (BarScan active-rect fill 1.00), so this is not a
  read failure: the engine does not write the 3D depth into the window backbuffer.
- **The engine does bind its own FBO** — `fbo=6` is the last non-zero draw
  framebuffer — but reading **its** depth also yields the far plane, so it is not
  a usable depth source either (its depth attachment is not the world pass's, or
  it is not readable this way).
- **One sampled frame (run C) DID expose real scene depth from the window**:
  `depth_z = [4.00 .. 2048.01] m`, `fullDisp = [0.03 .. 14.11] px`, `use=1` — a
  real 4.0 m world near plane (ADR-0488's measured world pass) with a 14 px
  disparity range across the grid. So the mechanism *works when depth is present*;
  the source is simply not reliably available **at `SwapBuffers` time** (1 of ~120
  sampled frames).
- **No regression:** with the flag on, `stereo_kind` was still `shift-only`
  on the graded gameplay frames and every other stereo/capture check passed
  (`both_eyes_non_black`, `no_vertical_disparity`, `same_eye_resolution`,
  `not_mono_duplicate`, `gameplay_full_bleed_reached`). `--require-parallax`
  FAILs, naming the shift — the honest verdict.

## Consequences

- The interim machinery now exists, is unit-tested, is opt-in, and is **safe**
  (measured no regression, default off).
- GH #76's *named* depth source (the window backbuffer's depth at swap time) is
  measured **not reliably available** on this title. The failure is a **timing /
  present-path** issue, not a math or GL-call issue: real world depth is present
  in the window backbuffer on some frames.
- Route (1) is recorded as architecturally blocked on this title: the ADR-0406
  seam is dead code and the engine's view transform is engine-side with an
  identity GL modelview, so the hook has no second-render entry point.

## Next (honest)

1. **Capture the world pass's depth at a deterministic point.** The one frame
   with real depth, together with the known mid-frame **depth-only clear**
   (ADR-0439's viewmodel-window opener), suggests the depth is cleared before the
   swap on most frames. Reading the depth **inside `Hook_glClear` when the mask is
   `GL_DEPTH_BUFFER_BIT` alone** (the instant before the engine clears it) should
   reliably capture the world-pass depth; cache that grid for the swap.
2. Failing (1), route (2) needs the engine's depth attachment identified (the
   engine's FBO `6` depth is not it, or not readable after unbind).
3. Route (1) proper needs an engine render seam (a second view), not a GL matrix
   hook; treat the ADR-0406 seam as unavailable and scope it separately.
4. Tune `DibrStrength`/grid on a headset; `StereoParallaxDibr=0` stays the default.

## Validation

- `tests/depth_parallax_tests.cpp` (new, CTest `depth_parallax`, x86 + x64): both
  conventions of `DepthToViewZ` (the measured z-flipped world matrix 4.0/2048.0
  and classic GL 0.05/1000.0), the unusable-input cases, `ParallaxShiftPx`
  monotonicity/clamp/scaling, `EyeBaselineMeters` from the eye views,
  `FocalPixelsFromProjection`, and `CellMinViewZ` (nearest wins; a far-only cell
  reads far, not near; clamping).
- `tools/build-all.ps1 -Config Release` clean (x86 hook + x64 host);
  **CTest 29/29 (x86) and 27/27 (x64)** (+1 each); all 12 `tools/test_*.py`
  exit 0; `xrsim --selftest`, `verify_reported_fixes`, `scenario_tests.py --check`,
  `verify_hook_init`, `validate_addresses`, `verify_test_artifacts` all PASS.
- Live: three `--reach-gameplay` runs on read-only per-run shadows (see
  `tools/autoloop/STATE.md` wake 99 for the artifacts and the per-run numbers);
  install untouched, no deploy.
