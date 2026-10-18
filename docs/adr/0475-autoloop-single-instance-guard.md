# ADR-0475: The autoloop is single-instance (liveness guard, attributed runs, per-run harness isolation)

- Status: Accepted (scheduled autoloop wake 74, GH #83)
- Date: 2026-10-18
- Related: GH #83 (this), GH #68 (iteration vehicle), GH #74 (a prior collision of
  two parallel implementations), GH #71 / GH #80 (the unit the interrupted wake was
  landing), ADR-0471 (XRSIM read-only shadow install)

## Context

The autoloop fires a wake roughly every 10 minutes with

```
opencode run --auto --standalone --model <m> --title "RiddickVR autoloop <stamp>" <prompt>
```

and the standing prompt makes the agent the **sole writer** of one checkout. On
2026-10-18 that premise broke: the `Local\RiddickVRAutoloop` mutex in
`run-autoloop.ps1` guards only the *runner* process, and the runner of the 00:59
wake was killed by a harness restart **while its `opencode run` child kept
working**. The mutex was therefore free when the 01:09 scheduled wake started, and
two agents ended up in `C:\Users\GigelCastel\riddick-vr` at once.

Measured damage (from the GH #83 record, `tools/autoloop/logs/STATE.log` and the
two sessions' logs):

1. **Torn build** — one session's `tools\build-all.ps1 -Config Release` compiled
   the x64 tree but failed at the final `Copy-Item dist\riddickvr-host.exe`
   because the other session's live run held the file.
2. **Rebuilt shadow under a running run** — both sessions used the *shared default*
   shadow directory (`%TEMP%\riddickvr-shadow`); one `build_shadow` deleted the
   other live run's `riddick_vr.log` mid-run, and a second `DarkAthena.exe` was
   left alive while an "isolated" run ran, so that run's frames were **partly
   borrowed** from the other instance.
3. **Staggered unit** — the killed wake left an unlanded change set (the sim input
   bridge observability) that the next wake had to recover from the worktree
   rather than from git.
4. **Unattributable overlaps** — `STATE.log` had a `start run` with no matching
   `end run`, so an overlap could only be *inferred* by hand.

## Decision

Three bounded, checkable rules, all offline and all in the loop's own tooling.

### 1. Liveness, not just a mutex (`tools/autoloop/loop_guard.ps1`, new)

Dotted into the runner. Before launching, the runner scans the live process list
(`Get-CimInstance Win32_Process`) for a process whose command line carries the
autoloop marker (`RiddickVR autoloop`) **and** this checkout, excluding itself
(`Test-LiveWakeProcess`, pure). A hit logs
`skip: live wake pid=<n> (previous runner gone; mutex free) - GH #83` and exits 0
instead of starting a second agent. The mutex is kept as the fast path; the scan
is what covers an orphaned child.

The guard never *kills* anything: the loop must not touch processes it did not
start (the same input-safety rule that forbids desktop input injection), and a
stale lock is handled by takeover (below) rather than by reaping.

### 2. Attributed runs

The lock file `tools/autoloop/loop.lock.json` (atomic write: temp + `Move-Item`)
records `runId`, `checkout`, `runnerPid` and `started`; `STATE.log` replaces the
bare `start run` line with
`start run id=<n> checkout=<path> pid=<pid>` and closes it with
`end run id=<n> exit=<c> log=<file>`. The `end run` line is emitted from
`finally`, so every exit path (including the skip) closes the record, and the run
id is `1 + (number of start lines)`, never going backwards even if a stale lock
holds a higher id. The runner removes only a lock **it** wrote, so a skipped wake
never erases a live peer's attribution.

### 3. Per-run harness isolation (`tools/xrsim.py`)

- **`--shadow-install` scratch is per run.** The default changes from the shared
  `%TEMP%\riddickvr-shadow` to `%TEMP%\riddickvr-shadow-<pid>`
  (`default_shadow_dir`), so no run can rebuild another run's tree. An explicit
  `--shadow-dir` still wins.
- **Single-live-run lock.** A real run takes `%TEMP%\riddickvr-live.lock`
  (`acquire_live_lock`/`release_live_lock`); a second live run *refuses* with
  exit 3 (`--ignore-live-lock` overrides deliberately). The decision is pure
  (`live_lock_decision`): `free` (no/unusable lock), `owned` (ours), **`stale`**
  (recorded pid is gone → taken over) or `held` (live peer → refuse). Because a
  dead holder is taken over and a non-owner never deletes a lock, a killed run
  cannot wedge the loop. Release is registered with `atexit`, and the lock state
  is recorded in the run report as `live_lock`.

## Consequences

- The two measured collision classes that are *mechanical* — a shared shadow
  rebuild and two live game/host instances — are now prevented by construction,
  not by documentation.
- An overlap caused by a runner being killed is now *detected* (skip line names
  the peer pids) instead of producing a torn build.
- Every wake is attributable from one `STATE.log` line.
- Honest limits: (a) the liveness scan is a best-effort process scan — it matches
  on the marker + checkout in a command line, so a *different* agent invocation
  (a manual `opencode` session with no autoloop title) is still not detected;
  (b) the runner still takes no action against an existing live peer beyond
  skipping (a wake may therefore be missed while a long wake runs — acceptable at
  a 10-minute cadence); (c) the guard is PowerShell, so its pure decisions are
  covered by `loop_guard.ps1 -SelfTest` rather than by the Python suite.

## Validation

- `powershell -NoProfile -File tools\autoloop\loop_guard.ps1 -SelfTest` —
  **PASS, 15 assertions** (liveness selection incl. "not its own peer"; run-id
  arithmetic incl. a higher id in a stale lock; lock round-trip; a half-written
  and an empty lock read as no lock; idempotent removal).
- Live detection, read-only, no launch: dot-sourcing the guard and running
  `Test-LiveWakeProcess (Get-AutoloopProcesses)` on this machine returned
  **exactly 1** hit for this checkout — the currently-running wake's
  `opencode.cmd run … --title RiddickVR autoloop …`, i.e. the guard sees the live
  wake that a second scheduled run must refuse to join.
- `python tools/test_xrsim.py` — 5 new cases
  (`test_default_shadow_dir_is_per_run`, `test_live_lock_decision_states`,
  `test_live_lock_refuses_while_held_and_takes_over_stale`,
  `test_live_lock_read_tolerates_partial_and_missing`,
  `test_live_lock_and_shadow_flags_parse`); full suite PASS.
- `python tools/xrsim.py --selftest` — +3 cases (`live_lock_free_held_stale_owned`,
  `shadow_dir_is_per_run`, `live_lock_round_trip`) PASS.
- `tools/verify_reported_fixes.py` — new GH #83 source invariant.
- No C++ changed, so no `build-all.ps1`/CTest; no live game run (the change is
  harness bookkeeping and cannot affect a run's rendering).
