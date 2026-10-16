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

import argparse
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
        dump_branch=kw.get("dump_branch", "gameplay"),
        reach_gameplay=kw.get("reach_gameplay", False), duration=kw.get("duration"),
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


# ------------------------------------------- run-scoped hook log (GH #69 req 4)
def _run_log(marker: str, run_ts: str = "2026-10-16 18:22:23") -> str:
    return (f"{xrsim.RUN_HEADER} {run_ts} ===\n"
            "[INFO] Riddick VR Hook DLL Attached (PID: 1)\n"
            f"[INFO] Combat Stance: {marker}\n"
            "[INFO] OnSwapBuffers: Frame 1\n")


def test_extract_run_section_prefers_last_run(td):
    old = _run_log("STALE MARKER", "2026-10-16 17:00:00")
    new = _run_log("COMBAT MODE entered (both triggers held)")
    text = old + new
    section = xrsim.extract_run_section(text)
    assert "COMBAT MODE entered" in section, section
    assert "STALE MARKER" not in section, section
    # Header-less producer -> whole text (never silently emptied).
    assert xrsim.extract_run_section("just a line") == "just a line"


def test_read_fresh_hook_log_survives_truncation(td):
    """The hook truncates its log each launch, so the pre-run byte mark can be
    larger than the new file: the legacy slice would drop the current markers."""
    log = Path(td) / "riddick_vr.log"
    log.write_text(_run_log("COMBAT MODE entered (both triggers held)"),
                   encoding="utf-8")
    log_mark = 9000  # previous run's size, larger than the truncated new log
    naive = log.read_bytes()[log_mark:].decode(errors="ignore")
    assert naive == "", naive  # the defect: markers vanish
    fresh = xrsim.read_fresh_hook_log(log, log_mark)
    assert "COMBAT MODE entered" in fresh, fresh


def test_read_fresh_hook_log_falls_back_without_header(td):
    log = Path(td) / "appended.log"
    log.write_text("HEAD" + "body MARKER tail", encoding="utf-8")
    assert xrsim.read_fresh_hook_log(log, 4) == "body MARKER tail"
    assert xrsim.read_fresh_hook_log(Path(td) / "missing.log") == ""


def test_marker_check_sees_current_run(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 3))
    scen = Path(td) / "s.json"
    scen.write_text(json.dumps(
        {"markers": ["COMBAT MODE entered", "RIOT BATON ARC JAB"]}), encoding="utf-8")
    log = Path(td) / "riddick_vr.log"
    log.write_text(_run_log("STALE") + _run_log("COMBAT MODE entered (both triggers held)"),
                   encoding="utf-8")
    rep = Path(td) / "report.json"
    with contextlib.redirect_stdout(io.StringIO()):
        rc = xrsim.main(["--evaluate-only", "--artifacts", str(art),
                         "--report-file", str(rep), "--hook-log", str(log),
                         "--scenario", str(scen)])
    payload = json.loads(rep.read_text(encoding="utf-8"))
    assert payload["markers"]["COMBAT MODE entered"] is True, payload["markers"]
    assert payload["markers"]["RIOT BATON ARC JAB"] is False, payload["markers"]
    assert payload["hook_log_scope"] == "current-run section", payload
    assert rc == 1  # the un-fired marker must still fail the run


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


# --------------------------------------------- dump gate (GH #69 req 1)
def test_build_host_cmd_defaults_to_gameplay_only(td):
    """A scripted run must request the gameplay-only dump gate so the capture
    window is live 3D gameplay, not the menu/cinema warm-up (#69 req 1)."""
    scen = Path(td) / "melee.json"
    scen.write_text("{}", encoding="utf-8")
    cmd = xrsim.build_host_cmd(_args(scenario=str(scen)), Path(td) / "art")
    assert "--dump-gameplay-only" in cmd, cmd
    # The scenario path must still be threaded through.
    assert "--scenario-file" in cmd and str(scen.resolve()) in cmd, cmd


def test_build_host_cmd_any_branch_keeps_legacy_cadence(td):
    cmd = xrsim.build_host_cmd(_args(dump_branch="any"), Path(td) / "art")
    assert "--dump-gameplay-only" not in cmd, cmd


# ------------------------------------------- reach gameplay (GH #71 req 1)
def test_build_host_cmd_reach_gameplay_is_unlimited(td):
    """A `--reach-gameplay` run must not stop at a frame budget (the mock follows
    the game's uncapped swap rate, so frames are not wall clock)."""
    a = argparse.Namespace(frames=180, interval=300, dump_branch="gameplay",
                           scenario=None, reach_gameplay=True)
    cmd = xrsim.build_host_cmd(a, Path(td) / "art")
    i = cmd.index("--max-frames")
    assert cmd[i + 1] == "0", cmd


