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


def _disparity_ramp(array, dmin=2, dmax=18):
    """A *depth-like* pair: a per-column horizontal disparity ramp (GH #76).

    Real stereo has depth-dependent disparity; a single global shift cannot
    align this, so the per-tile shifts differ -- the opposite of the shipped
    monoscopic shift.
    """
    h, w = array.shape[:2]
    out = np.empty_like(array)
    for x in range(w):
        d = int(round(dmin + (dmax - dmin) * x / max(1, w - 1)))
        out[:, x] = array[:, (x - d) % w]
    return out


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
# --------------------------------------- stereo structure (GH #76 / epic #57)
def test_shift_only_pair_is_classified(td):
    base = _content()
    _save(Path(td) / "test_frame_0001.bmp", base)
    _save(Path(td) / "right_0001.bmp", _shift_h(base, 6))
    m = xrsim.stereo_metrics(Path(td) / "test_frame_0001.bmp",
                             Path(td) / "right_0001.bmp", search=24)
    assert m["stereo_kind"] == "shift-only", m
    assert m["dx_spread_px"] == 0, m
    assert m["tiles_used"] >= xrsim.PARALLAX_TILE_MIN, m


def test_parallax_pair_is_classified(td):
    base = _content()
    _save(Path(td) / "test_frame_0001.bmp", base)
    _save(Path(td) / "right_0001.bmp", _disparity_ramp(base))
    m = xrsim.stereo_metrics(Path(td) / "test_frame_0001.bmp",
                             Path(td) / "right_0001.bmp", search=24)
    assert m["stereo_kind"] == "parallax", m
    assert m["dx_spread_px"] >= xrsim.PARALLAX_MIN_SPREAD_PX, m


def test_mono_duplicate_is_classified(td):
    base = _content()
    _save(Path(td) / "test_frame_0001.bmp", base)
    _save(Path(td) / "right_0001.bmp", base)
    m = xrsim.stereo_metrics(Path(td) / "test_frame_0001.bmp",
                             Path(td) / "right_0001.bmp", search=24)
    assert m["stereo_kind"] == "mono", m


def test_flat_pair_kind_is_unknown(td):
    base = _dim()
    _save(Path(td) / "test_frame_0001.bmp", base)
    _save(Path(td) / "right_0001.bmp", _shift_h(base, 3))
    m = xrsim.stereo_metrics(Path(td) / "test_frame_0001.bmp",
                             Path(td) / "right_0001.bmp", search=24)
    assert m["stereo_kind"] == "unknown", m
    # a missing/forward-compatible metrics dict must never raise
    assert xrsim.classify_stereo_kind({}) == "unknown"
    assert xrsim.classify_stereo_kind({"best_dx": None}) == "unknown"


def test_stereo_kind_stats_is_gameplay_authoritative(td):
    checks = [
        {"check": "x", "frame": 15, "branch": "gameplay",
         "stereo_kind": "shift-only", "best_dx": -4, "dx_spread_px": 0},
        {"check": "y", "frame": 15, "branch": "gameplay",
         "stereo_kind": "shift-only", "best_dx": -4, "dx_spread_px": 0},
        {"check": "z", "frame": 30, "branch": "cinema",
         "stereo_kind": "mono", "best_dx": 0, "dx_spread_px": 0},
        {"check": "w", "frame": 45, "branch": "gameplay",
         "stereo_kind": "unknown", "best_dx": -4, "dx_spread_px": None},
    ]
    st = xrsim.stereo_kind_stats(checks)
    assert st["kind"] == "shift-only", st
    assert st["frames"] == 3 and st["gameplay_frames"] == 2, st
    assert st["measurable_frames"] == 1, st
    assert st["spread_px_max"] == 0, st
    assert xrsim.stereo_kind_stats([])["kind"] == "unknown"
    # all frames unmeasurable -> unknown, never a false verdict
    unclear = [{"check": "u", "frame": 1, "branch": "gameplay",
                "stereo_kind": "unknown", "dx_spread_px": None}]
    assert xrsim.stereo_kind_stats(unclear)["kind"] == "unknown"


def test_require_parallax_grades_shift_stereo(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 4))
    # Default: the shifted pair still passes -- the interim blit is not graded.
    passed, checks = xrsim.evaluate(_args(), art, "", "")
    assert passed, checks
    assert not [c for c in checks if c["check"] == "stereo_parallax_present"]
    # --require-parallax: it must fail, naming the shift (GH #76).
    passed, checks = xrsim.evaluate(_args(require_parallax=True), art, "", "")
    assert not passed, checks
    gate = [c for c in checks if c["check"] == "stereo_parallax_present"]
    assert gate and gate[0]["ok"] is False, checks
    assert "shift" in gate[0]["detail"], gate[0]
    # A genuine (depth-varying) pair satisfies the same gate.
    _save(art / "right_0015.bmp", _disparity_ramp(base))
    passed, checks = xrsim.evaluate(_args(require_parallax=True), art, "", "")
    gate = [c for c in checks if c["check"] == "stereo_parallax_present"]
    assert gate and gate[0]["ok"] is True, checks


def test_scenario_can_require_parallax(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 4))
    scen = Path(td) / "s.json"
    scen.write_text(json.dumps({"require_parallax": True}), encoding="utf-8")
    passed, checks = xrsim.evaluate(_args(scenario=str(scen)), art, "", "")
    assert not passed, checks
    assert [c for c in checks if c["check"] == "stereo_parallax_present"]


def test_report_records_stereo_kind(td):
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
    assert payload["stereo_kind"]["kind"] == "shift-only", payload["stereo_kind"]
    assert payload["stereo_kind"]["per_frame"], payload["stereo_kind"]


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
        game_dir=kw.get("game_dir"), game_exe=kw.get("game_exe"),
        host_exe=kw.get("host_exe"), game_arg=kw.get("game_arg"),
        scenario_immediate=kw.get("scenario_immediate", False),
        require_parallax=kw.get("require_parallax", False),
        built_hook=kw.get("built_hook"),
        require_fresh_hook=kw.get("require_fresh_hook", False),
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


# ------------------------------------------- #74 union (branch features kept)
def test_build_host_cmd_honours_host_exe_override(td):
    """GH #74 union: the branch's --host-exe override is preserved alongside the
    autoloop harness (a second #68 implementation had to be reconciled)."""
    cmd = xrsim.build_host_cmd(_args(host_exe=r"C:\tmp\host.exe"), Path(td) / "art")
    assert cmd[0] == r"C:\tmp\host.exe", cmd


