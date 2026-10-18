# Riddick VR — Architecture Decision Records (Index)

**Numbering rules** (see the "ADR discipline" rule in `tools/autoloop/prompt.md`):
numbers are strictly unique and allocated `highest + 1`; one ADR per landed unit;
every ADR names the epic #57 subsystem it advances.
`tools/verify_reported_fixes.py` enforces uniqueness.

## Curated ranges

- `0001-0050` — founding decisions: graphics interception, IPC bridge, stereo
  rendering, camera decoupling, melee/combat gestures, HUD.
- `0371-0411` — VR ergonomics: locomotion deadzone, snap/smooth turning, aim
  stabilization, reload ergonomics, NanoMED interaction.
- `0412-0456` — REA engine binding, MOS `.xmd` decode (geometry / UV / skin /
  texture), the native-hand draw path, and the first XRSIM harness generation.
- `0457-0514` - the #67 first-person-arms investigation (ADR-0488..0499 are mostly
  *falsifications* that close routes), the XRSIM gameplay-reach / no-evidence work
  (ADR-0497..0499), and autoloop infra (ADR-0475 single-instance guard, ADR-0500
  append-only run trace). ADR-0501 opens the **player-visible** milestone: GH #76
  native stereo (the interim depth-based reprojection and its depth source);
  ADR-0502 captures the depth-only clear's depth and measures it far-only (the
  world-pass-capture hypothesis is not supported); ADR-0503 scans every engine
  framebuffer object's depth and measures that **no hook-readable buffer holds the
  near world pass**, so the interim depth reprojection has no usable depth source
  (GH #76 needs the separately-scoped per-eye view instead). ADR-0504 opens that
  route: the engine submits **eye-space** client-array geometry with an identity
  modelview, so a per-eye camera is a pure modelview translation — the ADR lands
  the tested replay maths + a live provenance probe (`StereoReplayProbe=1`).
  ADR-0505 applies that translation **at the draw site**: the opt-in native
  per-eye pass (`StereoDualRender=1`) re-issues each eligible world draw once per
  eye into per-eye frames (GH #76/#90), which is epic #57 item 1's render; the
  world pass is complete at the array hook (ADR-0491), so no state capture or
  recorded vertices are needed, and a black-eye gate keeps the shifted capture.
  ADR-0506 instruments that pass's **world-span census** to name *why* it lost
  ~92% of the span's draws (ADR-0505 counted them): the measured rejection is
  `noarray` — `GL_FLOAT`/3-component/stride-56 geometry read with a **null client
  pointer** — i.e. the world array is set where the draw hook cannot see it.
  ADR-0507 answers that with measurement: the engine resolves and calls the whole
  core **buffer-object** API and `glVertexAttribPointerARB` (5.2M `GL_ARRAY_BUFFER`
  + 10.7M `GL_ELEMENT_ARRAY_BUFFER` binds and 11.1M generic-attribute pointers over
  ~9.6k in-level swaps) and calls `glVertexPointer(..., NULL)` 83% of the time, so
  ADR-0482's "no buffer object" premise is falsified and the world position data is
  **not** in the fixed-function client array the sampler reads.
  ADR-0508 acts on that: the per-eye pass classifies each draw by the **array
  source** (a per-index generic-attribute record keeping the bind-time
  `GL_ARRAY_BUFFER`), so an undecodable draw on the generic path is re-issued per
  eye (no decoded vertices needed) instead of being skipped, while every decodable
  near-field (HUD/viewmodel) draw stays excluded.
  ADR-0509 measures that re-issue further: the span capture rose to 706/735 draws
  but the eye frames were still dark, so it reads the span's **output GL state**
  (`glColorMask`/depth mask/viewport/scissor/enables — global state a re-issued draw
  inherits) once per span and the eye frames' **mean luminance**, so "the span writes
  no colour" (a depth/colour-masked stage → a span-boundary fix) is distinguishable
  from "the re-issue is wrong" (a pass fix).
  ADR-0510 acts on that per-span read: it tracks the engine's `glColorMask` (an
  `RndrGL` IAT hook — the live 2009 `RndrGL.dll` statically imports `glColorMask`)
  and gates the re-issue **at each draw** on an RGB channel being enabled (a masked
  draw re-issues correctly but can never colour an eye frame), splitting the span
  census into eligible **colour-on/off** draws so a run states whether the measured
  `cmask=0x0` span ever turns colour back on. The measured in-level span **does** (547 colour-on /
  159 colour-off of 706), so the boundary need not move and the blocker is the
  re-issue; the pass now re-issues 547 per eye, not 706.
  ADR-0511 tests that conclusion against a control: it samples the **engine's own
  presented frame** in the same swap as the eye frames, reads the eye framebuffer's own
  state (draw buffer / viewport / depth test / colour mask) and probes the modelview at
  the draw. The in-level engine frame is measured **as dark as** the eye frames
  (`cover ~0.28`, `luma ~10.3`), so the fixed `0.5` coverage floor — not an empty
  re-issue — is what keeps `use=0`; the eye FBO's own state and the per-eye modelview
  (identity + the expected translate) are both correct; and the ADR-0510 tracker is
  measured to disagree with the driver's real `GL_COLOR_WRITEMASK` (`cw off=0` beside
  `cmask=0x0`), so the gate must move to the queried mask.
  ADR-0512 acts on both of ADR-0511's readings: the **use gate** is recalibrated to the
  *relative* form ADR-0511 asked for (`DualFrameUsableVsEngine` — each eye carries at
  least half the engine frame's own coverage, plus a low absolute floor, so a dark level
  is accepted while an empty re-issue is refused) and is judged **only on a frame that
  carried the span** (the 60-swap sample tick used to overwrite a good verdict with a
  black no-span reading, flipping the pass at ~1 Hz). Live, `use` goes **0→1** on every
  sampled in-level span (the first acceptance in seven wakes) and the flicker is gone
  (`used/frames` 50.6% → 99.6%) — but the renderer's own `DisplayMode:` branch is
  **CINEMA for the whole run** (zero `FULL-BLEED` lines), so the pair is still never
  presented and the harness cannot grade it: the **presentation branch** (GH #72) is now
  the measured blocker, not the pass.
  ADR-0513 instruments the branch inputs and fixes the term GH #91 names: the branch was
  held CINEMA by a *global* desktop cursor (`GetCursorInfo` on 286/286 sampled frames,
  `menuCursorRaw=1`, `cursorAgo=0`) that the sustained-cursor detector read as a menu, so
  a whole in-level run was demoted. The menu-cursor sighting is scoped to the **game
  window's client rect** (`MenuCursor: showing=1 overWindow=0 pos=777,1619 client=1280x605`),
  plus a pure `CinemaHoldTerm` helper and a throttled `DisplayBranch:` probe. Live, the
  branch now moves (five `DisplayMode:` lines), gameplay is labelled `gameplay` (45/52),
  `gameplay_captures_present` PASSes, and `--require-parallax` grades — the pair is
  presented but still `shift-only` (`spread=0`), so the remaining stereo blocker is the
  **per-eye image** (GH #76/#90).
  ADR-0514 implements GH #92's Required 1+2: the `DualRender:` line now logs **both**
  eyes' applied offsets (`offL`/`offR`) and, on the same 60-swap tick as the gate, a
  new `SampleDualPair` reads the same rows of **both** eye FBOs and aligns them
  (`pair=[cap=.. dx=.. rows=.. spread=.. id=..]`, pure policy `DualAlignPair` in
  `stereo_dual.h`). Live (wake 112), the offsets **differ** (`offL x=-0.0325`,
  `offR x=+0.0325`) yet the two eye FBOs are **byte-identical** (`id=1`, `spread=0` on
  40/41 samples, dark and bright spans alike): the per-eye `translate(-off)` at the
  draw site has **no effect** on the rendered image, so GH #92's cause is the
  re-issue's transform (not the presentation, not the offsets). This explicitly
  **closes** ADR-0504's fixed-function "eye-space + modelview translate" route as the
  per-eye camera for the shipped world pass; the leading next lead is the **ARB vertex
  program** the engine resolves and calls (a program's transform rides its env
  parameters, which `glTranslatef` cannot touch). Evidence-only, no draw changes,
  `StereoDualRender=0` stays the default.
  **Head = `0514`; the next new ADR is `0515`.**

## Known defect: duplicate numbers (GH #74)

`0445-0449` each exist **twice** — a native-hand/viewmodel file and an XRSIM file —
from the PR #70 merge that landed two parallel implementations (GH #74):

| # | native-hand / viewmodel | XRSIM |
|---|---|---|
| 0445 | `0445-native-hand-gl-state-and-cues.md` | `0445-xrsim-per-frame-display-branch-label.md` |
| 0446 | `0446-viewmodel-window-semantics-and-trace.md` | `0446-xrsim-current-run-hook-log.md` |
| 0447 | `0447-controller-driven-engine-viewmodel.md` | `0447-xrsim-gameplay-only-dump-gate.md` |
| 0448 | `0448-head-local-strike-velocity-and-held-clicks.md` | `0448-xrsim-reach-gameplay-window.md` |
| 0449 | `0449-xrsim-gameplay-gate.md` | `0449-xrsim-graded-window-content-gate.md` |

Renumbering them is entangled with the GH #74 reconciliation (which of the two
parallel implementations survives), so the uniqueness guard allow-lists exactly
these five prefixes until #74 closes. **Do not add a sixth collision.**

## Quarantine

`0255-0370` (EVA Solar / Incinerator / Whizzer singularity spam — 116 files)
were moved with history preserved (`git mv`) to
[`quarantine/`](quarantine/) and are **not** part of the curated set.
They remain in git history for audit; do not reference them from code.
