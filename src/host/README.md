# 64-bit OpenXR Host Component (`RiddickVRHost.exe`)

## Responsibility
The Host component is an independent 64-bit (x64) application responsible for all direct communication with the VR runtime.

## Key Duties
- Initialize the OpenXR instance and session with the active system runtime (SteamVR, Meta Link, Virtual Desktop).
- Create OpenXR swapchains matching the recommended per-eye render resolutions.
- Poll OpenXR Action Sets for motion controller tracking (poses, buttons, thumbsticks, triggers).
- Maintain frame pacing (`xrWaitFrame`, `xrBeginFrame`, `xrEndFrame`).
- Receive shared GPU textures from the 32-bit game hook and submit them to the OpenXR swapchain.
- In mock mode (`--mock`), label every dumped stereo capture with the display
  branch the hook published for that frame (`cinema` vs `gameplay`) in
  `<dump-dir>\display_branch.log`, so the XRSIM harness (#68/#69) can attribute a
  byte-identical eye pair instead of guessing (ADR-0445).
- In mock mode, `--dump-gameplay-only` gates the dump window on that branch:
  frames are captured only once the hook reports 3D gameplay, so a scripted run
  yields a live-gameplay capture instead of the menu/cinema and all-zero
  interop warm-up frames whose identical eyes are correct but cannot validate
  stereo (GH #69 requirement 1, ADR-0447). The gate decision is the pure helper
  `ShouldDumpFrame` (`src/host/dump_gate.h`), unit-tested by
  `tests/dump_gate_tests.cpp`. `tools/xrsim.py` opts scripted runs in by default.
- In mock mode, the gameplay-only window is also **content-gated** (GH #72
  requirement 2): a gameplay-labelled frame is skipped while it is all-black
  (the GL<->D3D11 interop warm-up, mean luma <= 1) or byte-identical to the last
  dumped frame (a frozen static 2D page). **Both** eyes are sampled, so a frame
  left with an empty eye against a lit one -- which would read as a spurious
  stereo failure -- is skipped too (`EyePairIsBlank`, `src/host/dump_gate.h`,
  unit-tested by `tests/dump_gate_tests.cpp`). Each skip is logged as
  `DumpGate skip frame=N reason=blank|frozen`, parsed by the harness into the
  report's `content_gate` key. This keeps the graded window live and advancing.
- In mock mode, every dumped frame also records the **camera transform** it was
  rendered with -- the HMD pose plus the per-eye view and projection matrices --
  in `<dump-dir>\eye_frames.log` (one line per frame; GH #68 requirement 1,
  format pinned by the pure helper `src/host/eye_frame.h`). This makes a capture
  self-describing in 3D: the harness reports it as `eye_frames` and the
  `capture_metadata_present` check confirms it, and it is the camera transform
  GH #67 step 3 needs to re-express the engine's first-person viewmodel in
  head/controller space. Each line ends with a provenance token (GH #77,
  ADR-0462/0463): `eye=runtime` (a real `xrLocateViews` call), `eye=sim` (the
  mock host reconstructed the per-eye views a runtime would return for the
  frame's scripted head pose via `ComputeEyeViewFromHeadPose`, because it has no
  runtime to ask) or `eye=synthetic` (the constant placeholder the hook actually
  consumes in mock mode). `tools/xrsim.py` measures the view/head rotation
  deviation and the left/right eye separation, grades `eye_views_scene_faithful`
  for `runtime`/`sim`, and skips a `synthetic`/legacy record -- so a sim run now
  exercises the pass branch instead of always skipping.
- In mock mode, the host prints a parseable **frame-time summary** every 30
  frames (`[Mock Host] FrameBudget samples=.. mean_ms=.. max_ms=..
  over_budget=.. budget_ms=.. p50_ms=.. p95_ms=..`; GH #68 requirement 2). The
  percentile maths and the wire line live in the pure helper
  `src/host/frame_time_stats.h` (unit-tested by `tests/frame_time_tests.cpp`),
  and `tools/xrsim.py` **asserts the budget on p50/p95** while still reporting
  `max_ms` and the drop count -- so a one-off level-load hitch is visible but
  does not read as a sustained 90 Hz regression (ADR-0455). The sample is the
  **game/hook's rendered-frame cadence**: the host subtracts its own artifact
  capture (two 26.8 MB BMPs + sidecars, printed per dump as
  `[Mock Host] ArtifactCapture frame=.. capture_ms=..`) from the loop delta via
  `FrameTimeExcludingHostWork`, so the harness cannot grade itself (GH #78,
  ADR-0464).
- In mock mode, a capture reads each shared eye texture **once**: the host
  stages both eyes back-to-back into private staging textures (`StageEyePair`)
  and runs the content gate (`SampleEyeImage`, `src/host/eye_sample.h`) and the
  BMP write (`WriteBmpFromMapped`, layout in `src/host/bmp_writer.h`) over that
  same copy. The hook blits both eyes on every swap and never calls
  `WaitForFrameConsumed`, so the old "sample then re-read to save" path could
  grade one frame and save another (measured: frame 540 had a lit probe and an
  all-black saved right eye; frame 1770 had an intra-image tear). A frame the
  branch wanted but that could not be staged is logged as `Capture skipped
  frame=N (eye staging failed)` rather than written from a second read. Pure
  helpers unit-tested by `tests/eye_capture_tests.cpp` (GH #79, ADR-0465); the
  hook-side wait is deploy-gated.
- In mock mode, the host also reads the hook-published locomotion **comfort
  level** (`comfortVignette`, Track C #9) back every frame and prints a running
  summary every 30 frames (`[Mock Host] ComfortVignette samples=.. max=.. mean=..
  above=.. locomotion=.. max_locomotion=.. threshold=..`; GH #68 requirement 5).
  The accumulate/format logic is the pure helper
  `src/host/comfort_vignette.h` (unit-tested by
  `tests/comfort_vignette_tests.cpp`), and `tools/xrsim.py` asserts
  `comfort_vignette_applied` -- the vignette must rise while the run publishes
  move/turn input -- so a regression that disables the comfort feature is caught
  without a headset (ADR-0456).
