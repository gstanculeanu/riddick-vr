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
import math
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


def _flat(level=12, shape=(96, 128, 3)):
    return np.full(shape, float(level))


def _dim(seed=11, shape=(96, 128, 3)):
    """A dark, very low-contrast pattern (std well below MIN_DY_CONTRAST)."""
    return np.random.default_rng(seed).random(shape) * 10.0 + 5.0


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


def test_stereo_metrics_reports_contrast_and_boundary(td):
    base = _content()
    _save(Path(td) / "test_frame_0001.bmp", base)
    _save(Path(td) / "right_0001.bmp", _shift_h(base, 5))
    m = xrsim.stereo_metrics(Path(td) / "test_frame_0001.bmp",
                             Path(td) / "right_0001.bmp", search=24)
    assert m["contrast"] >= xrsim.MIN_DY_CONTRAST, m
    assert m["dy_at_boundary"] is False, m


def test_high_contrast_interior_dy_still_fails(td):
    # A bright, textured frame with a *quantifiable* (interior) vertical offset
    # is a real defect -- the confidence gates must not excuse it.
    base = _content()
    _save(Path(td) / "test_frame_0001.bmp", base)
    _save(Path(td) / "right_0001.bmp", _shift_v(_shift_h(base, 4), 3))
    m = xrsim.stereo_metrics(Path(td) / "test_frame_0001.bmp",
                             Path(td) / "right_0001.bmp", search=24)
    assert m["contrast"] >= xrsim.MIN_DY_CONTRAST, m
    assert m["dy_at_boundary"] is False, m
    ok, checks = xrsim.assert_stereo(m)
    assert not ok, checks
    dy = [c for c in checks if c["check"] == "no_vertical_disparity"]
    assert dy and not dy[0].get("skip") and not dy[0]["ok"], checks


def test_boundary_dy_is_inconclusive_not_failed(td):
    # GH #72 requirement 3 / ADR-0454: an optimum pinned to the search edge means
    # the offset cannot be quantified, so it is inconclusive even on a bright
    # frame -- this is the live frame-2400 case (dx and dy both on their bounds).
    base = _content(shape=(256, 256, 3))
    _save(Path(td) / "test_frame_0001.bmp", base)
    _save(Path(td) / "right_0001.bmp", _shift_v(_shift_h(base, 4), 8))
    m = xrsim.stereo_metrics(Path(td) / "test_frame_0001.bmp",
                             Path(td) / "right_0001.bmp", search=24)
    assert m["contrast"] >= xrsim.MIN_DY_CONTRAST, m
    assert m["dy_at_boundary"] is True, m
    ok, checks = xrsim.assert_stereo(m)
    dy = [c for c in checks if c["check"] == "no_vertical_disparity"]
    assert dy and dy[0].get("skip") and dy[0]["ok"], checks
    assert "search edge" in dy[0]["detail"], dy
    assert ok, checks


def test_low_contrast_vertical_shift_is_inconclusive(td):
    # GH #72 requirement 3: the same 3 px vertical shift on a low-contrast frame
    # cannot be measured, so it must be reported inconclusive (a skip), never a
    # defect.
    base = _dim()
    _save(Path(td) / "test_frame_0001.bmp", base)
    _save(Path(td) / "right_0001.bmp", _shift_v(_shift_h(base, 5), 3))
    m = xrsim.stereo_metrics(Path(td) / "test_frame_0001.bmp",
                             Path(td) / "right_0001.bmp", search=24)
    assert m["contrast"] < xrsim.MIN_DY_CONTRAST, m
    ok, checks = xrsim.assert_stereo(m)
    dy = [c for c in checks if c["check"] == "no_vertical_disparity"]
    assert dy and dy[0].get("skip") and dy[0]["ok"], checks
    assert ok, checks


def test_flat_dark_pair_is_inconclusive_at_boundary(td):
    # The live artefact signature: a flat/dark frame's vertical optimum runs to
    # the search edge. It must be inconclusive, not a defect.
    _save(Path(td) / "test_frame_0001.bmp", _flat())
    _save(Path(td) / "right_0001.bmp", _flat())
    m = xrsim.stereo_metrics(Path(td) / "test_frame_0001.bmp",
                             Path(td) / "right_0001.bmp", search=8)
    assert m["dy_at_boundary"] is True, m
    assert m["contrast"] < xrsim.MIN_DY_CONTRAST, m
    ok, checks = xrsim.assert_stereo(m, require_disparity=False)
    dy = [c for c in checks if c["check"] == "no_vertical_disparity"]
    assert dy and dy[0].get("skip") and dy[0]["ok"], checks
    assert "search boundary" in dy[0]["detail"], dy
    assert ok, checks


