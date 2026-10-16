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
  python tools/xrsim.py --evaluate-only       # re-analyse captures on disk,
                                              # no game/host launch (CI-safe)
  python tools/xrsim.py --selftest            # logic check, no game (CI)

The ``--evaluate-only`` path is what makes the harness usable *without a
headset*: a previous run's ``test_frame_*.bmp`` / ``right_*.bmp`` pairs are
re-checked and written to ``tests/xrsim/report.json`` for diffing.
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

import numpy as np
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parent.parent

GAME_DIR = Path(r"F:\The Chronicles of Riddick - Assault on Dark Athena\System\Win32_x86")
GAME_EXE = GAME_DIR / "DarkAthena.exe"
HOST_EXE = REPO_ROOT / "dist" / "riddickvr-host.exe"
LOG_FILE = GAME_DIR / "riddick_vr.log"
DEFAULT_ARTIFACTS = REPO_ROOT / "tests" / "xrsim" / "artifacts"
DEFAULT_REPORT = REPO_ROOT / "tests" / "xrsim" / "report.json"

FRAME_BUDGET_RE = re.compile(
    r"FrameBudget samples=(\d+) mean_ms=([\d.]+) max_ms=([\d.]+) "
    r"over_budget=(\d+) budget_ms=([\d.]+)")


# ---------------------------------------------------------------- stereo diff
def stereo_metrics(left_path: Path, right_path: Path, search=48, downscale=1):
    """Best horizontal/vertical alignment of the two eye images.

    Returns dict with: non_black_l/r, best_dx, best_dy, residual (MSE at best),
    identical (bool). The mod is a monoscopic blit with an IPD shift, so a
    correct frame has a small horizontal dx and ~zero dy.

    ``downscale`` (integer >= 1) block-subsamples both images before the search
    so multi-megapixel eye buffers can be analysed quickly; dx/dy are then in
    downscaled pixels (multiply by ``downscale`` for the full-res shift).
    """
    L = np.array(Image.open(left_path).convert("L"), dtype=np.float32)
    R = np.array(Image.open(right_path).convert("L"), dtype=np.float32)
    ds = max(1, int(downscale))
    if ds > 1:
        L = L[::ds, ::ds]
        R = R[::ds, ::ds]
    out = {"shape_l": list(L.shape), "shape_r": list(R.shape), "downscale": ds}
    out["mean_l"] = float(L.mean())
    out["mean_r"] = float(R.mean())
    out["non_black_l"] = out["mean_l"] > 1.0
    out["non_black_r"] = out["mean_r"] > 1.0
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


def assert_stereo(metrics, require_disparity=True):
    checks = []
    ok = True

    def chk(name, cond, detail=""):
        nonlocal ok
        checks.append({"check": name, "ok": bool(cond), "detail": detail})
        ok = ok and bool(cond)

    non_black = metrics.get("non_black_l") and metrics.get("non_black_r")
    chk("both_eyes_non_black", non_black,
        f"meanL={metrics.get('mean_l'):.4g} meanR={metrics.get('mean_r'):.4g}"
        if metrics.get("mean_l") is not None else "")
    if not non_black:
        # GH #68: all-zero eye buffers mean the GL<->D3D11 interop copy never
        # produced content (distinct from a legitimately black scene). Keep the
        # remaining geometry checks running so the report still shows shapes.
        chk("interop_produced_content", False,
            "eye buffers are empty: check the hook 'Hardware Interop' lines and "
            "StereoRenderer::LockSharedTextures/UnlockSharedTextures")
    if metrics.get("shape_l") != metrics.get("shape_r"):
        chk("same_eye_resolution", False, "shape mismatch")
        return ok, checks
    chk("same_eye_resolution", True)
    dx, dy = metrics.get("best_dx"), metrics.get("best_dy")
    chk("no_vertical_disparity", dy is not None and abs(dy) <= 2, f"dy={dy}")
    if require_disparity:
        chk("horizontal_disparity_present", dx is not None and abs(dx) >= 1,
            f"dx={dx}")
    chk("not_mono_duplicate", not metrics.get("identical"),
        f"identical={metrics.get('identical')}")
    return ok, checks


# --------------------------------------------------------------- capture health
def frame_signature(path: Path):
    """Cheap 16x16 luma signature used to tell live frames from a static scene."""
    return Image.open(path).convert("L").resize((16, 16)).tobytes()


def capture_health_checks(lefts):
    """Flag captures where nothing changed: a static/menu screen cannot validate
    gameplay stereo, so an all-identical dump series is not a valid run."""
    checks = []
    if len(lefts) < 2:
        checks.append({"check": "scene_advanced", "ok": True, "skip": True,
                       "detail": f"only {len(lefts)} frame(s); cannot judge"})
        return checks
    sigs = [frame_signature(p) for p in lefts]
    distinct = len(set(sigs))
    detail = f"{distinct}/{len(lefts)} distinct frames"
    if distinct < 2:
        detail += " (frozen capture: cannot validate stereo on a static scene)"
    checks.append({"check": "scene_advanced", "ok": distinct >= 2, "detail": detail})
    return checks


