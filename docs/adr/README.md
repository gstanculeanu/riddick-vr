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
- `0457-0500` — the #67 first-person-arms investigation (ADR-0488..0499 are mostly
  *falsifications* that close routes), the XRSIM gameplay-reach / no-evidence work
  (ADR-0497..0499), and autoloop infra (ADR-0475 single-instance guard, ADR-0500
  append-only run trace). **Head = `0500`; the next new ADR is `0501`.**

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
