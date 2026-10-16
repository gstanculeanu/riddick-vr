# ADR-0454: The dark-frame `dy=±4` is an alignment-estimator artefact, not a stereo defect

- **Status:** Accepted (verdict resolved; harness-side, live-validated, no deploy)
- **Issues:** GH #72 (requirement 3), GH #68 (the iteration vehicle)
- **Date:** 2026-10-16

## Context

GH #72 requirement 3 asked the loop to investigate the `dy=±4` vertical
"disparity" the XRSIM stereo diff reports on live gameplay frames, and to
**reproduce it on a bright/contrastier frame before treating it as a stereo
defect**. It is an acceptance criterion of #72 ("the `dy=±4` question is resolved
with a stated verdict").

The symptom: a `--reach-gameplay` capture's graded window registers
`no_vertical_disparity` **FAIL** on *every* dark gameplay frame. The render path
has no vertical-offset mechanism at all -- the hook blits each eye with a
**horizontal** half-IPD source shift (`stereo_renderer.cpp`, GH #12) -- so a
vertical disparity would have to come from a resampling bug, and a real geometric
offset cannot depend on how bright the frame is.

## Decisive evidence (2026-10-16, `python tools/xrsim.py --reach-gameplay`, scratch
artifacts; eye buffers 2688x2496; RIDDICKVR_SIM=1)

One `--dump-branch any` run captured the bright startup pages and the dark 3D
gameplay of the **same blit path**, so the estimator is compared like-for-like.
Four controlled measurements pin the verdict:

1. **The bright frames are a rock-steady `dy=0` from every crop.** Frames
   600/2100 (mean luma 35-116, reference std 53-100) give `dy=0` and the *same*
   `dx` from all five disjoint crops (dy span 0), unchanged with downscale. So
   the estimator is reliable when the frame has texture, and this path has no
   vertical offset.
2. **The dark frames' `dy` is not a geometric quantity.** Frame 3600 gives
   `best_dy` **+6 / +3 / +2 / +1** at downscale **1 / 2 / 4 / 8**, and its five
   crops give **6 / 7 / 10 / 12 / 8** (span 6 px). Frame 10200's crops even flip
   the horizontal sign (`dx=+23` vs `-30/-32`). A real offset would scale cleanly
   and be the same everywhere.
3. **The optimum pins to the search edge.** With the tool's `±4` window, all 15
   dark gameplay frames report `|dy|=4` -- the edge. Widening the search does not
   find an interior minimum: frame 10200 and a *bright* frame whose registration
   failed both run to `-12` under a `±12` search. The landscape is degenerate, not
   clipped.
4. **The artefact is not confined to dark frames -- it is "the registration
   failed".** The graded run's frame 2400 is *bright* (mean 107, std 53) yet
   reports `dy=-4` **and** `dx=-48` (both on their search bounds); a widened
   search gives `dy=-12`, its crops spread **24 px** (`-10 .. +12`), and its
   residual MSE is ~6900 vs ~25 for a good frame. The whole registration did not
   lock. A brightness threshold alone would have mis-failed it.

**Verdict: artefact, not a defect.** The renderer's horizontal-only IPD shift,
the brightness-independence of a real offset, and the edge-pinned / crop- and
resolution-dependent optimum all agree. The honest report is **inconclusive**, not
"pass" and not "fail".

## Decision

