# ADR-0450: Observation-only probe of the first-person viewmodel pass

- Status: accepted
- Date: 2026-10-16
- Issues: GH #67 (step 2), GH #63, GH #66
- Supersedes/relates: ADR-0439 (per-draw viewmodel suppression window),
  ADR-0438 (the `noviewmodel` bit is inert)

## Context

GH #67 asks to **drive the engine's own first-person arms** from the controller
(F.E.A.R.-VR style) instead of re-drawing a baked overlay. Its plan step 2 is
"identify the first-person viewmodel pass": before the pass can be transformed
into head/controller space (or suppressed, GH #63), we must be able to say
*which* draws belong to it and *what* transform they use.

GH #66 already added per-draw interception (`RndrGL.dll`'s `glDrawElements` /
`glCallList`) and a PROVISIONAL window: a mid-frame **depth-only** clear opens
it, the next projection load or the frame swap closes it (ADR-0439). That path
only *suppresses* and is off by default; it produces no data about the pass.

## Decision

Add an **observation-only** mode to the `ViewmodelSuppressor` state machine
(ADR-0439's window) that reuses the same depth-only-clear boundary but **never
skips a draw**. With `ViewmodelObserve=1` (default **0**) the hook records, per
sampled frame:

- how many intercepted draws fell **inside** vs **outside** the window, and
- the `GL_MODELVIEW` in effect at the **first in-window draw** (the pass loads
  one viewmodel-relative matrix and draws its submeshes, so the first is the
  representative one; a later world/HUD matrix must not overwrite it).

It logs `ViewmodelProbe [#N]: inside=.. outside=.. sample=..` and a
`ViewmodelProbe matrix: [..]x4` line every 300 presented frames.

The observation state lives in the same header-only, dependency-free state
machine and is unit-tested (`viewmodel_suppressor_window`). The suppression
decision is unchanged and stays **last** in `ShouldSuppressDrawCall()`; observing
is subordinate to the suppression switch (both may be on, but each is
independent of the other's effect).

XRSIM (`tools/xrsim.py`) parses the probe (`viewmodel_probe_stats`) and adds the
graded check `viewmodel_pass_identified`: it **skips** when the probe was not run
(a pre-#67 host or `ViewmodelObserve=0`) and **fails** only when the probe ran but
no draw fell inside the window (the pass was not identified). The report carries a
`viewmodel_probe` key with the sampled matrices.

## Why not the suppression window's own counters

The GH #66 path only counts draws while `enabled` and reports `suppressed` — it
conflates "the window opened" with "we deleted the draws", and it does not record
the matrix at all. Identifying the pass must be possible with suppression **off**
so a headset run does not change what is rendered.

## Consequences

- A single INI flip (`ViewmodelObserve=1`) yields the pass boundary and the
  viewmodel transform on a real frame, with **no visual change** (nothing is
  skipped).
- The next steps (re-express the modelview in head/controller space; or suppress
  the pass for #63) have both a concrete signal and a validated capture path.
- Not yet validated live: the change is landed offline-verified (see below). A
  hook build must be deployed to the game install for `ViewmodelProbe` to appear;
  that deploy is outside the autoloop's constraints, so the probe is
  observation-only and inert until an interactive run enables it.

## Verification

- `build-all.ps1 -Config Release` OK (x86 `opengl32.dll` + x64 host; no deploy).
- CTest **10/10 PASS** on x86 **and** x64 (`viewmodel_suppressor_window` now also
  covers the observation mode: inert-by-default, counts, first-matrix capture,
  re-base, and that it never suppresses even with suppression enabled).
- `tools/test_xrsim.py` **38** tests PASS (probe parsing, skip-without-lines,
  in-window-draw-required).
- `verify_reported_fixes.py` GH #67 invariant; all `tools/test_*.py`,
  `validate_addresses`, `verify_hook_init`, `scenario_tests.py --check` PASS.