def test_vertical_disparity_summary_counts(td):
    checks = [
        {"check": "no_vertical_disparity", "ok": True},
        {"check": "no_vertical_disparity", "ok": True, "skip": True},
        {"check": "no_vertical_disparity", "ok": False},
        {"check": "both_eyes_non_black", "ok": True},
    ]
    stats = xrsim.vertical_disparity_stats(checks)
    assert stats == {"ok": 1, "inconclusive": 1, "failed": 1}, stats


def _metrics(**kw):
    """A synthetic ``stereo_metrics`` dict for the vertical-disparity verdict."""
    base = {"non_black_l": True, "non_black_r": True,
            "shape_l": [96, 128], "shape_r": [96, 128],
            "best_dx": 5, "best_dy": 3, "residual": 2.0, "identical": False,
            "contrast": 60.0, "dy_at_boundary": False}
    base.update(kw)
    return base


def test_boundary_dy_on_a_textured_frame_is_inconclusive(td):
    # GH #72 req 3 / ADR-0454: an optimum on the search edge cannot be quantified,
    # so it is inconclusive even when the frame is textured.
    ok, checks = xrsim.assert_stereo(
        _metrics(best_dy=8, dy_at_boundary=True))
    dy = [c for c in checks if c["check"] == "no_vertical_disparity"]
    assert dy and dy[0].get("skip") and dy[0]["ok"], checks
    assert "search edge" in dy[0]["detail"], dy
    assert ok, checks


def test_interior_dy_on_a_textured_frame_fails(td):
    ok, checks = xrsim.assert_stereo(_metrics())
    assert not ok, checks
    dy = [c for c in checks if c["check"] == "no_vertical_disparity"]
    assert dy and not dy[0].get("skip") and not dy[0]["ok"], checks


def test_interior_dy_on_a_low_contrast_frame_is_inconclusive(td):
    ok, checks = xrsim.assert_stereo(_metrics(contrast=10.0))
    dy = [c for c in checks if c["check"] == "no_vertical_disparity"]
    assert dy and dy[0].get("skip") and dy[0]["ok"], checks
    assert "low-contrast" in dy[0]["detail"], dy
    assert ok, checks


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


# ------------------------------------- comfort vignette (GH #68 req 5)
def _comfort_line(samples=600, mx=1.0, mean=0.9, above=580, locomotion=540,
                  max_loco=1.0, threshold=0.05):
    return (f"[Mock Host] ComfortVignette samples={samples} max={mx:.3f} "
            f"mean={mean:.3f} above={above} locomotion={locomotion} "
            f"max_locomotion={max_loco:.3f} threshold={threshold:.3f}")


def test_parse_comfort_vignette_line(td):
    assert xrsim.parse_comfort("") is None
    assert xrsim.parse_comfort("FrameBudget samples=30") is None
    # The last line of a run wins (the summary is printed periodically).
    text = _comfort_line(max_loco=0.0, above=0) + "\n" + _comfort_line(max_loco=0.97)
    c = xrsim.parse_comfort(text)
    assert c["samples"] == 600 and c["locomotion"] == 540, c
    assert c["max_locomotion"] == 0.97 and c["threshold"] == 0.05, c


def test_comfort_vignette_check_asserts_applied_during_locomotion(td):
    # Applied while moving -> pass.
    ok, rec = xrsim.comfort_vignette_check(xrsim.parse_comfort(_comfort_line()))
    assert ok and rec["ok"] and not rec.get("skip"), rec
    assert "while moving" in rec["detail"], rec
    # Locomotion happened but the vignette never rose -> fail (a real regression).
    ok, rec = xrsim.comfort_vignette_check(
        xrsim.parse_comfort(_comfort_line(max_loco=0.0, above=0)))
    assert not ok and not rec.get("skip"), rec


