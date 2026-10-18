# ADR 0506: Native stereo (epic #57 item 1) — the world-span census names *why* the per-eye pass lost its draws (GH #76 / GH #90)

- Status: Accepted (scheduled autoloop wake 104, GH #76 / GH #90)
- Date: 2026-10-18
- Related: GH #76 (this), GH #90 (the scoped option-1 unit), GH #57 (epic /
  definition-of-done item 1), ADR-0505 (the per-eye dual-issue pass this
  instruments: it measured itself inert on its own gate with
  `scale=[unknown=675..677 near=0 mid=2 world=58]`), ADR-0504 (the per-eye camera
  is a modelview translation), ADR-0490 (the measured near-field/world vertex
  separation), ADR-0491 (the array path is complete), ADR-0481/0482 (eye-space
  client arrays with an identity `GL_MODELVIEW`)

## Context

ADR-0505 landed the native per-eye render (dual-issue each eligible world draw once
per eye, GH #76/GH #90) and measured it **inert on its own gate**: of ~735 draws
inside the world span, only 58–60 were world-scale geometry and ~675 did not decode
at all (`scale=[unknown=675..677 near=0 mid=2 world=58]`), so the eye frames were
not a usable image and playback kept the shipped shifted capture.

That count is not a diagnosis. The sampler
(`vertex_submission_observe.h::SampleClientVertices`) rejects a draw for four
distinct reasons — a non-`GL_FLOAT` vertex array, a null/absent vertex pointer,
a vertex size outside 2..4, or an unknown index type — and the per-frame log said
*how many* were rejected but not *why*. Without the rejected signature, the next
step ADR-0505 named ("extend the decode") is a guess: the engine may set the world
geometry through a path the sampler cannot see (a different vertex type, a client
array it never records, or an index type it does not know), and each possibility
has a different fix.

This unit is therefore **diagnostic and evidence-only**: it never changes which
draws the pass re-issues.

## Decision

Name the rejection. Three additions, all pure and headless-tested except the log
line:

1. **The sample carries the GL state it was handed** (`vertex_submission_observe.h`).
   `VertexSample` gains `vertexSize`, `stride`, `pointerSet` and a
   `VertexSampleReason reason` enum (`Ok`, `NonFloatVertexType`, `NoVertexArray`,
   `ZeroCount`, `UnsupportedVertexSize`, `UnknownIndexType`). `SampleClientVertices`
   sets them on every early return, so a rejection is reported as a *signature*
   (`type`/`size`/`stride`/`pointer`/`index type`/`count`) rather than a bare count.

2. **A pure span census** (`stereo_dual.h::SpanDrawCensus`). `Add(sampled)` counts
   the measured bands (`unknown`/`nearField`/`mid`/`world` via the new
   `DualSampleScaleOf`, with the previously-unnamed **mid** band — the ~2 draws per
   span between the near-field floor and the world band — made explicit), the
   refused subset (`skipped`), its undecodable part (`decodeRejected`) bucketed by
   reason, and the **first** rejection's signature. `Reset()` re-bases per frame.
   The reason tokens are stable (`DualRejectReasonName`).

   > Naming: the field is `nearField`, not `near`, because `windef.h` defines a bare
   > `near` macro (the same reason ADR-0505's members are `m_dualScaleNear`).

3. **The evidence line prints it** (`stereo_renderer.cpp`). The throttled
   `DualRender:` line keeps `scale=[unknown near mid world]` and adds
   `reject=<n> rc=[nonfloat noarray zerocount badsize badindex] sig=[<reason>
   vtype vsize stride ptr itype count]`. `OnSceneDraw` calls `m_dualCensus.Add`
   and the eligibility check now only decides whether to re-issue — the census is
   never consulted for behaviour.

## Measurement (live, read-only per-run shadow, `RIDDICKVR_SIM=1`)

Command shape: `python tools/xrsim.py --shadow-install "F:\The Chronicles of
Riddick - Assault on Dark Athena" --reach-gameplay --duration 90 --advance-menu
--advance-min-taps 8 --require-fresh-hook --require-parallax --dump-branch any
--downscale 4 --ini StereoDualRender=1`. `deployed_hook_matches_build` PASS
(shadow hook `sha256 f68012dc818e`, `521216 B` = the repo build); no deploy, no
install write.

One run reached live gameplay (menu advance stopped after 16 taps; 2 gameplay / 3
cinema of 5 captured frames; `gameplay_full_bleed_reached` fill 0.95).

The census names the rejection — and it is **one** reason, not four:

```
swap=1205 frames=519 used=30 draws=60 eligible=60 skipped=672 mirrors=1124
  active=1 coverL=0.000 coverR=0.000 size=1280x605 use=0
  scale=[unknown=672 near=0 mid=2 world=58]
  reject=672 rc=[nonfloat=0 noarray=672 zerocount=0 badsize=0 badindex=0]
  sig=[noarray vtype=0x1406 vsize=3 stride=56 ptr=0 itype=0x1403 count=12]
```

- **Every undecodable draw is `noarray`** (`nonfloat=0 badsize=0 badindex=0`,
  `zerocount=0`): the sampler was handed `GL_FLOAT` (`0x1406`), **3 components**,
  an **interleaved stride of 56 bytes** and `GL_UNSIGNED_SHORT` (`0x1403`) indices,
  but a **null client vertex pointer**. So the earlier ambiguity ("a different
  vertex type? an unknown index type?" — ADR-0505's options) is resolved: the type
  and index type are exactly the ones the sampler decodes; only the **pointer** is
  missing.
- The `mid` band is real but tiny: `mid=2` on the 60-draw frame and `mid=7` on the
  larger 950-draw frame (swap 603, `unknown=878 world=58`) — so ADR-0505's "~2
  unexplained mid draws" are now a stated number and are not the blocker.
- A non-null recorded pointer is not a per-portal fluke: a `vtype/vsize/stride` of
  `0x1406/3/56` **is** recorded (`sig`), so a hooked `glVertexPointer(3, GL_FLOAT,
  56, …)` call *did* happen, while the same draws read a null pointer. Client
  vertex-pointer state is process-wide and persisted by the observer, so the state
  the sampler reads at the world draws is a **null** pointer with that stride —
  i.e. the world geometry's array is **not established through the hooked
  `glVertexPointer` at draw time**.
- The pass is unchanged: `use=0` (both eyes black), so gameplay still presents the
  shipped shifted capture. `stereo_parallax_present` still FAILs
  (`stereo_kind=unknown`, 2 gameplay frames). `frame_budget` PASS (n=510,
  p50=3.82 ms, p95=6.32 ms).

**Leading explanation (stated as a hypothesis, not a result):** a null pointer with
a real stride is the canonical **buffer-object** idiom (`glVertexPointer` offset 0
into a bound VBO), or the array is set by a path the hook does not instrument
(`glInterleavedArrays` is exported but not tracked; only *one* module,
`07720000`, gets IAT hooks, and `glBindBuffer`/core-GL binding via
`wglGetProcAddress` is not observed by the ADR-0481 probe, which only looked for
`*ARB*` names). Both readings say the same thing operationally: the world array is
set where the draw hook cannot see it, so the sampler reads stale state.
Discriminating test (Next 1): log every hooked `glVertexPointer` call and add a
`glInterleavedArrays` hook — if neither is seen for the world draws, the array is
buffer-backed and the fix is a buffer-object path, not a decode extension.

## Consequences

- ADR-0505's next step 1 is now answerable from a single log line: the rejection
  in the world span is named (`rc=[...]` and the first `sig=[...]`), so extending
  the sampler — or concluding it must move — is an evidence call, not a guess.
- The mid band is now counted instead of being silently eligible, so the "~2
  unexplained mid-scale draws per span" of ADR-0505 become a stated number.
- Zero behaviour change: the pass is still opt-in (`StereoDualRender=0`), gated,
  and falls back to the shipped shifted capture. The census only reads.
- Limits: it is instrumentation, not the fix — native stereo is still **not**
  achieved and `--require-parallax` still fails. Nothing here addresses the 2004
  build; no headset.

## Next (honest)

1. **Find where the world array is set** (the measured `noarray`): log every
   hooked `glVertexPointer` call and add a `glInterleavedArrays` hook (both
   currently uninstrumented/untracked), and check whether the draws bind a buffer
   object via a `wglGetProcAddress`'d core `glBindBuffer` (the ADR-0481 probe only
   looked for `*ARB*` names). If the array is buffer-backed, capture the bound
   buffer + offset instead of reading client memory; if it is set through an
   uninstrumented call, hook that call.
2. Prove (or replace) the world-span boundary from evidence (per-frame draw totals
   + the per-draw projection state); the mid band is now countable for this.
3. Grade parallax: fix the harness dump gate's gameplay label (GH #72/#69) or
   measure the eye pair's per-tile disparity in the hook.
4. Keep `StereoDualRender=0` / `StereoParallaxDibr=0` / `StereoReplayProbe=0` the
   defaults until 1–3 land.

## Validation

- `tools/build-all.ps1 -Config Release` clean (x86 hook `sha256 F68012DC818E`,
  `521216 B`; x64 host); CTest **31/31 (x86)** and **29/29 (x64)** (the extended
  `stereo_dual` test passes); all 12 `tools/test_*.py` exit 0.
- `verify_reported_fixes.py`, `xrsim --selftest`, `scenario_tests.py --check`,
  `verify_hook_init`, `validate_addresses`, `verify_test_artifacts` PASS.
- Live: one read-only per-run shadow (`%TEMP%\riddickvr-shadow-20392`),
  `deployed_hook_matches_build` PASS (hook `sha256 f68012dc818e`, `521216 B`), the
  census line quoted in Measurement. Install untouched (read-only shadow; no
  deploy, no install write); no leftover game/host processes; live lock released.
