# ADR-0451: Software GL_MODELVIEW stack for the draw-time viewmodel matrix

- Status: accepted
- Date: 2026-10-16
- Issues: GH #67 (step 1), GH #63, GH #66
- Supersedes/relates: ADR-0450 (observation probe), ADR-0439 (suppression
  window), `docs/rea/reference/RndrGL_GL_Path.txt`

## Context

GH #67's plan step 1 ends with "add hooks for the matrix concatenation calls".
ADR-0450 landed the observation probe (step 2) that records, per sampled frame,
the `GL_MODELVIEW` **in effect at the first in-window draw** so the first-person
viewmodel pass can later be re-expressed in head/controller space (step 3) or
suppressed (GH #63).

That captured matrix came from `s_lastView`, which is written **only** by
`OpenGLProxy::Hook_glLoadMatrixf` (`src/hook/opengl_proxy.cpp`). But REA
(`docs/rea/reference/RndrGL_GL_Path.txt` section 3) records that RndrGL.dll does
**not** import `glMultMatrixf`/`glRotatef`; it composes object transforms from
`glLoadMatrixf` + `glPushMatrix` + `glTranslatef` + `glPopMatrix`. So the value
the probe recorded was at best the last *loaded* base matrix, never the
`base * translate(...)` the engine actually rendered with for any pushed
sub-object — i.e. for exactly the viewmodel submeshes the probe exists to
measure. A viewmodel pass that pushes/translates would be captured **wrong**, and
step 3 would build on that wrong input.

## Decision

Mirror the fixed-function `GL_MODELVIEW` stack in software:

- New header-only, dependency-free `src/hook/gl_matrix_tracker.h`
  (`RiddickVR::GlMatrixTracker`): column-major 4x4, bounded stack (depth
  `kMaxDepth = 32`). `LoadMatrixf`/`LoadIdentity` replace the top; `Translatef`
  right-multiplies (`M' = M * T`, column 3 only); `Push`/`Pop` duplicate/restore;
  overflow and underflow are ignored so an unbalanced engine call cannot corrupt
  state. `Multiply` is provided for completeness (RndrGL does not import
  `glMultMatrixf`).
- `OpenGLProxy` owns the tracker (`s_modelviewTracker`) and re-publishes it into
  `s_lastView` after every tracked op, so `GetLastViewMatrix()` — the probe's
  input — is now the **effective** modelview. New entry points
  `NoteModelviewLoadIdentity/Push/Pop/Translatef` are no-ops outside
  `GL_MODELVIEW` (the projection stack is untouched) and are reset on
  `wglMakeCurrent` (a fresh context starts at identity).
- `dllmain.cpp` IAT-hooks `OPENGL32.dll!glLoadIdentity`, `!glPushMatrix`,
  `!glPopMatrix` and `!glTranslatef` in `RndrGL.dll` (the same mechanism as the
  existing `glDrawElements`/`glCallList` hooks), forwarding to the saved original
  verbatim. They never suppress a draw.
- The GH #67 step-2 observation snapshot gains `pushesInside` /
  `translatesInside`; the probe line becomes
  `ViewmodelProbe [#N]: inside=.. outside=.. sample=.. pushes=.. translates=..`.
  A non-zero count is direct in-game evidence that the pass composes a
  sub-transform and that the composed (not merely loaded) matrix is the one that
  matters. XRSIM parses the optional fields (backward compatible with a
  pre-#67-step-1 log line) and aggregates them.

## Consequences

- The probe's matrix is now the transform the engine actually used, which is the
  prerequisite for step 3 (re-express in head/controller space). This is a
  correctness fix in the hook, off across all render paths that do not push a
  sub-transform.
- The intercept is forwarding-only and cheap (a few matrix writes + a memcpy per
  composed object). It is always on because it only affects the value reported by
  `GetLastViewMatrix()`; it does not change what the engine renders.
- Not yet validated live: like the step-2 probe, the composed-modelview capture
  only appears in a hook build deployed to the game install, and the autoloop must
  not deploy. A headset run with `ViewmodelObserve=1` is expected to show non-zero
  `pushes=`/`translates=` in a viewmodel frame and a matrix whose translation
  differs from the plain `glLoadMatrixf` log line.

## Verification

- `build-all.ps1 -Config Release` OK (x86 `opengl32.dll` + x64 host; no deploy).
- CTest **11/11 PASS** on x86 **and** x64 (new `gl_matrix_tracker`;
  `viewmodel_suppressor_window` also covers the push/translate counters).
- `tools/test_xrsim.py` PASS (new `test_viewmodel_probe_counts_matrix_composition`
  + backward-compatible parse); `xrsim.py --selftest` PASS.
- `verify_reported_fixes.py` GH #67 step-1 invariant; all `tools/test_*.py`,
  `validate_addresses`, `verify_hook_init`, `scenario_tests.py --check` PASS.
