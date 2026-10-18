# ADR 0504: Native stereo (epic #57 item 1) — option 1 opens: the engine's eye-space geometry makes a per-eye replay a pure modelview translation (GH #76)

- Status: Accepted (scheduled autoloop wake 102, GH #76)
- Date: 2026-10-18
- Related: GH #76 (this), GH #77 / ADR-0463 (the eye-view provenance and the
  synthetic placeholder baseline the hook receives), GH #57 (epic /
  definition-of-done item 1), ADR-0501/0502/0503 (the interim depth reprojection
  and the measured absence of any hook-readable near-world depth), ADR-0481/0482
  (the engine's transform is applied engine-side with an identity `GL_MODELVIEW`),
  ADR-0461 (the shift-only stereo classification), `docs/rea/reference/RndrGL_GL_Path.txt`
  (the draw vocabulary: `glDrawElements` + `glBegin`, no `glDrawArrays`, no
  `glMultMatrixf`)

## Context

GH #76 is epic #57's definition-of-done item 1 and the port's top gap: the
renderer captures **one** backbuffer and blits it to both eyes with a single
global half-IPD source shift, so every gameplay frame is `stereo_kind:
shift-only` with zero depth-dependent parallax (ADR-0461).

Wake 99-101 exhausted the issue's **interim** option (2): the depth-based
reprojection's named depth source is absent at swap time (ADR-0501), the
depth-only clear's own buffer is a far-only pass (ADR-0502), and no framebuffer
object the hook can read holds the near world pass (ADR-0503). ADR-0503's Next 1
said plainly: option (2) is done by measurement; the remaining route is option
(1), which "needs a separately scoped engine render seam". This ADR scopes it.

The old reason option (1) looked blocked was ADR-0481/0482: the engine applies
its object/view transform **engine-side**, loads an **identity** `GL_MODELVIEW`
once per run, never pushes/translates it, and calls neither
`glMultMatrixf`/`glRotatef` nor an ARB program path. But that same measurement is
what makes a replay possible, and this ADR states the mechanism explicitly:

- RndrGL submits geometry as **client-array `glDrawElements`** (no `glDrawArrays`
  is bound by any shipped module; `docs/rea/reference/RndrGL_GL_Path.txt`), with
  the modelview identity, so the submitted vertices are already in **eye space**.
- The camera transform is therefore a *rigid translation of every eye-space
  vertex*. Moving the eye from the head origin to the left/right eye is exactly a
  `glTranslatef` on the modelview at replay time — mathematically identical to a
  real per-eye camera (a uniform translation of all eye-space points is a camera
  translation, independent of depth).

So option (1) does **not** need the engine to render twice or expose a view
matrix. It needs the hook to *record the engine's eye-space draws and replay them
once per eye*, and the per-eye camera offset is a translation the hook can compute
from the head pose.

## Decision

Open the option-1 route and land its first, tested primitive — the per-eye replay
maths and the live probe that measures the assumption it rests on. Deliberately
**no renderer change**: the shift-stereo path is untouched, so there is no
regression surface. This is the foundation the (separately scoped) draw-recorder +
per-eye replay will consume.

### 1. `src/hook/stereo_replay.h` — the pure, headless replay maths

- `ReplayViewFromPose(pos, quat, out)` — `world -> eye`, identical to the host's
  `eye_frame.h::ViewMatrixFromPose` (the unit test cross-checks the two).
- `ReplayEyePositionFromView(view, out)` — the eye's world origin from a view
  matrix.
- `EyeSpaceCameraOffset(headPos, headQuat, eyePos, out)` — the camera offset
  `d_eye = R_head^T (eyePos - headPos)`; the replay's `GL_MODELVIEW` is
  `translate(-d_eye)`.
- `EyeSpaceCameraOffsetFromViews(viewL, viewR, headPos, headQuat, rightEye, h,
  out, usedViews)` — prefers the host's per-eye views but **only when they are
  scene-faithful** (rotation matches the head pose and the recovered eye origin is
  within `kReplayMaxEyeOffsetMeters = 0.25 m`); otherwise falls back to
  `+/-halfIpd` along the head's local +X. `usedViews` reports which source was
  used.
- `BuildEyeReplayModelview(offset, out)` and `ReplayViewResidual(headView,
  eyeView, offset)` — the replay transform and an independent residual
  (`translate(-offset) * headView` vs `eyeView`).

The scene-faithfulness gate is not cosmetic: **the hook does not receive the
host's scene-faithful per-eye views** (GH #77 / ADR-0463). In mock/sim the host
publishes the *synthetic placeholder baseline* (identity rotation, origin at the
world origin); the `eye=sim` views go only to `eye_frames.log`. Without the gate,
the recovered offset is nonsense (`~ -headPos`, measured live — see below).

### 2. `StereoReplayProbe=1` / `StereoReplayProbeEvery` — the live probe

`stereo_renderer.cpp`'s `OnSwapBuffers` logs, every N swaps, one
`StereoReplay: swap=.. baseline=.. srcL=.. srcR=.. offL=(..) offR=(..)
residualL=.. residualR=..` line: the IPD implied by the live eye-view
translations, which source the offset came from, the eye-space offset the replay
would apply, and the residual against the published views. Default OFF, opt-in,
evidence only.

## Measurement (live, read-only per-run shadow, `RIDDICKVR_SIM=1`)

Command shape: `--shadow-install "F:\...Assault on Dark Athena" --reach-gameplay
--duration 70 --advance-menu --advance-min-taps 8 --require-fresh-hook
--downscale 4 --ini StereoReplayProbe=1`. `deployed_hook_matches_build` PASS
(hook `sha256 7e95f8c9a98e`, `514048 B` = the repo build). The graded run did not
reach full-bleed gameplay (2 captures; the known launcher/full-bleed flake) — the
probe fires on every live frame, so the measurement below does not depend on it.

Four probe lines (swaps 300/600/900/1200), all identical in shape:

```
StereoReplay: swap=300 baseline=0.0650m srcL=fallback srcR=fallback \
  offL=(-0.0325,0.0000,0.0000) offR=(0.0325,0.0000,0.0000) \
  residualL=1.685053 residualR=1.685053
```

- **baseline = 0.0650 m** — the IPD is recovered correctly from the published
  views' translations.
- **`srcL=srcR=fallback`** — the published per-eye views are **not scene-faithful**:
  the helper correctly refuses them. This is the hook-side confirmation of GH
  #77 / ADR-0463's documented fact (the hook receives the synthetic placeholder,
  not the sim-reconstructed views).
