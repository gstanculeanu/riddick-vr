# ADR-0465: The XRSIM capture stages both eyes once and grades/saves from that copy

- Status: Accepted (scheduled autoloop wake 62, GH #79 host half)
- Date: 2026-10-17
- Related: GH #79, GH #68 requirement 3, GH #76, ADR-0449 (content gate), ADR-0455/0464 (frame budget)

## Context

Every stereo claim the harness makes -- `both_eyes_non_black`,
`intersection`/`no_vertical_disparity`, `not_mono_duplicate`,
`stereo_kind` (GH #76) -- rests on the two eye images being **the same rendered
frame**. The OpenXR host publishes them through shared D3D11 textures that the
32-bit hook blits into on every swap. The IPC handshake is only half-wired: the
host calls `IpcBridgeHost::SignalFrameConsumed()`
(`src/host/openxr_manager.cpp`, both the compositor and mock loops), but the hook
**never calls** `IpcBridgeClient::WaitForFrameConsumed()`
(`src/bridge/ipc_bridge.cpp:308`; no hook caller exists). The hook therefore
re-blits both eye textures while the host reads them.

The mock host read each shared texture **twice** per capture:

1. `SampleEyeContent` (the GH #72 requirement 2 content gate) created its own
   staging texture, `CopyResource`d the shared texture into it, mapped it and
   computed the mean luma + frozen-page signature;
2. `SaveTextureToBMP` created a **second** staging texture, copied the same
   shared texture again, and wrote the BMP from that later read.

So both the number of exposures of the shared textures and the gap between the
*graded* read and the *saved* read were unnecessarily large. Measured in the
wake-61 `--reach-gameplay --interval 30` run (83 gameplay frames):

- `frame 540`: `both_eyes_non_black` FAIL -- `meanL=40.53 meanR=0`. The content
  probe had just sampled the right eye as lit, then the BMP write captured an
  all-black right eye: the hook cleared/rewrote the texture between the two
  reads.
- `frame 1770`: `no_vertical_disparity` FAIL with an **intra-image tear** (a
  quadrant probe: top `dy=0`, bottom/centre `dy=8`) -- the host read the right
  texture while the hook rewrote part of it.
- `frame 810`: a **global** vertical offset (top -5/-6, bottom -3, centre -4 at
  downscale 4) -- the two eyes were read at different camera times.
- The capture path takes ~112 ms mean (GH #78), while the hook's cadence is
  ~2 ms in sim, so the race window is large.

These false stereo defects also corrupt GH #76's `stereo_kind` (5 frames were
labelled `parallax`, which is impossible for the current monoscopic
+/- half-IPD blit).

## Decision

The clearer of the two fixes GH #79 offers is the **host-side** one, because it
is validatable in the loop (the hook half needs a deploy the scheduled loop must
not perform). The mock host now:

- **stages both eyes once, back-to-back** (`StageEyePair`,
  `src/host/openxr_manager.cpp:1357`): create both private staging textures,
  `CopyResource` left then right immediately (`:1376`), then map both. The pair
  is one GPU-time sample, so a shift-only pair no longer picks up a spurious
  global vertical offset; and the shared textures are exposed to the hook's
  concurrent blit for exactly two copies;
- **runs the content gate and the BMP write over that same private copy**: the
  sampler is the pure `SampleEyeImage` (`src/host/eye_sample.h`) and the BMP
  layout is the pure `MakeBmpHeader` / `ConvertRgbaRowToBgra`
  (`src/host/bmp_writer.h`); `WriteBmpFromMapped`
  (`src/host/openxr_manager.cpp:1389`) writes from the mapped private rows, so
  the graded pixels and the saved pixels are the same texels;
- **reports a staging failure instead of a second read**: if the pair cannot be
  staged, the frame the branch wanted is logged as `Capture skipped frame=N (eye
  staging failed)` and not written, rather than re-reading the shared texture.

`SaveTextureToBMP` (and its per-eye staging) is removed; the per-capture shared
texture reads drop from four to two, and they happen in one adjacent pair.

## Consequences

- The gate probe and the saved BMP can no longer be different frames, which is
  the host contribution to GH #79; the residual tear risk is the hook's
  unsynchronised blit, which only the deploy-gated `WaitForFrameConsumed` half
  removes.
- The capture is slightly cheaper: two `CopyResource` + two `Map` per dump
  instead of four copies (the GH #78 frame-budget exclusion still subtracts the
  whole capture block, so the budget is unaffected either way).
- The BMP on-disk layout and the luma/signature maths are now unit-testable
  without a GPU (`tests/eye_capture_tests.cpp`); the previous inline copies had
  no test.
- Honest limit: this does not make the capture *atomic*. A hook blit that lands
  between the two adjacent `CopyResource` calls still yields a torn pair, and the
  committed report can remain red on those checks until the hook half is
  deployed. `Capture skipped frame=.. (eye staging failed)` is the only new
  failure mode and is reported, never silent.

## Validation

`tests/eye_capture_tests.cpp` (CTest `eye_capture_helpers`, x86 + x64) pins the
54-byte BMP header, the top-down orientation, the R/B row swizzle, the padded
`RowPitch` handling, the alpha-invariance of the luma mean/signature, and that
the saved row's texels are the sampled row's texels. `tools/verify_reported_fixes.py`
guards that the host stages the pair back-to-back and no longer references
`SampleEyeContent` / `SaveTextureToBMP`. A live `--reach-gameplay --interval 30`
run re-measures `capture_ms` and the stereo gates (see STATE.md wake 62). The
hook-side `WaitForFrameConsumed` remains deploy-gated.
