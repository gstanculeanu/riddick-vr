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
  dumped frame (a frozen static 2D page). Each skip is logged as
  `DumpGate skip frame=N reason=blank|frozen`, parsed by the harness into the
  report's `content_gate` key. This keeps the graded window live and advancing.
- In mock mode, every dumped frame also records the **camera transform** it was
  rendered with -- the HMD pose plus the per-eye view and projection matrices --
  in `<dump-dir>\eye_frames.log` (one line per frame; GH #68 requirement 1,
  format pinned by the pure helper `src/host/eye_frame.h`). This makes a capture
  self-describing in 3D: the harness reports it as `eye_frames` and the
  `capture_metadata_present` check confirms it, and it is the camera transform
  GH #67 step 3 needs to re-express the engine's first-person viewmodel in
  head/controller space.
