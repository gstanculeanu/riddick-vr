#!/usr/bin/env python3
"""Self-contained tests for ``tools/xrsim.py`` (GH #68).

Exercises the *analysis* half of the OpenXR simulator harness with synthetic
eye captures -- stereo alignment/disparity, comfort rules, frame-budget parsing
and the ``--evaluate-only`` re-analysis path -- so the regression vehicle can be
trusted without a GPU, a game install, or synthesized input.

Run headlessly:  python tools/test_xrsim.py
Exit code 0 = pass, 1 = fail. If numpy/Pillow are unavailable the suite SKIPS
(exit 0) so it never breaks a lean CI runner.
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import numpy as np
    from PIL import Image
    import xrsim  # noqa: E402
except Exception as exc:  # pragma: no cover - lean runner without deps
    print(f"SKIP xrsim tests (missing dependency: {exc})")
    sys.exit(0)


def _save(path: Path, array):
    Image.fromarray(array.astype(np.uint8)).save(path)


def _content(seed=7, shape=(96, 128, 3)):
    arr = (np.random.default_rng(seed).random(shape) * 255)
    arr[:, :, 1] *= 0.5  # keep green below the chroma-glitch band (G>200,R<50,B<50)
    return arr


def _black(shape=(96, 128, 3)):
    return np.zeros(shape)


def _shift_h(array, dx):
    return np.roll(array, dx, axis=1)


def _shift_v(array, dy):
    return np.roll(array, dy, axis=0)


# ------------------------------------------------------------------- stereo
def test_shifted_pair_registers(td):
    base = _content()
    _save(Path(td) / "test_frame_0001.bmp", base)
    _save(Path(td) / "right_0001.bmp", _shift_h(base, 5))
    m = xrsim.stereo_metrics(Path(td) / "test_frame_0001.bmp",
                             Path(td) / "right_0001.bmp", search=24)
    assert m["best_dx"] == 5, m
    assert m["best_dy"] == 0, m
    ok, _ = xrsim.assert_stereo(m)
    assert ok, m


def test_identical_pair_is_mono_duplicate(td):
    base = _content()
    _save(Path(td) / "test_frame_0001.bmp", base)
    _save(Path(td) / "right_0001.bmp", base)
    m = xrsim.stereo_metrics(Path(td) / "test_frame_0001.bmp",
                             Path(td) / "right_0001.bmp", search=24)
    assert m["identical"] is True, m
    ok, checks = xrsim.assert_stereo(m)
    assert not ok
    assert any(c["check"] == "not_mono_duplicate" and not c["ok"] for c in checks)


def test_all_black_pair_names_the_interop_cause(td):
    _save(Path(td) / "test_frame_0001.bmp", _black())
    _save(Path(td) / "right_0001.bmp", _black())
    m = xrsim.stereo_metrics(Path(td) / "test_frame_0001.bmp",
                             Path(td) / "right_0001.bmp", search=8)
    ok, checks = xrsim.assert_stereo(m)
    assert not ok
    names = {c["check"] for c in checks if not c["ok"]}
    assert "both_eyes_non_black" in names, checks
    # The all-zero case must be reported as an interop failure, not silence.
    assert "interop_produced_content" in names, checks


def test_vertical_disparity_is_rejected(td):
    base = _content()
    _save(Path(td) / "test_frame_0001.bmp", base)
    _save(Path(td) / "right_0001.bmp", _shift_v(base, 3))
    m = xrsim.stereo_metrics(Path(td) / "test_frame_0001.bmp",
                             Path(td) / "right_0001.bmp", search=8)
    ok, checks = xrsim.assert_stereo(m)
    assert not ok
    assert any(c["check"] == "no_vertical_disparity" and not c["ok"] for c in checks)


def test_downscale_still_registers(td):
    base = _content(shape=(128, 128, 3))
    _save(Path(td) / "test_frame_0001.bmp", base)
    _save(Path(td) / "right_0001.bmp", _shift_h(base, 8))
    m = xrsim.stereo_metrics(Path(td) / "test_frame_0001.bmp",
                             Path(td) / "right_0001.bmp", search=12, downscale=4)
    assert m["downscale"] == 4
    assert m["best_dx"] == 2, m  # 8 px -> 2 at /4
    ok, _ = xrsim.assert_stereo(m)
    assert ok, m


# ------------------------------------------------------------------ comfort
def test_comfort_flags_empty_frame(td):
    p = Path(td) / "test_frame_0001.bmp"
    _save(p, _black())
    ok, checks = xrsim.comfort_checks(p)
    assert not ok
    assert any(c["check"] == "has_content" and not c["ok"] for c in checks)


def test_comfort_passes_on_content(td):
    p = Path(td) / "test_frame_0001.bmp"
    _save(p, _content())
    ok, checks = xrsim.comfort_checks(p)
    assert ok, checks


def test_comfort_flags_green_glitch(td):
    arr = _content()
    arr[:, :, 0] = 0
    arr[:, :, 1] = 255
    arr[:, :, 2] = 0
    p = Path(td) / "test_frame_0001.bmp"
    _save(p, arr)
    ok, checks = xrsim.comfort_checks(p)
    assert not ok
    assert any(c["check"] == "no_green_chroma_glitch" and not c["ok"] for c in checks)


# ------------------------------------------------------------------- budget
def test_parse_budget_accepts_and_rejects(td):
    ok, _ = xrsim.parse_budget(
        "FrameBudget samples=60 mean_ms=8.0 max_ms=12.0 over_budget=2 budget_ms=11.11",
        11.11)
    assert ok
    ok, _ = xrsim.parse_budget(
        "FrameBudget samples=60 mean_ms=30.0 max_ms=90.0 over_budget=50 budget_ms=11.11",
        11.11)
    assert not ok
    ok, rec = xrsim.parse_budget("no budget here", 11.11)
    assert not ok and rec["check"] == "frame_budget_reported"


# ------------------------------------------------------- evaluate / e2e path
def _args(**kw):
    import argparse
    return argparse.Namespace(
        scenario=kw.get("scenario"), frames=180, interval=15, timeout=90,
        max_frame_ms=1000.0 / 90.0, require_disparity=kw.get("require_disparity", True),
        downscale=kw.get("downscale", 1), artifacts=kw.get("artifacts"),
        report_file=kw.get("report_file"), hook_log=kw.get("hook_log"),
        evaluate_only=True, selftest=False)


def test_evaluate_passes_on_good_frame(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 3))
    passed, checks = xrsim.evaluate(
        _args(), art, "FrameBudget samples=30 mean_ms=6.0 max_ms=9.0 over_budget=0 budget_ms=11.11",
        "")
    assert passed, checks


def test_evaluate_fails_on_black_frames(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _save(art / "test_frame_0015.bmp", _black())
    _save(art / "right_0015.bmp", _black())
    passed, checks = xrsim.evaluate(_args(), art, "", "")
    assert not passed
    # frame-budget check must be skipped, not falsely failed, without a host log
    fb = [c for c in checks if c["check"] == "frame_budget"][0]
    assert fb.get("skip") is True, fb


def test_evaluate_only_writes_report_and_exit_code(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 4))
    rep = Path(td) / "report.json"
    with contextlib.redirect_stdout(io.StringIO()):
        rc = xrsim.main(["--evaluate-only", "--artifacts", str(art),
                         "--report-file", str(rep)])
    assert rc == 0
    payload = json.loads(rep.read_text(encoding="utf-8"))
    assert payload["passed"] is True, payload
    assert payload["mode"] == "evaluate-only"

    # A mono-duplicated capture must fail end-to-end.
    _save(art / "right_0015.bmp", base)
    with contextlib.redirect_stdout(io.StringIO()):
        rc = xrsim.main(["--evaluate-only", "--artifacts", str(art),
                         "--report-file", str(rep)])
    assert rc == 1
    assert json.loads(rep.read_text(encoding="utf-8"))["passed"] is False


def test_static_capture_is_flagged(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    for n in (15, 30):
        _save(art / f"test_frame_{n:04d}.bmp", base)
        _save(art / f"right_{n:04d}.bmp", _shift_h(base, 3))
    passed, checks = xrsim.evaluate(_args(), art, "", "")
    scene = [c for c in checks if c["check"] == "scene_advanced"][0]
    assert scene["ok"] is False, scene
    assert passed is False

    # Two genuinely different frames -> live scene, check skipped/absent.
    _save(art / "test_frame_0030.bmp", _content(seed=99))
    _save(art / "right_0030.bmp", _shift_h(_content(seed=99), 3))
    _, checks = xrsim.evaluate(_args(), art, "", "")
    scene = [c for c in checks if c["check"] == "scene_advanced"][0]
    assert scene["ok"] is True, scene


def test_evaluate_only_empty_dir_is_noop(td):
    empty = Path(td) / "empty"
    empty.mkdir()
    with contextlib.redirect_stdout(io.StringIO()):
        rc = xrsim.main(["--evaluate-only", "--artifacts", str(empty),
                         "--report-file", str(Path(td) / "r.json")])
    assert rc == 0


def test_evaluate_counts_scenario_markers(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 3))
    scen = Path(td) / "s.json"
    scen.write_text(json.dumps({"markers": ["BOXER GUARD engaged"]}), encoding="utf-8")
    _, checks = xrsim.evaluate(_args(scenario=str(scen)), art, "", "no marker here")
    m = [c for c in checks if c["check"] == "marker:BOXER GUARD engaged"][0]
    assert m["ok"] is False
    _, checks = xrsim.evaluate(_args(scenario=str(scen)), art, "", "BOXER GUARD engaged")
    m = [c for c in checks if c["check"] == "marker:BOXER GUARD engaged"][0]
    assert m["ok"] is True


def test_selftest_passes(td):
    with contextlib.redirect_stdout(io.StringIO()):
        assert xrsim.cmd_selftest() == 0


# ------------------------------------------------- display branch (GH #69)
def _write_labels(art: Path, mapping):
    lines = ["# frame=NNNN cinema=<0|1> left=<ok|err> right=<ok|err>"]
    for n, cinema in sorted(mapping.items()):
        lines.append(f"frame={n} cinema={1 if cinema else 0} left=ok right=ok")
    (art / xrsim.DISPLAY_LABEL_FILE).write_text("\n".join(lines) + "\n", encoding="utf-8")


def test_display_branch_labels_parsed(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _write_labels(art, {15: True, 30: False})
    labels = xrsim.read_display_branch_labels(art)
    assert labels == {15: True, 30: False}, labels
    # Absent file -> unknown, not fabricated.
    assert xrsim.read_display_branch_labels(Path(td) / "nope") == {}


def test_cinema_identical_eyes_are_skipped_not_failed(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", base)  # identical: correct on a cinema screen
    _write_labels(art, {15: True})
    passed, checks = xrsim.evaluate(_args(), art, "", "")
    for name in ("horizontal_disparity_present", "not_mono_duplicate"):
        rec = [c for c in checks if c["check"] == name and c.get("frame") == 15][0]
        assert rec["ok"] is True and rec.get("skip") is True, rec
        assert rec["branch"] == "cinema", rec
    # ... but a cinema-only run cannot validate gameplay stereo.
    gp = [c for c in checks if c["check"] == "gameplay_captures_present"][0]
    assert gp["ok"] is False, gp
    assert passed is False


def test_gameplay_identical_eyes_still_fail(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", base)
    _write_labels(art, {15: False})  # gameplay: identical eyes = mono duplicate
    passed, checks = xrsim.evaluate(_args(), art, "", "")
    dup = [c for c in checks if c["check"] == "not_mono_duplicate" and c.get("frame") == 15][0]
    assert dup["ok"] is False and not dup.get("skip"), dup
    assert dup["branch"] == "gameplay", dup
    assert passed is False


def test_gameplay_captures_present_gate(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _save(art / "test_frame_0015.bmp", _content(seed=1))
    _save(art / "right_0015.bmp", _content(seed=1))          # cinema, identical
    _save(art / "test_frame_0030.bmp", _content(seed=2))
    _save(art / "right_0030.bmp", _shift_h(_content(seed=2), 3))  # gameplay, shifted
    _write_labels(art, {15: True, 30: False})
    passed, checks = xrsim.evaluate(_args(), art, "", "")
    gp = [c for c in checks if c["check"] == "gameplay_captures_present"][0]
    assert gp["ok"] is True, gp
    assert passed is True, [c for c in checks if not c.get("skip") and not c["ok"]]


def test_evaluate_only_reads_persisted_host_log(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 3))
    xrsim.persist_host_log(
        art, "FrameBudget samples=30 mean_ms=6.0 max_ms=9.0 over_budget=0 budget_ms=11.11")
    rep = Path(td) / "report.json"
    with contextlib.redirect_stdout(io.StringIO()):
        rc = xrsim.main(["--evaluate-only", "--artifacts", str(art),
                         "--report-file", str(rep)])
    assert rc == 0
    payload = json.loads(rep.read_text(encoding="utf-8"))
    fb = [c for c in payload["checks"] if c["check"] == "frame_budget"][0]
    assert not fb.get("skip") and fb["ok"] is True, fb
    assert payload["host_log"], payload


def main() -> int:
    tests = (test_shifted_pair_registers, test_identical_pair_is_mono_duplicate,
             test_all_black_pair_names_the_interop_cause,
             test_vertical_disparity_is_rejected, test_downscale_still_registers,
             test_comfort_flags_empty_frame, test_comfort_passes_on_content,
             test_comfort_flags_green_glitch, test_parse_budget_accepts_and_rejects,
             test_evaluate_passes_on_good_frame, test_evaluate_fails_on_black_frames,
             test_evaluate_only_writes_report_and_exit_code,
             test_static_capture_is_flagged,
             test_evaluate_only_empty_dir_is_noop,
             test_evaluate_counts_scenario_markers, test_selftest_passes,
             test_display_branch_labels_parsed,
             test_cinema_identical_eyes_are_skipped_not_failed,
             test_gameplay_identical_eyes_still_fail,
             test_gameplay_captures_present_gate,
             test_evaluate_only_reads_persisted_host_log)
    with tempfile.TemporaryDirectory() as d:
        for t in tests:
            t(d)
            print(f"ok   {t.__name__}")
    print("PASS xrsim harness tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