def test_build_host_cmd_forwards_scenario_immediate(td):
    """GH #74 union: the branch's --scenario-immediate is preserved."""
    assert "--scenario-immediate" not in xrsim.build_host_cmd(_args(), Path(td) / "art")
    cmd = xrsim.build_host_cmd(_args(scenario_immediate=True), Path(td) / "art")
    assert "--scenario-immediate" in cmd, cmd


def test_parse_budget_tolerates_gameplay_only_suffix(td):
    """GH #74 union: the branch's gameplay-gated FrameBudget line still parses,
    and p50/p95 remain the asserted fields (GH #68 req 2)."""
    ok, rec = xrsim.parse_budget(
        "FrameBudget samples=30 mean_ms=6.0 max_ms=9.0 over_budget=0 "
        "budget_ms=11.11 p50_ms=5.0 p95_ms=7.0 gameplay_only=1", 11.11)
    assert ok and rec["p50_ms"] == 5.0 and rec["p95_ms"] == 7.0, rec
    # A gameplay_only line *without* percentiles is still a stale-host FAIL.
    ok2, rec2 = xrsim.parse_budget(
        "FrameBudget samples=30 mean_ms=6.0 max_ms=9.0 over_budget=0 "
        "budget_ms=11.11 gameplay_only=1", 11.11)
    assert not ok2 and rec2["check"] == "frame_budget_reported", rec2


def test_cmd_selftest_requires_image_deps_only_when_asked(td):
    """The pure-logic selftest runs without numpy/Pillow; --require-image-deps
    turns a skipped image case into a failure (branch's numpy-free selftest)."""
    with contextlib.redirect_stdout(io.StringIO()):
        assert xrsim.cmd_selftest() == 0
        assert xrsim.cmd_selftest(require_image_deps=False) == 0


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


# ------------------------------- capture gate integrity (GH #79 host half)
def _gate_line(frame, content="live", sampled=1, written=1, luma_l=0.0, luma_r=0.0,
               before=10, after=10, straddled=0):
    return (f"[Mock Host] GateSample frame={frame} content={content} "
            f"sampled={sampled} written={written} luma_l={luma_l:.2f} "
            f"luma_r={luma_r:.2f} frames={before}..{after} straddled={straddled}")


def test_parse_gate_samples_records_each_attempt(td):
    log = (_gate_line(30, luma_l=80.0, luma_r=79.5) + "\n" +
           _gate_line(60, content="frozen", written=0, luma_l=12.0,
                      before=40, after=41, straddled=1) + "\n")
    g = xrsim.parse_gate_samples(log)
    assert set(g) == {30, 60}, g
    assert g[30]["content"] == "live" and g[30]["written"] is True
    assert g[30]["luma_l"] == 80.0 and g[30]["sampled"] is True
    assert g[60]["content"] == "frozen" and g[60]["written"] is False
    assert g[60]["frames"] == (40, 41) and g[60]["straddled"] is True
    assert xrsim.parse_gate_samples("") == {}


def _gate_art(td, seed=3):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content(seed=seed)
    _save(art / "test_frame_0030.bmp", base)
    _save(art / "right_0030.bmp", _shift_h(base, 3))
    _write_labels(art, {30: False})
    return art, xrsim.saved_left_lumas(art, frames={30})[30]


def test_capture_gate_matches_saved_passes_when_agreeing(td):
    art, saved = _gate_art(td)
    log = _gate_line(30, luma_l=saved, luma_r=saved - 0.4) + "\n"
    _, checks = xrsim.evaluate(_args(), art, log, "")
    rec = [c for c in checks if c["check"] == "capture_gate_matches_saved"][0]
    assert rec["ok"] is True and not rec.get("skip"), rec
    assert rec["checked"] == 1 and rec["mismatches"] == 0


def test_capture_gate_fails_on_saved_mismatch(td):
    art, saved = _gate_art(td, seed=5)
    log = _gate_line(30, luma_l=saved + 25.0) + "\n"
    _, checks = xrsim.evaluate(_args(), art, log, "")
    rec = [c for c in checks if c["check"] == "capture_gate_matches_saved"][0]
    assert rec["ok"] is False and rec["mismatches"] == 1, rec


def test_capture_gate_flags_non_live_write(td):
    art, saved = _gate_art(td, seed=9)
    # A frozen verdict that was nonetheless written is a gate regression.
    log = _gate_line(30, content="frozen", luma_l=saved) + "\n"
    _, checks = xrsim.evaluate(_args(), art, log, "")
    rec = [c for c in checks if c["check"] == "capture_gate_matches_saved"][0]
    assert rec["ok"] is False and rec["non_live"] == 1, rec


def test_capture_gate_reports_straddle_and_skips_without_lines(td):
    art, saved = _gate_art(td, seed=11)
    log = _gate_line(30, luma_l=saved, before=5, after=9, straddled=1) + "\n"
    _, checks = xrsim.evaluate(_args(), art, log, "")
    rec = [c for c in checks if c["check"] == "capture_gate_matches_saved"][0]
    assert rec["ok"] is True and rec["straddled"] == 1, rec
    # A pre-#79 host (no GateSample line) skips, never fails.
    _, checks = xrsim.evaluate(_args(), art, "", "")
    rec = [c for c in checks if c["check"] == "capture_gate_matches_saved"][0]
    assert rec["ok"] is True and rec.get("skip"), rec


def test_capture_gate_stats_and_report_key(td):
    g = xrsim.parse_gate_samples(
        _gate_line(30, luma_l=80.0) + "\n" +
        _gate_line(60, content="blank", written=0, before=1, after=2, straddled=1))
    s = xrsim.capture_gate_stats(g)
    assert s == {"attempts": 2, "written": 1, "sampled": 2, "straddled": 1,
                 "blank": 1, "frozen": 0}, s


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
def _rot3(q):
    """Row-major rotation of quaternion q (x, y, z, w) -- mirrors the host."""
    x, y, z, w = q
    return [[1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w), 2.0 * (x * z + y * w)],
            [2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - x * w)],
            [2.0 * (x * z - y * w), 2.0 * (y * z + x * w), 1.0 - 2.0 * (x * x + y * y)]]


def _mat(rot=None, tx=0.0):
    """A column-major 16-float *view* token for a world rotation ``rot``.

    The host stores ``R_head^T`` (world -> eye) in the view's rotation block, so
    this helper transposes ``rot`` to match -- a faithful view then has
    ``R_view == R_head^T`` (what ``_view_head_rotation_deviation_deg`` expects).
    """
    r = rot or [[1.0 if i == j else 0.0 for j in range(3)] for i in range(3)]
    m = [0.0] * 16
    for c in range(3):
        for row in range(3):
            m[c * 4 + row] = r[c][row]
    m[12] = tx
    m[15] = 1.0
    return "[" + " ".join(f"{v:.4f}" for v in m) + "]"