def test_comfort_vignette_check_skips_without_line_or_locomotion(td):
    # No line (pre-#68 host / evaluate-only) -> skip, never fail.
    ok, rec = xrsim.comfort_vignette_check(None)
    assert ok and rec.get("skip"), rec
    # No locomotion input published -> the vignette is not expected -> skip.
    ok, rec = xrsim.comfort_vignette_check(
        xrsim.parse_comfort(_comfort_line(locomotion=0, max_loco=0.0)))
    assert ok and rec.get("skip"), rec


def test_comfort_vignette_check_in_evaluate(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 3))
    budget = ("FrameBudget samples=30 mean_ms=6.0 max_ms=9.0 over_budget=0 "
              "budget_ms=11.11 p50_ms=5.0 p95_ms=7.0")
    passed, checks = xrsim.evaluate(_args(), art, budget + "\n" + _comfort_line(), "")
    rec = [c for c in checks if c["check"] == "comfort_vignette_applied"][0]
    assert rec["group"] == "comfort" and rec["ok"] and not rec.get("skip"), rec
    assert passed, checks


def test_comfort_vignette_report_key(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 3))
    xrsim.persist_host_log(
        art, "FrameBudget samples=30 mean_ms=6.0 max_ms=9.0 over_budget=0 "
             "budget_ms=11.11 p50_ms=5.0 p95_ms=7.0\n" + _comfort_line())
    rep = Path(td) / "report.json"
    with contextlib.redirect_stdout(io.StringIO()):
        rc = xrsim.main(["--evaluate-only", "--artifacts", str(art),
                         "--report-file", str(rep)])
    assert rc == 0
    payload = json.loads(rep.read_text(encoding="utf-8"))
    c = payload["comfort_vignette"]
    assert c and c["locomotion"] == 540 and c["max_locomotion"] == 1.0, c
    rec = [x for x in payload["checks"]
           if x["check"] == "comfort_vignette_applied"][0]
    assert rec["ok"] and not rec.get("skip"), rec


# ------------------------------------------------------------------- budget
def test_parse_budget_asserts_p50_p95_not_max(td):
    # GH #68 req 2: the budget is asserted on p50/p95, so a one-off level-load
    # hitch (max_ms=763) does not read as a sustained 90 Hz regression.
    ok, rec = xrsim.parse_budget(
        "FrameBudget samples=600 mean_ms=6.0 max_ms=763.0 over_budget=12 "
        "budget_ms=11.11 p50_ms=3.2 p95_ms=4.8", 11.11)
    assert ok, rec
    assert rec["p50_ms"] == 3.2 and rec["p95_ms"] == 4.8, rec
    assert rec["max_ms"] == 763.0 and rec["over_budget"] == 12, rec
    # p50 above budget fails even if p95 were somehow lower.
    ok, _ = xrsim.parse_budget(
        "FrameBudget samples=60 mean_ms=14.0 max_ms=20.0 over_budget=40 "
        "budget_ms=11.11 p50_ms=13.0 p95_ms=10.0", 11.11)
    assert not ok
    # p95 above budget fails.
    ok, _ = xrsim.parse_budget(
        "FrameBudget samples=60 mean_ms=30.0 max_ms=90.0 over_budget=50 "
        "budget_ms=11.11 p50_ms=20.0 p95_ms=40.0", 11.11)
    assert not ok


def test_parse_budget_reports_missing_line_and_stale_host(td):
    ok, rec = xrsim.parse_budget("no budget here", 11.11)
    assert not ok and rec["check"] == "frame_budget_reported", rec
    # A legacy line without percentiles is reported, not silently passed.
    ok, rec = xrsim.parse_budget(
        "FrameBudget samples=60 mean_ms=8.0 max_ms=12.0 over_budget=2 budget_ms=11.11",
        11.11)
    assert not ok and rec["check"] == "frame_budget_reported", rec


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
        _args(), art, "FrameBudget samples=30 mean_ms=6.0 max_ms=9.0 over_budget=0 "
                      "budget_ms=11.11 p50_ms=5.0 p95_ms=7.0",
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
        art, "FrameBudget samples=30 mean_ms=6.0 max_ms=9.0 over_budget=0 "
             "budget_ms=11.11 p50_ms=5.0 p95_ms=7.0")
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


