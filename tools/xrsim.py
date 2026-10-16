#!/usr/bin/env python3
"""XRSIM - OpenXR simulator harness for Riddick VR (GH #68).

Runs the mock OpenXR host + game with *scripted* HMD/controller poses (no real
headset), captures BOTH eyes, and asserts:

  * frame-time budget   (host FrameBudget lines),
  * stereo correctness   (left/right eye diff: non-identical, horizontal
                          disparity present, no vertical disparity),
  * interaction state    (scenario markers present in the fresh hook log),
  * comfort rules        (non-black, no green chroma glitch, full-bleed/HUD).

Sim runs set RIDDICKVR_SIM=1 so the hook never injects desktop input (#68).

Usage:
  python tools/xrsim.py --scenario tools/scenarios/melee.json
  python tools/xrsim.py --selftest            # logic check, no game (CI)
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path


# numpy/Pillow are imported lazily so the pure-logic selftest runs without them.
_IMG = None


def image_deps():
    """Return (numpy, PIL.Image) or raise ImportError."""
    global _IMG
    if _IMG is None:
        import numpy as np
        from PIL import Image
        _IMG = (np, Image)
    return _IMG


def have_image_deps():
    try:
        image_deps()
        return True
    except ImportError:
        return False


GAME_DIR = Path(os.environ.get(
    "RIDDICKVR_GAME_DIR",
    r"F:\The Chronicles of Riddick - Assault on Dark Athena\System\Win32_x86"))
GAME_EXE = Path(os.environ.get("RIDDICKVR_GAME_EXE", str(GAME_DIR / "DarkAthena.exe")))
HOST_EXE = Path(os.environ.get(
    "RIDDICKVR_HOST_EXE", r"C:\Users\GigelCastel\riddick-vr\dist\riddickvr-host.exe"))
ARTIFACTS = Path(r"C:\Users\GigelCastel\riddick-vr\tests\xrsim\artifacts")
REPORT = Path(r"C:\Users\GigelCastel\riddick-vr\tests\xrsim\report.json")

MIN_BUDGET_SAMPLES = 30
CONTENT_STD_MIN = 2.0

FRAME_BUDGET_RE = re.compile(
    r"FrameBudget samples=(\d+) mean_ms=([\d.]+) max_ms=([\d.]+) "
    r"over_budget=(\d+) budget_ms=([\d.]+)( gameplay_only=1)?")
CAPTURE_RE = re.compile(r"Captured stereo frame (\d+) .*mode=(gameplay|cinema)")


# ---------------------------------------------------------------- stereo diff
def stereo_metrics(left_path: Path, right_path: Path, search=48):
    """Best horizontal/vertical alignment of the two eye images.

    Returns dict with: non_black_l/r, std_l (left luminance std-dev),
    best_dx, best_dy, residual (MSE at best), identical (bool). A correct
    gameplay frame has a small horizontal dx and ~zero dy.
    """
    np, Image = image_deps()
    L = np.array(Image.open(left_path).convert("L"), dtype=np.float32)
    R = np.array(Image.open(right_path).convert("L"), dtype=np.float32)
    out = {"shape_l": list(L.shape), "shape_r": list(R.shape)}
    out["non_black_l"] = float(L.mean()) > 1.0
    out["non_black_r"] = float(R.mean()) > 1.0
    out["std_l"] = float(L.std())
    if L.shape != R.shape:
        out.update(best_dx=None, best_dy=None, residual=None, identical=False,
                   note="shape mismatch")
        return out

    # Central crop to keep the search cheap and on-content.
    h, w = L.shape
    y0, y1 = h // 4, 3 * h // 4
    x0, x1 = w // 4, 3 * w // 4
    ref = L[y0:y1, x0:x1]
    best = (None, None, float("inf"))
    for dy in range(-4, 5):
        for dx in range(-search, search + 1):
            ry0, ry1 = y0 + dy, y1 + dy
            rx0, rx1 = x0 + dx, x1 + dx
            if ry0 < 0 or rx0 < 0 or ry1 > h or rx1 > w:
                continue
            mse = float(np.mean((ref - R[ry0:ry1, rx0:rx1]) ** 2))
            if mse < best[2]:
                best = (dx, dy, mse)
    out["best_dx"], out["best_dy"], out["residual"] = best
    # "Identical" = zero shift with a tiny residual (mono duplication).
    out["identical"] = (best[0] == 0 and best[1] == 0 and best[2] < 0.5)
    return out


def has_content(metrics):
    return metrics.get("std_l", 0.0) > CONTENT_STD_MIN


def assert_stereo(metrics, require_disparity=True):
    checks = []
    ok = True

    def chk(name, cond, detail=""):
        nonlocal ok
        checks.append({"check": name, "ok": bool(cond), "detail": detail})
        ok = ok and bool(cond)

    chk("both_eyes_non_black", metrics.get("non_black_l") and metrics.get("non_black_r"),
        f"L={metrics.get('non_black_l')} R={metrics.get('non_black_r')}")
    if metrics.get("shape_l") != metrics.get("shape_r"):
        chk("same_eye_resolution", False, "shape mismatch")
        return ok, checks
    chk("same_eye_resolution", True)
    dx, dy = metrics.get("best_dx"), metrics.get("best_dy")
    chk("no_vertical_disparity", dy is not None and abs(dy) <= 2, f"dy={dy}")
    if require_disparity:
        chk("horizontal_disparity_present", dx is not None and abs(dx) >= 1,
            f"dx={dx}")
    chk("not_mono_duplicate", not metrics.get("identical"), "eyes identical")
    return ok, checks


def stereo_frame_checks(n, mode, metrics, require_disparity=True):
    """Checks for one captured frame, honouring its mode label (GH #68).

    Cinema/menu frames are mono by design and gameplay frames without content
    (black, loading, uniform) say nothing about stereo, so neither can fail.
    """
    if mode == "cinema":
        return [{"group": "stereo", "frame": n, "check": "cinema_mono_expected",
                 "ok": True, "detail": "cinema frame, mono by design (info)"}]
    if not has_content(metrics):
        return [{"group": "stereo", "frame": n, "check": "stereo_skipped",
                 "ok": True,
                 "detail": f"skipped: no content (std={metrics.get('std_l', 0.0):.2f})"}]
    _, sub = assert_stereo(metrics, require_disparity=require_disparity)
    return [{**c, "group": "stereo", "frame": n,
             "detail": f"{c['detail']} {metrics.get('best_dx')}/{metrics.get('best_dy')}"}
            for c in sub]


# ------------------------------------------------------------------ comfort
def comfort_checks(left_path: Path):
    np, Image = image_deps()
    arr = np.array(Image.open(left_path).convert("RGB"))
    green = (arr[:, :, 1] > 200) & (arr[:, :, 0] < 50) & (arr[:, :, 2] < 50)
    green_n = int(green.sum())
    row_max = arr.max(axis=(1, 2))
    col_max = arr.max(axis=(0, 2))
    rows = np.where(row_max > 10)[0]
    cols = np.where(col_max > 10)[0]
    checks = [
        {"check": "no_green_chroma_glitch", "ok": green_n < 50,
         "detail": f"{green_n} green px"},
        {"check": "has_content", "ok": len(rows) > 0 and len(cols) > 0,
         "detail": f"rows={len(rows)} cols={len(cols)}"},
    ]
    return all(c["ok"] for c in checks), checks


# ------------------------------------------------------------------ frame budget
def parse_budget(host_log: str, budget_ms: float):
    """Use the last gameplay-only FrameBudget line; too few samples is a FAIL."""
    samples = [m for m in FRAME_BUDGET_RE.findall(host_log) if m[5]]
    if not samples:
        return False, {"check": "frame_budget_reported", "ok": False,
                       "detail": "no gameplay-only FrameBudget line from host"}
    n, mean, mx, over, budget, _ = samples[-1]
    n, mean, mx, over, budget = int(n), float(mean), float(mx), int(over), float(budget)
    if n < MIN_BUDGET_SAMPLES:
        return False, {"check": "frame_budget", "ok": False,
                       "detail": f"insufficient gameplay samples (n={n} < {MIN_BUDGET_SAMPLES})"}
    tol = max(budget_ms, budget) * 1.5
    rec = {"check": "frame_budget", "ok": (mean <= budget_ms and mx <= tol),
           "detail": f"n={n} mean={mean:.2f} max={mx:.2f} over={over} budget={budget:.2f}"}
    return rec["ok"], rec


# ------------------------------------------------------------------ captures
def parse_captures(host_log: str):
    """{frame number: 'gameplay'|'cinema'} from host 'Captured stereo frame' lines."""
    return {int(n): mode for n, mode in CAPTURE_RE.findall(host_log)}


# ------------------------------------------------------------------ markers
def check_markers(scenario: dict, fresh_log: str):
    """Marker checks: markers (all), markers_any (>=1), forbidden_markers (none),
    marker_order (markers must appear in listed order)."""
    checks = []
    markers = scenario.get("markers", []) or []
    for marker in markers:
        checks.append({"group": "interaction", "check": f"marker:{marker}",
                       "ok": marker in fresh_log, "detail": "fresh log"})
    any_markers = scenario.get("markers_any", []) or []
    if any_markers:
        hits = [m for m in any_markers if m in fresh_log]
        checks.append({"group": "interaction", "check": "markers_any",
                       "ok": bool(hits),
                       "detail": f"hit {hits[0]!r}" if hits else f"none of {any_markers}"})
    for marker in scenario.get("forbidden_markers", []) or []:
        checks.append({"group": "interaction", "check": f"forbidden:{marker}",
                       "ok": marker not in fresh_log, "detail": "fresh log"})
    if scenario.get("marker_order") and len(markers) > 1:
        pos, ordered = 0, True
        for marker in markers:
            idx = fresh_log.find(marker, pos)
            if idx < 0:
                ordered = False
                break
            pos = idx + len(marker)
        checks.append({"group": "interaction", "check": "marker_order", "ok": ordered,
                       "detail": "markers appear in listed order" if ordered
                       else "markers missing or out of order"})
    return checks


# ------------------------------------------------------------------ orchestration
def run_sim(args):
    artifacts = Path(args.artifacts)
    game_dir = Path(args.game_dir)
    game_exe = Path(args.game_exe) if args.game_exe else game_dir / "DarkAthena.exe"
    host_exe = Path(args.host_exe)
    log_file = game_dir / "riddick_vr.log"

    artifacts.mkdir(parents=True, exist_ok=True)
    for f in (list(artifacts.glob("*.bmp")) + list(artifacts.glob("*.png"))
              + list(artifacts.glob("frame_*.json"))):
        try:
            f.unlink()
        except OSError:
            pass
    log_mark = log_file.stat().st_size if log_file.exists() else 0

    host_cmd = [str(host_exe), "--mock", "--auto-exit-with-game",
                "--dump-dir", str(artifacts), "--dump-interval", str(args.interval),
                "--max-frames", str(args.frames)]
    if args.scenario:
        host_cmd += ["--scenario-file", str(Path(args.scenario).resolve())]
    if args.scenario_immediate:
        host_cmd += ["--scenario-immediate"]

    env = dict(os.environ, RIDDICKVR_SIM="1")  # GH #68: no desktop input
    print(f"[xrsim] host: {' '.join(host_cmd)}")
    host = subprocess.Popen(host_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1, env=env)
    time.sleep(1.5)
    print(f"[xrsim] game: {game_exe}")
    game = subprocess.Popen([str(game_exe)], cwd=str(game_dir), env=env)

    host_log = []
    start = time.time()
    while True:
        if host.poll() is not None or game.poll() is not None:
            break
        if time.time() - start > args.timeout:
            print("[xrsim] timeout")
            break
        line = host.stdout.readline()
        if line:
            host_log.append(line.rstrip())
        else:
            time.sleep(0.03)

    for p, name in ((game, game_exe.name), (host, host_exe.name)):
        try:
            if p.poll() is None:
                p.terminate(); p.wait(timeout=3)
        except Exception:
            subprocess.run(["taskkill", "/F", "/IM", name], capture_output=True)

    return "\n".join(host_log), log_mark


def evaluate(args, host_log, log_mark):
    artifacts = Path(args.artifacts)
    log_file = Path(args.game_dir) / "riddick_vr.log"
    checks = []
    # 1. frame budget
    _, rec = parse_budget(host_log, args.max_frame_ms)
    checks.append({**rec, "group": "frame_budget"})
    # 2. stereo, labelled per frame
    modes = parse_captures(host_log)
    lefts = sorted(artifacts.glob("test_frame_*.bmp"))
    rights_by_n = {int(p.stem.split("_")[1]): p for p in artifacts.glob("right_*.bmp")}
    gameplay_with_content = []
    if not lefts:
        checks.append({"group": "stereo", "check": "frames_captured", "ok": False,
                       "detail": "no test_frame_*.bmp"})
    else:
        for lp in lefts:
            n = int(lp.stem.split("_")[2])
            mode = modes.get(n, "gameplay")  # unlabelled frames are judged as gameplay
            rp = rights_by_n.get(n)
            if rp is None:
                checks.append({"group": "stereo", "check": f"right_frame_{n}", "ok": False,
                               "detail": "missing right_*.bmp"})
                continue
            m = stereo_metrics(lp, rp)
            if mode == "gameplay" and has_content(m):
                gameplay_with_content.append(lp)
            checks.extend(stereo_frame_checks(n, mode, m, args.require_disparity))
        if modes and "gameplay" not in modes.values():
            checks.append({"group": "stereo", "check": "gameplay_reached", "ok": False,
                           "detail": "game never left menus/cinema"})
    # 3. interaction markers
    scenario = {}
    if args.scenario and Path(args.scenario).is_file():
        scenario = json.loads(Path(args.scenario).read_text(encoding="utf-8"))
    fresh = ""
    if log_file.exists():
        fresh = log_file.read_bytes()[log_mark:].decode(errors="ignore")
    checks.extend(check_markers(scenario, fresh))
    # 4. comfort (last gameplay frame that shows content)
    if gameplay_with_content:
        _, sub = comfort_checks(gameplay_with_content[-1])
        for c in sub:
            checks.append({**c, "group": "comfort"})
    passed = all(c["ok"] for c in checks) if checks else False
    return passed, checks


# ------------------------------------------------------------------ selftest
def _selftest_logic(results):
    ok_b, _ = parse_budget(
        "FrameBudget samples=60 mean_ms=8.0 max_ms=12.0 over_budget=2 budget_ms=11.11 gameplay_only=1", 11.11)
    ok_b2, _ = parse_budget(
        "FrameBudget samples=60 mean_ms=30.0 max_ms=90.0 over_budget=50 budget_ms=11.11 gameplay_only=1", 11.11)
    ok_b3, rec3 = parse_budget(
        "FrameBudget samples=30 mean_ms=8.0 max_ms=9.0 over_budget=0 budget_ms=11.11 gameplay_only=1\n"
        "FrameBudget samples=10 mean_ms=8.0 max_ms=9.0 over_budget=0 budget_ms=11.11 gameplay_only=1", 11.11)
    ok_b4, _ = parse_budget(
        "FrameBudget samples=60 mean_ms=8.0 max_ms=9.0 over_budget=0 budget_ms=11.11", 11.11)
    results["budget_ok_detected"] = ok_b
    results["budget_blow_detected"] = not ok_b2
    results["budget_insufficient_samples_fails"] = (not ok_b3) and "insufficient" in rec3["detail"]
    results["budget_ignores_non_gameplay_lines"] = not ok_b4

    log = ("[Mock Host] Captured stereo frame 15 (L=ok R=ok) -> a / b mode=cinema\n"
           "[Mock Host] Captured stereo frame 30 (L=ok R=ok) -> a / b mode=gameplay\n"
           "[Mock Host] Captured stereo frame 45 (L=ok R=ok) -> a / b\n")
    results["capture_mode_parse"] = parse_captures(log) == {15: "cinema", 30: "gameplay"}

    def all_ok(checks):
        return all(c["ok"] for c in checks)

    sc = {"markers": ["A", "B"], "markers_any": ["X", "Y"],
          "forbidden_markers": ["BAD"], "marker_order": True}
    results["markers_pass"] = all_ok(check_markers(sc, "..A..B..Y.."))
    results["markers_missing_fails"] = not all_ok(check_markers({"markers": ["A", "B"]}, "A"))
    results["markers_any_one_hit"] = all_ok(check_markers({"markers_any": ["X", "Y"]}, "Y"))
    results["markers_any_no_hit_fails"] = not all_ok(check_markers({"markers_any": ["X", "Y"]}, "Z"))
    results["markers_forbidden_fails"] = not all_ok(check_markers({"forbidden_markers": ["BAD"]}, "A BAD"))
    results["markers_forbidden_absent_ok"] = all_ok(check_markers({"forbidden_markers": ["BAD"]}, "A"))
    results["markers_order_enforced"] = not all_ok(check_markers(
        {"markers": ["A", "B"], "marker_order": True}, "B A"))


def _selftest_images(results, skipped):
    import tempfile
    if not have_image_deps():
        skipped.extend(["shifted_pair_passes", "identical_gameplay_pair_fails",
                        "black_pair_skipped", "identical_cinema_pair_not_failed",
                        "uniform_frame_skipped"])
        return
    np, Image = image_deps()
    tmp = Path(tempfile.mkdtemp(prefix="xrsim_selftest_"))
    base = (np.random.default_rng(7).random((64, 96, 3)) * 255).astype(np.uint8)

    def save(arr, name):
        Image.fromarray(arr).save(tmp / name)
        return tmp / name

    def frame_ok(left, right, mode):
        m = stereo_metrics(left, right, search=20)
        return all(c["ok"] for c in stereo_frame_checks(1, mode, m, True)), m

    l = save(base, "l_shift.bmp")
    r = save(np.roll(base, 4, axis=1), "r_shift.bmp")
    results["shifted_pair_passes"] = frame_ok(l, r, "gameplay")[0]

    l = save(base, "l_same.bmp")
    r = save(base, "r_same.bmp")
    results["identical_gameplay_pair_fails"] = not frame_ok(l, r, "gameplay")[0]
    results["identical_cinema_pair_not_failed"] = frame_ok(l, r, "cinema")[0]

    black = np.zeros_like(base)
    l = save(black, "l_black.bmp")
    r = save(black, "r_black.bmp")
    ok_black, m_black = frame_ok(l, r, "gameplay")
    skipped_black = [c for c in stereo_frame_checks(1, "gameplay", m_black, True)
                     if c["check"] == "stereo_skipped"]
    results["black_pair_skipped"] = ok_black and bool(skipped_black)

    grey = np.full_like(base, 120)
    l = save(grey, "l_grey.bmp")
    r = save(grey, "r_grey.bmp")
    ok_grey, m_grey = frame_ok(l, r, "gameplay")
    results["uniform_frame_skipped"] = ok_grey and not has_content(m_grey)


def cmd_selftest(require_image_deps=False):
    """Validate the stereo/comfort/budget/marker logic without the game (CI-safe)."""
    results, skipped = {}, []
    _selftest_logic(results)
    _selftest_images(results, skipped)
    allok = all(results.values())
    for k, v in results.items():
        print(f"  {'ok ' if v else 'FAIL'} {k}")
    for k in skipped:
        print(f"  SKIP (numpy/Pillow missing) {k}")
    if skipped and require_image_deps:
        print("FAIL xrsim selftest: image cases skipped but --require-image-deps was given")
        return 1
    print("PASS xrsim selftest" if allok else "FAIL xrsim selftest")
    return 0 if allok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--scenario")
    ap.add_argument("--frames", type=int, default=180)
    ap.add_argument("--interval", type=int, default=15)
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--max-frame-ms", type=float, default=1000.0 / 90.0)
    ap.add_argument("--no-require-disparity", dest="require_disparity",
                    action="store_false")
    ap.add_argument("--game-dir", default=str(GAME_DIR),
                    help="game System\\Win32_x86 dir (env RIDDICKVR_GAME_DIR)")
    ap.add_argument("--game-exe", default=os.environ.get("RIDDICKVR_GAME_EXE"),
                    help="game exe (env RIDDICKVR_GAME_EXE; default <game-dir>\\DarkAthena.exe)")
    ap.add_argument("--host-exe", default=str(HOST_EXE),
                    help="riddickvr-host.exe (env RIDDICKVR_HOST_EXE)")
    ap.add_argument("--artifacts", default=str(ARTIFACTS), help="capture output directory")
    ap.add_argument("--report", default=str(REPORT), help="JSON report path")
    ap.add_argument("--scenario-immediate", action="store_true",
                    help="start the scenario at host frame 0 instead of waiting for gameplay")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--require-image-deps", action="store_true",
                    help="selftest: fail instead of skipping image cases without numpy/Pillow")
    args = ap.parse_args(argv)

    if args.selftest:
        return cmd_selftest(args.require_image_deps)
    if not have_image_deps():
        print("xrsim: numpy and Pillow are required for real runs (pip install numpy pillow)",
              file=sys.stderr)
        return 2

    host_log, log_mark = run_sim(args)
    passed, checks = evaluate(args, host_log, log_mark)

    report = Path(args.report)
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps({"passed": passed, "checks": checks}, indent=2),
                      encoding="utf-8")
    print("\n" + "=" * 60)
    for c in checks:
        print(f"  [{'PASS' if c['ok'] else 'FAIL'}] {c.get('group','')}: {c['check']} ({c['detail']})")
    print("=" * 60)
    print(f"  XRSIM RESULT: {'PASS' if passed else 'FAIL'}  -> {report}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