def _eye_frames_file(art, frames, source=None, faithful=False, half_ipd=0.0325):
    """Write an ``eye_frames.log`` in the host's wire format.

    ``frames`` is a list of ``(frame, yaw_deg)``; the head pose is a pure yaw
    rotation so the recorded yaw span is easy to assert. ``source`` appends the
    GH #77 ``eye=`` token (omitted when None = a legacy line). When ``faithful``
    the view rotation is built from the head pose (a runtime view / the mock
    reconstruction); otherwise it is the host's identity-rotation baseline.
    ``half_ipd`` is the per-eye translation magnitude (0 m breaks the eye pair).
    """
    lines = ["# frame=NNNN hmdpos=x,y,z hmdquat=x,y,z,w "
             "viewL=[16] viewR=[16] projL=[16] projR=[16] eye=<runtime|sim|synthetic>"]
    for frame, yaw in frames:
        half = math.radians(yaw) / 2.0
        qx, qy, qz, qw = 0.0, math.sin(half), 0.0, math.cos(half)
        rot = _rot3((qx, qy, qz, qw)) if faithful else None
        tok = f" eye={source}" if source else ""
        lines.append(f"frame={frame} hmdpos=0.0000,1.7000,0.0000 "
                     f"hmdquat={qx:.4f},{qy:.4f},{qz:.4f},{qw:.4f} "
                     f"viewL={_mat(rot, -half_ipd)} viewR={_mat(rot, half_ipd)} "
                     f"projL={_mat()} projR={_mat()}{tok}")
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
    # GH #77: a legacy line with no eye= token parses as source "unknown".
    assert rec["source"] == "unknown", rec
    stats = xrsim.eye_frame_stats(art)
    assert stats["count"] == 2 and stats["distinct"] == 2, stats
    assert abs(stats["yaw_span_deg"] - 40.0) < 0.5, stats
    # Absent file -> empty, never fabricated.
    assert xrsim.read_eye_frames(Path(td) / "nope") == {}
    assert xrsim.eye_frame_stats(Path(td) / "nope") == {
        "count": 0, "distinct": 0, "yaw_span_deg": None,
        "source": None, "view_head_rotation_deviation_deg": None,
        "view_lr_ipd_m": None, "proj_lr_identical": None}


def test_eye_frame_provenance_parsed(td):
    """GH #77: the eye= token is parsed into source and the view/head rotation
    deviation is measured -- ~0 for a faithful runtime view, ~the head's own
    rotation for the host's identity-rotation synthetic baseline."""
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _eye_frames_file(art, [(15, -30.0), (30, 30.0)], source="synthetic")
    assert xrsim.read_eye_frames(art)[15]["source"] == "synthetic"
    syn = xrsim.eye_frame_stats(art)
    assert syn["source"] == "synthetic", syn
    assert syn["proj_lr_identical"] is True, syn
    # Identity views vs a 30 deg head pose -> a large, measured deviation.
    assert syn["view_head_rotation_deviation_deg"] > 20.0, syn
    # The baseline still carries a nominal 65 mm eye separation.
    assert abs(syn["view_lr_ipd_m"] - 0.065) < 0.002, syn

    art2 = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _eye_frames_file(art2, [(15, -30.0), (30, 30.0)], source="runtime", faithful=True)
    rt = xrsim.eye_frame_stats(art2)
    assert rt["source"] == "runtime", rt
    assert rt["view_head_rotation_deviation_deg"] < 1.0, rt
    assert abs(rt["view_lr_ipd_m"] - 0.065) < 0.002, rt

    # GH #77 / ADR-0463: the mock host's reconstruction is tagged `sim` and is
    # scene-faithful in exactly the same way.
    art3 = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _eye_frames_file(art3, [(15, -30.0), (30, 30.0)], source="sim", faithful=True)
    sim = xrsim.eye_frame_stats(art3)
    assert sim["source"] == "sim", sim
    assert sim["view_head_rotation_deviation_deg"] < 1.0, sim


def test_eye_frame_check_skipped_without_file(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 3))
    passed, checks = xrsim.evaluate(_args(), art, "", "")
    rec = [c for c in checks if c["check"] == "capture_metadata_present"][0]
    assert rec["ok"] is True and rec.get("skip") is True, rec
    # GH #77: the provenance check is skipped alongside it, never silently passed.
    prov = [c for c in checks if c["check"] == "eye_views_scene_faithful"][0]
    assert prov["ok"] is True and prov.get("skip") is True, prov
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


def test_eye_view_scene_faithful_skips_synthetic(td):
    """GH #77: a mock/sim capture (identity-rotation synthetic views) must SKIP
    the scene-faithful check with the measured deviation as evidence -- it must
    not pass as if the placeholder matrices were the frame's real eye views."""
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 3))
    _eye_frames_file(art, [(15, 25.0)], source="synthetic")
    _, checks = xrsim.evaluate(_args(), art, "", "")
    rec = [c for c in checks if c["check"] == "eye_views_scene_faithful"][0]
    assert rec["ok"] is True and rec.get("skip") is True, rec
    assert "placeholder" in rec["detail"] and "GH #77" in rec["detail"], rec


def test_eye_view_scene_faithful_grades_runtime(td):
    """GH #77: a real-headset capture (views built from the head pose) passes;
    runtime-labelled but pose-inconsistent views fail the run."""
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 3))
    _eye_frames_file(art, [(15, 25.0)], source="runtime", faithful=True)
    _, checks = xrsim.evaluate(_args(), art, "", "")
    rec = [c for c in checks if c["check"] == "eye_views_scene_faithful"][0]
    assert rec["ok"] is True and not rec.get("skip"), rec

    art2 = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _save(art2 / "test_frame_0015.bmp", base)
    _save(art2 / "right_0015.bmp", _shift_h(base, 3))
    # Claims runtime but leaves the views at the synthetic baseline: inconsistent.
    _eye_frames_file(art2, [(15, 25.0)], source="runtime", faithful=False)
    _, checks2 = xrsim.evaluate(_args(), art2, "", "")
    rec2 = [c for c in checks2 if c["check"] == "eye_views_scene_faithful"][0]
    assert rec2["ok"] is False and not rec2.get("skip"), rec2


