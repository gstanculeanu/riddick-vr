# ADR 0458: Observation-only scenario profiles opt out of the marker requirement

## Status
**Accepted** — GH #75 (the loop's own `--check` gate was red on `main`). Tooling
+ docs only; no C++, no binary, nothing deployed, no game file touched.

## Context
`python tools/scenario_tests.py --check` is both a CI lane
(`.github/workflows/vr-worker.yml`, "Scenario manifest validation (GH #48)") and
the autoloop's standing-prompt validation step. It was **red on `main`**:

```
[scenarios] invalid manifest: hands_view.json: expected non-empty markers or markers_any
```

`tools/scenarios/hands_view.json` (added by `133a2d8`, "sim mode posts window
messages") raises both controllers in front of the face, **idle** (grips 0,
triggers 0), "so the VR/native hands are framed for inspection". The validator
(`scenario_tests.py`) required every mock profile to declare a non-empty
`markers`/`markers_any` list *and* required each marker to appear literally in
`src/hook/camera_hook.cpp`. An inspection pose has no interactive gesture, so
there is no hook marker to assert — and the only hand-draw diagnostics
(`NativeHand: LEFT/RIGHT DRAWN`, `4471562`) live in
`src/hook/vr_hand_renderer.cpp`, not `camera_hook.cpp`.

The marker requirement is right for interaction profiles (it stops a profile
from "passing" against a log line that no longer exists), but it is wrong to
force an inspection profile to fabricate one: the alternatives were to add a
hook log line (which needs a deploy to validate, and is out of loop scope) or to
move `hands_view.json` out of the validated glob entirely (losing the structural
checks the validator still provides for its keyframes).

## Decision
Give profiles an explicit, documented opt-out from *only* the marker
requirement:

1. `tools/scenario_tests.py` reads a new optional boolean `"observe"`. When
   `"observe": true`, the non-empty `markers`/`markers_any` requirement is
   skipped; a non-boolean value is a hard error. Every *other* check still runs
   (name/stem match, non-empty keyframes, required keyframe fields, increasing
   frames, marker fields well-typed, declared markers present in the hook
   source, and the scripted-profile file existing). A profile with neither
   markers nor `"observe": true` is still rejected.
2. `tools/scenarios/hands_view.json` sets `"observe": true` and keeps its empty
   `markers` list (the pose is genuinely marker-free).
3. `--check` reports the count of observation-only profiles so the opt-out is
   visible in the gate output.
4. `tools/test_scenario_tests.py` (new, headless) covers the validator with a
   synthetic workspace: the opt-out passes, a markerless profile *without* it is
   rejected, a non-boolean `observe` is rejected, a profile marker absent from
   the hook source is rejected, a missing scripted profile is rejected, and
   non-increasing keyframes are rejected. Its last case runs the *shipped*
   `--check` so a red `main` fails this suite too (the GH #75 guard).
5. A GH #75 invariant in `tools/verify_reported_fixes.py` asserts the opt-out
   handler, the flag on `hands_view.json`, and the new test exist; the CI lane
   now runs `tools/test_scenario_tests.py` before `scenario_tests.py --check`.

## Consequences
- `python tools/scenario_tests.py --check` is green on `main`
  (`8 test cases, 12 scripted profiles, 1 observation-only`), so the loop's
  validation step and the CI scenario lane stop reporting a false red.
- The marker requirement remains in force for every interaction profile; the
  opt-out is explicit and greppable rather than implicit in an empty list.
- The opt-out only relaxes validation. It says nothing about runtime behaviour:
  `xrsim.py --scenario tools/scenarios/hands_view.json` already performed no
  marker checks for an empty `markers` list, so nothing else needed to change.
- Pre-existing but still open: `tools/scenarios/hands_view.json` is not
  referenced by `tests/scenarios/manifest.json` and is used ad hoc; wiring it
  into the manifest (e.g. as a `manual` inspection scenario) is optional
  follow-up, not required for the gate to be green.
