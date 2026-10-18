# ADR-0482 — GH #67: capture the transform where it is (the engine's submitted vertices), and record whether the FP pass is array or display-list draws

- Status: accepted
- Date: 2026-10-18
- Refs: GH #67 (this — step 1, second correction), GH #63/#66 (the pass
  identification), GH #68 (iteration vehicle), ADR-0481 (the ARB candidate,
  measured silent), ADR-0450/0451 (the modelview observation), ADR-0459 (the
  display-branch latch), ADR-0471/0480 (live-capable, no-deploy shadow runs)

## Context

ADR-0481 eliminated GL state as the source of the engine's object/view
transform: the shipped 2009 `RndrGL.dll` has **no** matrix composition beyond
`glLoadMatrixf` + `glPushMatrix` + `glTranslatef` + `glPopMatrix`, a live run uses
exactly **one** (identity) modelview load, and the ARB vertex-program path is
measurably **silent**. Its conclusion was concrete but unmeasured in one
respect: "the transform must be applied to the submitted vertex data". This ADR
turns that inference into an observation, and answers the follow-up question that
decides whether the observation is even possible — are the first-person window's
draws client-array draws (whose vertices the hook can read) or display-list draws
(whose vertices replay inside the driver and are invisible)?

## Evidence (read-only, no install write; `tools/gl_vocabulary_scan.py --imports`)

The live target's `F:\…\Assault on Dark Athena\System\Win32_x86\RndrGL.dll`
(939248 bytes) binds exactly 96 `OPENGL32.dll` entry points. What matters here:

- **Client arrays are the geometry path**: `glVertexPointer`, `glNormalPointer`,
  `glColorPointer`, `glEnableClientState`/`glDisableClientState`, and
  `glDrawElements` are all imported; `glDrawArrays` and `glBindBuffer*` are
  **absent**. There is no buffer object, so the vertex pointer is always a plain
  client pointer and reading it at draw time is well-defined.
- **Display lists are also present**: `glNewList`/`glEndList`/`glCallList` are
  imported, so some geometry may be replayed inside the driver — which is why the
  observation must count both call kinds rather than assume.
- **Legacy immediate mode is present too** (`glBegin`/`glEnd`,
  `glVertex3f`/`glVertex3fv`, …); vertices issued inside a *display list* are not
  visible to a hook at all, and vertices issued at the top level would need the
  `glBegin` path. Neither is needed if `glDrawElements` carries the geometry —
  which the count below decides.

## Decision

Sample the geometry **at the draw entry point** the hook already owns, and record
which side of the first-person window each draw falls on. Add
`src/hook/vertex_submission_observe.h`: a header-only, dependency-free
`VertexSubmissionObserver`, inert until `Configure(true)` (the same
`ViewmodelObserve=1` switch):

- `NoteVertexPointer(size, type, stride, pointer)` records the client array
  (client state, not per draw).
- `NoteArrayDraw(insideWindow, count, indexType, indices)` counts a
  `glDrawElements` call and, for the **first** array draw on each side, samples up
  to `kMaxVertexSamples` (24) vertices spread across the index range:
  `SampleClientVertices(...)` returns the first vertex and the axis-aligned
  bounds, decoding only client, `GL_FLOAT` arrays with 1/2/4-byte indices. A
  non-float array, an unknown index type, a null pointer or `count == 0` yields
  `valid = false` and is logged **as such** (type/size/stride) rather than guessed.
- `NoteListDraw(insideWindow)` counts `glCallList` calls by side — the datum that
  says whether the first-person geometry is observable here at all.

Wiring: `dllmain.cpp` gains a RndrGL IAT hook for `glVertexPointer` and calls the
observer from the existing `glDrawElements`/`glCallList` hooks (after
`ShouldSuppressDrawCall()`, so the window state used to classify the draw is
current); `opengl_proxy.cpp` reports one line per sampled frame

```
ViewmodelProbe submission [#N]: arrayIn=… arrayOut=… listIn=… listOut=… ptrSize=… ptrType=… ptrStride=… inSample=… outSample=…
ViewmodelProbe submission fp: first=(x,y,z) min=(x,y,z) max=(x,y,z) n=… indexType=… vertexType=… valid=…
ViewmodelProbe submission world: first=(x,y,z) min=(x,y,z) max=(x,y,z) n=… indexType=… vertexType=… valid=…
```

`tools/xrsim.py` parses it (`viewmodel_probe_stats` → `submission_lines` +
`submission`) so a run's report carries the measurement; the parse is additive
(a pre-0482 hook yields `submission is None`).

## Reading the result

- `arrayIn > 0` with a small `fp` bbox (both `min`/`max` near the origin or at
  small camera-relative magnitudes) ⇒ the first-person geometry is object/view
  space client arrays and its transform is capturable at `glDrawElements`.
- `listIn > 0` with `arrayIn == 0` ⇒ the first-person pass is display-list
  geometry; the capture must move into list compilation (`glNewList`/`glEndList`),
  and no draw-time sample can see it.