def test_eye_view_scene_faithful_grades_sim(td):
    """GH #77 / ADR-0463: the mock host's reconstructed views (`eye=sim`) let a
    sim run exercise the *pass* branch -- the check grades them like a runtime
    capture instead of always skipping -- and catches an inconsistent or
    eye-collapsed (wrong IPD) reconstruction."""
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 3))
    _eye_frames_file(art, [(15, 25.0)], source="sim", faithful=True)
    _, checks = xrsim.evaluate(_args(), art, "", "")
    rec = [c for c in checks if c["check"] == "eye_views_scene_faithful"][0]
    assert rec["ok"] is True and not rec.get("skip"), rec
    assert "separation" in rec["detail"], rec

    # Views rotated with the head but collapsed onto one origin (no IPD).
    art2 = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _save(art2 / "test_frame_0015.bmp", base)
    _save(art2 / "right_0015.bmp", _shift_h(base, 3))
    _eye_frames_file(art2, [(15, 25.0)], source="sim", faithful=True, half_ipd=0.0)
    _, checks2 = xrsim.evaluate(_args(), art2, "", "")
    rec2 = [c for c in checks2 if c["check"] == "eye_views_scene_faithful"][0]
    assert rec2["ok"] is False and not rec2.get("skip"), rec2

    # Views left at the identity baseline but labelled sim: rotation deviates.
    art3 = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    _save(art3 / "test_frame_0015.bmp", base)
    _save(art3 / "right_0015.bmp", _shift_h(base, 3))
    _eye_frames_file(art3, [(15, 25.0)], source="sim", faithful=False)
    _, checks3 = xrsim.evaluate(_args(), art3, "", "")
    rec3 = [c for c in checks3 if c["check"] == "eye_views_scene_faithful"][0]
    assert rec3["ok"] is False and not rec3.get("skip"), rec3


def test_view_head_deviation_uses_transpose_convention(td):
    """GH #77: the host stores the *world->eye* rotation (R_head^T) in a view, so
    the deviation must compare R_view against R_head^T -- not R_head. A view that
    stores R_head directly (the transposed/wrong convention) must be flagged, and
    the host-convention view must read ~0."""
    q = (0.0, math.sin(math.radians(20.0) / 2.0), 0.0, math.cos(math.radians(20.0) / 2.0))
    r = xrsim._rot3_from_quat(q)
    r_t = [[r[i][j] for i in range(3)] for j in range(3)]

    def floats(tok):
        return [float(v) for v in tok.strip("[]").split()]

    # _mat(rot) stores transpose(rot): _mat(r) == R_head^T (host convention).
    good = xrsim._view_head_rotation_deviation_deg(floats(_mat(r)), q)
    # Passing the transpose stores R_head itself -- the wrong convention.
    bad = xrsim._view_head_rotation_deviation_deg(floats(_mat(r_t)), q)
    assert good is not None and good < 1.0, good
    assert bad is not None and bad > 30.0, bad


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


def test_confirm_key_messages_are_wellformed(td):
    """GH #71 req 1: the posted confirm tap must be a well-formed Win32
    keydown/WM_CHAR/keyup triple with a correct lParam (repeat bit + key-up
    transition/previous-state bits)."""
    msgs = xrsim.confirm_key_messages()
    assert [m[0] for m in msgs] == [xrsim.WM_KEYDOWN, xrsim.WM_CHAR, xrsim.WM_KEYUP]
    assert all(m[1] == xrsim.VK_RETURN for m in msgs), msgs
    down, _char, up = msgs
    assert down[2] == 1, msgs       # scan 0, repeat count 1
    assert up[2] == 1 | (1 << 30) | (1 << 31), msgs
    assert not (down[2] >> 30) & 1 and not (down[2] >> 31) & 1, down
    assert (up[2] >> 30) & 1 and (up[2] >> 31) & 1, up
    assert xrsim.key_lparam(0x1C, True) == (0x1C << 16) | 1


def test_menu_advancer_fires_on_cadence_and_honours_max_taps(td):
    t = {"now": 100.0}
    adv = xrsim.GameMenuAdvancer(lambda: 2, interval_s=2.0, max_taps=3,
                                 clock=lambda: t["now"])
    assert adv.poll() == 0 and adv.taps == 0          # not due yet
    t["now"] = 102.0
    assert adv.poll() == 1 and adv.taps == 1 and adv.windows == 2
    t["now"] = 103.0
    assert adv.poll() == 0 and adv.taps == 1          # inside the interval
    t["now"] = 104.0
    assert adv.poll() == 1 and adv.taps == 2
    t["now"] = 106.0
    assert adv.poll() == 1 and adv.taps == 3 and adv.stopped
    t["now"] = 108.0
    assert adv.poll() == 0 and adv.taps == 3          # max_taps reached


def test_menu_advancer_stop_is_honoured(td):
    t = {"now": 0.0}
    adv = xrsim.GameMenuAdvancer(lambda: 1, interval_s=1.0, clock=lambda: t["now"])
    t["now"] = 1.0
    assert adv.poll() == 1
    adv.stop()
    t["now"] = 9.0
    assert adv.poll() == 0 and adv.taps == 1 and adv.stopped


def test_menu_advancer_has_no_catchup_burst(td):
    """A stall must not release a burst of taps (which would spam the game)."""
    t = {"now": 0.0}
    adv = xrsim.GameMenuAdvancer(lambda: 1, interval_s=1.0, clock=lambda: t["now"])
    t["now"] = 60.0
    assert adv.poll() == 1 and adv.taps == 1
    assert adv.poll() == 0 and adv.taps == 1


def test_gameplay_reached_requires_menu_then_gameplay(td):
    """The startup splash is full-bleed-labelled (GH #72), so a bare full-bleed
    branch must NOT read as gameplay -- only a full-bleed *after* a cinema does."""
    text = ("DisplayMode: FULL-BLEED gameplay (3Dfresh=1 menuCursor=0)\n"
            "DisplayMode: CINEMA screen (menus/cinematics) (3Dfresh=0 menuCursor=1)\n"
            "DisplayMode: FULL-BLEED gameplay (3Dfresh=1 menuCursor=0)\n")
    seq = xrsim.display_branch_sequence(text)
    assert seq == [False, True, False], seq
    assert xrsim.gameplay_reached_from_sequence(seq)
    assert not xrsim.gameplay_reached_from_sequence([False])       # splash only
    assert not xrsim.gameplay_reached_from_sequence([False, False])
    assert not xrsim.gameplay_reached_from_sequence([True])        # still at menu
    assert xrsim.gameplay_reached_from_sequence([True, False])
    assert not xrsim.gameplay_reached_from_sequence([])


def test_post_confirm_is_a_noop_without_a_pid(td):
    assert xrsim.post_confirm_to_game_windows(0) == 0


def test_advance_menu_flags_parse(td):
    with contextlib.redirect_stdout(io.StringIO()):
        assert xrsim.main(["--selftest"]) == 0
        assert xrsim.main(["--no-advance-menu", "--selftest"]) == 0


