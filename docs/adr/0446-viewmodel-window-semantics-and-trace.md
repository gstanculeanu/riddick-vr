# ADR 0446: viewmodel window closes only after draws; near-plane opener; pass trace

## Status
**Accepted, unverified on a headset.** Code and unit tests only (GH #63, GH #66).

## Context
ADR-0439 opens the engine-viewmodel suppression window on a depth-only clear and
closed it on the next projection load. The first-person pass loads its own
projection right after that clear, before any viewmodel draw, so the window closed
with nothing suppressed (finding F6). Nobody has captured the real GL call order
of a frame, so the opener itself is also a guess.

## Decision
- `NoteProjectionLoad(bool perspective, float nearPlane)`: an ortho load always
  closes the window. A perspective load closes it only if at least one draw was
  seen inside the window. The no-argument form means "perspective, near unknown".
- New opener `NearPlaneDrop` (and `Either`): the first perspective near plane of
  the frame is the world's; a later load with `near < worldNear * ViewmodelNearRatio`
  opens the window. The hook derives near from the original matrix as
  `m[14] / (m[10] - 1)`.
- `track` keeps the window and counters live without suppressing. It is used by
  `ViewmodelTrace` and by the controller drive (ADR-0447).
- `FrameTrace`: a fixed 192-char token string per frame (`C`, `D`, `P<near>`, `O`,
  `d<n>`), latched on swap, no heap. With `ViewmodelTrace=1` the hook logs it every
  300 swaps as `Viewmodel pass trace [#N]: ... | draws=.. suppressed=..`.
- INI: `ViewmodelTrace`, `ViewmodelWindowOpener` (-1 derives from
  `ViewmodelDepthClearWindow`), `ViewmodelNearRatio`.

## Consequences
- With the old defaults the window now stays open through the FP projection load
  and closes on the first perspective/ortho load after viewmodel draws.
- Still unknown: whether the engine draws its arms between those boundaries. One
  headset run with `ViewmodelTrace=1` gives the real signature. If a draw happens
  with no preceding projection load, the window runs to the swap and would also
  hide the draws that follow it in that pass.
