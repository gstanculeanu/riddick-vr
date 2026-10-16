# ADR 0445: label every XRSIM capture with its display branch (cinema vs gameplay)

## Status
**Accepted** — the mock host now labels each dumped capture with the display
branch the hook published for that frame (`cinema` vs `gameplay`) and persists
that label beside the frames; `tools/xrsim.py` consumes the label so a
byte-identical left/right pair is **attributed** instead of guessed, and a run
that never reached 3D gameplay cannot pass the stereo gates. The host also
persists its stdout so frame-budget lines survive the run. No game file is
modified and no binary is deployed.

## Context
The XRSIM harness (#68) dumps both eyes and diffs them (`tools/xrsim.py`), but
its first real run could not be interpreted (#69): all 13 pairs were
**byte-identical** and only 3 distinct images appeared. The stereo gates said
"FAIL", yet the hook log for that run was healthy and the run had entered
`DisplayMode: CINEMA screen`.

That is expected behaviour, not a defect, on the *cinema* branch:
`src/hook/stereo_renderer.cpp` deliberately blits **both eyes with the same
source rect** on a 2D menu/cinematic frame ("flat screen: no IPD shift"), while
the *gameplay* branch applies a half-IPD source shift that must make the eyes
differ. Nothing recorded which branch produced each dumped frame, so an
identical-eye capture could not be distinguished from a genuine "no stereo in
gameplay" bug. The harness was therefore not yet a trustworthy regression gate.

## Decision
1. **Host labels the capture.** In the mock loop
   (`src/host/openxr_manager.cpp`), each frame's shared state is read once and
   its `cinemaMode` is captured. The capture line becomes
   `[Mock Host] Captured stereo frame NNNN (L=.. R=..) cinema=<0|1> -> ...`, and
   the host appends one machine-readable line per dumped frame to
   `<dump-dir>\display_branch.log`:
   `frame=NNNN cinema=<0|1> left=<ok|err> right=<ok|err>`.
2. **Host log survives the run.** `tools/xrsim.py` copies the mock host's stdout
   to `<artifacts>\host.log` after the run (and clears stale label/log files
   before it), so the frame-budget and capture lines are re-analysable later.
3. **Harness attributes per frame.** `read_display_branch_labels()` parses the
   label file; each stereo check is tagged `branch=cinema|gameplay|unknown`.
   On a **cinema** frame the two gameplay-only gates
   (`horizontal_disparity_present`, `not_mono_duplicate`) are reported as
   `skip` (identical eyes are correct there); `both_eyes_non_black` /
   `interop_produced_content` still apply. A frame absent from the label file is
   `unknown`, never assumed to be gameplay.
4. **A menu-only run cannot pass.** A new `gameplay_captures_present` check fails
   when the label file exists and **no** frame is gameplay. Without a label file
   (legacy captures, or a pre-label host) it is `skip`, so old evidence is not
   retro-graded. `--evaluate-only` reads the persisted `host.log` when present.
5. The report records `host_log` and the per-frame `display_branch` map so the
   cinema-vs-gameplay verdict is diffable.

## Evidence (2026-10-16, live mock run; build `dist/riddickvr-host.exe` 18:21)
- `python tools/xrsim.py --scenario tools/scenarios/melee.json --timeout 150`
  (live, `RIDDICKVR_SIM=1`) reached 3D gameplay and produced:
  `gameplay_captures_present` **PASS — 8 gameplay / 5 cinema / 0 unknown of 13**.
  The 8 gameplay frames all registered `dx=-14 px, dy=0, identical=False`
  (`horizontal_disparity_present` + `not_mono_duplicate` PASS); the 5 cinema
  frames (1/15/30/45/60) had their two gameplay-only gates correctly skipped.
- `<artifacts>\display_branch.log` shows the branch transition live
  (`frame=1..60 cinema=1`, `frame=75..180 cinema=0`), matching the hook's
  "transitioned from Menu Letterbox to Full 3D VR Immersion".
- `<artifacts>\host.log` was persisted (frame-budget line
  `samples=180 mean_ms=17.42 max_ms=899.54 over=30 budget=11.11`).
- Remaining live failures are now *attributed*, not guessed: frames 1/15/30 are
  all-zero eye buffers (`interop_produced_content` — interop warmup) and the
  melee scenario markers do not fire under the mock. Both stay tracked in #69.
- `python tools/test_xrsim.py` **21/21 PASS** (5 new: label parsing; cinema
  identical eyes skipped, not failed; gameplay identical eyes still fail; the
  gameplay-presence gate; `--evaluate-only` reading a persisted host log);
  `tools/xrsim.py --selftest` PASS; `build-all.ps1 -Config Release` OK (x86
  `opengl32.dll` + x64 host); CTest **9/9 PASS** x86 **and** x64;
  `verify_reported_fixes.py` (new GH #69 invariant) and the other verifiers PASS.

## Explicitly not claimed
- **Not a fix for the warmup gap.** The early all-zero eye buffers and the mock
  marker gaps remain open in #69; this ADR only makes them *labelable*.
- **`cinemaMode` is the renderer's published branch**, i.e. the same bit the host
  uses to choose the quad vs projection layer. It is not an independent ground
  truth about what the engine drew.
- **Not validated in-headset.** The label path is exercised with the mock host;
  a real OpenXR runtime was not used.

## Reproduce
```
python tools/test_xrsim.py
python tools/xrsim.py --selftest
python tools/xrsim.py --scenario tools/scenarios/melee.json --timeout 150 \
    --artifacts <scratch> --report-file <scratch>\report.json
python tools/xrsim.py --evaluate-only --downscale 4   # re-analyse committed captures
```

## Files
- `src/host/openxr_manager.cpp` (per-frame cinema label + `display_branch.log`)
- `tools/xrsim.py` (`read_display_branch_labels`, `persist_host_log`,
  per-frame `branch`, `gameplay_captures_present`, host-log reuse)
- `tools/test_xrsim.py` (5 new tests), `tools/verify_reported_fixes.py` (#69)
- `tests/xrsim/report.json` (regenerated baseline)
- cross-refs: ADR-0389/0399/0405/0409 (cinema mode), #68/#69, #67/#64/#61
