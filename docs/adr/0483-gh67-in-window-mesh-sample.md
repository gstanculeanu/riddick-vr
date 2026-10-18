# ADR-0483 — GH #67: sample the first-person window's *largest* submitted mesh, not its first draw

- Status: accepted
- Date: 2026-10-18
- Refs: GH #67 (this — step 1, third correction), ADR-0482 (the submitted-vertex
  capture; the "first in-window draw" rule this refines), ADR-0481 (the ARB
  candidate, eliminated), ADR-0439/0446 (the depth-only-clear window), GH #68
  (the iteration vehicle), GH #63/#66 (pass identification)

## Context

ADR-0482 settled where the engine's object/view transform is — on the submitted
client-array vertices — and captured it. It sampled the **first** array draw on
each side of the first-person window. Its live result was a decisive *negative*:
the first in-window array draw is a flat, screen-space 2-D mesh
(`first=(0.5,0.5,0) min=(-0.5,-0.5,0) max=(0.5,0.5,0)`), not the arms. So
"first" picked the wrong draw and the window's other draws — the arms candidate
among them — were never looked at.

The window itself is not empty: the live pass trace shows the depth-only clear
open a window around **5–15** array draws per frame (`… d19 P? D d5 P? …` and
`D d10 C …`, i.e. two windows per gameplay frame). Only the first of each was
sampled.

## Decision

Change the in-window sample from "the first array draw" to **the window's
largest decodable array draw**, and log the draw's index count so a 4–6 vertex
quad is distinguishable from a real mesh:

- `VertexSample` gains `count` — the `glDrawElements` index count of the draw
  (the total vertex count, not the ≤ `kMaxVertexSamples` actually read).
- `VertexSubmissionObserver::NoteArrayDraw(inside, …)` keeps, for the in-window
  side, the draw with the largest `count` **among decodable draws**: a larger but
  undecodable draw never replaces a valid smaller one, and if nothing decodes the
  first draw is kept so `valid=0` is still reported. The outside side is unchanged
  (the first draw is the World control).
- `opengl_proxy.cpp` logs `count=` on the `fp`/`world` sample lines (before
  `indexType=`), so the discriminant is visible.
- `tools/xrsim.py` parses the optional `count` (`VIEWMODEL_SUBMISSION_SAMPLE_RE`),
  so a pre-0483 log still parses with `count = null`; `viewmodel_probe.submission.fp.count`
  is the new datum in the run report.

The rationale: the first-person arms are the pass's biggest mesh, while the draws
that fooled ADR-0482 (a ±0.5 screen quad) are tiny. "Largest index count" is a
cheap, deterministic, unit-testable selector that a live run can confirm or
refute.

## Reading the result

- A `fp` sample with a **large `count`** and a bbox that is *not* the ±0.5 screen
  quad (and the same every frame) ⇒ the arms mesh is in the window and its
  submitted vertices are the captured transform surface — the input GH #67 needs.
- A `fp` sample that is still the ±0.5 screen quad or a large-but-flat 2-D mesh
  ⇒ the depth-only-clear window does not bracket the arms; the signature must
  change (a different opener, or a geometry classifier), and the largest-draw
  rule is not sufficient on its own.

## Result (live, 2026-10-18)

One run: `--shadow-install "F:\…\Assault on Dark Athena" --reach-gameplay
--duration 45 --ini ViewmodelObserve=1 --ini ViewmodelTrace=1
--require-fresh-hook`, hook = the repo build (`sha256 e82a2dee526e`, 495616 B,
`deployed_hook_matches_build` PASS). XRSIM **PASS** (29 gameplay captures, fill
1.00, `comfort_vignette_applied` PASS; shadow `%TEMP%\riddickvr-shadow-2748`).

- **The largest-draw rule reaches a real mesh, not the unit quad.** 16 sampled
  `fp` lines, **all identical**:
  `first=(0.400,-0.400,0.000) min=(-0.450,-0.450,-0.050) max=(0.450,0.450,0.000)
  n=24 count=2520 indexType=5123 vertexType=5126 valid=1`. `count = 2520` is a
  real mesh, and the bbox is small (~±0.45) — not ADR-0482's ±0.5 screen quad.
