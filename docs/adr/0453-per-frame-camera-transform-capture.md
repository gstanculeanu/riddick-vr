# ADR-0453: Per-frame camera transform record for XRSIM captures

- **Status:** Accepted (landed; live-validated host-side, not headset-validated)
- **Issues:** GH #68 (requirement 1, second half), prerequisite for GH #67 step 3
- **Date:** 2026-10-16

## Context

The XRSIM harness (#68) is the standing loop's iteration vehicle: it runs the
mock OpenXR host + game with scripted HMD/controller poses and dumps both eye
images so player-visible fixes can be reproduced and validated without a
headset.

By wake 47 it already:

- dumped **both eyes** (`test_frame_####.bmp` / `right_####.bmp`),
- labelled each frame's **display branch** (`display_branch.log`, ADR-0445),
- gated the dump window on live gameplay (#69) and **content** (#72),
- captured the engine's own **`GL_MODELVIEW`** at the first in-window draw
  (ADR-0450/0451) so the first-person viewmodel pass could be identified.

GH #68 requirement 1 asks the host to dump both eyes **and record the eye
pose/projection used for each frame**. The first half was done; the second was
not. Nothing in a capture said *which camera transform* produced it.

That gap matters concretely for **GH #67 step 3** ("re-express the engine's
first-person viewmodel in head/controller space"): a captured modelview is only
meaningful relative to the camera/eye transform the frame was rendered with.
Without it the capture cannot be interpreted in 3D, and step 3 has no input even
once a headset capture arrives.

## Decision

In mock mode, write one machine-readable line per **dumped** frame to
`<dump-dir>\eye_frames.log`, recording the HMD pose and the per-eye view and
projection matrices the host published for that frame:

```
# frame=NNNN hmdpos=x,y,z hmdquat=x,y,z,w \
#   viewL=[m0 .. m15] viewR=[m0 .. m15] projL=[m0 .. m15] projR=[m0 .. m15]
frame=300 hmdpos=0.0000,1.7000,-0.2500 hmdquat=... viewL=[...] viewR=[...] projL=[...] projR=[...]
```

- The file uses the same **truncate-on-first-dump / append-after** discipline as
  `display_branch.log`, so a run never mixes with the previous one.
- The wire format is a **pure, dependency-free helper** (`src/host/eye_frame.h`,
  `FormatEyeFrameLine`) so the shape is pinned by CTest
  (`tests/eye_frame_tests.cpp`, `eye_frame_record`) without a GPU.
- `tools/xrsim.py` parses it back (`read_eye_frames` / `eye_frame_stats`),
  reports it as the `eye_frames` key (`count`, `distinct`, `yaw_span_deg`), and
  adds a `capture_metadata_present` check. The check **skips** (never fails)
  when the file is absent, so legacy captures and pre-#68 hosts keep working.
- Matrices are column-major (the protocol/OpenGL layout) and space-separated
  inside `[...]`; the pose is comma-separated.

## Consequences

- A capture is now self-describing in 3D. Any harness consumer (and GH #67 step
  3) can recover the exact head/eye transform for a dumped frame.
- The `yaw_span_deg` summary is a cheap, concrete signal that the graded window
  was not a frozen head pose.
- **Honest limit:** this is host-side and therefore *live-validatable with the
  freshly built `dist\riddickvr-host.exe`* (no hook deploy needed); it records
  the *host-published* transform, which is the transform the hook injects. The
  hook-side modelview capture and the step-3 re-expression it enables still need
  a deployed (headset) run — this ADR only closes the metadata half of #68 req 1.

## Alternatives considered

- **Fold the pose into `display_branch.log`.** Rejected: that file is the branch
  label contract (ADR-0445); mixing a large matrix payload into it muddies a
  stable, widely-parsed format.
- **Log the pose to the host stdout only.** Rejected: the report and
  `--evaluate-only` re-analysis read artifacts from the dump directory;
  stdout is persisted separately and is the wrong home for per-frame matrix
  data.
