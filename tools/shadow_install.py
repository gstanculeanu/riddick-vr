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
# The hook resolves its config beside its own module (``vr_config.cpp``:
# ``<dir-of-opengl32.dll>\\riddick_vr.ini``, then CWD), so a shadow's real program
# directory is the only place a live run reads keys from. The keys live under the
# ``[VR]`` section; the reader is the Win32 profile API (case-insensitive).
INI_NAME = "riddick_vr.ini"
INI_SECTION = "VR"


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


def parse_ini_overrides(pairs) -> dict:
    """Parse repeatable ``KEY=VALUE`` CLI tokens into an ordered dict.

    Raises :class:`ValueError` on a token without ``=`` or an empty key. The
    value may be empty and may itself contain ``=`` (split on the first one).
    """
    out: dict[str, str] = {}
    for token in pairs or []:
        if "=" not in token:
            raise ValueError(f"ini override must be KEY=VALUE, got {token!r}")
        key, value = token.split("=", 1)
        key = key.strip()
        if not key:
            raise ValueError(f"ini override has an empty key: {token!r}")
        out[key] = value
    return out


def _section_bounds(lines: list[str], section: str) -> tuple[int, int] | None:
    """``(header_index, end_index)`` of ``[section]`` in *lines* (end exclusive).

    ``end_index`` is the next section header (or ``len(lines)``). Case- and
    whitespace-insensitive, matching the Win32 profile API. ``None`` when the
    section header is absent.
    """
    target = section.strip().lower()
    header = None
    for index, line in enumerate(lines):
        stripped = line.strip()
        if not (stripped.startswith("[") and stripped.endswith("]")):
            continue
        name = stripped[1:-1].strip().lower()
        if header is None:
            if name == target:
                header = index
        else:
            return header, index
    return (header, len(lines)) if header is not None else None


def merge_ini(text: str, overrides: dict, section: str = INI_SECTION) -> str:
    """Return *text* with each ``KEY=VALUE`` override set in *section*.

    Mirrors the reader the hook actually uses (``GetPrivateProfileIntA``): a key
    is only replaced **inside** *section* (a same-named key in another section is
    left alone), a missing key is inserted directly after the section header, and
    a missing section is appended. Every other line -- including comments -- is
    preserved verbatim, and applying the same overrides twice yields the same
    bytes (idempotent). The original line-ending style is kept.
    """
    if not overrides:
        return text
    eol = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines()

    bounds = _section_bounds(lines, section)
    if bounds is None:
        if lines and lines[-1].strip():
            lines.append("")
        lines.append(f"[{section}]")
        bounds = (len(lines) - 1, len(lines))
    header, end = bounds

    inserts: list[str] = []
    for key, value in overrides.items():
        name = key.strip().lower()
        replaced = False
        for i in range(header + 1, end):
            stripped = lines[i].lstrip()
            if stripped.startswith((";", "#")) or "=" not in stripped:
                continue
            if stripped.split("=", 1)[0].strip().lower() == name:
                lines[i] = f"{key.strip()}={value}"
                replaced = True
                break
        if not replaced:
            inserts.append(f"{key.strip()}={value}")
    if inserts:
        lines[header + 1:header + 1] = inserts

    out = eol.join(lines)
    if text == "" or text.endswith(("\n", "\r")):
        out += eol
    return out


def apply_ini_overrides(shadow_root: str | os.PathLike, overrides: dict,
                        game_system_rel: str = GAME_SYSTEM_REL) -> Path | None:
    """Write *overrides* into a shadow's program-dir ``riddick_vr.ini``.

    The hook resolves ``riddick_vr.ini`` beside its own module, so the shadow's
    **real** program directory (never a junction) is the only file this touches;
    the source install is never opened. Creates the file when the shadow has
    none. Returns the ini path, or ``None`` when there is nothing to set.
    """
    if not overrides:
        return None
    ini = Path(shadow_root) / game_system_rel / INI_NAME
    current = ini.read_text(encoding="utf-8", errors="replace") if ini.is_file() else ""
    ini.parent.mkdir(parents=True, exist_ok=True)
    ini.write_text(merge_ini(current, dict(overrides)), encoding="utf-8")
    return ini


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
                 plan: dict | None = None,
                 ini_overrides: dict | None = None) -> dict:
    """(Re)build a shadow of *source_root* at *dest_root*, with *hook* installed.

    Idempotent: an existing *dest_root* is removed first. Never writes to
    *source_root*. *ini_overrides* (``KEY=VALUE``) are written into the shadow's
    program-dir ``riddick_vr.ini`` by :func:`apply_ini_overrides`, so a live run
    can flip a hook flag (e.g. ``ViewmodelObserve=1``) without deploying. Returns
    :func:`shadow_manifest`.
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
    apply_ini_overrides(dest, ini_overrides or {})

    return shadow_manifest(source, dest, hook_path, ini_overrides=ini_overrides)


def shadow_manifest(source_root: str | os.PathLike,
                    dest_root: str | os.PathLike,
                    hook: str | os.PathLike,
                    ini_overrides: dict | None = None) -> dict:
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
        "ini_overrides": dict(ini_overrides or {}),
    }


def format_shadow(manifest: dict) -> str:
    """One-line human summary of a shadow manifest."""
    verdict = "current build" if manifest.get("fresh") else "STALE/absent"
    overrides = manifest.get("ini_overrides") or {}
    ini = ""
    if overrides:
        ini = " ini[" + " ".join(f"{k}={v}" for k, v in overrides.items()) + "]"
    return (f"shadow install {manifest['dest']} of {manifest['source']} "
            f"({manifest.get('junctions')} junction(s)) -> hook "
            f"{manifest.get('hook_sha256') or 'missing'} = {verdict} "
            f"[built {manifest.get('built_hook_sha256')}]{ini}")


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
    parser.add_argument("--ini", action="append", default=[], metavar="KEY=VALUE",
                        help="set a key in the shadow's program-dir riddick_vr.ini "
                             "(e.g. --ini ViewmodelObserve=1); repeatable. The "
                             "source install is never written")
    parser.add_argument("--json", action="store_true",
                        help="print the manifest as JSON")
    args = parser.parse_args(argv)

    manifest = build_shadow(args.source, args.dest, args.hook,
                            ini_overrides=parse_ini_overrides(args.ini))
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
