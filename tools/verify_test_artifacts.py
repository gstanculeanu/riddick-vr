#!/usr/bin/env python3
"""Guard: XRSIM eye-capture artifacts must never be git-tracked (GH #73 req 3).

``tools/xrsim.py`` dumps large per-eye BMPs into ``tests/xrsim/artifacts/`` for
every live run. Those are *evidence*, not source: ``.gitignore`` excludes the
whole directory and only the small, diffable ``report.json`` is committed.

Historically 26 such BMPs (665 MB on disk, only 3 unique blobs) were committed
*before* the ignore rule existed. GH #73 requirement 1 removed them from the
index with ``git rm --cached`` (``20b6995``). Requirement 3 makes a silent
re-add loud:

1. ``git ls-files tests/xrsim/artifacts`` must be **empty** -- no capture is
   tracked, regardless of extension.
2. ``.gitignore`` must still exclude ``tests/xrsim/artifacts/`` so a routine
   ``git add -A`` cannot re-track a fresh capture.
3. ``tests/xrsim/report.json`` must stay tracked and be valid JSON -- it is the
   committed diffable baseline regenerated from a live ``--reach-gameplay`` run
   (GH #73 requirement 2).

Dependency-free and headless. Exit 0 = pass, 1 = violation. If git is
unavailable the guard SKIPs (exit 0) so a non-repo runner is never broken.
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ARTIFACT_DIR = "tests/xrsim/artifacts"
IGNORE_RULE = "tests/xrsim/artifacts/"
REPORT = ROOT / "tests/xrsim/report.json"


def _git(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(ROOT), *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def main() -> int:
    if shutil.which("git") is None:
        print("SKIP verify_test_artifacts (git not available)")
        return 0
    probe = _git("rev-parse", "--is-inside-work-tree")
    if probe.returncode != 0 or probe.stdout.strip() != "true":
        print("SKIP verify_test_artifacts (not a git work tree)")
        return 0

    problems: list[str] = []

    tracked = _git("ls-files", ARTIFACT_DIR)
    if tracked.returncode != 0:
        problems.append(
            f"`git ls-files {ARTIFACT_DIR}` failed: {tracked.stderr.strip()}")
    else:
        files = [f for f in tracked.stdout.splitlines() if f.strip()]
        if files:
            shown = ", ".join(files[:5]) + (" ..." if len(files) > 5 else "")
            problems.append(
                f"{len(files)} file(s) under {ARTIFACT_DIR}/ are git-tracked "
                f"-- eye captures must stay untracked: {shown}")

    gitignore = (ROOT / ".gitignore").read_text(encoding="utf-8")
    if IGNORE_RULE not in gitignore:
        problems.append(
            f".gitignore no longer excludes {IGNORE_RULE} -- a routine "
            "`git add -A` could silently re-track a capture")

    if not REPORT.exists():
        problems.append("tests/xrsim/report.json is missing -- the diffable "
                        "baseline must stay committed")
    else:
        try:
            json.loads(REPORT.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            problems.append(f"tests/xrsim/report.json is not valid JSON: {exc}")

    if problems:
        print("[test-artifacts] FAIL")
        for problem in problems:
            print(f"  - {problem}")
        return 1

    print("[test-artifacts] ok: XRSIM eye captures untracked; "
          "report.json is the committed diffable baseline")
    return 0


if __name__ == "__main__":
    sys.exit(main())
