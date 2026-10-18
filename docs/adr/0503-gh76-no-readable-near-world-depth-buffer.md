# ADR 0503: Native stereo (epic #57 item 1) — no hook-readable framebuffer object holds the near world pass, so the interim depth reprojection has no usable depth source (GH #76)

- Status: Accepted (scheduled autoloop wake 101, GH #76)
- Date: 2026-10-18
- Related: GH #76 (this), GH #57 (epic / definition-of-done item 1), ADR-0501
  (the interim depth-based reprojection and its swap-time depth source),
  ADR-0502 (the depth-only clear's own depth is a far-only pass),
  ADR-0439 (the mid-frame depth-only clear the viewmodel window opens on),
  ADR-0488 (the measured world-pass projection: z-flipped, near 4.0 / far 2048.0),
  ADR-0481/0482 (the engine's transform is applied engine-side; the hook cannot
  ask for a second view)

## Context

ADR-0501 measured that the interim depth reprojection's *named* source (the window
backbuffer's depth at `SwapBuffers`) is not reliably available; ADR-0502 captured
the depth at the mid-frame depth-only clear and falsified that hypothesis too —
the clear's buffer holds nothing nearer than ~41 m, so it is a far-only pass, not
the visible world.

ADR-0502's Next 1 was the remaining, concrete step:

> find the buffer that actually holds the near world depth ... a run that dumps the
> *colour* buffer at that same instant (or **scans several FBOs' depth**) would
> name it.

This ADR does exactly that scan.

## Decision

Instrument the hook to name **every** framebuffer object the engine allocates, read
each one's depth at the depth-only clear, and log it. Deliberately **evidence
only**: the scan never rewrites the reprojection snapshot or the warp, so it cannot
regress the stereo pair (the depth sources and the `minSep` gate of ADR-0501/0502
are untouched).

### 1. Record the engine's framebuffer ids

- `opengl_proxy.{h,cpp}`: wrap `glGenFramebuffers`/`glGenFramebuffersEXT`
  (`Hook_glGenFramebuffers`) and record the generated names (bounded, 32). The
  wrapper is returned to the engine only when the real entry point resolved, so an
  engine allocation can never be swallowed; otherwise the call is forwarded
  untouched. `GetKnownFramebufferIds()` exposes the set. The engine's own scene
  FBO ids were previously invisible to the hook (only ids it *bound* passed
  through `Hook_glBindFramebuffer`).

### 2. Scan at the depth-only clear — the one proven readback point

- `stereo_renderer.cpp`'s `ScanFramebufferDepths()` binds each recorded id as
  `GL_READ_FRAMEBUFFER`, reads its depth (`glReadPixels(GL_DEPTH_COMPONENT,
  GL_FLOAT)`) over the window rect, and logs
  `DIBR fbo scan: fbo=.. bound=.. WxH status=0x.. rawDepth=[lo..hi]` plus a
  summary. The buffer is pre-filled with 1.0 (far) so a failed or partial read can
  only ever read "far", never false near geometry.
- It runs from `OnDepthOnlyClear()` (throttled by `kDibrFboScanEveryClears = 600`
  clears, latched off once a near buffer is seen) rather than at `SwapBuffers`.
  Reason: the depth-only clear is the *only* instant a readback is proven to
  return real depth — ADR-0502 measured this clear's own buffer at raw
  `0.905..1.000` — so it is the point where a scan's "all far" result is
  trustworthy, and the clear's own buffer is the scan's built-in **positive
  control**.
- `bound=` (the driver's `GL_READ_FRAMEBUFFER_BINDING` after the bind) is logged so
  "the reads returned far" can be distinguished from "the reads failed".
- Adopting a found buffer is left to a follow-up unit, so a stray near-depth buffer
  (e.g. a shadow map) cannot masquerade as a milestone here.

## Measurement (live, read-only per-run shadow, `RIDDICKVR_SIM=1`)

Command shape (unchanged from wake 100): `--shadow-install <install>
--reach-gameplay --duration 100 --advance-menu --advance-min-taps 8
--require-fresh-hook --require-parallax --downscale 4 --ini
StereoParallaxDibr=1`. Four attempts; two stalled in the launcher/menu (the known
live-run flake, 0 frames). `deployed_hook_matches_build` PASS in every attempt.

**Run D (in-level gameplay, 21,539/21,600 comfort frames, full-bleed fill 0.87-0.95,
17 scans, hook `sha256 97056c7473c1`):** every scan found the **same** picture —
`fbo=0` (window) `rawDepth=[1.000..1.000]`; `fbo=3,4,5,7,8,9,10`
`rawDepth=[1.000..1.000]`; **only `fbo=6`** had any depth, at raw
`[0.775..1.000]`/`[0.906..1.000]` (≈ 23-41 m at the measured world projection,
near 4.0 / far 2048.0, ADR-0488). `bound=` echoed each id, so the bindings and the
reads are real. **0 of 17 scans found a near buffer.** The paired reprojection line
stayed `DIBR: src=clear ... fbo=6 depth_z=[36.6..429.4]m fullDisp=[0.11..1.40]px
... use=0`, i.e. the ADR-0502 far-only pass.

**Run C (full-bleed reached but 0 frames captured):** the same result, plus the
positive control in one scan — `fbo=6 rawDepth=[0.812..1.000]` alongside the
clear's own capture `depth_z=[13.2..81.8]m` — proving the read returns real,
non-trivial depth for the one buffer that has a depth attachment.

## The falsification

ADR-0502's Next 1 is **not supported**: *no* framebuffer object the hook can read
holds the visible near world pass at the depth-only clear. FBO 6 is the only buffer
with any depth attachment and it is the already-known far-only pass (nearest
~23-41 m); the window reads uniformly far (consistent with ADR-0501). Combined with
ADR-0501 (window at `SwapBuffers`) and ADR-0502 (the clear's buffer), **every depth
source the hook can observe on this title is far-only**, so the interim depth-based
reprojection of GH #76 has no usable depth source here.

## Consequences

- GH #76's *interim* option (2) is now exhausted by measurement: its two named
  sources are closed (ADR-0501, ADR-0502) and the "find the buffer" route is closed
  (this ADR). The milestone (`--require-parallax`) is **not reached**, and it cannot
  be reached by reading the game's depth the way the hook can today.
- The remaining route is GH #76 option (1): a second engine view / per-eye
  projection. ADR-0501 measured the intended seam (`CameraHook::StabilizeViewMatrix`,
  ADR-0406) to be dead code and the engine's transform engine-side, so this needs a
  **separately scoped engine render seam**, not another depth readback.
- The scan instrument is landed, opt-in (`StereoParallaxDibr=0` stays the default),
  and evidence-only: it changes no rendering, so there is no regression surface.
- The `glGenFramebuffers` recorder also makes the engine's FBO set visible to future
  hook work generally.

## Next (honest)

1. **GH #76 needs a different scope**: per-eye rendering via an engine render seam
   (option 1), or accepting shift-stereo and dropping the depth hunt. Do not spend
   more wakes scanning for depth.
2. Move to the player-visible queue: GH #63/#64/#61 (hands), then the P1 bugs
   #45/#49/#50/#51/#65.
3. `StereoParallaxDibr=0` stays the default; the interim path stays as the
   instrument of record.

## Validation

- `tools/build-all.ps1 -Config Release` clean (x86 hook + x64 host); CTest
  **29/29 (x86)** and **27/27 (x64)**; all 12 `tools/test_*.py` exit 0;
  `xrsim --selftest`, `verify_reported_fixes`, `scenario_tests.py --check`,
  `verify_hook_init`, `validate_addresses`, `verify_test_artifacts` PASS. No new
  headless test was needed (the change is GL readback + a bounded id list; the warp
  math is unchanged and the scan does not feed the warp).
- Live: run D (above) is the gameplay evidence; run C carries the positive control.
  Artifacts under `%TEMP%\oc_wake101{,b,c,d}\` (report.json, hook logs). Install
  untouched (read-only per-run shadows; no deploy; no install write); no leftover
  game/host processes; live lock released.