# ------------------------------------------------------------------ comfort
def comfort_checks(left_path: Path):
    arr = np.array(Image.open(left_path).convert("RGB"))
    h, w, _ = arr.shape
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
    samples = FRAME_BUDGET_RE.findall(host_log)
    if not samples:
        return False, {"check": "frame_budget_reported", "ok": False,
                       "detail": "no FrameBudget line from host"}
    n, mean, mx, over, budget = samples[-1]
    n, mean, mx, over, budget = int(n), float(mean), float(mx), int(over), float(budget)
    tol = max(budget_ms, budget) * 1.5
    rec = {"check": "frame_budget", "ok": (mean <= budget_ms and mx <= tol),
           "detail": f"n={n} mean={mean:.2f} max={mx:.2f} over={over} budget={budget:.2f}"}
    return rec["ok"], rec


# ------------------------------------------------------------------ orchestration
def run_sim(args, artifacts: Path):
    artifacts.mkdir(parents=True, exist_ok=True)
    for f in list(artifacts.glob("*.bmp")) + list(artifacts.glob("*.png")):
        try:
            f.unlink()
        except OSError:
            pass
    log_mark = LOG_FILE.stat().st_size if LOG_FILE.exists() else 0

    host_cmd = [str(HOST_EXE), "--mock", "--auto-exit-with-game",
                "--dump-dir", str(artifacts), "--dump-interval", str(args.interval),
                "--max-frames", str(args.frames)]
    if args.scenario:
        host_cmd += ["--scenario-file", str(Path(args.scenario).resolve())]

    env = dict(os.environ, RIDDICKVR_SIM="1")  # GH #68: no desktop input
    print(f"[xrsim] host: {' '.join(host_cmd)}")
    host = subprocess.Popen(host_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1, env=env)
    time.sleep(1.5)
    print(f"[xrsim] game: {GAME_EXE}")
    game = subprocess.Popen([str(GAME_EXE)], cwd=str(GAME_DIR), env=env)

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

    for p, name in ((game, "DarkAthena.exe"), (host, "riddickvr-host.exe")):
        try:
            if p.poll() is None:
                p.terminate(); p.wait(timeout=3)
        except Exception:
            subprocess.run(["taskkill", "/F", "/IM", name], capture_output=True)

    return "\n".join(host_log), log_mark


def evaluate(args, artifacts: Path, host_log: str, fresh_log: str):
    """Evaluate one run's evidence.

    ``host_log`` is the mock host's stdout (may be empty for --evaluate-only);
    ``fresh_log`` is the text of the hook log to scan for interaction markers.
    A check with ``"skip": True`` is reported but does not affect the verdict.
    """
    checks = []
    # 1. frame budget (requires a live host log; skipped when re-analysing)
    if host_log.strip():
        ok_b, rec = parse_budget(host_log, args.max_frame_ms)
        checks.append({**rec, "group": "frame_budget"})
    else:
        checks.append({"group": "frame_budget", "check": "frame_budget", "ok": True,
                       "skip": True, "detail": "no host log (evaluate-only)"})
    # 2. stereo
    lefts = sorted(artifacts.glob("test_frame_*.bmp"))
    rights_by_n = {int(p.stem.split("_")[1]): p for p in artifacts.glob("right_*.bmp")}
    if not lefts:
        checks.append({"group": "stereo", "check": "frames_captured", "ok": False,
                       "detail": f"no test_frame_*.bmp in {artifacts}"})
    else:
        for c in capture_health_checks(lefts):
            checks.append({**c, "group": "capture"})
        for lp in lefts:
            n = int(lp.stem.split("_")[2])
            rp = rights_by_n.get(n)
            if rp is None:
                checks.append({"group": "stereo", "check": f"right_frame_{n}", "ok": False,
                               "detail": "missing right_*.bmp"})
                continue
            m = stereo_metrics(lp, rp, downscale=args.downscale)
            _, sub = assert_stereo(m, require_disparity=args.require_disparity)
            for c in sub:
                checks.append({**c, "group": "stereo", "frame": n,
                               "detail": f"{c['detail']} {m.get('best_dx')}/{m.get('best_dy')}"})
    # 3. interaction markers
    req = []
    if args.scenario and Path(args.scenario).is_file():
        req = json.loads(Path(args.scenario).read_text(encoding="utf-8")).get("markers", [])
    for marker in req:
        hit = marker in fresh_log
        checks.append({"group": "interaction", "check": f"marker:{marker}", "ok": hit,
                       "detail": "hook log"})
    # 4. comfort
    if lefts:
        _, sub = comfort_checks(lefts[-1])
        for c in sub:
            checks.append({**c, "group": "comfort",
                           "detail": f"{c.get('detail','')} (frame {lefts[-1].name})"})
    passed = all(c["ok"] for c in checks if not c.get("skip")) if checks else False
    return passed, checks