# ------------------------------------- content gate (GH #72 requirement 2)
def test_content_gate_stats_parses_skip_lines(td):
    log = ("[Mock Host] DumpGate skip frame=120 reason=blank\n"
           "[Mock Host] DumpGate skip frame=180 reason=blank\n"
           "[Mock Host] DumpGate skip frame=240 reason=frozen\n")
    s = xrsim.content_gate_stats(log)
    assert s == {"blank_skipped": 2, "frozen_skipped": 1}, s
    # A pre-#72 host (no skip lines) reports nothing, never fabricated.
    assert xrsim.content_gate_stats("") == {"blank_skipped": 0, "frozen_skipped": 0}


def test_blank_gameplay_frame_fails_window_content_check(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _save(art / "test_frame_0015.bmp", _black())            # gameplay, blank
    _save(art / "right_0015.bmp", _black())
    _save(art / "test_frame_0030.bmp", _content(seed=1))
    _save(art / "right_0030.bmp", _shift_h(_content(seed=1), 3))
    _write_labels(art, {15: False, 30: False})
    passed, checks = xrsim.evaluate(_args(), art, "", "")
    rec = [c for c in checks if c["check"] == "gameplay_window_has_content"][0]
    assert rec["ok"] is False, rec
    assert passed is False

    # A clean window (no blank gameplay frame) passes the check.
    _save(art / "test_frame_0015.bmp", _content(seed=4))
    _save(art / "right_0015.bmp", _shift_h(_content(seed=4), 3))
    _, checks = xrsim.evaluate(_args(), art, "", "")
    rec = [c for c in checks if c["check"] == "gameplay_window_has_content"][0]
    assert rec["ok"] is True, rec


def test_window_content_check_is_gameplay_only(td):
    """The content check is a gameplay-only assertion; the legacy `any` branch
    intentionally captures menu/blank frames, so it must not be added there."""
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _save(art / "test_frame_0015.bmp", _black())
    _save(art / "right_0015.bmp", _black())
    _write_labels(art, {15: False})
    _, checks = xrsim.evaluate(_args(dump_branch="any"), art, "", "")
    assert not [c for c in checks if c["check"] == "gameplay_window_has_content"], checks


# ------------------------------------------- reach gameplay (GH #71 req 1)
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


# ------------------------------------- viewmodel probe (GH #67 step 2)
_VM_PROBE = (
    "[INFO] ViewmodelProbe [#300]: inside=4 outside=612 sample=1\n"
    "[INFO] ViewmodelProbe matrix: "
    "[0.7071 0.0000 -0.7071 0.0000] [0.0000 1.0000 0.0000 0.0000] "
    "[0.7071 0.0000 0.7071 0.0000] [1.2300 -0.4000 -2.5000 1.0000]\n")


def test_viewmodel_probe_stats_parse(td):
    s = xrsim.viewmodel_probe_stats(_VM_PROBE)
    assert len(s["lines"]) == 1, s
    line = s["lines"][0]
    assert (line["index"], line["inside"], line["outside"], line["sample"]) == (300, 4, 612, True), line
    assert s["matrix"] is not None and len(s["matrix"]) == 16, s["matrix"]
    assert abs(s["matrix"][0] - 0.7071) < 1e-6, s["matrix"]
    assert abs(s["matrix"][15] - 1.0) < 1e-6, s["matrix"]
    # Absent probe (ViewmodelObserve=0 or pre-#67 host) -> no lines, no matrix.
    empty = xrsim.viewmodel_probe_stats("[INFO] Viewmodel suppression [#300]: draws=0")
    assert empty["lines"] == [] and empty["matrix"] is None, empty


def test_viewmodel_probe_check_skipped_without_lines(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 3))
    passed, checks = xrsim.evaluate(_args(), art, "", "")
    rec = [c for c in checks if c["check"] == "viewmodel_pass_identified"][0]
    assert rec["ok"] is True and rec.get("skip") is True, rec
    assert passed is True, [c for c in checks if not c.get("skip") and not c["ok"]]


