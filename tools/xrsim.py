#!/usr/bin/env python3
"""XRSIM - OpenXR simulator harness for Riddick VR (GH #68).

Runs the mock OpenXR host + game with *scripted* HMD/controller poses (no real
headset), captures BOTH eyes, and asserts:

  * frame-time budget   (host FrameBudget lines),
  * stereo correctness   (left/right eye diff: non-identical, horizontal
                          disparity present, no vertical disparity),
  * display branch       (per-frame cinema-vs-gameplay label: identical eyes are
                          correct on a cinema screen and only a defect when the
                          frame is 3D gameplay -- GH #69),
  * interaction state    (scenario markers present in the fresh hook log; the
                          log is truncated per launch, so the *current run's*
                          section is recovered by its header, not a byte offset),
  * comfort rules        (non-black, no green chroma glitch, full-bleed/HUD).

Sim runs set RIDDICKVR_SIM=1 so the hook never injects desktop input (#68), and
default to a *gameplay-only* dump window (``--dump-gameplay-only``) so the
captures are live 3D gameplay rather than the menu/cinema frames whose
identical eye pairs are correct but cannot validate stereo (#69).

Usage:
  python tools/xrsim.py --scenario tools/scenarios/melee.json
  python tools/xrsim.py --reach-gameplay --duration 45   # run long enough to
                                              # reach live 3D gameplay (GH #71)
  python tools/xrsim.py --evaluate-only       # re-analyse captures on disk,
                                              # no game/host launch (CI-safe)
  python tools/xrsim.py --selftest            # logic check, no game (CI)

The ``--evaluate-only`` path is what makes the harness usable *without a
headset*: a previous run's ``test_frame_*.bmp`` / ``right_*.bmp`` pairs are
re-checked and written to ``tests/xrsim/report.json`` for diffing.

``--reach-gameplay`` (GH #71 requirement 1) runs with an unlimited frame count
bounded by wall-clock ``--duration``, because the mock host follows the *game's*
uncapped swap rate (~250-300 FPS on the trivial startup splash): a 180-frame
window is ~0.6 s and never gets past the splash. With a ~15 s window the title
reaches live 3D gameplay. That mode also asserts ``gameplay_full_bleed_reached``
from the hook's ``BarScan`` active-rect fill (the splash is letterboxed; gameplay
fills the window) -- a structural proof, since the ``cinemaMode`` bit mislabels
the static splash pages as gameplay (GH #72).

The gameplay-only dump window is *content-gated* (GH #72 requirement 2): even a
gameplay-labelled frame is kept out while it is all-black (the GL<->D3D11 interop
warm-up) or byte-identical to the last dumped frame (a frozen static page), so the
graded window is live, advancing 3D gameplay. The report carries
``content_gate {blank_skipped, frozen_skipped}`` and, for gameplay-only runs, the
``gameplay_window_has_content`` check.
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

# GH #71 requirement 1: the mock host tracks the *game's* uncapped swap rate
# (~250-300 FPS on the trivial startup splash), so a frame-counted window is only
# a fraction of the wall-clock the title needs to finish its multi-page splash +
# level load. A 180-frame run is ~0.6 s; reaching live 3D gameplay needs ~15 s of
# wall clock. `--reach-gameplay` therefore runs with an *unlimited* frame count
# (`--max-frames 0`) and is bounded by wall-clock `--duration` instead.
GAMEPLAY_DEFAULT_DURATION = 60.0
GAMEPLAY_DEFAULT_INTERVAL = 300   # a startup-length run must not fill the disk
DEFAULT_DUMP_INTERVAL = 15

# GH #71 requirement 2 evidence: the hook's `BarScan` logs the game's active
# render rect. The startup splash is letterboxed to ~(336,80)-(944,477) of a
# 1280x605 window (~36% of the area); live gameplay is the full window. That
# *structural* signal -- unlike the (mislabelled) `cinemaMode` branch bit -- is
# what proves a full-bleed 3D scene was actually reached.
BARSCAN_RECT_RE = re.compile(r"\((-?\d+),\s*(-?\d+)\)\s*-\s*\((-?\d+),\s*(-?\d+)\)")
BARSCAN_WINDOW_RE = re.compile(r"of \((\d+),\s*(\d+)\) win")
FULL_BLEED_MIN_FILL = 0.9

# GH #69: the mock host writes one label line per dumped capture, recording the
# display branch the hook published for that frame (cinema screen vs 3D
# gameplay). Parsed here so identical eyes can be attributed, not guessed.
DISPLAY_LABEL_RE = re.compile(
    r"frame=(\d+)\s+cinema=([01])\s+left=(\w+)\s+right=(\w+)")
DISPLAY_LABEL_FILE = "display_branch.log"

# GH #72 requirement 2: the mock host's content gate logs every gameplay frame
# it kept out of the graded window (all-black interop warm-up = `blank`; a
# byte-identical repeat of the last dumped frame = `frozen`). Parsed so the
# report can show the gate doing its job.
DUMP_SKIP_RE = re.compile(r"DumpGate skip frame=(\d+) reason=(blank|frozen)")

# GH #67 step 2: observation-only probe of the engine's first-person viewmodel
# pass. The hook (ViewmodelObserve=1) logs, at the sampled frames, how many draws
# fell inside the depth-only-clear window and the GL_MODELVIEW captured at the
# first in-window draw. Parsed here so a run can *identify* the pass (the draw
# count / matrix are the input for driving it from the controller next).
VIEWMODEL_PROBE_RE = re.compile(
    r"ViewmodelProbe \[#(\d+)\]: inside=(\d+) outside=(\d+) sample=([01])"
    r"(?: pushes=(\d+) translates=(\d+))?")
VIEWMODEL_PROBE_MATRIX_RE = re.compile(r"ViewmodelProbe matrix:")
_MATRIX_ROW_RE = re.compile(r"\[([-\d.eE ]+)\]")
# GH #69 requirement 4: `Logger::Initialize` (src/hook/logging.cpp) opens
# riddick_vr.log with `std::ios::out | std::ios::trunc`, so every game launch
# rewrites the file from offset 0. A byte offset sampled *before* launch (the old
# `log_mark` slice) therefore drops the head of the new run -- including markers
# logged before the previous run's byte length. Split on the last run header so
# marker checks always see the run that was just captured.
RUN_HEADER = "=== Riddick VR Log Started at"


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
def read_display_branch_labels(artifacts: Path):
    """GH #69: map frame number -> isCinema from the host's ``display_branch.log``.

    Returns ``{}`` when the label file is absent (e.g. a legacy capture or a run
    from a host build predating the label). A frame missing from the map is
    reported as ``branch="unknown"`` rather than assumed to be gameplay.
    """
    path = Path(artifacts) / DISPLAY_LABEL_FILE
    labels = {}
    if not path.is_file():
        return labels
    for line in path.read_text(errors="ignore").splitlines():
        m = DISPLAY_LABEL_RE.search(line)
        if m:
            labels[int(m.group(1))] = (m.group(2) == "1")
    return labels


def frame_signature(path: Path):
    """Cheap 16x16 luma signature used to tell live frames from a static scene."""
    return Image.open(path).convert("L").resize((16, 16)).tobytes()


def extract_run_section(text: str) -> str:
    """Return the section of a hook log written by the *current* run.

    The hook truncates ``riddick_vr.log`` on every launch (see ``RUN_HEADER``),
    so a byte offset is not a run boundary. Everything after the last
    ``=== Riddick VR Log Started at ... ===`` header is this run; when no header
    is present (an appending producer) the whole text is returned.
    """
    idx = text.rfind(RUN_HEADER)
    return text[idx:] if idx >= 0 else text


def read_fresh_hook_log(path: Path, log_mark: int = 0) -> str:
    """Read the hook log's *current-run* text (GH #69 requirement 4).

    Prefers the last run header (truncation-proof). Falls back to the legacy
    byte mark only for a header-less/appending producer.
    """
    path = Path(path)
    if not path.is_file():
        return ""
    try:
        raw = path.read_bytes()
    except OSError:
        return ""
    text = raw.decode(errors="ignore")
    if RUN_HEADER in text:
        return extract_run_section(text)
    return raw[log_mark:].decode(errors="ignore")


def marker_summary(checks):
    """Map each scenario marker to its pass/fail verdict (for the report)."""
    out = {}
    for c in checks:
        name = c.get("check", "")
        if name.startswith("marker:"):
            out[name[len("marker:"):]] = bool(c.get("ok"))
    return out


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


def gameplay_capture_stats(artifacts, labels, min_mean=1.0, sig_cache=None):
    """Summarise the gameplay-labelled captures of a run (GH #71 req 1/3).

    Returns the count of gameplay-labelled frames, how many were blank
    (GL<->D3D11 interop warm-up: an all-black eye buffer is not "live"), how many
    cinema/unknown frames there were, and how many *distinct* non-blank gameplay
    frames (16x16 luma signature) the run produced.

    ``sig_cache`` (a ``{path: signature}`` dict) lets the caller reuse decoded
    signatures across calls -- each eye BMP is multi-megabyte, so re-decoding
    them on every poll would dominate the run.
    """
    artifacts = Path(artifacts)
    total = cinema = unknown = blank = 0
    sigs = set()
    for lp in sorted(artifacts.glob("test_frame_*.bmp")):
        n = int(lp.stem.split("_")[2])
        branch = labels.get(n)
        if branch is True:
            cinema += 1
            continue
        if branch is None:
            unknown += 1
            continue
        total += 1
        key = str(lp)
        if sig_cache is not None and key in sig_cache:
            sig = sig_cache[key]
        else:
            try:
                arr = np.asarray(Image.open(lp).convert("L"), dtype=np.float32)
            except Exception:
                continue
            sig = (b"" if float(arr.mean()) <= min_mean else
                   Image.fromarray(arr.astype(np.uint8)).resize((16, 16)).tobytes())
            if sig_cache is not None:
                sig_cache[key] = sig
        if sig:
            sigs.add(sig)
        else:
            blank += 1
    return {"gameplay": total, "cinema": cinema, "unknown": unknown,
            "blank_gameplay": blank, "distinct_gameplay": len(sigs)}


def viewmodel_probe_stats(log_text):
    """GH #67 step 2: summarise the observation-only first-person viewmodel probe.

    Read from the hook log (needs ``ViewmodelObserve=1``): the sampled frames'
    in/out-of-window draw counts and the GL_MODELVIEW captured at the first
    in-window draw. Returns ``{"lines": [...], "matrix": [...16 floats]|None}``;
    an empty ``lines`` list means the probe was not enabled / not present (a
    pre-#67 hook or a run without the flag), so callers must *skip*, not fail.
    """
    lines = []
    matrix = None
    pushes = 0
    translates = 0
    text = log_text or ""
    for m in VIEWMODEL_PROBE_RE.finditer(text):
        p = int(m.group(5)) if m.group(5) is not None else None
        t = int(m.group(6)) if m.group(6) is not None else None
        if p:
            pushes += p
        if t:
            translates += t
        lines.append({"index": int(m.group(1)), "inside": int(m.group(2)),
                      "outside": int(m.group(3)), "sample": m.group(4) == "1",
                      "pushes": p, "translates": t})
    mm = VIEWMODEL_PROBE_MATRIX_RE.search(text)
    if mm:
        rows = _MATRIX_ROW_RE.findall(text[mm.end():])
        if len(rows) >= 4:
            try:
                vals = [float(v) for row in rows[:4] for v in row.split()]
            except ValueError:
                vals = []
            if len(vals) == 16:
                matrix = vals
    return {"lines": lines, "matrix": matrix, "pushes": pushes, "translates": translates}


def content_gate_stats(host_log):
    """GH #72 requirement 2: frames the host's content gate kept out of the
    graded window (``blank`` interop warm-up / ``frozen`` byte-identical
    repeats). Empty when the host log carries no ``DumpGate skip`` lines (a
    pre-#72 host, or a run that needed no gating)."""
    blank = frozen = 0
    for line in (host_log or "").splitlines():
        m = DUMP_SKIP_RE.search(line)
        if not m:
            continue
        if m.group(2) == "blank":
            blank += 1
        else:
            frozen += 1
    return {"blank_skipped": blank, "frozen_skipped": frozen}


def active_rect_fill(log_text):
    """Largest active-render-rect fill ratio the hook's BarScan recorded.

    ``1.0`` = the game filled the whole window (full-bleed 3D gameplay); the
    startup splash is ~0.36. Returns ``None`` when the log carries no BarScan
    window/rect (a pre-BarScan host or an unrelated log).
    """
    win = BARSCAN_WINDOW_RE.search(log_text or "")
    if not win:
        return None
    w, h = int(win.group(1)), int(win.group(2))
    if w <= 0 or h <= 0:
        return None
    best = 0.0
    for m in BARSCAN_RECT_RE.finditer(log_text):
        x0, y0, x1, y1 = (int(g) for g in m.groups())
        area = max(0, x1 - x0) * max(0, y1 - y0)
        if area / float(w * h) > best:
            best = area / float(w * h)
    return best if best > 0.0 else None


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
def build_host_cmd(args, artifacts: Path):
    """Build the mock-host command line for one scripted run.

    GH #69 requirement 1: scripted runs default to ``--dump-gameplay-only`` so
    the capture window is live 3D gameplay -- the frames that can actually
    validate stereo -- instead of the menu/cinema and all-zero interop warm-up
    frames (where identical eyes are correct). Pass ``--dump-branch any`` to
    keep the legacy capture-everything cadence.
    """
    cmd = [str(HOST_EXE), "--mock", "--auto-exit-with-game",
           "--dump-dir", str(artifacts), "--dump-interval", str(args.interval),
           "--max-frames", str(0 if getattr(args, "reach_gameplay", False) else args.frames)]
    if getattr(args, "dump_branch", "gameplay") == "gameplay":
        cmd.append("--dump-gameplay-only")
    if args.scenario:
        cmd += ["--scenario-file", str(Path(args.scenario).resolve())]
    return cmd


def run_sim(args, artifacts: Path):
    artifacts.mkdir(parents=True, exist_ok=True)
    for f in (list(artifacts.glob("*.bmp")) + list(artifacts.glob("*.png")) +
              [artifacts / DISPLAY_LABEL_FILE, artifacts / "host.log"]):
        try:
            f.unlink()
        except OSError:
            pass
    log_mark = LOG_FILE.stat().st_size if LOG_FILE.exists() else 0

    host_cmd = build_host_cmd(args, artifacts)

    env = dict(os.environ, RIDDICKVR_SIM="1")  # GH #68: no desktop input
    print(f"[xrsim] host: {' '.join(host_cmd)}")
    host = subprocess.Popen(host_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1, env=env)
    time.sleep(1.5)
    print(f"[xrsim] game: {GAME_EXE}")
    game = subprocess.Popen([str(GAME_EXE)], cwd=str(GAME_DIR), env=env)

    duration = getattr(args, "duration", None)
    host_log = []
    start = time.time()
    while True:
        if host.poll() is not None or game.poll() is not None:
            break
        elapsed = time.time() - start
        if elapsed > args.timeout:
            print("[xrsim] timeout")
            break
        # GH #71: an unlimited-frame run is bounded by wall-clock, not by a frame
        # count (the mock follows the game's uncapped swap rate).
        if duration and elapsed > duration:
            print(f"[xrsim] reached --duration {duration:.0f}s")
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


def host_log_path(artifacts: Path) -> Path:
    return Path(artifacts) / "host.log"


def persist_host_log(artifacts: Path, host_log: str) -> Path:
    """GH #69: keep the mock host's stdout so frame-budget / capture lines
    survive the run and can be re-analysed by ``--evaluate-only``."""
    path = host_log_path(artifacts)
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(host_log, encoding="utf-8")
    except OSError:
        pass
    return path


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
    labels = read_display_branch_labels(artifacts)
    branch_counts = {"cinema": 0, "gameplay": 0, "unknown": 0}
    if not lefts:
        checks.append({"group": "stereo", "check": "frames_captured", "ok": False,
                       "detail": f"no test_frame_*.bmp in {artifacts}"})
    else:
        for c in capture_health_checks(lefts):
            checks.append({**c, "group": "capture"})
        for lp in lefts:
            n = int(lp.stem.split("_")[2])
            branch = labels.get(n)
            branch_name = ("cinema" if branch is True else
                           "gameplay" if branch is False else "unknown")
            branch_counts[branch_name] += 1
            rp = rights_by_n.get(n)
            if rp is None:
                checks.append({"group": "stereo", "check": f"right_frame_{n}", "ok": False,
                               "branch": branch_name,
                               "detail": "missing right_*.bmp"})
                continue
            m = stereo_metrics(lp, rp, downscale=args.downscale)
            _, sub = assert_stereo(m, require_disparity=args.require_disparity)
            for c in sub:
                rec = {**c, "group": "stereo", "frame": n, "branch": branch_name,
                       "detail": f"{c['detail']} {m.get('best_dx')}/{m.get('best_dy')}"}
                # GH #69: on a cinema screen the renderer deliberately blits the
                # same source rect to both eyes ("flat screen: no IPD shift"), so
                # identical eyes are CORRECT there. The gameplay-only stereo
                # gates become informational instead of failures.
                if branch is True and c["check"] in ("horizontal_disparity_present",
                                                     "not_mono_duplicate"):
                    rec["ok"] = True
                    rec["skip"] = True
                    rec["detail"] = ("cinema frame: identical eyes expected "
                                     "(no IPD shift); " + rec["detail"])
                checks.append(rec)
        # GH #69 acceptance: a run is only meaningful for the stereo gates if it
        # captured at least one live 3D gameplay frame. A menu-only run cannot
        # validate gameplay stereo and must not be recorded as a pass.
        if labels:
            checks.append({"group": "stereo", "check": "gameplay_captures_present",
                           "ok": branch_counts["gameplay"] >= 1,
                           "detail": (f"{branch_counts['gameplay']} gameplay / "
                                      f"{branch_counts['cinema']} cinema / "
                                      f"{branch_counts['unknown']} unknown of {len(lefts)}")})
            # GH #72 requirement 2: the graded window must not contain blank
            # (all-black interop warm-up) gameplay frames. The host's content
            # gate keeps them out; this asserts the window really is live.
            if getattr(args, "dump_branch", "gameplay") == "gameplay":
                stats = gameplay_capture_stats(artifacts, labels)
                checks.append({"group": "capture", "check": "gameplay_window_has_content",
                               "ok": stats["blank_gameplay"] == 0,
                               "detail": (f"{stats['blank_gameplay']} blank of "
                                          f"{stats['gameplay']} gameplay frame(s)")})
        else:
            checks.append({"group": "stereo", "check": "gameplay_captures_present",
                           "ok": True, "skip": True,
                           "detail": (f"no {DISPLAY_LABEL_FILE}: per-frame display "
                                      "branch unknown (legacy capture or pre-label host)")})
    # GH #71 requirement 1: a `--reach-gameplay` run asserts it actually reached a
    # full-bleed 3D scene. The *structural* proof is the hook's BarScan active
    # rect (the letterboxed startup splash is ~0.36 of the window; gameplay fills
    # it), deliberately NOT the cinemaMode branch bit -- that bit mislabels the
    # static 2D splash pages as gameplay (GH #72).
    if getattr(args, "reach_gameplay", False):
        fill = active_rect_fill(fresh_log)
        if fill is None:
            detail = "no BarScan active-rect line in the hook log"
        else:
            detail = (f"max BarScan active-rect fill={fill:.2f} "
                      f"(need >= {FULL_BLEED_MIN_FILL})")
        checks.append({"group": "capture", "check": "gameplay_full_bleed_reached",
                       "ok": fill is not None and fill >= FULL_BLEED_MIN_FILL,
                       "detail": detail})
    # 3. interaction markers
    req = []
    if args.scenario and Path(args.scenario).is_file():
        req = json.loads(Path(args.scenario).read_text(encoding="utf-8")).get("markers", [])
    for marker in req:
        hit = marker in fresh_log
        checks.append({"group": "interaction", "check": f"marker:{marker}", "ok": hit,
                       "detail": f"hook log run section ({len(fresh_log)} chars)"})
    # GH #67 step 2: when the observation-only viewmodel probe is enabled
    # (ViewmodelObserve=1), assert it actually identified a non-empty
    # first-person pass -- the input for driving the engine's own arms from the
    # controller. Skipped (not failed) when the probe was not run, so this does
    # not fail a normal capture.
    probe = viewmodel_probe_stats(fresh_log)
    if probe["lines"]:
        opened = [p for p in probe["lines"] if p["inside"] > 0]
        max_inside = max((p["inside"] for p in probe["lines"]), default=0)
        detail = (f"{len(probe['lines'])} probe line(s), max in-window draws="
                  f"{max_inside}" + (", matrix sampled" if probe["matrix"] else ""))
        checks.append({"group": "viewmodel", "check": "viewmodel_pass_identified",
                       "ok": bool(opened), "detail": detail})
    else:
        checks.append({"group": "viewmodel", "check": "viewmodel_pass_identified",
                       "ok": True, "skip": True,
                       "detail": "no ViewmodelProbe lines (ViewmodelObserve=0 or pre-#67 host)"})
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
    fresh = read_fresh_hook_log(log_path)
    if not fresh:
        print(f"[xrsim] evaluate-only: no hook log at {log_path}; marker checks will be empty.")
    # GH #69: if a previous live run persisted its host stdout, reuse it so the
    # frame-budget group still has evidence during offline re-analysis.
    hlp = host_log_path(artifacts)
    host_text = hlp.read_text(errors="ignore") if hlp.is_file() else ""
    if not host_text:
        print(f"[xrsim] evaluate-only: no persisted host log at {hlp}; "
              "frame-budget check is skipped.")
    passed, checks = evaluate(args, artifacts, host_text, fresh)
    labels = read_display_branch_labels(artifacts)
    write_report(report, passed, checks,
                 extra={"mode": "evaluate-only", "artifacts": str(artifacts),
                        "hook_log": str(log_path),
                        "hook_log_scope": "current-run section" if RUN_HEADER in fresh
                                          else "whole file",
                        "markers": marker_summary(checks),
                        "live_gameplay": gameplay_capture_stats(artifacts, labels),
                        "content_gate": content_gate_stats(host_text),
                        "viewmodel_probe": viewmodel_probe_stats(fresh),
                        "host_log": str(hlp) if hlp.is_file() else None,
                        "display_branch": {str(k): v for k, v in sorted(labels.items())}})
    print_summary(checks, report, passed)
    return 0 if passed else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--scenario")
    ap.add_argument("--frames", type=int, default=180)
    ap.add_argument("--interval", type=int, default=None,
                    help="dump every N frames (default 15; 300 with --reach-gameplay)")
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--max-frame-ms", type=float, default=1000.0 / 90.0)
    ap.add_argument("--no-require-disparity", dest="require_disparity",
                    action="store_false")
    ap.add_argument("--downscale", type=int, default=1,
                    help="block-subsample eye images by this factor (speed)")
    ap.add_argument("--dump-branch", choices=("gameplay", "any"), default="gameplay",
                    help="which display branch to capture: live gameplay only "
                         "(default) or any frame (GH #69)")
    ap.add_argument("--reach-gameplay", action="store_true",
                    help="drive the run to live gameplay: unlimited frames bounded by "
                         "--duration (the mock follows the game's uncapped swap rate, so "
                         "a frame budget cannot express the wall-clock startup time) and "
                         "assert a full-bleed 3D scene was reached (GH #71)")
    ap.add_argument("--duration", type=float, default=None,
                    help=f"wall-clock run cap in seconds (default {GAMEPLAY_DEFAULT_DURATION:.0f} "
                         "with --reach-gameplay)")
    ap.add_argument("--artifacts", help="capture directory (default tests/xrsim/artifacts)")
    ap.add_argument("--report-file", help="report path (default tests/xrsim/report.json)")
    ap.add_argument("--hook-log", help="hook log to scan for markers (default: game log)")
    ap.add_argument("--evaluate-only", action="store_true",
                    help="re-analyse existing captures; do not launch host/game")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)

    if args.interval is None:
        args.interval = (GAMEPLAY_DEFAULT_INTERVAL if args.reach_gameplay
                         else DEFAULT_DUMP_INTERVAL)
    if args.reach_gameplay:
        if args.duration is None:
            args.duration = GAMEPLAY_DEFAULT_DURATION
        args.timeout = max(args.timeout, int(args.duration) + 20)

    if args.selftest:
        return cmd_selftest()
    if args.evaluate_only:
        return cmd_evaluate_only(args)

    artifacts = Path(args.artifacts) if args.artifacts else DEFAULT_ARTIFACTS
    report = Path(args.report_file) if args.report_file else DEFAULT_REPORT
    host_log, log_mark = run_sim(args, artifacts)
    persist_host_log(artifacts, host_log)  # GH #69: survive the run

    # GH #69 requirement 4: the hook truncates its log on launch, so recover the
    # *current run's* section by its header rather than the pre-run byte mark
    # (which would slice away markers logged early in the new run).
    fresh = read_fresh_hook_log(LOG_FILE, log_mark)
    passed, checks = evaluate(args, artifacts, host_log, fresh)
    labels = read_display_branch_labels(artifacts)
    write_report(report, passed, checks,
                 extra={"mode": "run", "artifacts": str(artifacts),
                        "hook_log": str(LOG_FILE),
                        "hook_log_scope": "current-run section" if RUN_HEADER in fresh
                                          else "whole file",
                        "markers": marker_summary(checks),
                        "live_gameplay": gameplay_capture_stats(artifacts, labels),
                        "content_gate": content_gate_stats(host_log),
                        "viewmodel_probe": viewmodel_probe_stats(fresh),
                        "host_log": str(host_log_path(artifacts)),
                        "display_branch": {str(k): v for k, v in sorted(labels.items())}})
    print_summary(checks, report, passed)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
