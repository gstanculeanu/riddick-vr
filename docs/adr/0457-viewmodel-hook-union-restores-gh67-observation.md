# ADR-0457: Union merge restores the GH #67 viewmodel observation machinery (GH #74 hook half)

- Status: accepted
- Date: 2026-10-17
- Issues: GH #74 (harness reconciliation), GH #67 (step 1 + step 2), GH #63, GH #66
- Relates: ADR-0450 (observation probe), ADR-0451 (software modelview stack),
  ADR-0446/0447 (window semantics + controller drive), GH #74

## Context

The PR #70 merge (`e6c6c6e`, parents `f5fd7f7` + branch) replaced the autoloop's
XRSIM host + hook with a parallel implementation. Wake 52 restored the **Python**
harness half; wake 53 restored the **host** half (`src/host/openxr_manager.cpp`,
`verify_reported_fixes.py` host invariants). The **hook** half was still
clobbered: `src/hook/opengl_proxy.cpp` had `s_modelviewTracker` removed and the
`NoteModelview*` entry points reduced to **no-op linker stubs**, and
`src/hook/viewmodel_suppressor.h` had lost the `observe` mode
(`ViewmodelObservationSnapshot`, `NoteObservedDraw`, `SetObservedSample`,
`ComputeSnapshot`, the push/translate counters). `vr_config.{h,cpp}` no longer
read `ViewmodelObserve`.

The surviving artifacts still **assumed** the restored code:

- `tools/xrsim.py` parses `ViewmodelProbe [#N]: inside=.. outside=.. sample=..`
  lines and grades `viewmodel_pass_identified`; `tools/test_xrsim.py` pins it.
- `src/hook/README.md`, ADR-0450 and ADR-0451 describe the probe and the tracker.
- `src/hook/dllmain.cpp` still installs the `glLoadIdentity`/`glPushMatrix`/
  `glPopMatrix`/`glTranslatef` IAT hooks; `CMakeLists.txt` still builds the
  `gl_matrix_tracker` CTest and lists `src/hook/gl_matrix_tracker.h`.

The branch *also* added real features that must be kept: the `track` switch and
per-frame pass trace (`viewmodelTrace`), the `NearPlaneDrop`/`Either` openers
(`viewmodelWindowOpener`, `viewmodelNearRatio`), and the opt-in controller drive
(`viewmodel_drive.h`, `DriveEngineViewmodel`, ADR-0447).

## Decision

Reconcile the hook half as a **union**: keep PR #70's `track`/trace/`NearPlaneDrop`
window and the controller drive, and re-add the GH #67 step-1/step-2 observation
machinery beside them.

- `viewmodel_suppressor.h`: `ViewmodelSuppressionConfig` gains `observe`;
  `ViewmodelObservationSnapshot` and `NoteObservedDraw`/`SetObservedSample`/
  `ComputeSnapshot`/`NoteObservedPush`/`NoteObservedTranslate`/`Observing` are
  restored. A private `Tracking()` (`Active() || observe`) drives the window state
  machine so the boundary is shared by suppression, trace/drive, and observation;
  only `enabled` can actually skip a draw (`track`/`observe` never suppress).
- `opengl_proxy.cpp`: `s_modelviewTracker` and the `NoteModelview*` bodies are
  restored; the `GL_MODELVIEW` load path feeds the tracker with the **effective**
  matrix (the controller-drive correction when drive is active, else `m`).
  `BuildViewmodelSuppressionConfig` sets `cfg.observe` from `ViewmodelObserve`;
  `Hook_wglSwapBuffers` emits the `ViewmodelProbe` + `ViewmodelProbe matrix` lines;
  `ShouldSuppressDrawCall` records the observation before the unchanged
  suppression decision. The `viewmodelTrace` line and drive branch are untouched.
- `vr_config.{h,cpp}` + `riddick_vr.ini` + `tools/riddick_vr.ini.example`:
  `ViewmodelObserve` (default 0) is read/parsed/documented again.
- `tests/viewmodel_suppressor_tests.cpp`: the GH #67 observation cases (open on a
  depth-only clear, in/out tallies, first-in-window sample, push/translate
  counting, observe-with-suppression, inert-without-observe) are restored on top
  of the branch's `track`/trace/near-plane cases.
- `tools/verify_reported_fixes.py`: the GH #67 step-2 and step-1 invariants are
  re-added (`ViewmodelObservationSnapshot`/`ComputeSnapshot`/`ViewmodelProbe`/
  `s_viewmodelTracker`/`NoteModelview*`/`gl_matrix_tracker_tests.cpp`); `xrsim_tool`
  is read once near the top so the earlier block can reference it.

## Consequences

- The repo is internally consistent again: the harness, docs and unit tests that
  survived the merge now match the hook code they describe, so GH #67 step 3
  (re-express the captured modelview in head/controller space) has its input
  restored.
- Behaviour is unchanged by default: `observe` defaults off, the tracker is
  forwarding-only (it only changes what `GetLastViewMatrix()` reports), and the
  drive path is opt-in and unchanged.
- Still not validated live: the probe only appears in a hook build deployed to the
  game install, and this loop must not deploy (the sim install's hook is stale).
  A headset run with `ViewmodelObserve=1` is expected to print non-zero
  `inside=`/`pushes=`/`translates=` and a matrix for a viewmodel frame.

## Verification

- `build-all.ps1 -Config Release` OK (x86 `opengl32.dll` + x64 host; no deploy).
- CTest **19/19 PASS** on x86 **and** x64 (restored observation cases in
  `viewmodel_suppressor_window`; `gl_matrix_tracker` present).
- `tools/test_xrsim.py`, `xrsim.py --selftest`, all `tools/test_*.py`,
  `validate_addresses`, `verify_hook_init`, `verify_reported_fixes` (with the
  re-added #67 invariants) PASS.
- `scenario_tests.py --check` remains red only on `hands_view.json` (GH #75,
  pre-existing, unchanged).

## Update — GH #75 (ADR-0458)

`scenario_tests.py --check` is green again: observation-only profiles now opt
out of the marker requirement explicitly (`"observe": true`, set on
`hands_view.json`), guarded by `tools/test_scenario_tests.py`. The red noted
above was tooling-only and had no bearing on the hook reconciliation.
