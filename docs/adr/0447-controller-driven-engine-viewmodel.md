# ADR 0447: opt-in controller-driven engine viewmodel

## Status
**Accepted, experimental, off by default, unverified on a headset** (GH #67).

## Context
Hiding the engine's arms (ADR-0439, ADR-0446) leaves only our hands. The other
way to avoid duplicate hands is to make the engine's arms follow the controller.

## Decision
`viewmodel_drive.h` computes a column-major correction
`C = T(p * upm) * R(q) * T(-anchor * upm)`, with `p` the controller position in
head space and `q = hmd^-1 * ctrl * offset(yaw, pitch, roll)`. With the controller
at the anchor and the head's orientation, `C` is the identity. When
`DriveEngineViewmodel=1`, suppression is off and the viewmodel window is open,
`Hook_glLoadMatrixf` loads `C * m` for the modelview. `C` is computed once per
window from the latest shared state: HMD pose and the dominant grip pose (right,
or left with `southpaw`). If the poses are invalid the engine matrix is used
unchanged. If both suppress and drive are on, suppression wins and a warning is
logged once.

## Consequences
- `ViewmodelUnitsPerMeter=30.3` (about 1/0.033) and the anchor
  (0.18, -0.22, -0.38) are provisional; tune them on a headset.
- The engine may not set the viewmodel modelview through `glLoadMatrixf` (it could
  use `glMultMatrixf` or `glLoadIdentity`, which are not hooked). The trace log
  will show whether the window opens; the drive has no effect if it does not.
- Out of scope: hiding the baked overlay when driving, and per-eye handling in the
  stereo renderer.
