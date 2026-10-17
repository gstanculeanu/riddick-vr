# ADR 0463: Record a simulated, scene-faithful per-eye view in mock mode

## Status
**Proposed** — GH #77 follow-up (epic #57 DoD *"all claims backed by captured evidence"*).
Host + harness + docs; nothing deployed, no game file touched.

## Context
ADR-0462 tagged the `eye_frames.log` provenance and made XRSIM **skip** the
scene-faithfulness check on a mock/sim capture (`eye=synthetic`): in mock mode
the host has no runtime to ask, so the record held a constant placeholder
(identity rotation, `±half-IPD`, identical projections) with **no head rotation**.
That was honest, but it left the graded branch of `eye_views_scene_faithful`
reachable only on a real headset — so the sim could never prove the check itself,
and the record could not be used as 3D evidence by a headless run.

## Decision
In mock mode the host **reconstructs** the per-eye views a runtime *would* return
for the frame's scripted head pose:

- Pure, dependency-free helpers in `src/host/eye_frame.h`:
  `QuatToRowMajor3x3`, `ViewMatrixFromPose` (the same rigid inverse the host's
  `CreateViewFromPose` uses; `CreateViewFromPose` now forwards to it) and
  `ComputeEyeViewFromHeadPose(headPos, headQuat, halfIpd, rightEye, out)` —
  `eye = head + R_head * (±halfIpd, 0, 0)`, both eyes sharing the head
  orientation.
- `EyeFrameSample.viewsFromRuntime` (bool) becomes
  `viewSource ∈ {Runtime, Simulated, Synthetic}`; `FormatEyeFrameLine` emits
  `eye=runtime|sim|synthetic`.
- The mock `RunLoop` dump writes `eye=sim` and reconstructs `viewL/viewR` from
  the frame's own `state.hmdPose`; `frame_####.json`'s `leftEyeView`/`rightEyeView`
  and `"eyeViews"` follow. **The published shared-state eye views are not
  changed** — the hook still consumes the placeholder baseline, so nothing about
  the sim's rendering or the procedural-hand overlay moves.
- `tools/xrsim.py`: `EYE_SCENE_FAITHFUL_SOURCES = ("runtime", "sim")`; the
  `eye_frames` key gains `view_lr_ipd_m` (mean left/right view-translation
  separation), and `eye_views_scene_faithful` grades a `runtime`/`sim` capture on
  **both** the view/head rotation deviation (`≤ 1°`) and the eye separation
  (`0.03–0.10 m`), so a broken reconstruction (missing offset, wrong scale/sign)
  fails instead of passing vacuously. `synthetic`/legacy still **skips**.

## Consequences
- A sim run now exercises the graded **pass** branch end-to-end (parse → stats →
  tolerance → report key), which was previously unreachable offline.
- Honest limit: `eye=sim` is a *reconstruction*, not the matrices the hook
  consumed — the record's provenance says so. Passing the check therefore proves
  the harness path and the reconstruction's structure (rotation + IPD), **not**
  that the sim rendered a head-relative stereo pair (that is GH #76's hook work).
  The authoritative camera input remains the recorded head pose
  (`hmdpos`/`hmdquat`).
- The `synthetic` value is now produced only by the real-session
  no-`xrLocateViews` fallback; `eye=runtime` still requires a headset. Both
  remain parseable/skimmable, so no consumer breaks.

## Two numerical defects this exposed (fixed here)
Building the sim path was the first time the check ran against host-convention
views, and it caught two latent bugs in ADR-0462's implementation:

1. **Transpose convention.** The host's view stores the *world → eye* rotation
   `R_head^T`, so the deviation must be `R_view * R_head` (identity iff
   `R_view == R_head^T`). ADR-0462 computed `R_view * R_head^T`, i.e. `R_head^-2`,
   which only read ~0 because the unit test's fixture stored `R_head` (the
   transposed, non-host convention). Now fixed, with a test pinning the correct
   convention and a fixture that stores `R_head^T` like the host.
2. **`acos` ill-conditioning.** `angle = acos((trace-1)/2)` has unbounded
   derivative near identity, so the wire format's 4-decimal rounding (~1e-5
   matrix error) read as **0.436 deg**. Replaced with the well-conditioned
   `atan2(‖skew‖/2, (trace-1)/2)`, which reads **0.009 deg** live — so the graded
   sim check cannot flake near the 1 deg tolerance.

Live evidence (mock host + game, `RIDDICKVR_SIM=1`, gameplay-only): 27 gameplay
frames, `eye views=sim`, `view_head_rotation_deviation_deg = 0.009`,
`view_lr_ipd_m = 0.065`, `eye_views_scene_faithful` **PASS**; the run's only FAIL
is the known sim-install `frame_budget` (~10 FPS).

See also: GH #77 (the defect), ADR-0462 (provenance tagging; amended), GH #68
requirement 1 (`eye_frames.log`), GH #67 step 3 (the consumer), GH #76 (per-eye
rendering, still hook-side).
