# ADR-0464: The XRSIM frame budget measures rendered frames, not harness capture

- Status: Accepted (scheduled autoloop wake 61, GH #78)
- Date: 2026-10-17
- Supersedes/amends: ADR-0455 (p50/p95 frame-pacing assertion)
- Related: GH #68 requirement 2, ADR-0448, ADR-0454

## Context

GH #68 requirement 2 asks the simulator harness to assert a 90 Hz frame-time
budget. The mock host samples the delta between its own loop iterations while
the game is in 3D gameplay and prints `FrameBudget ... p50_ms=.. p95_ms=..`;
`tools/xrsim.py` fails the run when p50 or p95 exceed 11.11 ms.

The host loop also performs the harness's **artifact capture**: on every
`--dump-interval` frame it writes two full-resolution BMPs (`test_frame_####.bmp`
and `right_####.bmp`, 2496x2688 x 32 bpp = 26.8 MB each at the simulated eye
size) plus `display_branch.log`, `eye_frames.log` and `frame_####.json`. That
work runs on the same thread and inside the same loop iteration whose delta is
sampled. At ~54 MB of writes per captured frame the capture takes tens of
milliseconds -- occasionally >100 ms -- so the sample did not describe a rendered
frame at all.

Observed live (`tests/xrsim/artifacts/host.log`, 90 s `--reach-gameplay
--interval 30`):

```
n=300 p50=3.56 p95=5.42
n=390 p50=3.62 p95=16.89
n=810 p50=3.51 p95=104.83
```

`p50` (3.5 ms ≈ 285 FPS) is the game/hook; the p95 spikes track the capture
cadence and `over_budget` incremented by 1-2 per 30-frame window. The committed
`tests/xrsim/report.json` baseline recorded `p50=104.89 p95=179.00`, leaving the
harness permanently red for a reason that is not a sustained render regression.

## Decision

The frame-time sample is the **rendered-frame cadence**. The host measures the
duration of its artifact-capture block and subtracts it from the next loop delta
before sampling:

- `FrameTimeExcludingHostWork(loopDeltaMs, hostWorkMs)` in
  `src/host/frame_time_stats.h` (pure, unit-tested): `max(0, delta - work)`.
- A delta that is entirely host work (net <= 0) is **not** recorded: there was
  no rendered frame in it, and recording 0 ms would invent a fast frame.
- The raw `dtMs < 1000` idle-gap guard is unchanged, so an idle stall between
  gameplay frames is still ignored.
- Real stalls (level load, shader compile, GC) are not host work and remain in
  the sample, so `max_ms` and `over_budget` still surface them.
- The excluded cost is **reported**, not hidden: every dump logs
  `[Mock Host] ArtifactCapture frame=N capture_ms=X`.

The subtraction is applied only to the artifact-capture block. The capture
duration of a *skipped* dump (content gate) is included too, because it is the
same harness work.

## Consequences

- A clean run now reads p50 ≈ 3.5 ms / p95 ≈ 5 ms and passes, and the committed
  report baseline becomes a real regression signal.
- A harness change that makes captures slower can no longer masquerade as a
  game regression -- and cannot hide one, because the game's own frame time is
  unchanged by the subtraction.
- The budget remains a *host-observed* cadence: it cannot separate the hook's GL
  work from the game's own render cost, and it does not measure the OpenXR
  compositor's `xrWaitFrame` pacing (that needs a runtime). Unchanged limits.

## Validation

`tests/frame_time_tests.cpp` pins the helper (subtraction, no-work pass-through,
clamp to 0), and reproduces the old failure (a 3.5 ms cadence with 104 ms
captures on >5% of frames fails p95) and the fix (the same series passes once
the measured capture is removed) while the sustained-regression fixture still
fails. A live `--reach-gameplay --interval 30` run under the fix regenerates
`tests/xrsim/report.json` (see STATE.md wake 61 for the numbers).