# ----------------------------------- GH #71 req 1: input-free route to gameplay
def test_game_command_passes_tokens_verbatim(td):
    """--game-arg tokens reach the game argv unchanged (no shell re-splitting),
    so a `+loadgame checkpoint` console sequence can drive the title with no
    input at all (the only input-safe route, ADR-0466)."""
    assert xrsim.game_command("DarkAthena.exe", ["+loadgame", "checkpoint"]) == [
        "DarkAthena.exe", "+loadgame", "checkpoint"]


def test_game_command_defaults_to_bare_exe(td):
    assert xrsim.game_command("DarkAthena.exe") == ["DarkAthena.exe"]
    assert xrsim.game_command("DarkAthena.exe", None) == ["DarkAthena.exe"]
    assert xrsim.game_command("DarkAthena.exe", []) == ["DarkAthena.exe"]
    # Empty tokens are dropped, never passed as argv holes.
    assert xrsim.game_command("g.exe", ["+exec", "", "auto.cfg"]) == [
        "g.exe", "+exec", "auto.cfg"]


def test_game_arg_flags_parse(td):
    with contextlib.redirect_stdout(io.StringIO()):
        assert xrsim.main(["--game-arg", "+loadgame", "--game-arg", "checkpoint",
                           "--selftest"]) == 0
        # Leading-dash game tokens (e.g. -env) need the `--game-arg=VALUE` form,
        # or argparse reads them as harness options.
        assert xrsim.main(["--game-arg=-env", "--game-arg", "x.cfg",
                           "--game-arg=-nowriteenv", "--selftest"]) == 0
        assert xrsim.main(["--selftest"]) == 0


# ------------------------- GH #80: the harness half of the sim input bridge gate
def test_sim_confirm_env_is_off_by_default(td):
    """The bridge is a two-token gate: RIDDICKVR_SIM alone (already set by every
    sim run) must never fabricate input, so with no --sim-confirm the harness
    adds nothing to the game environment (GH #80 requirement 3)."""
    assert xrsim.sim_confirm_env() == {}
    assert xrsim.sim_confirm_env(None, 5, 250, 80) == {}


def test_sim_confirm_env_sets_route_and_cadence(td):
    assert xrsim.sim_confirm_env("keyboard") == {"RIDDICKVR_SIM_CONFIRM": "keyboard"}
    assert xrsim.sim_confirm_env("both", max_taps=7) == {
        "RIDDICKVR_SIM_CONFIRM": "both",
        "RIDDICKVR_SIM_CONFIRM_MAX_TAPS": "7"}
    assert xrsim.sim_confirm_env("gamepad", 12, 250, 80) == {
        "RIDDICKVR_SIM_CONFIRM": "gamepad",
        "RIDDICKVR_SIM_CONFIRM_MAX_TAPS": "12",
        "RIDDICKVR_SIM_CONFIRM_PERIOD_MS": "250",
        "RIDDICKVR_SIM_CONFIRM_HOLD_MS": "80"}
    # max_taps 0 is the legal "never tap" budget, not "not set".
    assert xrsim.sim_confirm_env("keyboard", max_taps=0)["RIDDICKVR_SIM_CONFIRM_MAX_TAPS"] == "0"


def test_sim_confirm_flags_parse(td):
    with contextlib.redirect_stdout(io.StringIO()):
        assert xrsim.main(["--selftest"]) == 0
        # A bare --sim-confirm means the default route ("both"); an explicit
        # route, the max-taps bound and the leading-dash-free cadence flags all
        # parse without argparse treating them as game tokens.
        assert xrsim.main(["--sim-confirm", "--selftest"]) == 0
        assert xrsim.main(["--sim-confirm", "keyboard",
                           "--sim-confirm-max-taps", "3", "--selftest"]) == 0
        assert xrsim.main(["--sim-confirm=gamepad", "--sim-confirm-period-ms", "250",
                           "--sim-confirm-hold-ms", "80", "--selftest"]) == 0


# ----------------------------- GH #81: the hook build under test is identified
def test_file_identity_reports_hash_and_tracks_change(td):
    """A build is identified by content (sha256), not by path or mtime alone."""
    p = Path(td) / "opengl32.dll"
    p.write_bytes(b"hook-build-one")
    a = xrsim.file_identity(p)
    assert a["bytes"] == len(b"hook-build-one") and len(a["sha256"]) == 64
    assert a["mtime_utc"].endswith("Z"), a
    # A same-size edit must not be masked by the identity cache: pin a new mtime
    # like a rebuild would, and the hash must change.
    p.write_bytes(b"hook-build-two")
    os.utime(p, (1700000000, 1700000000))
    b = xrsim.file_identity(p)
    assert b["sha256"] != a["sha256"], (a, b)
    assert xrsim.file_identity(Path(td) / "absent.dll") is None


def test_deployed_hook_identity_matches_and_differs(td):
    game = Path(td) / "gamedir"
    game.mkdir()
    built = Path(td) / "dist" / "opengl32.dll"
    built.parent.mkdir()
    built.write_bytes(b"current-build")
    # Nothing deployed yet: the deployed hook is unknown, and nothing is compared.
    ident = xrsim.deployed_hook_identity(game, built)
    assert ident["deployed"] is None and ident["deployed_present"] is False
    assert ident["matches_build"] is None
    assert "unknown" in xrsim.format_hook_identity(ident)
    # Same bytes -> the run exercised the current build.
    (game / "opengl32.dll").write_bytes(b"current-build")
    assert xrsim.deployed_hook_identity(game, built)["matches_build"] is True
    # A different deployed hook -> stale, named in the summary.
    (game / "opengl32.dll").write_bytes(b"older-deployed-hook!!")
    stale = xrsim.deployed_hook_identity(game, built)
    assert stale["matches_build"] is False
    assert "DIFFERS" in xrsim.format_hook_identity(stale)
    # No repo build to compare against: the deployed identity is still recorded.
    no_build = xrsim.deployed_hook_identity(game, None)
    assert no_build["deployed_present"] is True and no_build["matches_build"] is None
    assert "no repo build to compare" in xrsim.format_hook_identity(no_build)


def test_deployed_hook_check_skips_by_default_and_grades_on_request(td):
    built = {"path": "b", "sha256": "b" * 64, "bytes": 2, "mtime_utc": "t"}
    stale = {"deployed": {"path": "g", "sha256": "a" * 64, "bytes": 1,
                          "mtime_utc": "t"},
             "built": built, "deployed_present": True, "matches_build": False}
    rec = xrsim.deployed_hook_check(stale)
    assert rec["skip"] is True and rec["check"] == "deployed_hook_matches_build"
    assert "STALE" in rec["detail"] and "reported only" in rec["detail"]
    assert rec["deployed_hook"] is stale
    graded = xrsim.deployed_hook_check(stale, require_fresh=True)
    assert graded["ok"] is False and not graded.get("skip")
    fresh = dict(stale, matches_build=True)
    assert xrsim.deployed_hook_check(fresh, require_fresh=True)["ok"] is True
    assert xrsim.deployed_hook_check(fresh)["skip"] is True
    # Nothing deployed: reported (skipped) but never silently graded green.
    missing = {"deployed": None, "built": built, "deployed_present": False,
               "matches_build": None}
    assert xrsim.deployed_hook_check(missing)["skip"] is True
    assert xrsim.deployed_hook_check(missing, require_fresh=True)["ok"] is False


