# ADR 0447: XRSIM gates its capture window on the display branch (gameplay-only dumps)

## Status
**Accepted** — the mock host can now capture only frames the hook reports as 3D
gameplay (`--dump-gameplay-only`), and the XRSIM harness opts scripted runs in by
default. This is GH #69 requirement 1's *mechanism*. The live validation also
exposes that the mechanism alone does **not** yet yield live gameplay: every mock
run (scripted or built-in) stalls in the multi-page startup splash, and the
hook's `cinemaMode` mislabels those static 2D pages as 3D gameplay. No game file
is modified; no binary is deployed; the gate is off by default in the host.

## Context
Wake 40 left req 1 as the one open #69 item, with the concrete plan "gate mock
dumps on the now-published `cinemaMode`". Before this change the mock host dumped
on a fixed frame cadence (`totalFrames == 1 || totalFrames % interval == 0`), so a
180-frame scripted run captured **5 cinema + 8 gameplay** frames. The cinema
frames (including the all-zero GL↔D3D11 interop warm-up frames 1/15/30) have
identical left/right eyes *correctly* — the renderer's cinema branch deliberately
blits the same source rect with no IPD shift (ADR-0445) — so they cannot validate
gameplay stereo and are reported as skips.

## Decision
1. **A pure, testable gate.** `src/host/dump_gate.h` adds
   `RiddickVR::ShouldDumpFrame(dumpDue, dumpGameplayOnly, stateRead, cinemaMode)`:
   a due frame is captured when `dumpGameplayOnly` is false (legacy), or — when
   true — only if the frame's branch is *known* (`stateRead`) and is 3D gameplay
   (`!cinemaMode`). An unread state is never captured as if it were gameplay.
   Unit-tested by `tests/dump_gate_tests.cpp` (`dump_gate_gameplay_only`,
   built in **both** x86 and x64).
2. **Host wiring.** `--dump-gameplay-only` (default **off**, so pre-existing
   tools such as `self_test_pipeline.py` are unchanged) sets
   `OpenXRManager::SetDumpGameplayOnly`. The mock loop calls the gate and the
   `display_branch.log` header/trunc is now keyed on the first *dumped* frame
   (`dumps == 0`) rather than `totalFrames == 1`, so it is still written when the
   gate skips the early frames.
3. **The harness opts in.** `tools/xrsim.py` gains `build_host_cmd(...)` (extracted
   for testing) and `--dump-branch {gameplay,any}` (default `gameplay`), so a
   scripted run requests `--dump-gameplay-only`. `--dump-branch any` restores the
   legacy capture-everything cadence.
4. **Regression invariant.** `tools/verify_reported_fixes.py` pins the helper,
   its call site, the CLI flag and the unit test.

## Evidence (2026-10-16, live mock runs; `dist/riddickvr-host.exe` 18:52)
- **Gate works.** `python tools/xrsim.py --scenario tools/scenarios/melee.json
  --timeout 150` (scratch artifacts) → `gameplay_captures_present` **PASS — 5
  gameplay / 0 cinema / 0 unknown of 5** (was 8/5/0 of 13). All 5 gameplay frames
  register `dx=-14, dy=0, identical=False` → `horizontal_disparity_present` and
  `not_mono_duplicate` **PASS**; `display_branch.log` has `cinema=0` for every
  frame. Markers still PASS (`COMBAT MODE entered`, `RIOT BATON ARC JAB`).
- **But the window is a static splash, not gameplay.** `scene_advanced` **FAIL —
  1/5 distinct**. The five dumps (frames 120–180) are **byte-identical**; a longer
  probe (`--frames 900 --interval 150`) is still **1/6 distinct**. Rendered, the
  dumped frame is the **legal/attribution text page** (`© 2009 Atari…`); with A
  taps it advances to the **ESRB "MATURE 17+" page** — i.e. the run is in the
  startup splash sequence. The hook nevertheless logs `DisplayMode: FULL-BLEED
  gameplay (3Dfresh=1 menuCursor=0)` for those pages, so `cinemaMode` (the only
  branch signal) mislabels static 2D startup pages as 3D gameplay.
- **Not scenario-specific.** The built-in loop (no scenario) — which *does* tap
  Button A every 60 frames — also stalls on the same ESRB page:
  `--frames 600 --interval 60` → 9 gameplay / 0 cinema, 3/9 distinct, but the
  first two dumps are all-black (`both_eyes_non_black` FAIL, interop warm-up) and
  the remaining "distinct" frames are the same splash page (plus an eyeshine
  brightness toggle).

## Explicitly not claimed
- **Requirement 1 acceptance is NOT met:** "≥2 distinct live gameplay frames"; the
  run never leaves the startup splash, so the gameplay stereo gates are exercised
  only on a static 2D page (stereo *is* present — `dx=-14` — but the scene does not
  advance). The blocker is now precisely characterized (splash stall +
  `cinemaMode` mislabel) and re-scoped to a new issue.
- **The branch signal is not fixed.** Distinguishing a static 2D splash page from
  live 3D gameplay is the open problem; it is related to #43 (menus/cinematics
  should render on the letterboxed cinema screen).
- **The leading all-black gameplay dumps remain.** With gameplay-only gating the
  interop warm-up frames are no longer *cinema* but the built-in run still shows
  two black gameplay dumps; a content check in the gate was not added.
- **Not validated in-headset.** All evidence is the mock host.
- **`frame_budget` still fails** (`mean≈13.6 max≈764 ms` vs an 11.11 ms budget) —
  a known mock-host cadence artefact, not a render cost.

## Reproduce
```
python tools/test_xrsim.py
python tools/xrsim.py --selftest
python tools/xrsim.py --scenario tools/scenarios/melee.json --timeout 150 \
    --artifacts <scratch> --report-file <scratch>\report.json
```

## Files
- `src/host/dump_gate.h` (new: `ShouldDumpFrame`)
- `tests/dump_gate_tests.cpp` (new) + `CMakeLists.txt` (`dump_gate_gameplay_only`)
- `src/host/openxr_manager.{h,cpp}` (`m_dumpGameplayOnly`, gate call, `dumps` header)
- `src/host/main.cpp` (`--dump-gameplay-only`)
- `tools/xrsim.py` (`build_host_cmd`, `--dump-branch`)
- `tools/test_xrsim.py` (2 new tests → 27)
- `tools/verify_reported_fixes.py` (extended GH #69 invariant)
- `src/host/README.md`
- cross-refs: ADR-0445 (#69 per-frame branch), ADR-0446 (#69 current-run log),
  `src/hook/stereo_renderer.cpp` (cinema vs gameplay blit), #43, #68, #69