def write_report(report: Path, passed: bool, checks, extra=None):
    report.parent.mkdir(parents=True, exist_ok=True)
    payload = {"passed": passed, "checks": checks}
    if extra:
        payload.update(extra)
    report.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return report


def print_summary(checks, report: Path, passed: bool):
    print("\n" + "=" * 60)
    for c in checks:
        tag = "SKIP" if c.get("skip") else ("PASS" if c["ok"] else "FAIL")
        print(f"  [{tag}] {c.get('group','')}: {c['check']} ({c.get('detail','')})")
    print("=" * 60)
    print(f"  XRSIM RESULT: {'PASS' if passed else 'FAIL'}  -> {report}")


def cmd_selftest():
    """Validate the stereo/comfort/budget logic without the game (CI-safe)."""
    import tempfile
    tmp = Path(tempfile.mkdtemp(prefix="xrsim_selftest_"))
    # shifted pair -> should pass
    base = (np.random.default_rng(7).random((64, 96, 3)) * 255).astype(np.uint8)
    Image.fromarray(base).save(tmp / "test_frame_0001.bmp")
    shifted = np.roll(base, 4, axis=1)
    Image.fromarray(shifted).save(tmp / "right_0001.bmp")
    m = stereo_metrics(tmp / "test_frame_0001.bmp", tmp / "right_0001.bmp", search=20)
    ok_shift, _ = assert_stereo(m, require_disparity=True)
    # identical pair -> should fail (mono duplicate)
    Image.fromarray(base).save(tmp / "right_0002.bmp")
    Image.fromarray(base).save(tmp / "test_frame_0002.bmp")
    m2 = stereo_metrics(tmp / "test_frame_0002.bmp", tmp / "right_0002.bmp", search=20)
    ok_dup, checks_dup = assert_stereo(m2, require_disparity=True)
    ok_b, _ = parse_budget("FrameBudget samples=60 mean_ms=8.0 max_ms=12.0 over_budget=2 budget_ms=11.11", 11.11)
    ok_b2, _ = parse_budget("FrameBudget samples=60 mean_ms=30.0 max_ms=90.0 over_budget=50 budget_ms=11.11", 11.11)
    results = {
        "shifted_pair_passes": ok_shift,
        "identical_pair_fails": (not ok_dup),
        "budget_ok_detected": ok_b,
        "budget_blow_detected": (not ok_b2),
    }
    allok = all(results.values())
    for k, v in results.items():
        print(f"  {'ok ' if v else 'FAIL'} {k}")
    print("PASS xrsim selftest" if allok else "FAIL xrsim selftest")
    return 0 if allok else 1


def cmd_evaluate_only(args):
    artifacts = Path(args.artifacts) if args.artifacts else DEFAULT_ARTIFACTS
    report = Path(args.report_file) if args.report_file else DEFAULT_REPORT
    lefts = sorted(artifacts.glob("test_frame_*.bmp"))
    if not lefts:
        print(f"[xrsim] evaluate-only: no captures in {artifacts}; nothing to do.")
        return 0
    log_path = Path(args.hook_log) if args.hook_log else LOG_FILE
    fresh = log_path.read_text(errors="ignore") if log_path.is_file() else ""
    if not fresh:
        print(f"[xrsim] evaluate-only: no hook log at {log_path}; marker checks will be empty.")
    passed, checks = evaluate(args, artifacts, "", fresh)
    write_report(report, passed, checks,
                 extra={"mode": "evaluate-only", "artifacts": str(artifacts),
                        "hook_log": str(log_path)})
    print_summary(checks, report, passed)
    return 0 if passed else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--scenario")
    ap.add_argument("--frames", type=int, default=180)
    ap.add_argument("--interval", type=int, default=15)
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--max-frame-ms", type=float, default=1000.0 / 90.0)
    ap.add_argument("--no-require-disparity", dest="require_disparity",
                    action="store_false")
    ap.add_argument("--downscale", type=int, default=1,
                    help="block-subsample eye images by this factor (speed)")
    ap.add_argument("--artifacts", help="capture directory (default tests/xrsim/artifacts)")
    ap.add_argument("--report-file", help="report path (default tests/xrsim/report.json)")
    ap.add_argument("--hook-log", help="hook log to scan for markers (default: game log)")
    ap.add_argument("--evaluate-only", action="store_true",
                    help="re-analyse existing captures; do not launch host/game")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        return cmd_selftest()
    if args.evaluate_only:
        return cmd_evaluate_only(args)

    artifacts = Path(args.artifacts) if args.artifacts else DEFAULT_ARTIFACTS
    report = Path(args.report_file) if args.report_file else DEFAULT_REPORT
    host_log, log_mark = run_sim(args, artifacts)

    fresh = ""
    if LOG_FILE.exists():
        fresh = LOG_FILE.read_bytes()[log_mark:].decode(errors="ignore")
    passed, checks = evaluate(args, artifacts, host_log, fresh)
    write_report(report, passed, checks,
                 extra={"mode": "run", "artifacts": str(artifacts)})
    print_summary(checks, report, passed)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
