# ADR 0508: Native stereo (epic #57 item 1) — the per-eye pass classifies draws by the **array source** the engine actually uses, so the generic-attribute world geometry is re-issued per eye (GH #76 / GH #90)

- Status: Accepted (scheduled autoloop wake 106, GH #76 / GH #90)
- Date: 2026-10-18
- Related: GH #76 (this), GH #90 (the scoped option-1 unit), GH #57 (epic /
  definition-of-done item 1), ADR-0507 (**whose Next 1 this implements**: the world
  geometry is submitted through `glVertexAttribPointer[ARB]` into buffer objects and
  the fixed-function `glVertexPointer` the sampler reads is null *by design*),
  ADR-0506 (the span census: every undecodable world draw is `noarray`),
  ADR-0505 (the per-eye dual-issue pass this unblocks), ADR-0504 (the per-eye camera
  is a pure `GL_MODELVIEW` translation), ADR-0481/0482 (the ARB vertex-program path
  is silent, so the fixed-function modelview applies)

## Context

ADR-0507 measured the array source: the engine resolves and calls the whole core
buffer-object set plus `glVertexAttribPointerARB` (~695 calls/frame),
`glEnableVertexAttribArrayARB` (~44/frame) and the `glMultiTexCoord*ARB` set, and it
calls `glVertexPointer` with a **null** pointer ~83% of the time. Its verdict was
that the per-eye pass (`StereoDualRender`, ADR-0505) captures only ~60 of ~735
span draws because the array sampler reads the *fixed-function client array*, which
the engine leaves null on exactly the draws that matter.

The crucial consequence was not acted on: **re-issuing a draw does not need decoded
vertices**. The whole point of dual-issue (ADR-0505) is that the complete
fixed-function *and* generic-attribute state is current at the `glDrawElements`
hook, so re-issuing the identical call with a translated `GL_MODELVIEW` reproduces
the geometry with no state capture and no copied vertices. The decoded sample was
only ever used to **classify** a draw as 3-D world vs. screen-space/HUD. So the fix
is to classify by the array *source*, not by the decoded bounds.

## Decision

Let the per-eye pass treat an **undecodable draw that used the generic-attribute
path** as world geometry and re-issue it (ADR-0507 Next 1, first half: track the
per-index generic-attribute state — the second half, decoding the position bounds
out of the bound buffer, was *not* needed to re-issue and is left as a future
refinement if the coarse classification proves too wide).

1. `gl_entry_census.h` (pure, headless-tested): `AttributePathCensus` is now a
   **per-index** record (`kGenericAttribMax = 16`) — `enabled[]`, `size[]`,
   `type[]`, `stride[]`, `pointerSet[]` and, decisively, `arrayBufferId[]`: the
   `GL_ARRAY_BUFFER` binding **in force when the pointer was set** (GL keeps one
   buffer per attribute, so the live binding at the draw is a different attribute's
   buffer). `NoteEnableArray(index)`/`NoteDisableArray(index)` keep the enabled
   state; `AnyEnabled()`, `PositionAttribIndex()` (the lowest enabled
   `GL_FLOAT`/≥3-component index) and `PositionBufferId()`/`PositionBufferBacked()`
   answer the draw-time question.
2. `vertex_submission_observe.h`: `VertexSample` carries
   `genericAttributeEnabled` and `genericPositionBufferId`, stamped by the proxy at
   the draw (the pure sampler cannot know them).
3. `stereo_dual.h`: `DualRenderDrawEligible` becomes — a *decodable* draw is
   eligible iff it is not near-field (unchanged), an *undecodable* draw is eligible
   iff it used the generic-attribute path. `SpanDrawCensus` gains `genericEligible`
   / `genericBufferBacked` and the first such draw's `enabled`/`buffer` signature.
4. `opengl_proxy.cpp`: the `glVertexAttribPointer` wrapper records the bind-time
   array buffer; `glEnableVertexAttribArray`/`glDisableVertexAttribArray` wrappers
   keep the per-index enabled state; `SampleSubmittedArray` stamps the two new
   `VertexSample` fields; the `GLProcCensus` and `DualRender` evidence lines print
   the new state (`attrLive=…`, `gen=/gbacked=/gsig=`).

The change is opt-in (`StereoDualRender=0` stays the default), the fail-safe
coverage gate (ADR-0505) still refuses an unusable per-eye frame, and every
decodable near-field draw (HUD/viewmodel) is still excluded, so the screen-space
path is never eye-translated.

## Evidence

Offline (this wake): `build-all.ps1` Release clean (x86 `opengl32.dll` + x64 host);
CTest both arches; the extended `gl_entry_census` / `stereo_dual` tests pass; all
`tools/test_*.py` and the harness self-tests pass. Live (RIDDICKVR_SIM=1, read-only
per-run shadow, `StereoDualRender=1`): see the wake record in
`tools/autoloop/STATE.md` / `tools/autoloop/logs/STATE.log` for the measured
`gen=`/`gbacked=` counts, the coverage gate outcome and whether the modelview
translation is visually applied.

## Limits (honest)

- The classification is a **measured hypothesis**: a generic-attribute draw is
  *assumed* to be 3-D world geometry. If the HUD/viewmodel also uses the generic
  path without decoding, it would be misclassified; the near-field exclusion on
  *decodable* draws is the guard, and the live evidence line is what decides.
- The position bounds are **not** decoded from the bound buffer; a future refinement
  can use `glGetBufferSubData`/`glMapBuffer` for a bounds-based classifier.
- Nothing here changes the default: shift-stereo remains until a headset run grades
  a usable per-eye pair.
- The 2004 build is untouched; no headset.
