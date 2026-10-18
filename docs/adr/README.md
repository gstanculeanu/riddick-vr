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
- `0457-0508` - the #67 first-person-arms investigation (ADR-0488..0499 are mostly
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
  **Head = `0508`; the next new ADR is `0509`.**

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