def test_evaluate_records_hook_identity_and_grades_only_when_asked(td):
    """The report carries `deployed_hook`; the graded form is opt-in so a normal
    (deploy-gated) run cannot redden on the expected stale hook."""
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 3))
    game = Path(td) / "gamedir_eval"
    game.mkdir()
    built = Path(td) / "dist_eval" / "opengl32.dll"
    built.parent.mkdir()
    built.write_bytes(b"current-build")
    (game / "opengl32.dll").write_bytes(b"older-deployed-hook!!")
    budget = ("FrameBudget samples=30 mean_ms=6.0 max_ms=9.0 over_budget=0 "
              "budget_ms=11.11 p50_ms=5.0 p95_ms=7.0")

    def hook_rec(checks):
        return [c for c in checks if c["check"] == "deployed_hook_matches_build"][0]

    passed, checks = xrsim.evaluate(
        _args(game_dir=str(game), built_hook=str(built)), art, budget, "")
    rec = hook_rec(checks)
    assert rec["skip"] is True and passed, checks
    assert rec["deployed_hook"]["matches_build"] is False

    passed2, checks2 = xrsim.evaluate(
        _args(game_dir=str(game), built_hook=str(built), require_fresh_hook=True),
        art, budget, "")
    rec2 = hook_rec(checks2)
    assert not rec2.get("skip") and rec2["ok"] is False and passed2 is False, checks2

    # Installing the built hook flips the graded check green.
    (game / "opengl32.dll").write_bytes(b"current-build")
    passed3, checks3 = xrsim.evaluate(
        _args(game_dir=str(game), built_hook=str(built), require_fresh_hook=True),
        art, budget, "")
    assert hook_rec(checks3)["ok"] is True and passed3, checks3


def test_hook_identity_flags_parse(td):
    with contextlib.redirect_stdout(io.StringIO()):
        assert xrsim.main(["--selftest"]) == 0
        assert xrsim.main(["--require-fresh-hook", "--selftest"]) == 0
        assert xrsim.main(["--built-hook", str(Path(td) / "dist" / "opengl32.dll"),
                           "--selftest"]) == 0


# ------------------------- sim input bridge observability (GH #71 req 1 / #80)
def _bridge_gate(route="both", installed="yes", get_proc="90", input_req="1",
                 patched="0", wrapped="0"):
    return (f"[INFO] XRSIM input bridge: gate on route={route} installed={installed} "
            f"getProcCalls={get_proc} inputProcRequests={input_req} "
            f"createDevicePatched={patched} keyboardWrapped={wrapped}")


def _bridge_summary(route="both", polls=1000, taps=3, kpolls=0, ktaps=0,
                    last_real="1167", proc_list="XInputGetState:owned"):
    return (f"[INFO] XRSIM input bridge: summary route={route} polls={polls} taps={taps} "
            f"keyboardPolls={kpolls} keyboardTaps={ktaps} lastRealResult={last_real} "
            f"createDevicePatched=0 keyboardWrapped=0 getProcCalls=90 "
            f"inputProcRequests=1 procs[{proc_list}]")


def test_parse_input_bridge_is_none_without_lines(td):
    assert xrsim.parse_input_bridge("") is None
    assert xrsim.parse_input_bridge("[INFO] unrelated line\n") is None
    assert "not opted in" in xrsim.input_bridge_stats(None)


def test_parse_input_bridge_reads_gate_procs_and_summary(td):
    text = "\n".join([
        _bridge_gate(route="gamepad", input_req="2"),
        "[INFO] XRSIM input bridge: engine requested XInputGetState (owned=yes)",
        "[INFO] XRSIM input bridge: engine requested XInputGetCapabilities (owned=no)",
        "[INFO] XRSIM input bridge: input procs resolved so far: "
        "XInputGetState:owned,DirectInputCreateA:observed",
        "[INFO] XRSIM input bridge: XInputGetState first poll (userIndex=0 result=1167)",
        "[INFO] XRSIM input bridge: XInputGetState result changed -> 0 (poll=420)",
        "[INFO] XRSIM input bridge: GetDeviceState first poll",
        _bridge_summary(polls=1500, taps=7, kpolls=0, ktaps=0, last_real="1167",
                        proc_list="XInputGetState:owned,DirectInputCreateA:observed"),
    ])
    b = xrsim.parse_input_bridge(text)
    assert b["present"] is True
    assert b["gate"]["route"] == "gamepad" and b["gate"]["installed"] is True
    assert b["gate"]["input_proc_requests"] == 2
    # "observed" then "owned" (the summary's list) -> both names recorded.
    assert b["procs"] == {"XInputGetState": True, "XInputGetCapabilities": False,
                          "DirectInputCreateA": False}
    assert b["polls"] == 1500 and b["taps"] == 7
    assert b["keyboard_polls"] == 1
    assert b["result_changes"] == [[0, 420]]
    assert b["last_real_result"] == 1167  # the periodic summary wins the timeline
    assert b["xinput_device_not_connected"] is True and b["xinput_connected"] is False


def test_input_bridge_stats_names_the_procs(td):
    b = xrsim.parse_input_bridge(_bridge_summary(polls=60, taps=1, last_real="0",
                                                 proc_list="XInputGetState:owned"))
    line = xrsim.input_bridge_stats(b)
    assert "polls=60" in line and "taps=1" in line and "lastRealResult=0" in line
    assert "XInputGetState" in line


def test_input_bridge_report_key_written_by_evaluate_only(td):
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    base = _content()
    _save(art / "test_frame_0015.bmp", base)
    _save(art / "right_0015.bmp", _shift_h(base, 3))
    log = Path(td) / "riddick_vr.log"
    log.write_text(xrsim.RUN_HEADER + "\n" + _bridge_gate() + "\n" +
                   _bridge_summary() + "\n", encoding="utf-8")
    rep = Path(td) / "report.json"
    with contextlib.redirect_stdout(io.StringIO()):
        rc = xrsim.main(["--evaluate-only", "--artifacts", str(art),
                         "--hook-log", str(log), "--report-file", str(rep)])
    assert rc == 0
    payload = json.loads(rep.read_text(encoding="utf-8"))
    assert payload["input_bridge"]["gate"]["route"] == "both"
    assert payload["input_bridge"]["polls"] == 1000
    # A run with no bridge lines records null, not an empty dict.
    log.write_text(xrsim.RUN_HEADER + "\n[INFO] nothing here\n", encoding="utf-8")
    with contextlib.redirect_stdout(io.StringIO()):
        assert xrsim.main(["--evaluate-only", "--artifacts", str(art),
                           "--hook-log", str(log), "--report-file", str(rep)]) == 0
    assert json.loads(rep.read_text(encoding="utf-8"))["input_bridge"] is None


