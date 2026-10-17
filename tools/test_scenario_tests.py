#!/usr/bin/env python3
"""Self-contained tests for ``tools/scenario_tests.py`` (GH #48, GH #75).

Builds a synthetic scenario workspace in a temp directory (manifest + hook
source stub + scripted controller profiles) and exercises the manifest
validator, including the GH #75 observation-only opt-out ("observe": true).
The final case runs the *shipped* ``--check`` gate so a red ``main`` (e.g. a
profile with neither markers nor the opt-out) fails this suite too.

Run headlessly:  python tools/test_scenario_tests.py
Exit code 0 = pass, 1 = fail.
"""
from __future__ import annotations

import contextlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenario_tests as st  # noqa: E402  (path injected above)

HOOK_STUB = "void CameraHook::Update() { /* MARKER_ONE */ }\n"
HOST_STUB = ("float v = gripPose.linearVelocity;\n"
             "float hz = 90.0f / frameSpan;\n")


def _keyframe(frame: int) -> dict:
    return {"frame": frame, "lx": 0.0, "ly": 1.5, "lz": -0.3,
            "lgrip": 0.0, "ltrig": 0.0, "lbtn": 0,
            "rx": 0.0, "ry": 1.5, "rz": -0.3,
            "rgrip": 0.0, "rtrig": 0.0, "rbtn": 0}


def _profile(name: str, **extra) -> dict:
    return {"name": name, "keys": [_keyframe(0), _keyframe(60)], **extra}


@contextlib.contextmanager
def workspace(tmp: str, profiles: dict, manifest: dict,
              hook_source: str = HOOK_STUB, host_source: str = HOST_STUB):
    root = Path(tmp)
    (root / "tests" / "scenarios").mkdir(parents=True, exist_ok=True)
    (root / "src" / "hook").mkdir(parents=True, exist_ok=True)
    (root / "src" / "host").mkdir(parents=True, exist_ok=True)
    mock = root / "tools" / "scenarios"
    mock.mkdir(parents=True, exist_ok=True)
    (root / "tests" / "scenarios" / "manifest.json").write_text(
        json.dumps(manifest), encoding="utf-8")
    (root / "src" / "hook" / "camera_hook.cpp").write_text(hook_source, encoding="utf-8")
    (root / "src" / "host" / "openxr_manager.cpp").write_text(host_source, encoding="utf-8")
    for name, profile in profiles.items():
        (mock / f"{name}.json").write_text(json.dumps(profile), encoding="utf-8")
    saved = (st.MANIFEST, st.HOOK_SOURCE, st.HOST_SOURCE, st.MOCK_SCENARIOS)
    st.MANIFEST = root / "tests" / "scenarios" / "manifest.json"
    st.HOOK_SOURCE = root / "src" / "hook" / "camera_hook.cpp"
    st.HOST_SOURCE = root / "src" / "host" / "openxr_manager.cpp"
    st.MOCK_SCENARIOS = mock
    try:
        yield root
    finally:
        st.MANIFEST, st.HOOK_SOURCE, st.HOST_SOURCE, st.MOCK_SCENARIOS = saved


def _manifest(*scenarios) -> dict:
    return {"version": 1, "scenarios": list(scenarios)}


def _scenario(sid: str, mode: str, markers=None, profile=None) -> dict:
    item = {"id": sid, "mode": mode, "title": sid, "steps": ["do a thing"],
            "markers": markers or []}
    if profile:
        item["profile"] = profile
    return item


def _expect_value_error(tmp: str, needle: str, **kwargs):
    try:
        with workspace(tmp, **kwargs):
            st.load_manifest()
    except ValueError as exc:
        assert needle in str(exc), f"expected {needle!r} in {exc!r}"
    else:
        raise AssertionError(f"expected ValueError mentioning {needle!r}")


def test_observe_only_profile_passes(tmp_dir):
    manifest = _manifest(_scenario("smoke", "automated"))
    with workspace(tmp_dir, {"hands_view": _profile("hands_view", observe=True,
                                                    markers=[])}, manifest):
        assert set(st.load_manifest()) == {"smoke"}


def test_observing_profile_may_still_declare_markers(tmp_dir):
    manifest = _manifest(_scenario("reload", "scripted", markers=["MARKER_ONE"],
                                   profile="reload"))
    with workspace(tmp_dir, {"reload": _profile("reload", observe=True,
                                                markers=["MARKER_ONE"])}, manifest):
        assert "reload" in st.load_manifest()


def test_markerless_profile_without_opt_out_is_rejected(tmp_dir):
    manifest = _manifest(_scenario("smoke", "automated"))
    _expect_value_error(tmp_dir, "expected non-empty markers or markers_any",
                        profiles={"hands_view": _profile("hands_view")},
                        manifest=manifest)


def test_observe_must_be_boolean(tmp_dir):
    manifest = _manifest(_scenario("smoke", "automated"))
    _expect_value_error(tmp_dir, "observe must be a boolean",
                        profiles={"hands_view": _profile("hands_view",
                                                         observe="yes")},
                        manifest=manifest)


def test_profile_marker_must_exist_in_hook_source(tmp_dir):
    manifest = _manifest(_scenario("reload", "scripted", markers=["MARKER_ONE"],
                                   profile="reload"))
    _expect_value_error(tmp_dir, "marker not found in hook source",
                        profiles={"reload": _profile("reload",
                                                     markers=["GHOST_MARKER"])},
                        manifest=manifest)


def test_scripted_profile_must_exist(tmp_dir):
    manifest = _manifest(_scenario("reload", "scripted", markers=["MARKER_ONE"],
                                   profile="reload"))
    _expect_value_error(tmp_dir, "missing scripted profile",
                        profiles={"other": _profile("other", markers=["MARKER_ONE"])},
                        manifest=manifest)


def test_profiles_require_increasing_frames(tmp_dir):
    manifest = _manifest(_scenario("smoke", "automated"))
    bad = {"name": "hands_view", "observe": True,
           "keys": [_keyframe(30), _keyframe(10)]}
    _expect_value_error(tmp_dir, "increasing integers",
                        profiles={"hands_view": bad}, manifest=manifest)


def test_shipped_tree_check_is_green(tmp_dir):
    """GH #75 guard: the real manifest/profiles must pass ``--check``."""
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "scenario_tests.py")
    result = subprocess.run([sys.executable, script, "--check"],
                            capture_output=True, text=True)
    assert result.returncode == 0, f"--check red:\n{result.stdout}\n{result.stderr}"
    assert "manifest valid" in result.stdout, result.stdout


def main() -> int:
    tests = [test_observe_only_profile_passes,
             test_observing_profile_may_still_declare_markers,
             test_markerless_profile_without_opt_out_is_rejected,
             test_observe_must_be_boolean,
             test_profile_marker_must_exist_in_hook_source,
             test_scripted_profile_must_exist,
             test_profiles_require_increasing_frames]
    with tempfile.TemporaryDirectory() as d:
        for test in tests:
            # a fresh workspace per test: profiles must not leak between cases
            with tempfile.TemporaryDirectory(dir=d) as case:
                test(case)
            print(f"ok   {test.__name__}")
    test_shipped_tree_check_is_green(None)
    print("ok   test_shipped_tree_check_is_green")
    print("PASS scenario_tests manifest tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
