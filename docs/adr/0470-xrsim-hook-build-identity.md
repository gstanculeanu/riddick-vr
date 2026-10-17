# ADR-0470: Every XRSIM run records the hook build it exercised

- Status: Accepted (scheduled autoloop wake 69, GH #81)
- Date: 2026-10-18
- Related: GH #81, GH #68 (iteration vehicle), GH #71/#72/#76/#79/#80 (the
  deploy-gated ladder), ADR-0466 (sim input routes), ADR-0467/0468 (sim input
  bridge), ADR-0465 (single staged pair), ADR-0469 (capture gate record)

## Context

A live XRSIM run does **not** exercise the source tree it is reported against:

- `tools/xrsim.py` launches the mock host from the repo
  (`dist/riddickvr-host.exe`, `HOST_EXE`), i.e. the current build;
- the game loads **`opengl32.dll` from its own `System\Win32_x86` directory** --
  a *deployed* copy. Installing a freshly built one is a **deploy**, and the
  autoloop's hard rules forbid writing the game install ("never pass `-Deploy`").

Measured on 2026-10-18 (`main` @ `3086396`, read-only):

| | path | bytes | mtime (UTC) | sha256 |
|---|---|---|---|---|
| deployed (loaded by the game) | `F:\...\System\Win32_x86\opengl32.dll` | 479232 | 2026-10-17T10:47:05Z | `8514d35adec1…` |
| repo build | `dist\opengl32.dll` | 488448 | 2026-10-17T22:11:44Z | `14867cf370f0…` |

The two differ, so every live run in this session graded a hook roughly **11
hours older** than the tree whose changes it was validating. `STATE.md` recorded
this as an honest limit ("the sim install runs the stale 12:47 hook") but nothing
*measured* it, and nothing stopped a stale-hook run from being read as a
validation of new code. That is a correctness hazard for exactly the claims the
loop most needs:

- `--sim-confirm` (GH #80) is **inert** against a hook that predates the bridge,
  so a stale-hook run cannot distinguish "the bridge is broken" from "the bridge
  is not installed";
- `--require-parallax` (#76), the `gameplay_full_bleed_reached` assert (#71) and
  the comfort-vignette check (#68 req 5) all grade the deployed hook;
- the loop's whole ladder (#71 req 1, #72 req 1, #67 step 3, #68 req 5, #79 hook
  half, #76 step 1) is "one interactive deploy" away, and the invariant that
  deploy establishes was never recorded.

## Decision

Identify the hook build in every run, and grade it only on request.

1. **Identity.** `file_identity(path)` returns
   `{path, sha256, bytes, mtime_utc}` (or `None` when absent), cached on
   `(path, size, mtime_ns)` so many analyses of one run hash each binary once.
   `deployed_hook_identity(game_dir, built_hook)` pairs the game dir's
   `opengl32.dll` with the repo build (`dist\opengl32.dll`, or `--built-hook` /
   `RIDDICKVR_BUILT_HOOK`) and reports `matches_build` as `True` / `False` /
   `None` (nothing to compare).
2. **Loud line.** `run_sim` prints one line at launch --
   `[xrsim] hook under test: <path> sha256=… <bytes> B mtime=…Z -> MATCHES|DIFFERS …`
   -- so the live log (and every pasted run excerpt) carries the truth.
3. **Report key.** Every run and `--evaluate-only` run writes `deployed_hook`
   (`{deployed, built, deployed_present, matches_build}`) into `report.json`, so
   two runs can be diffed to see whether the hook changed between them.
4. **Opt-in gate.** `deployed_hook_check(ident, require_fresh)`: the
   `deployed_hook_matches_build` check is **reported and skipped** by default --
   the mismatch is the loop's documented deploy gate, so it must not redden a
   normal run -- and **graded** with `--require-fresh-hook`, failing when the run
   did not exercise the current build (or when there is nothing to compare
   against). It is the acceptance instrument for "this live run tested current
   source", e.g. after the one interactive deploy.

## Consequences

- A live result is now self-identifying: a run that grades a stale hook says so,
  in its log, its report and (via the invariant) its review.
- The deploy-gated claims gain an executable form. `python tools/xrsim.py
  --reach-gameplay --require-fresh-hook` is the check that a deploy actually
  installed the built `opengl32.dll`; until then it fails with the measured hash
  pair instead of leaving the mismatch implicit.
- A default run's verdict is unchanged (skipped checks are excluded from
  `passed`), so this does not affect the committed baseline; the committed
  `tests/xrsim/report.json` was deliberately **not** regenerated (this session's
  sim reaches only the letterboxed startup page, so a fresh run would replace an
  83-gameplay-frame baseline with a worse one -- wakes 62/68 restored it for the
  same reason).
- Honest limit: this identifies the hook, it does not install it. The loop still
  cannot make a live run exercise current source; it can only refuse to pretend
  it did.

## Validation

`tools/test_xrsim.py` adds five cases: content-hash identity (and a same-size
edit that must not be masked by the cache), deployed-vs-built match/differ/no
compare, the check's skip-by-default and grade-on-request semantics (including
"nothing deployed" never grading green), the `evaluate()` integration with the
`deployed_hook` report data, and the flag parse. `tools/xrsim.py --selftest`, all
`tools/test_*.py`, `verify_reported_fixes.py` (new GH #81 invariant; summary
`#75-81`), `verify_hook_init`, `validate_addresses`, `verify_test_artifacts` and
`scenario_tests.py --check` pass.

Live (allowed sim exception `RIDDICKVR_SIM=1`, no deploy): `python tools/xrsim.py
--reach-gameplay --duration 20 --interval 30 --downscale 4` printed the
identity line naming the stale deployed hook, and the report carried
`deployed_hook` with both hashes and the check **SKIP** (reported-only).
`--evaluate-only --require-fresh-hook` over the same captures turned it into
**[FAIL] … run exercised a STALE hook … -- the deploy-gated change under test is
not installed (GH #81)**: the gate works from the deployed 12:47 hook, exactly
as it will pass once a deploy installs the build.