- `fp` and `world` bboxes at the **same** magnitude scale ⇒ the view transform is
  baked in for both; the arms cannot be re-expressed by editing a per-object
  matrix the engine owns, only by transforming the vertices or by suppressing the
  engine pass and re-drawing from the decoded mesh (#55/#58).

## Result (live, 2026-10-18)

One run: `--shadow-install "F:\…\Assault on Dark Athena" --reach-gameplay
--duration 45 --ini ViewmodelObserve=1 --require-fresh-hook`, hook = the repo
build (`sha256 e6c0a03a710c`, `deployed_hook_matches_build` PASS), XRSIM **PASS**
(30 gameplay captures, fill 1.00, `comfort_vignette_applied` PASS). 23 sampled
`ViewmodelProbe submission` lines; `viewmodel_probe.submission` in the report:

- **The display-list hypothesis is falsified, and the array capture is
  complete:** every line is `listIn=0 listOut=0` — RndrGL never calls
  `glCallList` through the hooked IAT in the whole run — and
  `ptrSize=3 ptrType=5126 (GL_FLOAT) ptrStride=0` on every line. Geometry is
  `glDrawElements` client arrays, so a draw-time sample can see it.
- **The depth-only-clear opener is contaminated (a new, decisive negative):** the
  first in-window array draw is the **unit quad** `first=(0.5,0.5,0.0)
  min=(-0.5,-0.5,0.0) max=(0.5,0.5,0.0)` (24 sampled vertices), i.e. a
  screen-space/2D draw — *not* the viewmodel arms. The "world" (outside) sample is
  the frame's first array draw: large 2D magnitudes with a **constant
  `z=2032.000`** (x ∈ ±3518, y ∈ ±1663). `arrayIn≈4500` vs `arrayOut≈24700` per
  300 gameplay frames (~15 % in-window) matches wake 79's "the window opens on the
  world pass too".
- The window only opens in gameplay (`inSample=0` for the first 8 sampled frames =
  the splash/menu, `inSample=1` from #2400 on).

**Consequence for GH #67:** the observable surface is settled (array draws), but
the FP-pass *signature* is not — the current depth-only-clear window's first
in-window draw is a 2D quad. So the next step is a better opener/signature (e.g.
`NearPlaneDrop`, or "the submitted geometry is camera-relative/near-field"), then
re-run the same command and read a *non*-unit-quad `fp` sample. Driving the arms
cannot start from the contaminated window.

## Consequences

- The next #67 step is now a decision with evidence, not a guess: whichever of the
  three readings the run shows names the mechanism.
- The observer is opt-in and inert: no GL state, no submitted data and no draw is
  changed, so a run without `ViewmodelObserve=1` is byte-for-byte the old
  behaviour (unit-tested).
- Reading up to 24 client vertices per draw is bounded and cheap, but it *does*
  dereference engine-owned client memory at draw time (the array the engine is
  about to draw). It only happens while observing; a null/undecodable array
  short-circuits to `valid = false`.
- `vertex_program_observe.h`'s premise is corrected in place: the ARB entry points
  are **not** in RndrGL's static import table (they are `wglGetProcAddress`
  literals), and the path is silent; the ARB observer stays as the completed,
  measured-eliminated candidate. ADR-0481's title overstates its finding (the ARB
  path is *candidate*, not *source*); its body/conclusion are correct.

## Honest limits

- This is *measurement*, not the fix. Nothing player-visible changes.
- `glDrawArrays` is not hooked because RndrGL does not import it; if a later build
  does, the array count would under-report.
- Immediate-mode geometry outside a display list (`glBegin`/`glVertex3f`) is not
  sampled; a pass drawn that way would show neither `array*` nor `list*`.
- The sample is the **first** array draw on each side (a pass shares its buffer),
  so it characterises a pass, not every mesh in it.
- Only `GL_FLOAT` vertex arrays are decoded; an `GL_SHORT`/`GL_INT` array logs
  `valid=0` with its type/stride, on purpose.

## Validation

- Offline: `tests/vertex_submission_observe_tests.cpp` (CTest
  `vertex_submission_observe`): the inert default, client-pointer state, ubyte/
  short/uint index widths, stride, a 2-component array (z = 0), the bounded spread
  reaching the last vertex, the undecodable cases, per-side first-sample retention,
  `ComputeSnapshot` re-basing, and `Configure(false)` dropping state.
  `tools/test_xrsim.py` +1 (`test_viewmodel_probe_submission_lines_are_parsed`,
  incl. the additive/legacy case); `tools/xrsim.py --selftest` +1
  (`viewmodel_submission_parsed`); the GH #67 source invariant in
  `tools/verify_reported_fixes.py` now requires the submitted-vertex capture.
  `build-all.ps1` Release (no deploy), CTest 24/24 x64 + 26/26 x86, all 12
  `tools/test_*.py`, `scenario_tests --check`, `verify_test_artifacts`,
  `verify_hook_init`, `validate_addresses`, `verify_reported_fixes` PASS.
- Live: one run recorded in `tools/autoloop/STATE.md`
  (`--shadow-install … --ini ViewmodelObserve=1 --require-fresh-hook`, hook
  `sha256 e6c0a03a710c` = the repo build): XRSIM `PASS` (30 gameplay captures),
  23 sampled submission lines, `viewmodel_probe.submission` = `listIn=0
  listOut=0` (no display lists) with the in-window sample the unit quad — see
  "Result (live)" above. Install untouched (deployed `opengl32.dll` 479232 B
  17-Oct 12:47, install `riddick_vr.log` 18-Oct 00:25 both unchanged); no leftover
  processes; live lock released.
