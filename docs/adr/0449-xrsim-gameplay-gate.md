# ADR 0449: XRSIM judges only gameplay frames and starts scenarios on gameplay

## Status
**Accepted** — the XRSIM mock host and `tools/xrsim.py` (GH #68) now separate
cinema/menu frames from gameplay frames. No game file is modified.

## Context
The first XRSIM runs reported "left/right eyes identical" and no frame-budget
samples. Both were harness artifacts, not mod bugs:

- Black frames produce a fake best shift (`dx=-48`, the first search value), and
  content frames in cinema or menu mode are mono by design
  (`stereo_renderer.cpp`, cinema branch).
- The scenario timeline started at host frame 0, so punches and guards played
  during loading, before the game accepted any input.
- The frame budget mixed menu pacing (about 58 ms per frame) with gameplay.

## Decision
1. The host starts the scenario timeline only after 30 consecutive frames with
   `cinemaMode == false` and prints `Gameplay detected at frame N; scenario
   timeline starts`. Until then the first keyframe is held.
   `--scenario-immediate` restores the frame-0 start.
2. The host samples the frame budget on gameplay frames only and tags the line
   `gameplay_only=1` (appended, so the old regex still matches).
3. Each captured frame is labelled `mode=gameplay|cinema` on the
   `Captured stereo frame` line, and a `frame_NNNN.json` sidecar records the HMD
   pose and eye matrices.
4. `xrsim.py` checks stereo only on gameplay frames whose left-eye luminance
   std-dev is above 2.0. Other gameplay frames are `skipped: no content`;
   cinema frames record an info entry. A run with no gameplay frame fails
   `gameplay_reached`. The budget needs at least 30 gameplay samples.
5. Scenario files may use `markers` (all), `markers_any` (at least one),
   `forbidden_markers` (none) and `marker_order` (in order).
   `scenario_tests.py` checks `markers` and `markers_any` against the hook
   source; `forbidden_markers` are not checked because logged lines can come
   from format strings.
6. numpy and Pillow are imported lazily. `--selftest` always runs the logic
   cases; image cases print `SKIP (numpy/Pillow missing)` unless
   `--require-image-deps` makes a skip a failure.
7. New scenarios cover the boxer guard (`guard_block`, `guard_raise`) and punches
   (`punch_swing`, `punch_turned` with the head yawed 90 degrees).

## Consequences
- A run that never leaves the menus now fails loudly instead of reporting a fake
  stereo bug.
- `RIDDICKVR_SIM=1` runs need a game that reaches gameplay; the scenario then
  runs on real input state.
- Real runs still happen on Windows only. CI cannot run the game, so the
  workflows are unchanged.
