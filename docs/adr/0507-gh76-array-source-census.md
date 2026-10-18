# ADR 0507: Native stereo (epic #57 item 1) — the world vertex array is submitted through the **generic-attribute + buffer-object** path, not the fixed-function `glVertexPointer` (GH #76 / GH #90)

- Status: Accepted (scheduled autoloop wake 105, GH #76 / GH #90)
- Date: 2026-10-18
- Related: GH #76 (this), GH #90 (the scoped option-1 unit), GH #57 (epic /
  definition-of-done item 1), ADR-0506 (the span census that named the rejection:
  every undecodable world draw is `noarray` — a null client pointer with a real
  stride), ADR-0482 (**whose premise this falsifies**: "RndrGL imports
  `glVertexPointer` but no `glBindBuffer*`, so the pointer is always a plain client
  pointer"), ADR-0481 (the ARB probe only looked for `*ARB*` names), ADR-0491 (the
  array path is complete), ADR-0505 (the per-eye dual-issue pass this diagnoses)

## Context

ADR-0506 measured that *every* undecodable draw in the per-eye pass's world span is
`noarray` — the sampler is handed `GL_FLOAT` (`0x1406`), 3 components, a 56-byte
stride and `GL_UNSIGNED_SHORT` (`0x1403`) indices, but a **null** client vertex
pointer — and named three candidate readings: a buffer object reached through a
`wglGetProcAddress`'d core `glBindBuffer`, the generic-attribute path
(`glVertexAttribPointer[ARB]`), or an array set where the hook cannot see it. Its
Next 1 was to *measure* which, by logging the dynamic entry points the engine
resolves and tracking the array source.

That measurement did not exist. `ArrayBindingTracker`, `AttributePathCensus` and
`GlProcNameCensus` (`gl_entry_census.h`) now provide it, fed by the
`wglGetProcAddress` hook (each distinct resolved name logged once) and by
forwarding wrappers for `glBindBuffer[ARB]`, `glVertexAttribPointer[ARB]` and
`glEnableVertexAttribArray[ARB]`, plus an IAT hook for the (expected-absent)
`glInterleavedArrays`. The sampler's `VertexSample` carries the binding that was in
force at the draw, and the observer tallies null vs non-null `glVertexPointer`
calls. Everything is read-only: no draw decision changes, the pass stays opt-in
(`StereoDualRender=0`), and the evidence line prints `ptrNull=` and `buf=/bid=`.

## Decision

Keep the census and let a live run name the array source, with two new evidence
fields wired into the existing logs:

