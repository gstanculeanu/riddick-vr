# ADR 0515: Native stereo (epic #57 item 1) — the world draw's **transform source** is read at the draw (GH #76 / GH #90)

- Status: Accepted (scheduled autoloop wake 113, GH #76 / GH #90)
- Date: 2026-10-20
- Related: GH #76 (native stereo, epic #57 DoD item 1), GH #90 (the scoped
  option-1 unit), GH #92 (the uniform-shift pair), ADR-0514 (**whose Next 1 this
  implements**: log the bound program and the matrix sources at the world draw),
  ADR-0504 (the eye-space / fixed-function modelview route that ADR-0514 closed by
  measurement), ADR-0508 (the generic-attribute array source), ADR-0510/0511 (the
  colour-write gate and the engine-frame control), ADR-0481/0482 (the ARB
  vertex-program observer, measured silent on the 2009 target)

## Context

ADR-0514 measured the pass's own output directly: the two eye framebuffers are
**byte-identical** (`pair=[id=1 spread=0 dx=0]`, 40/41 samples) even though the
per-eye offsets differ (`offL=-0.0325`, `offR=+0.0325`). Re-issuing the same
`glDrawElements` with `translate(-off)` therefore has **no effect on the rendered
image**, so the per-eye camera is not applied where the world pass's transform
lives. The fixed-function `GL_MODELVIEW` route (ADR-0504) is closed by that
measurement.

ADR-0514's named next lead was an **ARB vertex program**: the engine resolves
`glProgramStringARB` / `glBindProgramARB` / `glProgramEnvParameter4fvARB` through
`wglGetProcAddress` (measured), and a program's transform rides its **environment
parameters**, which a fixed-function `glTranslatef` cannot touch. But the
*resolved* API is not the *bound program at a world draw*, and that had never been
read. Without it, "apply the offset in the program" is a guess.

## Decision

One evidence-only unit: name the transform source **at the span's first eligible
draw**, with no change to which draws are re-issued and no change to the presented
pair. `StereoDualRender=0` stays the default.

1. **A live (non-publishing) program read** (`vertex_program_observe.h`). The
   existing `VertexProgramObserver` only published a frame's tallies through
   `ComputeSnapshot()`, which **re-bases** them, so it could not answer "what is
   bound *at this draw*?". It gains `VertexProgramState` (`observing`,
   `programId`, `programHash`, `envParams`, `matrixAssembled`, `matrixBase`,
   `matrix[16]`) and `Current()`, a const read of the live members.
2. **Observation is on for the dual pass** (`opengl_proxy.cpp`). The observer is
   configured by `viewmodelObserve` **or** `stereoDualRender`, because the per-eye
   pass now needs the program state. It only counts/records; nothing is suppressed,
   rewritten or delayed. `OpenGLProxy::GetVertexProgramState()` exposes the live read.
3. **A pure classifier** (`stereo_dual.h`):
   `DualTransformSource` = `Unknown` (observation off — a zero id says nothing,
   never "fixed function"), `FixedFunction` (observing, no program bound), or
   `Program` (a program id is bound), plus stable tokens
   (`unknown`/`fixedfn`/`program`).
4. **The span census records it** (`SpanDrawCensus::Add` gains the program state,
   defaulted so existing callers compile). The **first eligible draw** is where the
   pass's own transform is applied, so its program signature is kept once per span:
   id, string hash, env-parameter count, matrix base and assembled flag.
5. **The evidence line prints it** (`stereo_renderer.cpp`):
   `prog=[src=… id=… hash=0x… env=… mbase=… assembled=…]`. The state is fetched only
   until the census records it, so the per-draw cost stops after the span's first
   eligible draw.

## Measurement (live, read-only per-run shadow, `RIDDICKVR_SIM=1`)

Command shape (wake 113):
`python tools/xrsim.py --shadow-install "F:\\The Chronicles of Riddick - Assault
on Dark Athena" --reach-gameplay --duration 90 --advance-menu --advance-min-taps 8
--require-fresh-hook --require-parallax --dump-branch any --downscale 4 --ini
StereoDualRender=1`. `deployed_hook_matches_build` PASS (shadow hook
`sha256 9ea4ea6f63ad`, `532992 B` = the repo build); no deploy, no install write.

The probe fires and the run named the source:

```
DualRender: swap=603 frames=23 used=23 draws=3 eligible=3 skipped=0 …
  scale=[unknown=0 near=0 mid=0 world=3] …
  sstate=[cap=1 cmask=0x0 dmask=0 drawbuf=0x8CE0 vp=0,0,1280x605 …]
  mv=[cap=1 id=1 pre=0.000,0.000,0.000 post=0.032,0.000,0.000]
  pair=[cap=1 dx=0 rows=12 spread=0 id=1]
  prog=[src=fixedfn id=0 hash=0x0 env=0 mbase=0 assembled=0]
  offL=-0.0325,0.0000,0.0000 offR=0.0325,0.0000,0.0000
ViewmodelProbe program [#300]: strings=0 binds=0 env=0 lastProg=0 strLen=0
  strHash=00000000 matrix=0 base=0
GLProcCensus name: glProgramStringARB calls=2
```

