#!/usr/bin/env python3
"""Self-contained tests for ``tools/shadow_install.py`` (GH #81).

The shadow install is the route that lets a live XRSIM run exercise the *current*
hook build without writing to the game install (implicit imports resolve the
app directory first, and the loop must never deploy). These tests prove the
properties the harness relies on, headlessly and without a game:

* the plan mirrors directories as junctions and files as copies, materialising
  the program directory as real files,
* the deployed hook and the stale hook log are *never* carried over,
* a built shadow really does route ``opengl32.dll`` to the repo build while game
  assets resolve to the source (read through the junction),
* rebuilding is idempotent and never modifies the source install, and
* the manifest flags a shadow whose hook does not match the build (so
  ``--require-fresh-hook`` can never be satisfied by accident).

Run headlessly:  python tools/test_shadow_install.py
Exit code 0 = pass, 1 = fail. The junction-dependent tests SKIP off Windows.
"""
from __future__ import annotations

import hashlib
import json
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import shadow_install  # noqa: E402

IS_WINDOWS = os.name == "nt"


def _write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def _make_source(root: Path) -> None:
    """A synthetic install: the bulk under junctions, the program dir real."""
    _write(root / "BuildInfo.txt", b"build-info\n")
    _write(root / "Environment.cfg", b"COMMANDLINE=\n")
    _write(root / "Content/Models/Riddick_H01.xmd", b"MOS DATAFILE2.0" + b"x" * 64)
    _write(root / "ExtraContent/patch.bin", b"patch")
    _write(root / "System/SystemGammaRamp.xrg", b"gamma")
    _write(root / "System/GL/VP.xrg", b"vp")
    _write(root / "System/Sound/DSPConfig.xrg", b"dsp")
    _write(root / "System/Win32_x86/DarkAthena.exe", b"MZ game")
    _write(root / "System/Win32_x86/RndrGL.dll", b"MZ renderer")
    _write(root / "System/Win32_x86/MCCDyn.dll", b"MZ mcc")
    # The deployed (stale) hook and its stale log must never be carried over.
    _write(root / "System/Win32_x86/opengl32.dll", b"STALE DEPLOYED HOOK")
    _write(root / "System/Win32_x86/riddick_vr.log", b"stale log from 12:47")


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def test_plan_mirrors_dirs_as_junctions_and_files_as_copies(d: str) -> None:
    source = Path(d) / "src"
    _make_source(source)
    plan = shadow_install.shadow_plan(source)
    rels = sorted(j["rel"] for j in plan["junctions"])
    assert rels == ["Content", "ExtraContent", "System/GL", "System/Sound"], rels
    # System and System/Win32_x86 must NOT be junctions (they are real dirs).
    assert "System" not in rels and "System/Win32_x86" not in rels
    copies = sorted(plan["copies"])
    assert copies == [
        "BuildInfo.txt",
        "Environment.cfg",
        "System/SystemGammaRamp.xrg",
        "System/Win32_x86/DarkAthena.exe",
        "System/Win32_x86/MCCDyn.dll",
        "System/Win32_x86/RndrGL.dll",
    ], copies
    assert plan["hook_rel"] == "System/Win32_x86/opengl32.dll"
    assert plan["game_system_rel"] == "System/Win32_x86"
    assert plan["game_exe_rel"] == "System/Win32_x86/DarkAthena.exe"


def test_plan_skips_deployed_hook_and_stale_log(d: str) -> None:
    source = Path(d) / "src"
    _make_source(source)
    plan = shadow_install.shadow_plan(source)
    assert sorted(plan["skipped_files"]) == [
        "System/Win32_x86/opengl32.dll",
        "System/Win32_x86/riddick_vr.log",
    ]
    blob = json.dumps(plan)
    assert "riddick_vr.log" not in json.dumps(plan["copies"])
    assert "opengl32.dll" not in json.dumps(plan["copies"])
    assert "STALE DEPLOYED HOOK" not in blob  # the plan is names, not bytes


def test_plan_rejects_missing_source(d: str) -> None:
    try:
        shadow_install.shadow_plan(Path(d) / "does-not-exist")
    except NotADirectoryError:
        return
    raise AssertionError("a missing source install must be rejected")