- **`offL=(-0.0325,0,0)`, `offR=(+0.0325,0,0)`** — the replay offset is exactly
  `+/-half-IPD` along the head's local X, i.e. a pure eye-space translation (no
  rotation), which is what the mechanism predicts.
- **`residual ~ 1.68`** — the placeholder eye view differs from
  `translate(-offset) * headView` by ~`-headPos.y`: the placeholder's origin is
  the **world origin**, not the head. The residual names the placeholder cleanly.

Offline, `tests/stereo_replay_tests.cpp` proves the mechanism the other way:
against the host's own `ComputeEyeViewFromHeadPose` views (scene-faithful), the
recovered offset is exactly `+/-halfIpd` on eye-space X and
`translate(-offset) * headView == eyeView` to `< 1e-5`, across head yaws of 0,
35, 90 and -140 deg; a placeholder pair is rejected with a large residual.

## Consequences

- **Option (1) is now scoped with a concrete, verified mechanism** rather than
  "blocked": the engine's geometry is eye-space client arrays (`glDrawElements`,
  identity modelview), so a per-eye replay is a modelview translation — recorded
  in a new issue ("GH #76 option 1: per-eye replay of the engine's eye-space
  draws"). The remaining engineering is a **draw recorder + replay** (record the
  frame's `glDrawElements` calls with their client-array state and replay them per
  eye); that is a separate, larger unit and carries the real risk, so it is not
  attempted here.
- The replay offset must be taken from the **head pose + IPD**, not from the
  views the hook receives in mock/sim (they are the placeholder). A real headset
  (`eye=runtime`) would make `usedViews` true; the gate makes the helper correct
  in both cases.
- `StereoParallaxDibr=0` and `StereoReplayProbe=0` stay the defaults; no rendering
  behaviour changed, so the existing shift-stereo and its gates are unaffected.
- The probe's `residual` is a reusable instrument: a future run with
  `StereoReplayProbe=1` on a real headset would show `src=views` and
  `residual ~ 0`, confirming the runtime's eyes are head-aligned.

## Next (honest)

1. **GH #76 option 1, step 2 (the real unit):** record the frame's eye-space
   `glDrawElements` draws (mode/count/type/indices + the client vertex/normal/
   color/texcoord pointers) and replay them per eye into the eye FBOs with the
   `BuildEyeReplayModelview` translation and the host's per-eye projections; keep
   it off by default and gate it. Decide how to handle the HUD/immediate-mode 2-D
   batch (which must **not** be replayed with an eye offset).
2. Re-run `--require-parallax` after step 2 and require a non-zero per-tile
   disparity spread; do not close GH #76 on the shift-stereo.
3. `StereoParallaxDibr=0` / `StereoReplayProbe=0` stay the defaults.
4. The player-visible queue GH #63/#64/#61 and the P1 bugs remain code-complete
   but headset/deploy-blocked.

## Validation

- `tools/build-all.ps1 -Config Release` clean (x86 hook `514048 B`, x64 host);
  CTest **30/30 (x86)** and **28/28 (x64)** (+1 each: `stereo_replay`); all 12
  `tools/test_*.py` exit 0; `xrsim --selftest`, `verify_reported_fixes`,
  `scenario_tests.py --check`, `verify_hook_init`, `validate_addresses`,
  `verify_test_artifacts` PASS.
- Live: one read-only per-run shadow run (`%TEMP%\oc_wake102b\`,
  `%TEMP%\riddickvr-shadow-10900\`), `deployed_hook_matches_build` PASS; the probe
  lines above are the evidence. Install untouched (read-only shadow; no deploy, no
  install write); no leftover game/host processes; live lock released.