def test_viewmodel_probe_counts_matrix_composition(td):
    log = ("[INFO] ViewmodelProbe [#300]: inside=4 outside=612 sample=1 pushes=3 translates=5\n"
           "[INFO] ViewmodelProbe [#600]: inside=2 outside=610 sample=1 pushes=1 translates=2\n")
    s = xrsim.viewmodel_probe_stats(log)
    assert [ln["pushes"] for ln in s["lines"]] == [3, 1], s["lines"]
    assert [ln["translates"] for ln in s["lines"]] == [5, 2], s["lines"]
    assert s["pushes"] == 4 and s["translates"] == 7, s
    # Backward compatible: a line without the composition fields parses to None/0.
    old = xrsim.viewmodel_probe_stats(
        "[INFO] ViewmodelProbe [#300]: inside=4 outside=612 sample=1\n")
    assert old["lines"][0]["pushes"] is None, old
    assert old["pushes"] == 0 and old["translates"] == 0, old


def test_viewmodel_probe_check_requires_an_in_window_draw(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 3))
    # Window opens but nothing draws inside it: the pass is NOT identified.
    _, checks = xrsim.evaluate(_args(), art, "",
                               "[INFO] ViewmodelProbe [#300]: inside=0 outside=610 sample=0\n")
    rec = [c for c in checks if c["check"] == "viewmodel_pass_identified"][0]
    assert rec["ok"] is False, rec
    # A real in-window draw identifies the pass.
    _, checks = xrsim.evaluate(_args(), art, "", _VM_PROBE)
    rec = [c for c in checks if c["check"] == "viewmodel_pass_identified"][0]
    assert rec["ok"] is True, rec


# ----------------------- per-frame camera transform (GH #68 requirement 1)
def _eye_frames_file(art, frames):
    """Write an ``eye_frames.log`` in the host's wire format.

    ``frames`` is a list of ``(frame, yaw_deg)``; the head pose is a pure yaw
    rotation so the recorded yaw span is easy to assert.
    """
    def mat():
        vals = ["0.0000"] * 16
        for i in (0, 5, 10, 15):
            vals[i] = "1.0000"
        return "[" + " ".join(vals) + "]"

    lines = ["# frame=NNNN hmdpos=x,y,z hmdquat=x,y,z,w "
             "viewL=[16] viewR=[16] projL=[16] projR=[16]"]
    for frame, yaw in frames:
        half = math.radians(yaw) / 2.0
        qx, qy, qz, qw = 0.0, math.sin(half), 0.0, math.cos(half)
        lines.append(f"frame={frame} hmdpos=0.0000,1.7000,0.0000 "
                     f"hmdquat={qx:.4f},{qy:.4f},{qz:.4f},{qw:.4f} "
                     f"viewL={mat()} viewR={mat()} projL={mat()} projR={mat()}")
    (art / xrsim.EYE_FRAMES_FILE).write_text("\n".join(lines) + "\n", encoding="utf-8")


def test_eye_frame_log_parsed(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _eye_frames_file(art, [(15, -20.0), (30, 20.0)])
    frames = xrsim.read_eye_frames(art)
    assert set(frames) == {15, 30}, frames
    rec = frames[15]
    assert rec["hmd_pos"] == [0.0, 1.7, 0.0], rec
    assert len(rec["hmd_quat"]) == 4, rec
    assert len(rec["view_l"]) == 16 and len(rec["proj_r"]) == 16, rec
    stats = xrsim.eye_frame_stats(art)
    assert stats["count"] == 2 and stats["distinct"] == 2, stats
    assert abs(stats["yaw_span_deg"] - 40.0) < 0.5, stats
    # Absent file -> empty, never fabricated.
    assert xrsim.read_eye_frames(Path(td) / "nope") == {}
    assert xrsim.eye_frame_stats(Path(td) / "nope") == {
        "count": 0, "distinct": 0, "yaw_span_deg": None}


def test_eye_frame_check_skipped_without_file(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 3))
    passed, checks = xrsim.evaluate(_args(), art, "", "")
    rec = [c for c in checks if c["check"] == "capture_metadata_present"][0]
    assert rec["ok"] is True and rec.get("skip") is True, rec
    assert passed is True, [c for c in checks if not c.get("skip") and not c["ok"]]


def test_eye_frame_check_present_with_file(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 3))
    _eye_frames_file(art, [(15, 0.0)])
    _, checks = xrsim.evaluate(_args(), art, "", "")
    rec = [c for c in checks if c["check"] == "capture_metadata_present"][0]
    assert rec["ok"] is True and not rec.get("skip"), rec
    assert "camera transform" in rec["detail"], rec


