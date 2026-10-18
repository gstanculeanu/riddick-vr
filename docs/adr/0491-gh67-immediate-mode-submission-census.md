# ADR-0491: The first-person window's submission *mechanism* is censused — including the immediate-mode path every previous sample missed (GH #67 step 1, ninth correction)

- Status: Accepted (scheduled autoloop wake 89, GH #67)
- Date: 2026-10-18
- Related: GH #67 (this), GH #63/#66 (the suppression boundary), GH #68 (iteration
  vehicle), ADR-0439 (the PROVISIONAL depth-only-clear window), ADR-0482/0483
  (the submitted-vertex capture), ADR-0484 (the planar candidate is not the
  viewmodel), ADR-0487/0488 (the window openers and the projection near plane),
  ADR-0489/0490 (per-interval sampling; the near-field candidate falsified)

## Context

Eight corrections of GH #67 step 1 have sampled the engine's submitted geometry
and each one looked in the same place: `glDrawElements` (client vertex arrays)
and `glCallList` (display lists), the two draw entry points
`InstallRndrGLHooks` IAT-hooks. The last two measurements were:

- the window's largest in-window array draw is a **12-vertex plane** at constant
  `z = 6.25` view space (`extent [12.5, 12.5, 0.0]`, ADR-0484/0487);
- a **window-independent near-field** candidate exists only on the title pages,
  and the live level submits **no** draw within ±16 units of the origin at all
  (ADR-0490), so the geometric route is a dead end.

