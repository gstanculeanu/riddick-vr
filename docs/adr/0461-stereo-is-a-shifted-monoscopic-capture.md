# ADR 0461: The gameplay stereo is one global shift of a monoscopic capture; the harness now measures the disparity structure

## Status
**Accepted** — GH #76 (filed by this wake; epic #57 definition-of-done item 1).
Tooling + docs only; no C++, no binary, nothing deployed, no game file touched.

## Context
The mod has never rendered stereo. `src/hook/stereo_renderer.cpp` captures **one**
backbuffer and blits it into both eye FBOs:

- `:811-812` — capture the rendered backbuffer, then `glBlitFramebuffer` into the
  left and right eye FBOs;
- `:916-919` — cinema path, verbatim: *"flat screen: no IPD shift"* (both eyes
  blit the same source rect — correct there, see GH #69);
- `:932-960` — gameplay path, verbatim: *"GH #12: monoscopic capture has no IPD
  parallax (both eyes blit the same backbuffer). Apply a small half-IPD horizontal
  shift per eye."* The only per-eye difference is `+/- (halfIpdPx + comfortPx)` on
  the blit **source rect** (`halfIpdPx = max(1, spanW/240)`).

So every gameplay frame's eye pair is related by exactly **one global integer
horizontal translation**. The XRSIM stereo gates (`tools/xrsim.py`) only asserted
that a horizontal disparity *exists* (`horizontal_disparity_present`) and that the
eyes are not byte-identical (`not_mono_duplicate`) — both satisfied by a pure
shift — so "stereo correct" was never actually tested, and the epic's #1
definition-of-done item ("Engine renders **native stereo** (per-eye
view/projection), not a shifted blit") could pass unnoticed.

## Decision
Measure the **structure** of the disparity, not just its existence, and report it:

1. **Per-tile disparity** (`tools/xrsim.py: tile_disparities`). After the existing
   global alignment (`best_dx`), the interior is cut into a `4x4` grid (inset by
   the local search radius so no tile is edge-constrained, which would fake a
   spread). Each textured tile (`std >= PARALLAX_TILE_MIN_STD`) is re-aligned
   locally within `+/- PARALLAX_TILE_SEARCH_PX` of the global best. `dx_spread_px`
   is `max - min` of the per-tile shifts, in full-resolution pixels.
2. **Pure classifier** (`classify_stereo_kind`): `mono` (the existing `identical`
   flag), `parallax` (spread `>= PARALLAX_MIN_SPREAD_PX = 6`), `shift-only`
   (spread `<= SHIFT_ONLY_SPREAD_PX = 2`), else `unknown`. A flat frame is refused
   by the same confidence rule the vertical-disparity verdict already uses
   (`contrast < MIN_DY_CONTRAST`, ADR-0454) — a dark frame's tile shifts are
   estimator noise, never a measurement.
3. **Run verdict** (`stereo_kind_stats`): gameplay frames are authoritative (a
   cinema frame is *deliberately* flat, GH #69); unmeasurable frames are recorded
   but excluded, so a dark stretch cannot turn a shift-only run into a false
   "mixed". The verdict is written to the report as the `stereo_kind` key
   (`kind`, `frames`, `gameplay_frames`, `measurable_frames`, `spread_px_max`,
   `per_frame`).
4. **Opt-in gate** (`--require-parallax`, or a scenario's `"require_parallax":
   true`): adds the graded check `stereo_parallax_present`, which **fails** unless
   the run's `stereo_kind` is `parallax`. It is deliberately off by default: the
   shift *is* the current, documented interim design, so the default run must not
   be graded against a requirement it does not yet meet. The flag is the
   acceptance instrument for GH #76.

### Measurement (the evidence, not an inference)
Shipped build `F:\The Chronicles of Riddick - Assault on Dark Athena`, committed
captures, `--downscale 4`:

| capture | global dx (ds4) | per-tile dx (full-res) | spread | kind |
|---|---|---|---|---|
| frames 720–1020 (previous baseline) | -23 / -14 | all -92 / all -56 | **0 px** | `shift-only` |
| live `--reach-gameplay` re-run (10 frames) | -12…-23 | the 4 bright frames all agree | **0 px** | `shift-only` |

`--require-parallax` on the same captures fails, naming the gap; a synthetic
per-column disparity ramp (depth-varying) classifies `parallax` in both the
selftest and `tools/test_xrsim.py`.

## Consequences
- A flat, globally-shifted eye pair can no longer be recorded as valid stereo: the
  report says `stereo_kind: shift-only` even on a passing run, and
  `--require-parallax` turns it into a hard failure when a fix is claimed.
- The thresholds (`2` / `6` px) are empirical, like the existing
  `MIN_DY_CONTRAST`/`DY_SEARCH_BOUND`; the measurement (0 px spread over every
  textured tile) is the hard part and is order-of-magnitude clear of the gates.
- Honest limit: on the scripted run only a few gameplay frames are bright enough
  to measure the structure (the GUI intro/level frames are dark); the run verdict
  is taken over the **measurable** gameplay frames and the count is reported.
- No behaviour change for a default run: `stereo_kind` is an added report key and
  the existing gates/verdicts are untouched (ADR-0406/0407/0436 unchanged). The
  renderer itself is **not** fixed here — that is GH #76.

See also: GH #76 (the gap + required work), epic #57, ADR-0449 (graded window),
ADR-0454 (dy confidence), GH #69 (cinema eyes are correctly flat).