def test_active_rect_fill_reads_barsan(td):
    splash = ("[INFO] BarScan: first active rect (336,80)-(944,477) of (1280,605) win\n")
    fill = xrsim.active_rect_fill(splash)
    assert fill is not None and 0.30 < fill < 0.40, fill
    gameplay = splash + "[INFO] BarScan: snap to (0,0)-(1280,605)\n"
    assert xrsim.active_rect_fill(gameplay) == 1.0, xrsim.active_rect_fill(gameplay)
    # No BarScan window -> unknown, never fabricated.
    assert xrsim.active_rect_fill("[INFO] DisplayMode: FULL-BLEED") is None
    assert xrsim.active_rect_fill("") is None


def test_gameplay_capture_stats_excludes_blank_and_cinema(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _save(art / "test_frame_0015.bmp", _black())              # gameplay, blank
    _save(art / "test_frame_0030.bmp", _content(seed=1))      # gameplay, content
    _save(art / "test_frame_0045.bmp", _content(seed=2))      # gameplay, distinct
    _save(art / "test_frame_0060.bmp", _content(seed=1))      # gameplay, duplicate
    _save(art / "test_frame_0075.bmp", _content(seed=3))      # cinema
    _write_labels(art, {15: False, 30: False, 45: False, 60: False, 75: True})
    stats = xrsim.gameplay_capture_stats(art, xrsim.read_display_branch_labels(art))
    assert stats["gameplay"] == 4, stats
    assert stats["blank_gameplay"] == 1, stats
    assert stats["cinema"] == 1, stats
    assert stats["distinct_gameplay"] == 2, stats


def test_reach_gameplay_check_uses_full_bleed_signal(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _save(art / "test_frame_0030.bmp", _content(seed=1))
    _save(art / "right_0030.bmp", _shift_h(_content(seed=1), 3))
    _write_labels(art, {30: False})
    splash_log = "[INFO] BarScan: first active rect (336,80)-(944,477) of (1280,605) win\n"
    _, checks = xrsim.evaluate(_args(reach_gameplay=True), art, "", splash_log)
    rec = [c for c in checks if c["check"] == "gameplay_full_bleed_reached"][0]
    assert rec["ok"] is False, rec
    live_log = splash_log + "[INFO] BarScan: snap to (0,0)-(1280,605)\n"
    _, checks = xrsim.evaluate(_args(reach_gameplay=True), art, "", live_log)
    rec = [c for c in checks if c["check"] == "gameplay_full_bleed_reached"][0]
    assert rec["ok"] is True, rec
    # The check is opt-in: a plain run does not assert it.
    _, checks = xrsim.evaluate(_args(), art, "", splash_log)
    assert not [c for c in checks if c["check"] == "gameplay_full_bleed_reached"]


def test_reach_gameplay_report_records_live_gameplay_stats(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _save(art / "test_frame_0030.bmp", _content(seed=1))
    _save(art / "right_0030.bmp", _shift_h(_content(seed=1), 3))
    _write_labels(art, {30: False})
    rep = Path(td) / "report.json"
    log = Path(td) / "riddick_vr.log"
    log.write_text(_run_log("x") + "[INFO] BarScan: snap to (0,0)-(1280,605)\n"
                   "[INFO] BarScan: first active rect (0,0)-(1280,605) of (1280,605) win\n",
                   encoding="utf-8")
    with contextlib.redirect_stdout(io.StringIO()):
        rc = xrsim.main(["--evaluate-only", "--artifacts", str(art),
                         "--report-file", str(rep), "--hook-log", str(log)])
    payload = json.loads(rep.read_text(encoding="utf-8"))
    assert payload["live_gameplay"]["distinct_gameplay"] == 1, payload["live_gameplay"]
    assert rc == 0, payload


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
             test_extract_run_section_prefers_last_run,
             test_read_fresh_hook_log_survives_truncation,
             test_read_fresh_hook_log_falls_back_without_header,
             test_marker_check_sees_current_run,
             test_display_branch_labels_parsed,
             test_cinema_identical_eyes_are_skipped_not_failed,
             test_gameplay_identical_eyes_still_fail,
             test_gameplay_captures_present_gate,
             test_evaluate_only_reads_persisted_host_log,
             test_build_host_cmd_defaults_to_gameplay_only,
             test_build_host_cmd_any_branch_keeps_legacy_cadence,
             test_build_host_cmd_reach_gameplay_is_unlimited,
             test_active_rect_fill_reads_barsan,
             test_gameplay_capture_stats_excludes_blank_and_cinema,
             test_reach_gameplay_check_uses_full_bleed_signal,
             test_reach_gameplay_report_records_live_gameplay_stats)
    with tempfile.TemporaryDirectory() as d:
        for t in tests:
            t(d)
            print(f"ok   {t.__name__}")
    print("PASS xrsim harness tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