- **`src=fixedfn`** at the span's first eligible draw: **no ARB vertex program was
  bound** there (`id=0`).
- The observer's own run-wide tally agrees: **`strings=0 binds=0 env=0`** — the
  engine *resolved* the ARB names twice (`GLProcCensus … calls=2`) but never
  called them. So ADR-0514's leading hypothesis — "the transform rides the bound
  program's env parameters" — is **not supported by this run**; the program path is
  *resolved but not used* in the state measured.
- The modelview was identity before the pass's translate and carried exactly the
  per-eye offset afterwards (`pre=0.000` → `post=0.032`), so the translate *is*
  applied; the eye pair was still identical (`pair id=1`).
- **Honest limit of this measurement:** the run reached only a 3-draw span whose
  colour mask is **fully off** (`cmask=0x0`) — a depth/occlusion stage, which cannot
  colour an eye frame (ADR-0510). The rich in-level colour span measured at wake 112
  (549 draws, `cmask` turning on) was **not reached**: five consecutive live
  attempts ended early (15–19 s, the known launcher/menu flake; the game exits
  cleanly, code 0) before the level. So the probe is **exercised and populated**, but
  "the in-level world draw binds no program either" is not yet a full-span statement.

## Verdict (honest)

- The **transform source is now named at the draw** (the ADR-0514 Next 1), with a
  pure, tested classifier and a per-span signature in the evidence line.
- The ARB vertex-program hypothesis is **not supported** in the measured state: the
  program path is resolved but silent (`strings=0 binds=0 env=0`), and the span's
  first eligible draw reads `src=fixedfn`.
- Because that span was a **depth-only** stage, the result does not yet close the
  route. The distinguishing question is now narrow and concrete: **on the in-level
  colour span (the wake-112 549-draw span), is the program also unbound?** If yes,
  the fixed-function modelview is the source and the unexplained lack of effect
  lies elsewhere (e.g. the re-issued draw's vertices are already transformed
  engine-side per eye); if no, the offset must move into the program's env
  parameters. Nothing here changes what is drawn or presented.
- **No regression:** evidence only; `StereoDualRender=0` stays the default; the
  presentation path is untouched.

## Consequences

- A run now states *where* the world pass's transform lives instead of inferring it
  from the API's resolution — the exact question ADR-0514 left open.
- Enabling the observer for the dual pass is observation only: it adds counters and
  a live struct read, and cannot suppress or rewrite an engine call.
- Limits: the read is a single signature per span (the first eligible draw), not a
  per-draw program timeline; the run-wide ARB tally is re-based every snapshot; the
  in-level span was not reached this wake; nothing here addresses the 2004 build;
  no headset.

## Next (honest)

1. **Re-run with the probe on an in-level colour span** (the wake-112 shape:
   `--reach-gameplay --duration 80 --advance-menu --advance-min-taps 8
   --dump-branch any`) and read `prog=` on the 549-draw span. The menu flake must
   be worked around (retry, or a longer `--advance-interval`); only then is the
   ARB-program route closed or opened.
2. If `src=fixedfn` holds there too, the next unit is the *other* half of
   ADR-0514's measurement: confirm whether the re-issued draw's vertices are
   already camera-transformed engine-side (so a modelview translate is a no-op by
   construction) and, if so, apply the per-eye offset where that transform is
   produced.
3. Keep `StereoDualRender=0` / `StereoParallaxDibr=0` / `StereoReplayProbe=0`
   defaults.
4. Gate on the **queried** `GL_COLOR_WRITEMASK` at the draw (ADR-0511 Next 2).
5. Player-visible queue GH #63/#64/#61, then the P1 bugs #45/#49/#50/#51/#65;
   `DEFAULT_GAME_DIR` 2004-vs-2009; GH #83/#88/#89/#74.

## Validation

- `tools/build-all.ps1 -Config Release` clean (x86 hook `532992 B`
  `sha256 9ea4ea6f63ad` + x64 host). **CTest 32/32 (x86)** and **30/30 (x64)**
  (invoked from the Visual Studio CMake path, since `ctest` is not on this shell's
  PATH), including the new `stereo_dual` section 16 and the
  `vertex_program_tracker` `Current()` section.
- `verify_reported_fixes.py` (extended GH #67 invariant + new GH #76/#90/ADR-0515
  invariant), `xrsim --selftest`, `scenario_tests.py --check`,
  `verify_hook_init.py`, `validate_addresses.py`, `verify_test_artifacts.py`,
  and all 12 `tools/test_*.py` PASS.
- Live: the run quoted in Measurement (`deployed_hook_matches_build` PASS;
  read-only shadow; install untouched).
