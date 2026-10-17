# ADR-0473: Synthetic `.xmd` test models are written to an isolated temp dir

- Status: Accepted (scheduled autoloop wake 72, GH #82)
- Date: 2026-10-18
- Related: GH #82, GH #84, ADR-0418 (SKELETON NODES layout), ADR-0471 (XRSIM
  read-only shadow install)

## Context

`tools/test_xmd_pose.py` and `tools/test_xmd_fk.py` build a synthetic
`MOS DATAFILE2.0` model, write it with `test_mos_xmd._write_tmp` and then run the
**corpus** check over `os.path.dirname(path)`: `xmd_pose.check(...)` and
`xmd_fk.main(["--inventory", root, "--check"])`, both of which `os.walk` the root
recursively.

`_write_tmp` used `tempfile.mkstemp()` with no `dir`, i.e. the **OS temp root**.
So the corpus scan's input was whatever happened to be in `%TEMP%` — and after
wake 70/71 an XRSIM **shadow install** (ADR-0471, `--shadow-dir`) left a full
game tree there. The scan swept the installed 2004 game's 1285 `.xmd` models and
`xmd_pose.check` / `xmd_fk.check` call `Xmd.skeleton()` *outside* their
`try/except`, so the first model whose `NODES` chunk is not the 34-byte skeleton
aborted the run with `GeometryError: NODES size 228 != 19 * 34`.

That made both tests **red on `main`** (GH #82), but only on a machine whose
`%TEMP%` contained unlucky files — the failure was environment-dependent, not a
stale-fixture failure, and it reappeared as soon as a live harness ran. The
underlying real-asset tool bug (`skeleton()` picking the first `NODES`, not the
`SKELETON` one) is tracked separately as GH #84.

## Decision

Synthetic models are written under **one per-process directory that the test
owns**, never the OS temp root:

- `test_mos_xmd._write_tmp(blob, directory=None)` writes into
  `directory or _TMP_ROOT`, where `_TMP_ROOT = tempfile.mkdtemp(prefix="riddickvr-xmd-tests-")`
  and is removed at process exit (`atexit`). A caller that scans
  `os.path.dirname(path)` can then only ever see models *this process* wrote.
- A regression test (`test_write_tmp_is_isolated`) pins both properties: the
  default directory is `_TMP_ROOT` and is **not** `tempfile.gettempdir()`, and an
  explicit `directory` is honoured.

The scanning tests are left as they are (`dirname(path)` now resolves to the
isolated dir); no test needs to know the path.

## Consequences

- `test_mos_xmd.py`, `test_xmd_pose.py` and `test_xmd_fk.py` are deterministic
  regardless of what else is in `%TEMP%` (shadow installs, other agents' temp
  files). The two corpus checks now assert on the synthetic model the test built,
  which is what they meant to do.
- A test that *wants* to check the real install must pass an explicit `dir` and
  will still hit GH #84 until that is fixed — the isolation does not hide the
  real-asset mismatch, it just stops the synthetic tests from being collateral.
- Cost: one extra temp directory per test process, removed on exit; the previous
  code already leaked the temp files it wrote.

## Validation

- `python tools/test_mos_xmd.py` (incl. the new `test_write_tmp_is_isolated`),
  `tools/test_xmd_pose.py` and `tools/test_xmd_fk.py` all PASS; the full
  `tools/test_*.py` set is 11/11 green.
- `tools/verify_reported_fixes.py`, `tools/verify_test_artifacts.py`,
  `tools/scenario_tests.py --check`, `tools/xrsim.py --selftest` PASS.
- No C++ changed, so no `build-all`/CTest (documented).