# ----------------------------- live-run isolation + lock (GH #83)
def test_default_shadow_dir_is_per_run(td):
    a = xrsim.default_shadow_dir(11)
    b = xrsim.default_shadow_dir(22)
    assert a != b
    assert a.name == "riddickvr-shadow-11"
    assert xrsim.default_shadow_dir(11, root=td) == Path(td) / "riddickvr-shadow-11"


def test_live_lock_decision_states(td):
    assert xrsim.live_lock_decision(None, lambda _p: True, 1) == ("free", None)
    assert xrsim.live_lock_decision({}, lambda _p: True, 1) == ("free", None)
    assert xrsim.live_lock_decision({"pid": "nope"}, lambda _p: True, 1) == ("free", None)
    assert xrsim.live_lock_decision({"pid": 1}, lambda _p: True, 1) == ("owned", 1)
    assert xrsim.live_lock_decision({"pid": 2}, lambda _p: True, 1) == ("held", 2)
    assert xrsim.live_lock_decision({"pid": 2}, lambda _p: False, 1) == ("stale", 2)


def test_live_lock_refuses_while_held_and_takes_over_stale(td):
    lock = Path(td) / xrsim.DEFAULT_LIVE_LOCK_NAME
    ok, state, _ = xrsim.acquire_live_lock(lock, 101, is_alive=lambda _p: False)
    assert ok and state == "free" and lock.exists()
    held, state2, message = xrsim.acquire_live_lock(
        lock, 202, is_alive=lambda p: p == 101)
    assert not held and state2 == "held" and "pid 101" in message
    # The holder's own lock is left untouched by the refusal.
    assert xrsim.read_live_lock(lock)["pid"] == 101
    ok3, state3, message3 = xrsim.acquire_live_lock(
        lock, 202, is_alive=lambda _p: False)
    assert ok3 and state3 == "stale" and "stale pid 101" in message3
    assert xrsim.read_live_lock(lock)["pid"] == 202
    # A non-owner never deletes a lock (so a takeover is not clobbered), and the
    # owner's release removes it.
    assert xrsim.release_live_lock(lock, 101) is False
    assert xrsim.release_live_lock(lock, 202) is True
    assert not lock.exists()


def test_live_lock_read_tolerates_partial_and_missing(td):
    lock = Path(td) / "half.json"
    lock.write_text('{"pid": 1, "star', encoding="utf-8")
    assert xrsim.read_live_lock(lock) is None
    lock.write_text("", encoding="utf-8")
    assert xrsim.read_live_lock(lock) is None
    assert xrsim.read_live_lock(Path(td) / "absent.json") is None
    assert xrsim.release_live_lock(Path(td) / "absent.json", 1) is False


def test_live_lock_and_shadow_flags_parse(td):
    # The flags exist and do not disturb the pure paths that run before any lock.
    with contextlib.redirect_stdout(io.StringIO()):
        assert xrsim.main(["--selftest"]) == 0
        assert xrsim.main(["--ignore-live-lock", "--selftest"]) == 0
        assert xrsim.main(["--shadow-dir", td, "--selftest"]) == 0


# ------------------ GH #87: an early game exit is not a measurement
def test_game_start_decision_states(td):
    """The measured GH #87 signature (game ended in ~2 s, no hook log, no frames)
    is an early exit that must not be read as evidence; *any* evidence or a game
    that stayed live is a real measurement."""
    state, detail = xrsim.game_start_decision(True, 2.0, 0, False)
    assert state == "early_exit" and "GH #87" in detail
    # A captured frame / a fresh hook log / a live game is a real measurement.
    assert xrsim.game_start_decision(True, 2.0, 1, False)[0] == "started"
    assert xrsim.game_start_decision(True, 2.0, 0, True)[0] == "started"
    assert xrsim.game_start_decision(True, 30.0, 0, False)[0] == "started"
    # The harness terminating the game after a host-bounded/duration run is not an
    # exit: game_exited is False, so a short --frames run is never misretried.
    assert xrsim.game_start_decision(False, 0.5, 0, False)[0] == "started"


def test_launch_retry_is_bounded_to_one_early_exit(td):
    assert xrsim.launch_retry_decision("early_exit", 1, 2) is True
    assert xrsim.launch_retry_decision("early_exit", 2, 2) is False
    assert xrsim.launch_retry_decision("started", 1, 2) is False
    # A run configured for a single attempt never retries.
    assert xrsim.launch_retry_decision("early_exit", 1, 1) is False


def test_no_game_start_fails_once_and_demotes_capture_checks(td):
    """A second early exit still fails -- as one distinct `game_started`, never as
    broken captures; the hook identity stays graded because it is independent of
    whether this attempt reached a live scene (GH #87)."""
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    game = Path(td) / "gamedir_nostart"
    game.mkdir()
    built = Path(td) / "dist_nostart" / "opengl32.dll"
    built.parent.mkdir()
    built.write_bytes(b"current-build")
    (game / "opengl32.dll").write_bytes(b"current-build")
    launch = {"game_started": False, "max_attempts": 2, "min_live_seconds": 5.0,
              "attempts": [
                  {"attempt": 1, "live_seconds": 2.1, "frames": 0,
                   "hook_fresh": False, "game_exited": True},
                  {"attempt": 2, "live_seconds": 1.9, "frames": 0,
                   "hook_fresh": False, "game_exited": True}]}
    passed, checks = xrsim.evaluate(
        _args(game_dir=str(game), built_hook=str(built), require_fresh_hook=True),
        art, "[Mock Host] Game Connected: WAITING", "", launch=launch)
    by = {c["check"]: c for c in checks}
    assert by["game_started"]["ok"] is False and not by["game_started"].get("skip")
    assert "2/2" in by["game_started"]["detail"]
    # The noisy capture FAILs are reported but not graded ...
    assert by["frames_captured"].get("skip") is True
    assert "not graded" in by["frames_captured"]["detail"]
    # ... while the hook identity (independent of a live scene) still grades.
    assert by["deployed_hook_matches_build"].get("skip") is not True
    assert by["deployed_hook_matches_build"]["ok"] is True
    assert passed is False