def test_build_shadow_routes_hook_and_mirrors_assets(d: str) -> None:
    if not IS_WINDOWS:
        print("     (skip junction build: not Windows)")
        return
    source = Path(d) / "src"
    _make_source(source)
    built_hook = Path(d) / "dist"
    built_hook.mkdir()
    _write(built_hook / "opengl32.dll", b"FRESH BUILT HOOK")

    manifest = shadow_install.build_shadow(source, Path(d) / "shadow", built_hook)
    shadow = Path(manifest["game_dir"])
    assert manifest["fresh"] is True
    assert manifest["hook_sha256"] == manifest["built_hook_sha256"]
    # The hook slot is the *built* hook, not the deployed one.
    assert (shadow / "opengl32.dll").read_bytes() == b"FRESH BUILT HOOK"
    # The stale log is not inherited (per-run evidence only).
    assert not (shadow / "riddick_vr.log").exists()
    # Program-directory files are real copies.
    for name in ("DarkAthena.exe", "RndrGL.dll", "MCCDyn.dll"):
        assert (shadow / name).read_bytes() == (source / "System/Win32_x86" / name).read_bytes()
    # Game assets resolve through the junction to the untouched source.
    asset = Path(d) / "shadow/Content/Models/Riddick_H01.xmd"
    assert asset.read_bytes() == (source / "Content/Models/Riddick_H01.xmd").read_bytes()


def test_build_is_idempotent_and_never_writes_the_source(d: str) -> None:
    if not IS_WINDOWS:
        print("     (skip junction build: not Windows)")
        return
    source = Path(d) / "src"
    _make_source(source)
    before = {p.relative_to(source).as_posix(): _sha(p)
              for p in source.rglob("*") if p.is_file()}
    built_hook = Path(d) / "dist"
    built_hook.mkdir()
    _write(built_hook / "opengl32.dll", b"FRESH BUILT HOOK")

    shadow_install.build_shadow(source, Path(d) / "shadow", built_hook)
    # Rebuild over a populated shadow: must succeed and stay consistent.
    manifest = shadow_install.build_shadow(source, Path(d) / "shadow", built_hook)
    assert manifest["fresh"] is True
    after = {p.relative_to(source).as_posix(): _sha(p)
             for p in source.rglob("*") if p.is_file()}
    assert after == before, "a shadow build must not touch the source install"


def test_manifest_flags_a_stale_hook(d: str) -> None:
    source = Path(d) / "src"
    _make_source(source)
    built_hook = Path(d) / "dist"
    built_hook.mkdir()
    _write(built_hook / "opengl32.dll", b"FRESH BUILT HOOK")
    dest = Path(d) / "shadow"
    if IS_WINDOWS:
        shadow_install.build_shadow(source, dest, built_hook)
        # Swap the built hook after the fact: the manifest must notice.
        _write(built_hook / "opengl32.dll", b"A DIFFERENT BUILD")
        manifest = shadow_install.shadow_manifest(source, dest, built_hook)
        assert manifest["fresh"] is False
    else:
        print("     (skip junction build: not Windows)")


def test_format_shadow_is_one_line_with_a_verdict(d: str) -> None:
    line = shadow_install.format_shadow({
        "dest": "D:/shadow", "source": "F:/game", "junctions": 12,
        "hook_sha256": "aa", "built_hook_sha256": "aa", "fresh": True})
    assert "\n" not in line and "current build" in line and "12 junction" in line
    stale = shadow_install.format_shadow({
        "dest": "D:/shadow", "source": "F:/game", "junctions": 12,
        "hook_sha256": None, "built_hook_sha256": "aa", "fresh": False})
    assert "STALE/absent" in stale


def main() -> int:
    tests = (test_plan_mirrors_dirs_as_junctions_and_files_as_copies,
             test_plan_skips_deployed_hook_and_stale_log,
             test_plan_rejects_missing_source,
             test_build_shadow_routes_hook_and_mirrors_assets,
             test_build_is_idempotent_and_never_writes_the_source,
             test_manifest_flags_a_stale_hook,
             test_format_shadow_is_one_line_with_a_verdict)
    with tempfile.TemporaryDirectory() as d:
        for test in tests:
            # Each test gets its own scratch directory (they build trees/g/heads).
            case = Path(d) / test.__name__
            case.mkdir()
            test(str(case))
            print(f"ok   {test.__name__}")
    print("PASS shadow install tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
