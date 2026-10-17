#!/usr/bin/env python3
"""Run a live XRSIM against *current source* from a read-only shadow install (GH #81).

Why this exists
---------------
The game loads its OpenGL proxy from its **own directory**
(``<install>/System/Win32_x86/opengl32.dll``): implicit imports resolve
app-directory first. The autoloop must never write to the game install, so every
live XRSIM run has been exercising a *stale deployed hook* while reporting
against the current tree (GH #81). That made every deploy-gated claim
(#71/#72/#76/#79/#80) impossible to attribute: a stale-hook run cannot separate
"the fix is broken" from "the fix is not installed".

The fix is a **shadow install**: a scratch tree that mirrors the real install
with **directory junctions** (reparse points; they cross volumes and need no
admin rights) for the read-only bulk (``Content*``, ``System/GL``,
``System/Sound``) and **real file copies** only for the small program directory,
with the repository's freshly built hook dropped into the hook slot. Launching
``DarkAthena.exe`` from the shadow makes the loader pick the shadow's
``opengl32.dll`` -- i.e. the current build -- while every game asset still
resolves through the junction to the untouched install.

The source install is opened **read-only**: the plan only *lists* it, junctions
point at it, and nothing in the shadow ever writes back through a junction
during a normal XRSIM run (the hook writes its log beside its *own module*, which
is the real file copy in the shadow). ``shadow_manifest`` records the source and
the built hook identity so a run can prove which build it exercised; xrsim's
``--require-fresh-hook`` then grades it.

This is not a deploy: no byte of the install is created, modified or removed
(verified by before/after hashing of the install's program directory).

Usage
-----
    python tools/shadow_install.py --source "<install root>" --dest "<scratch>"
    # prints the shadow game dir; then:
    python tools/xrsim.py --shadow-install "<install root>" --reach-gameplay ...

``tools/xrsim.py --shadow-install <source>`` calls :func:`build_shadow` itself,
so the CLI here is for inspection/manual runs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

# The proxy the loader resolves from the executable's directory. This is the one
# file the shadow must *replace* with the repo build -- everything else mirrors.
HOOK_NAME = "opengl32.dll"
# The hook's own log lives beside its module; it is per-run evidence, never
# copied (a stale log would be mistaken for this run's output).
HOOK_LOG_NAME = "riddick_vr.log"
GAME_SYSTEM_REL = "System/Win32_x86"
GAME_EXE_NAME = "DarkAthena.exe"
# Files in the program directory that carry per-run/session state and must not be
# inherited from the install.
SKIP_FILE_NAMES = {HOOK_NAME, HOOK_LOG_NAME}


def _rel(path: Path, root: Path) -> str:
    """POSIX-style path relative to *root* (stable across platforms/tests)."""
    return path.relative_to(root).as_posix()


def shadow_plan(source_root: str | os.PathLike) -> dict:
    """Pure plan of what the shadow mirrors from *source_root*.

    Reads only directory listings -- it never opens or writes a source byte. The
    rules are structural:

    * every **directory** becomes a junction, **except** ``System`` and
      ``System/Win32_x86``, which are materialised as real directories,
    * every **file** becomes a copy,
    * the program directory ``System/Win32_x86`` is copied without the deployed
      hook and the hook log (its directory must be real for the loader override,
      and its hook slot is filled from the repo build).

    Returns a dict with ``junctions`` (``[{rel, target}]``), ``copies`` (rel
    paths), ``hook_rel``, ``game_system_rel`` and ``skipped_files``.
    """
    root = Path(source_root)
    if not root.is_dir():
        raise NotADirectoryError(f"shadow source is not a directory: {root}")
    junctions: list[dict] = []
    copies: list[str] = []
    skipped: list[str] = []

    def walk(directory: Path, real_dirs: frozenset[str]) -> None:
        for entry in sorted(directory.iterdir(), key=lambda p: p.name.lower()):
            if entry.is_dir():
                if entry.name in real_dirs:
                    # Materialised as a real directory elsewhere in the plan (the
                    # program directory must be real so the shadow's hook slot is
                    # a file we control; System is its parent).
                    continue
                junctions.append({"rel": _rel(entry, root), "target": str(entry)})
            elif entry.is_file():
                copies.append(_rel(entry, root))

    walk(root, frozenset({"System"}))            # install root: dirs -> junctions
    system = root / "System"
    if system.is_dir():
        walk(system, frozenset({"Win32_x86"}))   # System: GL/Sound -> junctions
    program = root / GAME_SYSTEM_REL
    if program.is_dir():
        for entry in sorted(program.iterdir(), key=lambda p: p.name.lower()):
            if not entry.is_file():
                continue
            if entry.name in SKIP_FILE_NAMES:
                skipped.append(_rel(entry, root))
            else:
                copies.append(_rel(entry, root))

    return {
        "source": str(root),
        "junctions": junctions,
        "copies": copies,
        "hook_rel": f"{GAME_SYSTEM_REL}/{HOOK_NAME}",
        "game_system_rel": GAME_SYSTEM_REL,
        "game_exe_rel": f"{GAME_SYSTEM_REL}/{GAME_EXE_NAME}",
        "skipped_files": skipped,
    }


def _file_identity(path: Path) -> dict | None:
    """sha256/bytes/mtime_utc of *path*, or ``None`` when it does not exist."""
    if not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    stat = path.stat()
    return {
        "name": path.name,
        "bytes": stat.st_size,
        "sha256": digest.hexdigest(),
        "mtime_utc": _iso_utc(stat.st_mtime),
    }


def _iso_utc(epoch: float) -> str:
    from datetime import datetime, timezone

    return datetime.fromtimestamp(epoch, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _make_junction(link: Path, target: Path) -> None:
    """Create a directory junction ``link`` -> ``target`` (no admin required)."""
    link.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        ["cmd", "/c", "mklink", "/J", str(link), str(target)],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise OSError(
            f"mklink /J {link} -> {target} failed ({result.returncode}): "
            f"{(result.stderr or result.stdout).strip()}")


def _resolve_hook(hook: str | os.PathLike) -> Path:
    """Accept either the built ``opengl32.dll`` or the directory holding it."""
    path = Path(hook)
    if path.is_dir():
        path = path / HOOK_NAME
    return path


def build_shadow(source_root: str | os.PathLike,
                 dest_root: str | os.PathLike,
                 hook: str | os.PathLike,
                 plan: dict | None = None) -> dict:
    """(Re)build a shadow of *source_root* at *dest_root*, with *hook* installed.

    Idempotent: an existing *dest_root* is removed first. Never writes to
    *source_root*. Returns :func:`shadow_manifest`.
    """
    source = Path(source_root)
    dest = Path(dest_root)
    hook_path = _resolve_hook(hook)
    if not hook_path.is_file():
        raise FileNotFoundError(f"built hook not found: {hook_path}")
    plan = plan or shadow_plan(source)

    if dest.exists():
        shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True, exist_ok=True)
    (dest / GAME_SYSTEM_REL).mkdir(parents=True, exist_ok=True)

    for junction in plan["junctions"]:
        _make_junction(dest / junction["rel"], Path(junction["target"]))
    for rel in plan["copies"]:
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / rel, target)
    shutil.copy2(hook_path, dest / plan["hook_rel"])

    return shadow_manifest(source, dest, hook_path)


def shadow_manifest(source_root: str | os.PathLike,
                    dest_root: str | os.PathLike,
                    hook: str | os.PathLike) -> dict:
    """Identity + layout of a built shadow, so a run can prove what it exercised."""
    source = Path(source_root)
    dest = Path(dest_root)
    hook_path = _resolve_hook(hook)
    built = _file_identity(hook_path)
    in_shadow = _file_identity(dest / f"{GAME_SYSTEM_REL}/{HOOK_NAME}")
    fresh = (built is not None and in_shadow is not None
             and built["sha256"] == in_shadow["sha256"])
    return {
        "source": str(source),
        "dest": str(dest),
        "game_dir": str(dest / GAME_SYSTEM_REL),
        "game_exe": str(dest / f"{GAME_SYSTEM_REL}/{GAME_EXE_NAME}"),
        "hook": str(dest / f"{GAME_SYSTEM_REL}/{HOOK_NAME}"),
        "built_hook": str(hook_path),
        "hook_sha256": (in_shadow or {}).get("sha256"),
        "built_hook_sha256": (built or {}).get("sha256"),
        "fresh": fresh,
        "junctions": len(shadow_plan(source)["junctions"]),
    }


def format_shadow(manifest: dict) -> str:
    """One-line human summary of a shadow manifest."""
    verdict = "current build" if manifest.get("fresh") else "STALE/absent"
    return (f"shadow install {manifest['dest']} of {manifest['source']} "
            f"({manifest.get('junctions')} junction(s)) -> hook "
            f"{manifest.get('hook_sha256') or 'missing'} = {verdict} "
            f"[built {manifest.get('built_hook_sha256')}]")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Build a read-only shadow of a Riddick install with the "
                    "repo's freshly built hook, so a live XRSIM run exercises "
                    "current source without writing to the install (GH #81).")
    parser.add_argument("--source", required=True,
                        help="install root to mirror read-only "
                             "(e.g. '...\\System' parent, containing Content/)")
    parser.add_argument("--dest", required=True,
                        help="scratch directory for the shadow (outside the repo)")
    parser.add_argument("--hook", default=str(Path(__file__).resolve().parent.parent
                                              / "dist" / HOOK_NAME),
                        help="built opengl32.dll to install in the shadow")
    parser.add_argument("--json", action="store_true",
                        help="print the manifest as JSON")
    args = parser.parse_args(argv)

    manifest = build_shadow(args.source, args.dest, args.hook)
    if args.json:
        print(json.dumps(manifest, indent=2))
    else:
        print(format_shadow(manifest))
        print(f"game dir: {manifest['game_dir']}")
    return 0 if manifest["fresh"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, FileNotFoundError) as exc:
        print(f"[shadow] FAIL: {exc}", file=sys.stderr)
        raise SystemExit(2)