def test_eye_frame_report_key_records_yaw_span(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _save(art / "test_frame_0015.bmp", _content(seed=1))
    _save(art / "right_0015.bmp", _shift_h(_content(seed=1), 3))
    _save(art / "test_frame_0030.bmp", _content(seed=2))
    _save(art / "right_0030.bmp", _shift_h(_content(seed=2), 3))
    _write_labels(art, {15: False, 30: False})
    _eye_frames_file(art, [(15, -10.0), (30, 10.0)])
    rep = Path(td) / "report.json"
    with contextlib.redirect_stdout(io.StringIO()):
        xrsim.main(["--evaluate-only", "--artifacts", str(art),
                    "--report-file", str(rep)])
    payload = json.loads(rep.read_text(encoding="utf-8"))
    assert payload["eye_frames"]["count"] == 2, payload["eye_frames"]
    assert abs(payload["eye_frames"]["yaw_span_deg"] - 20.0) < 0.5, payload["eye_frames"]


def test_truncated_capture_is_reported_not_crashed(td):
    """A run is ended by ``terminate``, so the last dump can be a partial BMP.
    One unreadable frame must be *reported*, never abort the harness report."""
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _save(art / "test_frame_0015.bmp", _content(seed=1))
    _save(art / "right_0015.bmp", _shift_h(_content(seed=1), 3))
    _save(art / "test_frame_0030.bmp", _content(seed=2))
    _save(art / "right_0030.bmp", _shift_h(_content(seed=2), 3))
    for name in ("test_frame_0030.bmp", "right_0030.bmp"):
        p = art / name
        data = p.read_bytes()
        p.write_bytes(data[: len(data) // 2])
    rep = Path(td) / "report.json"
    with contextlib.redirect_stdout(io.StringIO()):
        rc = xrsim.main(["--evaluate-only", "--artifacts", str(art),
                         "--report-file", str(rep)])
    payload = json.loads(rep.read_text(encoding="utf-8"))
    assert rc == 1, payload
    unreadable = [c for c in payload["checks"] if c["check"].startswith("frame_readable")]
    assert unreadable and unreadable[0]["ok"] is False, payload["checks"]


def main() -> int:
    tests = (test_shifted_pair_registers, test_identical_pair_is_mono_duplicate,
             test_all_black_pair_names_the_interop_cause,
             test_vertical_disparity_is_rejected, test_downscale_still_registers,
             test_stereo_metrics_reports_contrast_and_boundary,
             test_high_contrast_interior_dy_still_fails,
             test_boundary_dy_is_inconclusive_not_failed,
             test_low_contrast_vertical_shift_is_inconclusive,
             test_flat_dark_pair_is_inconclusive_at_boundary,
             test_vertical_disparity_summary_counts,
             test_boundary_dy_on_a_textured_frame_is_inconclusive,
             test_interior_dy_on_a_textured_frame_fails,
             test_interior_dy_on_a_low_contrast_frame_is_inconclusive,
             test_comfort_flags_empty_frame, test_comfort_passes_on_content,
             test_comfort_flags_green_glitch, test_parse_budget_asserts_p50_p95_not_max,
             test_parse_comfort_vignette_line,
             test_comfort_vignette_check_asserts_applied_during_locomotion,
             test_comfort_vignette_check_skips_without_line_or_locomotion,
             test_comfort_vignette_check_in_evaluate,
             test_comfort_vignette_report_key,
             test_parse_budget_reports_missing_line_and_stale_host,
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
             test_content_gate_stats_parses_skip_lines,
             test_blank_gameplay_frame_fails_window_content_check,
             test_window_content_check_is_gameplay_only,
             test_reach_gameplay_report_records_live_gameplay_stats,
             test_viewmodel_probe_stats_parse,
             test_viewmodel_probe_counts_matrix_composition,
             test_viewmodel_probe_check_skipped_without_lines,
             test_viewmodel_probe_check_requires_an_in_window_draw,
             test_eye_frame_log_parsed,
             test_eye_frame_check_skipped_without_file,
             test_eye_frame_check_present_with_file,
             test_eye_frame_report_key_records_yaw_span,
             test_truncated_capture_is_reported_not_crashed)
    with tempfile.TemporaryDirectory() as d:
        for t in tests:
            t(d)
            print(f"ok   {t.__name__}")
    print("PASS xrsim harness tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
