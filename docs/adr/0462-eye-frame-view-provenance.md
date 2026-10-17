# ADR 0462: The `eye_frames.log` eye views are a synthetic placeholder in mock mode; tag their provenance

## Status
**Accepted** — GH #77 (filed by this wake; epic #57 DoD *"all claims backed by captured evidence"*).
Host + harness + docs; nothing deployed, no game file touched.

## Context
GH #68 requirement 1 added `eye_frames.log`: the host writes one line per dumped
frame with the head pose and the per-eye view/projection matrices so a capture is
"self-describing in 3D" and GH #67 step 3 can make the captured `GL_MODELVIEW`
camera-relative. The header (`src/host/eye_frame.h:3-13`) calls these matrices
"the camera transform the hook injected".

That is **not true in mock/sim mode**:

- Mock mode never establishes an OpenXR session — `Initialize()` returns early
  (`src/host/openxr_manager.cpp:623-628`), so `UpdateTracking`'s real branch
  (`:823`) and its `xrLocateViews` call (`:1007`) never run.
- The mock `RunLoop` fills the eye views/projections with a **constant synthetic
  baseline** (`:1679-1697`): identity rotation, `m[12] = -/+ kHalfIpdMeters`
  (65 mm), and `rightEyeProjection = leftEyeProjection` byte-identical. (The same
  placeholder is duplicated in the tracking fallback at `:1030-1055`.)
- Those exact matrices are written verbatim to `eye_frames.log` (`:1993-1996`) and
  `frame_####.json` (`:2028-2030`).
- Observed `tests/xrsim/artifacts/eye_frames.log`: on every line `viewL`/`viewR`
  rotation is identity, `m[12] = -0.0325 / +0.0325`, `projL == projR`, while
  `hmdquat` changes with the scripted keyframe. The recorded "eye view" is
  therefore independent of the head — no head rotation at all.

## Decision
1. **Tag provenance.** `EyeFrameSample` gains `viewsFromRuntime`; the host sets it
   `true` only inside the successful `xrLocateViews` branch (`:1013-1017`) and
   `false` in the fallback (`:1030`), and writes an `eye=runtime|synthetic` token
   on every `eye_frames.log` line (`src/host/eye_frame.h: FormatEyeFrameLine`) and
   an `"eyeViews"` field in `frame_####.json`. The token is optional on read, so a
   legacy capture still parses (`source = "unknown"`).
2. **Measure, don't assume.** `tools/xrsim.py` computes, per frame, the angular
   distance between the recorded `viewL` rotation and the head pose's rotation
   (`_view_head_rotation_deviation_deg`) — ~0 for a faithful runtime view (both
   eyes share the head orientation; the host builds the view from the eye pose),
   and the head's own rotation for the identity-rotation synthetic baseline. The
   `eye_frames` report key gains `source`, `view_head_rotation_deviation_deg` and
   `proj_lr_identical`.
3. **Grade real, skip mock.** A new `eye_views_scene_faithful` check **passes** a
   `runtime` capture whose views track the head pose (tol `1°`, the wire format's
   4-decimal rounding) and **fails** a `runtime`-labelled capture that does not.
   For `synthetic`/`unknown` it is **skipped** with the measured deviation as the
   detail — so a mock run neither silently passes nor is failed, and the report
   itself says the matrices are a placeholder.

## Consequences
- No behaviour change to what is rendered or to the shared state the hook reads;
  only the *record* gains provenance. The eye views still carry no head rotation
  in mock mode — that is now stated, not implied.
- GH #67 step 3 must use the recorded **head pose** (`hmdpos`/`hmdquat`, which the
  hook applies), not the `eye=synthetic` matrices. GH #76's per-eye work likewise
  cannot take the mock eye views as real.
- Honest limit: the `eye=runtime` path is only exercised on a real headset, so the
  graded branch is a tripwire that a sim run cannot itself prove; the sim run
  proves the *skip* and the measured deviation (>20° for a scripted head).

See also: GH #77 (the defect), GH #68 requirement 1 (`eye_frames.log`), GH #67
step 3 (the consumer), ADR-0461 (the stereo measurement that also cannot trust a
shifted monoscopic capture), ADR-0460 (capture artifacts stay untracked).
