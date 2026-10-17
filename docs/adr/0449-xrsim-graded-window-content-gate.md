# ADR 0449: XRSIM graded-window content gate (blank warm-up + frozen repeats)

Status: Accepted (scheduled autoloop wake 43, GH #72 requirement 2)
Amended by ADR-0465 (GH #79): the probe is now the pure `SampleEyeImage`
(`src/host/eye_sample.h`) run over the one private staged eye pair, instead of
`SampleEyeContent` reading each shared texture separately. The decision below is
unchanged.

## Context

The XRSIM mock harness (#68) is the *iteration vehicle* for the player-visible
VR fixes: it runs the game headless (`RIDDICKVR_SIM=1`), scripts HMD/controller
poses, captures both eyes, and asserts stereo/frame-time/interaction/comfort. The
gameplay-only dump gate (#69 requirement 1, ADR-0447) narrowed the capture window
to frames the hook labels as 3D gameplay, so identical eyes on a menu/cinema
frame no longer look like a stereo defect.

But the hook's only branch bit, `cinemaMode`, **mislabels the game's static 2D
startup pages as 3D gameplay** (GH #72 requirement 1): during the multi-second
startup splash a perspective projection is loaded, so a gameplay-labelled frame
can still be the legal/attribution page or the ESRB page. Worse, the first
GL↔D3D11 interop frames are **all-black** even though they are branch-labelled
gameplay. Wake 41 measured (GH #72 requirement 2):

- a `--frames 600 --interval 60` scripted run's "gameplay" window began with two
  all-black dumps (mean luma 0) — `interop_produced_content` + `no_vertical_disparity`
  failed on frames that carry no game image at all; and
- a long `--reach-gameplay` window can contain byte-identical static splash
  frames (wake 38: 13 byte-identical eye pairs).

Neither an all-black frame nor a frozen page can validate gameplay stereo, so they
must not enter the *graded* window.

## Decision

Add a **content gate** to the gameplay-only dump window, implemented as a pure,
unit-tested extension of `src/host/dump_gate.h`.

1. **`RiddickVR::FrameContent { Live, Blank, Frozen }`.** `ShouldDumpFrame` gains
   an optional `content` argument (default `Live`, so the existing call sites and
   legacy behaviour are unchanged):

   ```cpp
   inline bool ShouldDumpFrame(bool dumpDue, bool dumpGameplayOnly,
                               bool stateRead, bool cinemaMode,
                               FrameContent content = FrameContent::Live) {
       if (!dumpDue) return false;
       if (!dumpGameplayOnly) return true;               // legacy: any due frame
       if (!(stateRead && !cinemaMode)) return false;    // branch must be known gameplay
       return content == FrameContent::Live;             // GH #72 req 2
   }
   ```

   The content gate applies **only** in gameplay-only mode (the legacy
   `--dump-branch any` cadence is unchanged), and it is subordinate to the branch
   gate (a blank *cinema* frame is still skipped by the branch rule).

2. **Host probe (`SampleEyeContent`).** Only when a dump is due *and* the branch
   gate would otherwise pass, the mock host samples **both** eye shared textures
   (the 16×16 signature comes from the left eye):
   - **mean luma over every pixel** (`sum((R+G+B)/3)/count`) →
     `Blank` when `mean <= kBlankMeanLuma (1.0)`, matching the harness's own
     `blank_gameplay` rule (mean ≤ 1). A *full-image* mean (not a sparse grid) so
     a dark-but-real frame with a small bright element is not misjudged as blank;
   - a **16×16 luma signature** (FNV-1a) compared against the last **dumped**
     frame → `Frozen` when identical (a static page repeated across captures);
   - otherwise `Live`.

   **Both-eye rule (wake 53).** The GL↔D3D11 interop warm-up and level
   transitions can leave one eye all-black while the other already has pixels, so
   a frame is `Blank` when *either sampled* eye is blank (`EyePairIsBlank`, pure
   and unit-tested). Without this a gameplay-labelled frame with an empty right
   eye was graded and produced a spurious stereo FAIL (measured: frame 1320,
   `meanL=35.6 meanR=0`).

   A probe failure leaves the frame `Live` (never blocks a capture); an eye that
   could not be sampled is never treated as blank.

3. **Reporting.** Each skip is logged as
   `DumpGate skip frame=N reason=blank|frozen`; `tools/xrsim.py` parses that into
   the report's `content_gate {blank_skipped, frozen_skipped}` key, and — for
   gameplay-only runs — adds the graded check `gameplay_window_has_content`
   (`blank_gameplay == 0`). The `live_gameplay` report key already carried the
   blank/distinct counts (ADR-0448).

## Evidence (LIVE, `RIDDICKVR_SIM=1`, scratch artifacts, deleted after)

Same command before/after the host rebuild, `--frames 600 --interval 60`
(gameplay-only default):

| | graded frames | `blank_gameplay` | stereo FAILs |
|---|---|---|---|
| **before** (old host) | 120,180,…,600 (9) | **2** | frame 120/180 all-black (`both_eyes_non_black`, `interop_produced_content`, `no_vertical_disparity`), frame 240 near-black (`no_vertical_disparity dy=-4`) |
| **after** (content gate) | 300,…,600 (6) | **0** | none — every retained frame `dx=-25 dy=0` PASS |

Host log (after): `DumpGate skip frame=120 reason=blank` / `180 reason=blank` /
`240 reason=blank`; `content_gate {blank_skipped: 3, frozen_skipped: 0}`. No live
frame was dropped (9 candidates before, 9 candidates after this window).

A full `--reach-gameplay --duration 30 --interval 300` regression run:
`gameplay_captures_present` **27 gameplay / 0 cinema**, `gameplay_window_has_content`
**PASS 0 blank of 27**, `gameplay_full_bleed_reached` **PASS (fill 1.00)**,
`live_gameplay {gameplay 27, blank 0, distinct 24}`. (The `blank_skipped: 0`
there is correct: a 300-frame interval steps over the warm-up.)

Offline: `tests/dump_gate_tests.cpp` extended (content gate: Live dumped, Blank and
Frozen skipped, legacy mode ignores content) → CTest `dump_gate_gameplay_only`
**10/10 PASS x86 and x64**; `tools/test_xrsim.py` **35/35 PASS** (+3:
`content_gate_stats`, the blank-frame window check, and the gameplay-only
scoping); `xrsim.py --selftest` PASS; `verify_reported_fixes` PASS (new #72
invariant); `--evaluate-only --downscale 4` regenerates `tests/xrsim/report.json`
(+ the `content_gate` key only). `tools/build-all.ps1 -Config Release` OK (x86
`opengl32.dll` + x64 host; no deploy).

## Consequences

- The graded window is now live, advancing 3D gameplay: the all-black interop
  warm-up is excluded and a static page repeated across captures is excluded.
- **Requirement 1 remains open**: the branch bit still labels the *first*
  letterboxed static page as gameplay — the content gate only removes *repeats*
  (the first occurrence of a new static page is dumped). Fixing the branch signal
  itself is a hook change that must be deployed to be validated live, which this
  unattended loop must not do (recorded on GH #72 with the BarScan evidence:
  splash fill ≈0.31 vs gameplay 1.00).
- **Requirement 3 is resolved (artifact).** The same `--reach-gameplay` run shows
  the `dy=±4` vertical "disparity" appears **only on very dark frames** (mean luma
  2.5–13 → `dy=+3/−4`) and is exactly `0` on every bright frame (mean 35–107). A
  real vertical misalignment would be brightness-independent and constant-signed;
  this is the stereo-alignment estimator on a flat, low-contrast MSE landscape —
  corroborated by the before-run where gating the all-black frame removed its
  spurious `dy=-4` failure.

## Files

- `src/host/dump_gate.h` (`FrameContent`, content-gated `ShouldDumpFrame`)
- `src/host/openxr_manager.{h,cpp}` (`SampleEyeContent`, `kBlankMeanLuma`,
  `m_lastDumpSignature`, the gate call, `DumpGate skip` log line)
- `tests/dump_gate_tests.cpp` (content cases) + CTest `dump_gate_gameplay_only`
- `tools/xrsim.py` (`content_gate_stats`, `gameplay_window_has_content`,
  `content_gate` report key) + `tools/test_xrsim.py` (+3)
- `tools/verify_reported_fixes.py` (#72 invariant), `src/host/README.md`,
  `tests/xrsim/report.json`

Refs: #68 (harness), #69 req 1 (gameplay-only gate, ADR-0447), #71 (reach
gameplay, ADR-0448), #72 (req 1 open, req 2 this ADR, req 3 artifact).