1. **Two confidence gates select the verdict.** `stereo_metrics` returns the
   reference-crop **`contrast`** (stddev) and **`dy_at_boundary`** (whether the
   vertical optimum sits on the search edge). `assert_stereo` keeps
   `no_vertical_disparity` as a **FAIL** only when a non-zero `dy` is
   *quantifiable*: contrast `>= MIN_DY_CONTRAST` (**25.0**, from the measured
   separation -- dark frames 15-17, textured frames 47-100) **and** the optimum is
   interior. Otherwise it is reported as a **skip** ("inconclusive ... estimator
   artefact"), so it stays visible but never fails the run.
2. **The vertical search is widened to `±8`** (`DY_SEARCH_BOUND`). A real (if
   unlikely) offset then stays interior and *measurable*, while a degenerate
   landscape still just runs to the edge and is reported inconclusive.
3. **The verdict is in the report.** Both report paths add a `vertical_disparity`
   summary (`vertical_disparity_stats`: `ok` / `inconclusive` / `failed`), so a run
   states its vertical-disparity evidence instead of hiding it in per-frame rows.
4. **The boundary is part of the evidence.** The check detail appends
   `[search boundary]` and names the reason (low contrast / pinned to the edge).

## Evidence (this wake, live)

- **Graded gameplay-only run** (`--reach-gameplay --duration 45`, default
  `--dump-branch gameplay`, re-analysed `--evaluate-only --downscale 4`):
  `vertical_disparity` = **`{ok: 11, inconclusive: 30, failed: 0}`** -- 11 bright
  frames measure `dy=0`, 30 dark/degenerate frames are inconclusive, and **no
  frame reports a vertical defect**. Before this change the same capture failed
  `no_vertical_disparity` on 15 dark frames *and* on the bright frame 2400. The
  only remaining recorded failures are pre-existing and unrelated: `frame_budget`
  (mock cadence) and the truncated last dump (`frame_readable_12600`).
- `python tools/test_xrsim.py` **53/53 PASS** (9 new stereo/verdict tests: contrast
  + boundary reporting, a textured interior offset still fails, a bright
  edge-pinned offset is inconclusive, a low-contrast offset is inconclusive, a
  flat dark pair is inconclusive at the edge, and the summary counts);
  `xrsim.py --selftest` PASS.

## Consequences

- The graded stereo verdict on dark gameplay frames is now honest: the
  vertical-disparity axis reports *no evidence of a defect* instead of a false
  FAIL, while a textured frame with a real, interior offset is still caught
  (covered by regression tests).
- **Honest limits (not claimed).** (a) The verdict is about the *measurement*:
  this ADR does **not** claim the game's frames were checked for a real offset --
  it claims the estimator cannot quantify one there, and that the renderer has no
  mechanism for it. (b) `MIN_DY_CONTRAST` is empirical (measured separation ~3x);
  a brighter/contrastier live gameplay scene should reconfirm a measurable `dy=0`
  in the graded window. (c) An offset `>= DY_SEARCH_BOUND` (8 px) is reported
  inconclusive ("out of range"), not failed -- the tool is a "no vertical
  disparity" check, not a depth-robust offset meter. (d) The pre-existing
  `frame_budget` FAIL (the mock host follows the game's uncapped swap rate; a
  ~750 ms startup spike blows the `max` term) is **unchanged** and documented in
  ADR-0448 -- a separate #68 requirement-2 item.

## Alternatives considered

- **Gate on contrast alone.** Rejected: the graded run's bright frame 2400 has
  contrast 53 yet its registration failed (`dx` and `dy` both on their bounds, a
  24 px crop-to-crop spread). Contrast is necessary but not sufficient.
- **Gate on crop-to-crop stability.** Measured and rejected as the *primary*
  signal: at the tight `±4` search a degenerate frame pins to `-4` in most crops
  (so it looks "stable"), and a genuine small offset can be mis-measured in a
  narrow crop. The edge-pinned test is the robust, cheap one.
- **Fix the search range only** (widen `dy`). Rejected alone: the optimum then
  just runs to the new edge (measured `-12` under `±12`); the landscape is flat,
  not clipped -- so the *boundary* is the signal, and widening is only useful to
  keep a *real* offset interior.
- **Report a low-confidence FAIL but keep it failing.** Rejected: a red check that
  is known to be an artefact on the vehicle's own default gameplay branch makes
  every graded run unreadable and is exactly the "dishonest validation" the loop
  is meant to avoid.
