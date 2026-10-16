# ADR 0456: XRSIM asserts the locomotion comfort vignette is actually applied

## Status
**Accepted** — GH #68 requirement 5 (comfort rules), first slice. Host + tooling
plus a one-line hook publish fix; no game file is touched and no binary is
deployed by this change.

## Context
GH #68 requirement 5 asks the simulator harness to assert the comfort rules,
including "comfort vignette applied during scripted movement/turning". The
existing comfort group only inspected the last captured *image* for a green
chroma glitch and non-empty content — nothing checked the vignette at all.

The feature is real and already implemented:

- `src/hook/camera_hook.cpp` (Track C #9) derives a comfort level
  `vrState.comfortVignette` (0..1) from thumbstick move/turn activity — a 0.2
  lerp toward `(moveActive + turnActive) * comfortStrength` when
  `ComfortEnabled=1`.
- `src/hook/stereo_renderer.cpp` consumes it: a motion-scaled extra per-eye
  shift and, while `comfortVignette >= 0.01`, a real peripheral dimming of each
  eye FBO (`ApplyComfortVignetteToEye`) scaled by `ComfortMaxDim`.
- The level *should* be published to the host in the shared state
  (`riddick_vr_protocol.h:93`, protocol v2).

So a config/parse regression that silently disabled the vignette (e.g.
`ComfortEnabled` mis-parsed) would be invisible until someone wore a headset.

## The defect this assertion found (live, reproduced)
The field is **not published**. `IpcBridgeClient::WriteState` copies hook→host
fields *by hand* (`src/bridge/ipc_bridge.cpp`), and `comfortVignette` was the one
hook→host field of `RiddickVRSharedState` it omitted. `IpcBridgeHost::ReadState`
copies the whole struct, so the host read the never-written offset: **0.0**.

Reproduced on a live mock `--reach-gameplay` run (allowed sim exception,
`RIDDICKVR_SIM=1`, fresh `dist\riddickvr-host.exe`, stale in-game hook):

```
[Mock Host] ComfortVignette samples=12990 max=0.000 mean=0.000 above=0
            locomotion=12929 max_locomotion=0.000 threshold=0.050
```

12 929 of 12 990 frames had locomotion input published (the default mock walks
forward and turns), yet the level was 0.0 the entire run — the vignette *is*
applied in-headset (`StereoRenderer` reads the hook's *local* state, which
`CameraHook::Update` sets) but was structurally unobservable host-side.

## Decision
1. **Fix the publish.** `IpcBridgeClient::WriteState` now copies
   `m_pSharedState->comfortVignette = state.comfortVignette;` (the hook binary is
   rebuilt; nothing is deployed by this loop).
2. **Summarise, in the host.** `src/host/comfort_vignette.h` (new, pure,
   dependency-free) defines `ComfortVignetteStats`, `AccumulateComfort`
   (clamps to [0,1], tracks max/mean, counts frames above `kComfortVignetteMin`,
   and — separately — the frames on which locomotion input was published and the
   max level on those frames), and `FormatComfortVignetteLine`. The mock host
   reads `clientState.comfortVignette` back every frame, classifies that frame's
   published thumbstick input (`|lx|+|ly| >= kComfortLocomotionMag` or
   `|rx| >= kComfortLocomotionMag`), and prints, every 30 frames:
   `[Mock Host] ComfortVignette samples=.. max=.. mean=.. above=..
   locomotion=.. max_locomotion=.. threshold=..`.
3. **Assert, in the harness.** `tools/xrsim.py::parse_comfort` parses the last
   line and `comfort_vignette_check` adds a `comfort` check
   `comfort_vignette_applied`: with locomotion present it passes when the level
   rose above the summary's own threshold while moving/turning. When the line is
   absent (pre-#68 host, or `--evaluate-only` with no persisted host log) **or**
   no locomotion input was published, the check is **skipped**, never failed —
   the vignette is simply not expected there. The report carries the summary as
   `comfort_vignette`. A zero level *with* locomotion present is a FAIL: it is
   exactly the publish defect above.

## Consequences
- The harness now asserts the vignette reaches the renderer's gate without a
  headset; the check FAILed live and thereby proved the publish defect.
- **The fix is hook-side and cannot be live-validated by this loop** — the hook
  deployed in the game install is older than this change and the loop must not
  deploy, so the committed `tests/xrsim/report.json` baseline continues to show
  `comfort_vignette_applied` FAIL until one interactive deploy rebuilds +
  installs `opengl32.dll`. The same boundary GH #67/#72 req 1 hit.
- The check asserts the *published level*, which is what the renderer gates on
  (`>= 0.01`, `ComfortEnabled=1`). It does not yet measure the dimming in the
  captured eye image; an image-based peripheral-dim check is follow-up work (it
  needs an at-rest reference frame for the same scene).
- Still open in GH #68 requirement 5: snap-turn behaviour + recenter, and gross
  HUD clipping (both need hook-side signals and a deploy).