Both readings share an unstated premise: that the arms are an *array* draw. The
project's own reference contradicts it. `docs/rea/reference/RndrGL_GL_Path.txt`
section 2 records `glBegin`/`glEnd` as **imported and not intercepted** ("a
begin/end pair gate is left as a follow-up **if the first-person arms turn out to
use immediate mode**"), and its 2009 correction measures the live module's static
OPENGL32 import table (96 entry points) and calls it a "fixed-function
**immediate-mode** renderer". Re-running the same read-only scan on the live
install (this wake) confirms the binding:

```
glBegin  glEnd  glVertex3f  glVertex3fv  glVertex2f  glVertex2i   (among 96)
```

A mesh the engine submits as

```c
glBegin(GL_TRIANGLE_STRIP);
  glVertex3f(x, y, z); ...
glEnd();
```

emits **no `glDrawElements` call at all**. It therefore produced no sample in any
of the eight corrections, and no array-based or list-based reading could ever
have been the arms. The census was incomplete in a way the readings could not
see — the open question the reference wrote down was never instrumented.

## Decision

Instrument the missing mechanism and census the window by *mechanism*, not by
threshold. Observation only: nothing is suppressed and no default changes.

### 1. A pure immediate-mode observer (`src/hook/immediate_mode_observe.h`, new)

`ImmediateModeObserver` counts, per 300-swap interval and split by the
first-person window's state at the `glBegin`:

- `beganInside/Outside` and `endsInside/Outside` (blocks),
- `vertex3Inside/Outside` (`glVertex3f`/`glVertex3fv`) and
  `vertex2Inside/Outside` (`glVertex2f`/`glVertex2i`) — the 2-D path is kept
  separate so "the window is 2-D HUD geometry" is distinguishable from "it is a
  3-D mesh" (the engine also imports `glVertex2f`/`glVertex2i`),
- the window's **largest** block, sampled with *exact* bounds over every vertex of
  the block (a block is one `glBegin`..`glEnd` primitive batch, i.e. one submitted
  mesh — the immediate-mode analogue of ADR-0483's "largest mesh"). Three compares
  per vertex is the whole cost.

A block is classified by its starting side (an engine cannot legally emit a
vertex outside `glBegin`..`glEnd`, so a block that began inside counts inside);
an unmatched `glEnd` is ignored and a `glBegin` while a block is open closes the
previous block rather than merging two. Inert until `Configure(true)`, i.e. the
same `ViewmodelObserve=1` switch as every other probe.

### 2. IAT hooks in RndrGL, forwarded verbatim (`dllmain.cpp`)

`InstallRndrGLHooks` additionally IAT-hooks `OPENGL32.dll!glBegin`, `!glEnd`,
`!glVertex3f`, `!glVertex3fv`, `!glVertex2f`, `!glVertex2i`. Each hook observes
and then calls the saved original (the proxy's own forwarder) unconditionally —
the same contract as the existing `glDrawElements`/`glCallList`/`glVertexPointer`
hooks, so enabling observation cannot change a frame. RndrGL is the only shipped
GL client, so these calls are exactly the engine's own submissions; the mod's own
overlay loads `glBegin` from the real system `opengl32` and is not counted.

### 3. Logged in the existing submission form

`OnSwapBuffers` logs, on the same sampled frames:

```
ViewmodelProbe immediate [#N]: beganIn=.. beganOut=.. endIn=.. endOut=.. v3In=..
                               v3Out=.. v2In=.. v2Out=.. openIn=0|1 sample=0|1
ViewmodelProbe submission fpimm: first=(..) min=(..) max=(..) n=.. count=.. … valid=1
```

The candidate is emitted as a `submission <label>:` line with a **new label
(`fpimm`)**, so the harness reuses the existing planar-vs-volumetric geometry
predicate and the same shape gate the `fp`/`world`/`near` samples use — no second
shape implementation to disagree with.

### 4. Graded by XRSIM

`tools/xrsim.py` parses the census (`VIEWMODEL_IMMEDIATE_RE`) into
`viewmodel_probe["immediate"]`, adds the pure `viewmodel_immediate_census()`
helper, and grades two claims in the existing `viewmodel` group:

- **`viewmodel_in_window_immediate_geometry`** — PASS when the window held 3-D
  immediate-mode vertices (`v3In > 0`, i.e. the arms *could* be this geometry),
  FAIL when the census ran and it held none (the hypothesis is falsified for this
  target and the arms must be arrays/lists/absent), SKIP (never FAIL) when the log
  carries no census line at all (a pre-#0491 hook).
- **`viewmodel_immediate_candidate_is_volumetric`** — the window's largest
  immediate block graded by the same thickness-ratio predicate as the other
  samples; SKIP when no interval held an in-window block (nothing to grade).

## Consequences

- The census now covers **all three** submission mechanisms the shipped module
  binds: indexed arrays, display lists, and immediate mode. A future reader can no
  longer confuse "no sample" with "the mesh is not there".
- The reference's own open question ("do the first-person arms use immediate
  mode?") is now *answerable by measurement* in a single run, and the harness
  FAILs loudly on the negative answer instead of skipping.
- Honest limits: the census is per 300-swap interval, not per frame; the candidate
  is the interval's largest in-window block, so a small arms block in an interval
  dominated by a large HUD block would not be the candidate (the tallies still show
  its vertices); the `glVertex*` hooks add one observe-check per vertex, which is
  measurable in the run's frame time; and discovering that the window holds
  immediate-mode vertices is **not** identifying the arms — it only removes the
  mechanism blind spot (the same distinction ADR-0489 made for the window).

## Validation

- `python tools/gl_vocabulary_scan.py --game-dir "F:\The Chronicles of Riddick -
  Assault on Dark Athena" --imports` (read-only, this wake): RndrGL.dll (939248 B)
  binds 96 static `OPENGL32.dll` imports including `glBegin`, `glEnd`,
  `glVertex3f`, `glVertex3fv`, `glVertex2f`, `glVertex2i` — the bindings the hook
  now intercepts. (Measure first, then code: the interception is on the measured
  import table, not on the 2004 build's reference text.)
- `tests/immediate_mode_observe_tests.cpp` (new, CTest `immediate_mode_observe`,
  both architectures): 7 groups incl. inert-until-enabled, exact block bounds,
  2-D/3-D separation by side, largest-block selection (not the first), the
  begin-while-open and unmatched-`glEnd` cases, and
  `ComputeSnapshot` re-basing while an open block stays pending.
- `tools/test_xrsim.py` `test_immediate_mode_census_is_parsed_and_graded`: the
  census parses (window-side tallies, 2-D vs 3-D), the `fpimm` sample is graded by
  the shared shape predicate, the mechanism check PASSes with 3-D in-window
  vertices, **FAILs** when the census ran with none, both checks **SKIP** on a
  pre-#0491 hook, and the parse is additive (`immediate is None`).
- `python tools/xrsim.py --selftest` +1
  (`viewmodel_immediate_census_is_parsed`).
- `tools/verify_reported_fixes.py` — the #67 invariant extended (the header, the
  six IAT hooks, the log lines, the parser/checks/tests, the ADR).
- Live: one `--reach-gameplay` run with `ViewmodelObserve=1` on a read-only
  per-run shadow (no deploy, no install write) — see `tools/autoloop/STATE.md`
  wake 89 for the measured census.