def test_no_launch_record_keeps_capture_grading(td):
    """Backward compatibility: an --evaluate-only / launch-less evaluation grades
    the capture checks as before and records no `game_started` (GH #87)."""
    art = Path(tempfile.mkdtemp(dir=td, prefix="art_"))
    passed, checks = xrsim.evaluate(
        _args(), art, "[Mock Host] Game Connected: WAITING", "")
    by = {c["check"]: c for c in checks}
    assert "game_started" not in by
    assert by["frames_captured"].get("skip") is not True
    assert by["frames_captured"]["ok"] is False
    assert passed is False


def test_launch_reliability_flags_parse(td):
    with contextlib.redirect_stdout(io.StringIO()):
        assert xrsim.main(["--selftest"]) == 0
        assert xrsim.main(["--min-live-seconds", "3", "--selftest"]) == 0
        assert xrsim.main(["--max-launch-attempts", "1", "--selftest"]) == 0


# ------------------ GH #67 live tuning: --ini writes only the shadow
def test_ini_flag_parses_and_refuses_without_a_shadow(td):
    """`--ini KEY=VALUE` must be accepted (repeatable), and must be refused
    without `--shadow-install` -- the hook reads its config beside its own
    module, and the loop must never write the game install's ini (GH #67)."""
    with contextlib.redirect_stdout(io.StringIO()):
        assert xrsim.main(["--selftest"]) == 0
        assert xrsim.main(["--ini", "ViewmodelObserve=1", "--selftest"]) == 0
        assert xrsim.main(["--ini", "A=1", "--ini", "B=2", "--selftest"]) == 0
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        rc = xrsim.main(["--ini", "ViewmodelObserve=1"])
    assert rc == 2 and "--shadow-install" in err.getvalue(), err.getvalue()


def test_ini_overrides_reach_the_shadow_and_the_report(td):
    """The override is written into the shadow's program-dir ini and echoed in
    the manifest/report, so a run says which flags it exercised (GH #67). Uses
    the pure merge so no game launch is needed."""
    import shadow_install
    merged = shadow_install.merge_ini("[VR]\nViewmodelObserve=0\n",
                                      {"ViewmodelObserve": "1", "ViewmodelTrace": "1"})
    assert "ViewmodelObserve=1" in merged and "ViewmodelTrace=1" in merged
    manifest = {"fresh": True, "junctions": 1, "dest": "D:/s", "source": "F:/g",
                "hook_sha256": "a", "built_hook_sha256": "a",
                "ini_overrides": {"ViewmodelObserve": "1"}}
    assert "ini[ViewmodelObserve=1]" in shadow_install.format_shadow(manifest)
    assert manifest["ini_overrides"] == {"ViewmodelObserve": "1"}


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
             test_shift_only_pair_is_classified, test_parallax_pair_is_classified,
             test_mono_duplicate_is_classified, test_flat_pair_kind_is_unknown,
             test_stereo_kind_stats_is_gameplay_authoritative,
             test_require_parallax_grades_shift_stereo,
             test_scenario_can_require_parallax, test_report_records_stereo_kind,
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
             test_build_host_cmd_honours_host_exe_override,
             test_build_host_cmd_forwards_scenario_immediate,
             test_parse_budget_tolerates_gameplay_only_suffix,
             test_cmd_selftest_requires_image_deps_only_when_asked,
             test_active_rect_fill_reads_barsan,
             test_gameplay_capture_stats_excludes_blank_and_cinema,
             test_reach_gameplay_check_uses_full_bleed_signal,
             test_content_gate_stats_parses_skip_lines,
             test_blank_gameplay_frame_fails_window_content_check,
             test_window_content_check_is_gameplay_only,
             test_parse_gate_samples_records_each_attempt,
             test_capture_gate_matches_saved_passes_when_agreeing,
             test_capture_gate_fails_on_saved_mismatch,
             test_capture_gate_flags_non_live_write,
             test_capture_gate_reports_straddle_and_skips_without_lines,
             test_capture_gate_stats_and_report_key,
             test_reach_gameplay_report_records_live_gameplay_stats,
             test_viewmodel_probe_stats_parse,
             test_viewmodel_probe_counts_matrix_composition,
             test_viewmodel_probe_check_skipped_without_lines,
             test_viewmodel_probe_check_requires_an_in_window_draw,
             test_eye_frame_log_parsed,
             test_eye_frame_check_skipped_without_file,
             test_eye_frame_check_present_with_file,
             test_eye_frame_report_key_records_yaw_span,
             test_eye_frame_provenance_parsed,
             test_view_head_deviation_uses_transpose_convention,
             test_eye_view_scene_faithful_skips_synthetic,
             test_eye_view_scene_faithful_grades_runtime,
             test_eye_view_scene_faithful_grades_sim,
             test_confirm_key_messages_are_wellformed,
             test_menu_advancer_fires_on_cadence_and_honours_max_taps,
             test_menu_advancer_stop_is_honoured,
             test_menu_advancer_has_no_catchup_burst,
             test_gameplay_reached_requires_menu_then_gameplay,
             test_post_confirm_is_a_noop_without_a_pid,
             test_advance_menu_flags_parse,
             test_game_command_passes_tokens_verbatim,
             test_game_command_defaults_to_bare_exe,
             test_game_arg_flags_parse,
             test_sim_confirm_env_is_off_by_default,
             test_sim_confirm_env_sets_route_and_cadence,
             test_sim_confirm_flags_parse,
             test_file_identity_reports_hash_and_tracks_change,
             test_deployed_hook_identity_matches_and_differs,
             test_deployed_hook_check_skips_by_default_and_grades_on_request,
             test_evaluate_records_hook_identity_and_grades_only_when_asked,
             test_hook_identity_flags_parse,
             test_parse_input_bridge_is_none_without_lines,
             test_parse_input_bridge_reads_gate_procs_and_summary,
             test_input_bridge_stats_names_the_procs,
             test_input_bridge_report_key_written_by_evaluate_only,
             test_default_shadow_dir_is_per_run,
             test_live_lock_decision_states,
             test_live_lock_refuses_while_held_and_takes_over_stale,
             test_live_lock_read_tolerates_partial_and_missing,
             test_live_lock_and_shadow_flags_parse,
             test_game_start_decision_states,
             test_launch_retry_is_bounded_to_one_early_exit,
             test_no_game_start_fails_once_and_demotes_capture_checks,
             test_no_launch_record_keeps_capture_grading,
             test_launch_reliability_flags_parse,
             test_ini_flag_parses_and_refuses_without_a_shadow,
             test_ini_overrides_reach_the_shadow_and_the_report,
             test_truncated_capture_is_reported_not_crashed)
    with tempfile.TemporaryDirectory() as d:
        for t in tests:
            t(d)
            print(f"ok   {t.__name__}")
    print("PASS xrsim harness tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
