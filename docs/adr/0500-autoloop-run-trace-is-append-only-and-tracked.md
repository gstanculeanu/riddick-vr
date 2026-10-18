# ADR-0500: The autoloop run trace is append-only and git-tracked (`log_state.ps1`)

- Status: Accepted (infra, GH #88; interactive session, alongside the loop retarget)
- Date: 2026-10-18
- Related: GH #88 (this), GH #83 / ADR-0475 (the single-instance guard that made
  the trace attributable), GH #87 / ADR-0498 (the previous harness-only change),
  epic #57 (why the loop exists at all)

## Context

`tools/autoloop/logs/STATE.log` is the loop's append-only run trace: the standing
prompt told every wake to "append one line", and the next wake reads its tail for
state. It sat under `tools/autoloop/.gitignore` (`logs/`, `*.log`), so it had **no
git copy**.

Measured (wake 84, 2026-10-18T05:52): an agent edited the file with a
replace-style write instead of appending — **107195 → 5262 bytes**, wakes ~1-83
gone. `git ls-files` / `git log --all` confirm that no copy existed; the loss was
unrecoverable. The same hazard had already produced ad-hoc append helpers in
`%TEMP%` (`append_state_log.py`), i.e. this was a recurring trap, not a one-off.

`tools/autoloop/STATE.md` (tracked) survived, but it is a per-wake summary, not the
run trace. The trace is the only record that pairs `start run id=<n>` with
`end run id=<n>` (ADR-0475), so losing it also loses overlap attribution.

## Decision

Make the trace durable and the overwrite impossible-in-practice, in three parts.

### 1. Track it

`.gitignore` changes from `logs/` to `logs/*` plus a final `!logs/STATE.log`, so
the directory stays ignored (per-run chatter is not source) while the single traced
file is committed and therefore recoverable.

### 2. Append only through a tracked helper (`tools/autoloop/log_state.ps1`, new)

- `-Message "<one line>"` (or pipeline) appends `[<iso8601>] <line>`.
- Before appending it compares the live size against a `.sizemark` sidecar and
  **refuses to write when the file has shrunk** — the wake-84 mistake becomes a
  loud error instead of silent data loss.
- `-Verify` asserts the first line is the log header and that the working file is
  never smaller than the `HEAD:` copy (the durable invariant).
- `-SelfTest` covers growth, both lines surviving, the size mark recording a byte
  size, the refusal after a truncation, and the header check — no repo, game or
  network needed.
- The `.sizemark` is per-machine runtime state (ignored); the git copy is the
  durable record.

### 3. Point the loop at the helper, and guard it

The standing prompt's step 6 now names `log_state.ps1` instead of the file, and
`tools/verify_reported_fixes.py` gains a GH #88 invariant (helper functions, the
`SHRUNK` guard, `Add-Content`, the `.gitignore` negation, the prompt reference).
`.github/workflows/vr-worker.yml` runs `log_state.ps1 -Verify` and `-SelfTest`.

## Consequences

- The trace is recoverable from git, and an overwrite is caught by the next append
  rather than discovered as a 20× size drop.
- A truncated log can no longer be *committed* silently (`-Verify` runs in CI).
- Honest limits: (a) the live guard only sees a shrink relative to the **last
  append this machine made** — a fresh clone has no sidecar, which is why `-Verify`
  also compares against `HEAD`; (b) `-Verify` catches a shrink only once the log
  has been committed at least once (before that `committed = -1` and it checks the
  header only); (c) a wake that ignores the prompt and writes the file directly can
  still shrink the *working copy*, but the committed copy and the sidecar both make
  that loud rather than silent.

## Validation

- `powershell -NoProfile -File tools\autoloop\log_state.ps1 -SelfTest` — **PASS**.
  The self-test earned its keep during development: an early revision read the
  *mark file's byte length* instead of its content, which defeated the shrink guard
  and failed the "append after a truncation must be refused" case; fixed with
  `Read-MarkValue` and pinned by the added "the size mark must record the byte
  size" case.
- `... \log_state.ps1 -Verify` — **PASS** (`working=64352 bytes`, header ok).
- `... \loop_guard.ps1 -SelfTest` — **PASS, 15 assertions** (GH #83 unaffected).
- `git add -n tools/autoloop/logs/STATE.log` → `add '...'` (no longer ignored).
- `python tools/verify_reported_fixes.py` — PASS, new GH #88 invariant.
- `python tools/scenario_tests.py --check` — PASS.
- No C++ changed, so no `build-all.ps1`/CTest (harness bookkeeping only).