- **It is camera-relative, i.e. the viewmodel signature GH #67 needs.** The
  sample is byte-identical on every sampled frame while the run's HMD yaw varied
  by 39.5°: a world mesh's submitted vertices would move with the camera, a
  camera-relative (view-space) mesh does not. The window is bracketed by a
  depth-only clear (`… d19 P? D d7 P? …` / `D d12 C …`); `arrayIn=5700` per 300
  frames ≈ 19 in-window draws/frame (12 + 7), `listIn=0` throughout.
- The World control still shows a 6-vertex screen quad with a constant `z=2032`
  (`count=6`), confirming the outside sample is a 2-D element — the `count` field
  is what distinguishes it from the 2520-vertex in-window mesh.

**Consequence for GH #67:** step 1's capture surface is now a concrete,
stable, camera-relative first-person mesh (`count = 2520`, bbox ±0.45, z ∈
[-0.05, 0]) — the input the drive/re-project step needs. The next step is to
confirm it *is* the engine's FP viewmodel (e.g. suppress/observe that pass and
check the arms disappear, or correlate the mesh with the Riddick arm surfaces)
before re-expressing it in head/controller space.

## Consequences

- The observation is sharper at no cost: the same per-draw sample, one extra
  integer on the log line, and a unit-tested selector. Still inert unless
  `ViewmodelObserve=1`, so a run without it is byte-for-byte the old behaviour.
- The `fp` sample's semantics change from "the window's first draw" to "the
  window's largest decodable draw"; ADR-0482's reading rules still apply to the
  returned geometry, but the "first" wording is superseded here.
- The pre-existing `NearPlaneDrop` opener is separately measured dead on this
  target: every projection load logs `P?` (the near plane is never recovered from
  the engine's `glLoadMatrixf` projection matrix by the `m[14]/(m[10]-1)` rule),
  so `worldNear_` is never set and the near-drop opener can never fire. This ADR
  does not change that; it is recorded so the next signature step does not
  re-try it blind.

## Honest limits

- "Largest index count" is a heuristic, not a proof: a big wall mesh drawn inside
  a contaminated window would win. It is a *measurement* selector; the live run
  decides whether it is the arms.
- The window may still open on the world pass (ADR-0482's `arrayIn ≈ 15 %`), so
  "largest in-window" can still be a world mesh if the window is contaminated.
- Only client `GL_FLOAT` arrays are decoded; a larger `GL_SHORT`/`GL_INT` draw is
  logged `valid=0` and does not win (by design).
- Nothing player-visible changes; this is step 1 (capture) only.

## Validation

- Offline: `tests/vertex_submission_observe_tests.cpp` (CTest
  `vertex_submission_observe`) +2 cases (largest-decidable wins over the first
  draw; an undecodable larger draw does not clobber a valid one) and the
  count-carrying assertion; `tools/test_xrsim.py`
  (`test_viewmodel_probe_submission_lines_are_parsed`) now asserts `count`
  parsing incl. the pre-0483 `count = null` case;
  `tools/xrsim.py --selftest` (`viewmodel_submission_parsed`) asserts the count;
  the GH #67 invariant in `tools/verify_reported_fixes.py` requires ADR-0483, the
  `s.count > insideSample_.count` selector and the `count=` log field.
  `build-all.ps1` Release (no deploy), CTest 24/24 x64 + 26/26 x86, all 12
  `tools/test_*.py`, `scenario_tests --check`, `verify_test_artifacts`,
  `verify_hook_init`, `validate_addresses`, `verify_reported_fixes` PASS.
- Live: one run recorded in `tools/autoloop/STATE.md`
  (`--shadow-install … --ini ViewmodelObserve=1 --ini ViewmodelTrace=1
  --require-fresh-hook`, hook `sha256 e82a2dee526e` = the repo build): XRSIM
  `PASS` (29 gameplay captures), 16 identical `fp` samples with `count=2520` and
  a small camera-relative bbox — see "Result (live)" above. Install untouched; no
  leftover processes; live lock released.
