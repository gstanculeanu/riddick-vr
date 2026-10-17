# ADR-0469: The XRSIM capture records its gate sample and the hook-publish race window

- Status: Accepted (scheduled autoloop wake 68, GH #79 host-half capstone)
- Date: 2026-10-18
- Related: GH #79, GH #68 requirement 3, GH #72 requirement 2, ADR-0465 (single staged pair), ADR-0464 (frame budget)

## Context

ADR-0465 made the mock host stage both eyes **once** and run the content gate and
the BMP write over that one private copy, so the gate sample and the saved pixels
are the same texels. That is the host contribution to GH #79 -- but it was only
*inspected*, never *asserted*: nothing in the harness would notice a regression
back to two reads, and nothing measured how often the hook's unsynchronised blit
lands inside the staging window (the residual race GH #79's hook half removes).

GH #79's XRSIM acceptance says, verbatim: *"the gate sample and the saved BMP must
agree; `capture_ms` should drop (or the capture should no longer be able to
tear)"*. Until now the harness decoded the BMPs for stereo, but never compared
them to the gate's own sample, and the hook-publish counter was never read.

## Decision

The mock host records, for **every capture attempt**, one parseable line and a
sidecar field:

```
[Mock Host] GateSample frame=N content=live|blank|frozen sampled=0|1 written=0|1 \
            luma_l=X luma_r=Y frames=<before>..<after> straddled=0|1
```

- `luma_l` / `luma_r` are the content gate's own per-eye mean luma
  (`SampleEyeImage`, plain `(R+G+B)/3`) from the **staged** copy;
- `written` is whether a BMP was actually written for that frame;
- `frames=<before>..<after>` are the hook's `RiddickVRSharedState.clientFrameCount`
  read either side of `StageEyePair`. It is the hook's per-publish counter
  (`IpcBridgeClient::WriteState`, `src/bridge/ipc_bridge.cpp:270`), so a move
  means the hook published -- and may have re-blitted the shared eye textures --
  while the pair was copied: the GH #79 race window, now **measured**;
- the per-frame `frame_####.json` sidecar carries the same record under `"gate"`.

`tools/xrsim.py` parses these (`parse_gate_samples`) and adds the
`capture_gate_matches_saved` check (report key `capture_gate`):

- recompute each written frame's mean luma from the **saved BMP** on disk
  (`saved_left_lumas`, plain `(R+G+B)/3` -- a Rec.601 `convert("L")` would weight
  the channels differently and read a different number on a tinted frame);
- **fail** if it disagrees with the gate sample beyond 1.0 luma (the two-read
  regression), or if a frame whose verdict was not `Live` was written;
- **report** the straddle rate (`N/M attempts straddled a hook frame`) without
  failing -- a straddle does not prove the pair tore, so failing on it would be a
  false negative;
- **skip** (never fail) when the host emitted no `GateSample` line: a pre-#79 host
  or an `--evaluate-only` run with no persisted host log.

## Consequences

- GH #79's XRSIM acceptance is now machine-checked, offline, and deploy-free: the
  host half can regress and be caught without a headset or a hook deploy.
- The hook-write race is quantified per run instead of inferred. A live
  `--reach-gameplay --duration 60` run measured **13/136 attempts straddling a
  hook frame** (~10%) with **9/9 written frames agreeing** and **0 non-live
  writes** -- i.e. the staged copy is self-consistent, and the residual tear risk
  is exactly the hook blit the deploy-gated `WaitForFrameConsumed` half removes.
- The straddle counter is *not* used to reject a capture: at the sim's swap rate a
  strict "counter must not move" rule would reject most frames (the wider the
  window, the more straddles), so the honest use is measurement.
- Honest limit: this proves the host's gate and its saved bytes agree; it does not
  remove the race. A torn pair can still be written, and only the deploy-gated
  hook half makes the capture atomic (ADR-0465's limit still holds).

## Validation

`tools/test_xrsim.py` adds six cases: the `GateSample` parse, agreement passes,
a saved-luma mismatch fails, a non-`Live` write fails, the straddle is reported
while a missing line skips, and the `capture_gate` report key. `tools/xrsim.py
--selftest`, all `tools/test_*.py`, `verify_reported_fixes.py` (GH #79 invariant
extended), `verify_hook_init`, `validate_addresses`, `verify_test_artifacts` and
`scenario_tests.py --check` pass; CTest is green on x86 (24/24) and x64 (22/22).
The live run above is the harness-side proof (see STATE.md wake 68).
