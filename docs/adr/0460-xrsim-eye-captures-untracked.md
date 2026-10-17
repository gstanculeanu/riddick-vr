# ADR 0460: XRSIM eye captures stay untracked; only `report.json` is committed

## Status
**Accepted** — GH #73 requirement 3. Tooling + docs only; no C++, no binary,
nothing deployed, no game file touched.

## Context
`tools/xrsim.py` (the GH #68 OpenXR simulator harness) writes per-eye BMP
captures into `tests/xrsim/artifacts/` on every live run: `test_frame_NNNN.bmp`
(the left/combined eye) and `right_NNNN.bmp`, ~25 MB each at the sim install's
resolution. They are *evidence*, not source.

`b97c4a8` ("chore(autoloop): prioritize #68 … allow xrsim sim runs") committed
**26** of them before `.gitignore` grew the rule. The damage was extreme for the
value delivered: 665 MB on disk, but only **3 unique blobs** (8× + 12× + 6× of
three frozen images) — the "13 frames" in that capture were three repeated
pictures, so the committed `report.json` described a broken harness while the
real live run passed everything but one deploy-gated check.

GH #73 requirement 1 removed the index entries (`git rm --cached -r
tests/xrsim/artifacts`, commit `20b6995`; files kept on disk, now matched by
`.gitignore:38`). Requirement 2 regenerated `tests/xrsim/report.json` from a
live `--reach-gameplay` run (landed during the #74 reconciliation wakes 52–53).
`report.json` is small and diffable, so it is the *right* committed artifact.

The remaining hole: nothing stopped a future `git add -f tests/xrsim/artifacts`
or a commit made while the ignore rule is edited from quietly re-tracking
hundreds of megabytes of captures. `.gitignore` alone is a suggestion — `git add
-f` and a pre-existing index entry both defeat it — so the rule needed an
enforced check, not just a comment.

## Decision
1. Add **`tools/verify_test_artifacts.py`** — a dependency-free, headless guard
   that fails (exit 1) unless:
   - `git ls-files tests/xrsim/artifacts` is **empty** (no capture is tracked in
     the index, whatever its extension);
   - `.gitignore` still excludes `tests/xrsim/artifacts/` (so a routine
     `git add -A` cannot silently re-track a fresh capture);
   - `tests/xrsim/report.json` exists, is tracked, and is valid JSON (the
     committed diffable baseline must stay, per GH #73 requirement 2).
   If git is unavailable or the tree is not a work tree the guard SKIPs with
   exit 0, so a lean/non-repo runner is never broken (same posture as
   `tools/test_xrsim.py`).
2. Run the guard in the CI lane (`.github/workflows/vr-worker.yml`, immediately
   after "XRSIM harness tests (GH #68)"). It is dependency-free, so it runs
   before or after the numpy/pillow install step.
3. A **GH #73 invariant** in `tools/verify_reported_fixes.py` asserts the guard
   exists, that it consults `git ls-files`, that the `.gitignore` rule is
   present, and that the CI lane invokes it — so deleting the guard or its
   wiring fails the invariant lane too.

## Consequences
- A silently re-added eye capture now fails CI and the loop's own validation run
  with the offending path listed, instead of bloating the repo unnoticed.
- The committed XRSIM baseline is exactly `tests/xrsim/report.json` plus the
  scenario/script definitions; large raw frames never enter history again.
- The guard is deliberately scoped to `tests/xrsim/artifacts/` (the GH #68 dump
  target). `tests/artifacts/` (the legacy desktop capture dir, also ignored) is
  outside this guard's remit; if that path ever needs the same protection it
  should be added here explicitly rather than by widening the glob.
- No behaviour change for `xrsim.py`: it still writes captures to disk for a
  run; only their git status is policed.