1. `gl_entry_census.h` (pure, headless-tested): `GlProcNameCensus` (a bounded,
   deduplicating table of resolved names + per-name call counts),
   `ArrayBindingTracker` (live `GL_ARRAY_BUFFER`/`GL_ELEMENT_ARRAY_BUFFER` ids +
   bind tallies), `AttributePathCensus` (generic-attribute pointer/enable counts +
   the last pointer's shape, and a separate `glInterleavedArrays` count).
2. `vertex_submission_observe.h`: `VertexSample.arrayBufferBound`/`arrayBufferId`,
   and the interval's `pointerCalls`/`pointerNullCalls`.
3. `stereo_dual.h` / `stereo_renderer.cpp`: the span census carries the first
   rejection's array-buffer binding; the `DualRender:` line prints `buf=<n> bid=<id>`.
4. `opengl_proxy.{h,cpp}` + `dllmain.cpp`: the wrappers and the once-per-name
   `GLProcCensus resolve:` / per-interval `GLProcCensus [...]` lines.

## Measurement (live, read-only per-run shadow, `RIDDICKVR_SIM=1`)

Command shape: `python tools/xrsim.py --shadow-install "F:\The Chronicles of
Riddick - Assault on Dark Athena" --reach-gameplay --duration 70 --advance-menu
--advance-min-taps 8 --require-fresh-hook --require-parallax --dump-branch any
--downscale 4 --ini StereoDualRender=1 --ini ViewmodelObserve=1`.
`deployed_hook_matches_build` PASS (shadow hook `sha256 4ce0a0cbb402`, `522752 B`
= the repo build); no deploy, no install write. (A first, identical run stayed in
the menu — the known launcher/window flake — and is recorded, not graded.)

Run B reached **live in-level gameplay** (`gameplay_captures_present`: 50 gameplay
/ 7 cinema / 1 unknown of 58; `gameplay_full_bleed_reached` max BarScan fill
**1.00**; `frame_budget` PASS n=15240 p50=2.91 ms p95=5.15 ms).

**1. ADR-0482's premise is falsified: the engine resolves and calls the whole core
buffer-object API.** The name census lists, as resolved through
`wglGetProcAddress`, `glBindBuffer`, `glGenBuffers`, `glDeleteBuffers`,
`glIsBuffer`, `glBufferData`, `glBufferSubData`, `glGetBufferSubData`,
`glMapBuffer`, `glUnmapBuffer`, `glGetBufferParameteriv`, `glGetBufferPointerv`,
and — from the extension families — `glVertexAttribPointerARB`,
`glEnableVertexAttribArrayARB`, `glDisableVertexAttribArrayARB`,
`glVertexAttrib1fARB`, the `glMultiTexCoord{2,3,4}{f,fv}ARB` set and the
`glSecondaryColor*EXT` / `glCompressedTex*ARB` sets (`procs=147 names=64`; the
name table is capped at 64). `glInterleavedArrays` is **not** resolved and never
called (`interleaved=0`).

**2. They are used heavily in-level.** At swap 9600:

```
GLProcCensus [#9600]: procs=147 names=64 arrayBuf=0 elemBuf=0
  bindCalls=15932431(ary=5234634 elem=10697797) attribPtr=11110069
  enableAttr=705159 interleaved=0
  attribLast=(idx=8 size=2 type=0x1406 stride=0 ptr=1)
```

i.e. over ~9.6k swaps (≈15.2k frames, `frame_budget`) the engine bound
`GL_ARRAY_BUFFER` **5.23M** times (≈325/frame), `GL_ELEMENT_ARRAY_BUFFER`
**10.70M** times (≈670/frame), and called `glVertexAttribPointerARB` **11.11M**
times (≈695/frame) with `glEnableVertexAttribArrayARB` 705k times. ADR-0482's
"it never uses a buffer object, so the pointer is always a plain client pointer"
was a statement about the **static** import table; by measurement the engine's
in-level geometry path is buffer-backed and generic-attribute-driven.

**3. The null pointer is the engine's own doing, not staleness.** In-level the
sampler's tally reads a stable `ptrCalls=124,385 ptrNull=102,785` per 300 swaps
(≈413 calls/frame, **83% of them with a null pointer**), with `ptrStride=0`
recorded at the interval boundary:

```
ViewmodelProbe submission [#9600]: arrayIn=3600 arrayOut=220838 listIn=0
  listOut=0 ptrSize=3 ptrType=5126 ptrStride=0 ptrCalls=124385 ptrNull=102785
```

So the engine really does call `glVertexPointer(3, GL_FLOAT, <stride>, NULL)`
hundreds of times per frame — the fixed-function array the sampler reads is a
null pointer **by design**, and the world draws inherit it.

**4. At the census's first rejection no array buffer is bound.** The new field:

```
DualRender: swap=17459 frames=7611 used=426 draws=60 eligible=60 skipped=675
  mirrors=16392 active=1 coverL=0.289 coverR=0.289 size=1280x605 use=0
  scale=[unknown=675 near=0 mid=2 world=58]
  reject=675 rc=[nonfloat=0 noarray=675 zerocount=0 badsize=0 badindex=0]
  sig=[noarray vtype=0x1406 vsize=3 stride=56 ptr=0 itype=0x1403 count=12
       buf=0 bid=0]
```

`buf=0 bid=0` — so the `noarray` draws are **not** the "null pointer = offset 0
into a currently-bound VBO" case either. The engine assembles its arrays with the
generic-attribute pointers (and the element buffer) and drives the vertex array
outside the fixed-function `glVertexPointer` state that the sampler reads.

## Consequences

- The **array source is named**: it is the `glVertexAttribPointerARB` +
  buffer-object path, and the fixed-function `glVertexPointer` client array is not
  where the world position data for these draws comes from. The `noarray` label of
  ADR-0506 is therefore *not* "the array is set where the hook cannot see it" (the
  hook sees the bind and pointer-set calls) but "the sampler is reading the wrong
  array".
- The route to an eligible-world-draw sampler changes accordingly: it must read the
  **generic attribute** state (`glVertexAttribPointerARB` per index, and which
  indices are enabled) and, for buffer-backed arrays, the bound buffer +
  `glGetBufferSubData`/`glMapBuffer` — not client memory via `glVertexPointer`.
  That is a decode extension with real work behind it, not a one-line threshold
  change, which is why it is the next unit rather than this one.
- Zero behaviour change: the pass is still opt-in (`StereoDualRender=0`), gated,
  `use=0`, and gameplay still presents the shipped shifted capture
  (`stereo_kind=shift-only`, `spread=0`, `stereo_parallax_present` FAIL — the
  honest #76 gap). No regression: `gameplay_full_bleed_reached` PASS (1.00),
  `frame_budget` PASS.
- Limits: instrumentation, not the fix — native stereo is still **not** achieved.
  The generic-attribute *index* that carries position is not identified (the last
  sampled attribute pointer is index 8, size 2), the independent-vertex-array
  enable state (`glEnableClientState(GL_VERTEX_ARRAY)`) is not yet tracked, and the
  name table is capped at 64 so a name past the cap is not listed (the ones in the
  measurement are). Nothing here addresses the 2004 build; no headset.

## Next (honest)

1. **Move the sampler to the array the engine actually uses**: track the generic
   attribute state per index (`glVertexAttribPointerARB` size/type/stride/pointer +
   `glEnableVertexAttribArrayARB`), the independent `glEnableClientState` flags, and
   the bound buffers at the draw; then decode the world position bounds from the
   bound buffer (`glGetBufferSubData` offset/size from `glGetBufferParameteriv`).
   Only then can `DualRenderDrawEligible` classify these draws at all.
2. Re-run `--require-parallax` and require a non-zero per-tile disparity spread.
3. Prove (or replace) the world-span boundary from evidence (per-frame draw totals
   + the per-draw projection state).
4. Keep `StereoDualRender=0` / `StereoParallaxDibr=0` / `StereoReplayProbe=0` the
   defaults until 1–3 land.
5. Then the player-visible queue GH #63/#64/#61 and the P1 bugs #45/#49/#50/#51/#65.

## Validation

- `tools/build-all.ps1 -Config Release` clean (x86 hook `sha256 4ce0a0cbb402`,
  `522752 B`; x64 host); CTest **32/32 (x86)** and **30/30 (x64)** (the new
  `gl_entry_census` test passes); all 12 `tools/test_*.py` exit 0.
- `verify_reported_fixes.py`, `xrsim --selftest`, `scenario_tests.py --check`,
  `verify_hook_init`, `validate_addresses`, `verify_test_artifacts` PASS.
- Live: two read-only per-run shadows, `deployed_hook_matches_build` PASS (hook
  `sha256 4ce0a0cbb402`, `522752 B`); run B in-level, the census lines quoted in
  Measurement. Install untouched (read-only shadows; no deploy, no install write);
  no leftover game/host processes; live lock released.
