#!/usr/bin/env python3
"""XRSIM - OpenXR simulator harness for Riddick VR (GH #68).

Runs the mock OpenXR host + game with *scripted* HMD/controller poses (no real
headset), captures BOTH eyes, and asserts:

  * frame-time budget   (host FrameBudget lines),
  * stereo correctness   (left/right eye diff: non-identical, horizontal
                          disparity present, no vertical disparity),
  * stereo structure     (GH #76: is the disparity one global shift of a
                          monoscopic capture, or does it vary with depth? The
                          run is labelled ``stereo_kind`` and
                          ``--require-parallax`` grades it),
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

Real runs can *attempt* to advance the startup splash/menu with
``--advance-menu`` (GH #71 requirement 1): ``RIDDICKVR_SIM=1`` suppresses the
hook's ``SendInput``, so the mock host has no route to drive the game forward and
a run stalls on the splash. The harness knows the game's PID and posts the confirm
keystroke (VK_RETURN) as *targeted* window messages to the game's own HWND -- the
sim-safe route ``src/hook/gesture_tap.cpp`` intends but never completes (it never
learns the HWND). ``PostMessage`` touches only that window's queue, so it is not
desktop input.

**Measured ineffective (2026-10-17):** with ``--advance-menu`` the harness found
the game window (class ``MOS Win32/OpenGL``, visible and *foreground*) and posted
21 confirm taps over 45 s; the game stayed on its ``PRESS ENTER`` splash the whole
time. The engine does not consume posted window messages: ``MSystem.dll`` imports
**DirectInput** and ``RndrGL.dll`` imports **Raw Input**
(``RegisterRawInputDevices``/``GetRawInputData``), both device-sourced and fed
only by OS-level ``SendInput`` -- exactly what the simulator suppresses. So
``--advance-menu`` is **off by default** and is kept only as the sim-safe harness
half of a future hook-side bridge of the engine's input in simulator mode. The
taps auto-stop once the hook's ``DisplayMode`` log shows the full-bleed branch
*after* a cinema/menu branch (the splash is full-bleed-labelled, so the
transition -- not a bare full-bleed -- is the signal). Tune with
``--advance-interval``/``--advance-max-taps``; the report records it as
``menu_advance``.

``--game-arg`` (GH #71 requirement 1, repeatable) passes extra tokens straight to
the game's argv, so a scripted run can try to drive the engine's *own* id-style
console without any input. That is the second input-free route: the engine's
command surface is real (``exec``/``echo``/``history``/``commands`` in
``MSystem.dll``; ``loadgame``/``savegame``/``changemap``/``cg_newgame``/
``cg_loadlastvalidprofile`` in ``Content/GameClasses_Win32_x86.dll`` and
``Content/GameWorld_Win32_x86.dll``), and real savegames exist under
``%APPDATA%/Starbreeze/Riddick/Content/Save/<profile>/`` (``Checkpoint``,
``01.Escape``, ``_profile``).

**Measured ineffective (2026-10-17):** three input-free attempts all left the run
on the ``PRESS ENTER`` splash for its whole wall-clock budget -- ``--game-arg
+loadgame Checkpoint`` (the engine's own console command), ``--game-arg +quit``
(a ``+command`` that would exit immediately *if* the engine executed console
commands from the command line), and ``-env <cfg with COMMANDLINE=+quit>
-nowriteenv`` (MSystem documents a ``COMMANDLINE`` override in the environment
file). The engine does not run ``+``-prefixed console commands from its command
line, and the environment-file ``COMMANDLINE`` override does not either, so
``--game-arg`` alone cannot reach gameplay. It is kept because it is the exact
harness handle for the *hook-side* bridge (route (a) in ADR-0466): the same
tokens are what the bridge would execute in-process. Leading-dash tokens (e.g.
``-env``) must use the ``--game-arg=VALUE`` form so argparse does not read them as
options. The report records the tokens as ``game_args``.

``--sim-confirm [ROUTE]`` (GH #80) is the third and last route to gameplay: the
hook's simulator-only input bridge fabricates the confirm **in-process** on the
engine's own device read path, which is the only surface that is not fed by the
suppressed ``SendInput``. The hook installs the intercept at attach time, before
``CInputContext_DInput``'s constructor resolves ``XInputGetState``/
``DirectInputCreateA`` (ADR-0467), and is inert unless ``RIDDICKVR_SIM`` *and*
``RIDDICKVR_SIM_CONFIRM`` are both set; this flag sets the second token for the
game process. ``ROUTE`` is ``keyboard`` (DirectInput ``DIK_RETURN``, the splash
prompt), ``gamepad`` (XInput ``START|A``) or ``both``; tuning lives in
``--sim-confirm-max-taps``/``-period-ms``/``-hold-ms``. The report records the
env it passed as ``sim_confirm``. The mechanism is in ADR-0468; its live
validation is deploy-gated (the sim install runs a stale hook build).

The gameplay-only dump window is *content-gated* (GH #72 requirement 2): even a
gameplay-labelled frame is kept out while it is all-black (the GL<->D3D11 interop
warm-up) or byte-identical to the last dumped frame (a frozen static page), so the
graded window is live, advancing 3D gameplay. The report carries
``content_gate {blank_skipped, frozen_skipped}`` and, for gameplay-only runs, the
``gameplay_window_has_content`` check.

GH #79 host-half capstone: the host also records, for every capture attempt, the
gate's own per-eye luma sample and the hook's publish counter either side of the
staging copies (``GateSample`` lines). The report carries ``capture_gate`` and the
``capture_gate_matches_saved`` check asserts the saved BMP matches the gate sample
(both are one staged copy, ADR-0465) and that only live frames were written; the
straddle rate quantifies the hook-write race window (reported, not failed).

``--require-fresh-hook`` (GH #81) makes a run say *which hook build it exercised*.
The game loads ``<game-dir>\\opengl32.dll`` -- a deployed copy this loop must not
write -- so a live run normally tests an older hook than the tree it is reported
against, which makes a deploy-gated claim impossible to attribute. Every run
records that binary's path/sha256/size/mtime plus the repo build's (report key
``deployed_hook``) and prints one line at launch; the
``deployed_hook_matches_build`` check is *reported* by default (skipped) and
*graded* only with this flag -- the acceptance instrument for "this live run
tested current source" on the deploy-gated ladder (#71/#72/#76/#79/#80).

Every dumped frame also carries the head/eye transform and projection the host
rendered it with (``eye_frames.log`` in the dump dir, GH #68 requirement 1),
so a capture is self-describing in 3D. The report's ``eye_frames`` key summarises
it and the ``capture_metadata_present`` check confirms it when present (skipped
for legacy captures). GH #77 / ADR-0463: the record's trailing ``eye=`` token
states where the four matrices came from -- ``runtime`` (real ``xrLocateViews``
output), ``sim`` (the mock host reconstructed the per-eye views a runtime would
return for the frame's scripted head pose; scene-faithful in construction, but
not what the hook consumed) or ``synthetic`` (the constant placeholder baseline
the hook *does* consume in mock mode, carrying no head rotation). The key reports
``source``, how far the recorded view rotation is from the head pose, and the
left/right eye separation, so the ``eye_views_scene_faithful`` check grades a
``runtime`` **or** ``sim`` capture and *skips* (never silently passes) a
``synthetic``/legacy one instead of treating the placeholder as a real eye view.

The mock host also summarises the hook-published locomotion comfort level
(``comfortVignette``, Track C #9) together with the move/turn input it published,
so the ``comfort_vignette_applied`` check (GH #68 requirement 5) proves the
vignette actually applies while the scenario moves/turns -- no headset needed. The
report carries the summary as ``comfort_vignette``; it is skipped, never failed,
on a pre-#68 host or an ``--evaluate-only`` run with no persisted host log.

GH #74 reconciliation: this is the *union* of the autoloop harness and the
independently-merged PR #70 harness. The PR #70 conveniences are retained:
``--game-dir/--game-exe/--host-exe`` overrides (env
``RIDDICKVR_GAME_DIR/RIDDICKVR_GAME_EXE/RIDDICKVR_HOST_EXE``),
``--scenario-immediate``, the gameplay-gated ``gameplay_only=1`` FrameBudget
suffix (tolerated by the parser), per-frame ``frame_*.json`` sidecar cleanup, and
a numpy-free selftest (``--require-image-deps`` demands full coverage). The
autoloop capabilities (``--reach-gameplay``/``--duration``, ``--evaluate-only``,
``display_branch.log``/``eye_frames.log``, the content gate, the p50/p95 budget,
the full-bleed BarScan and comfort-vignette checks) live here as the canonical
implementation; its host half (``src/host/openxr_manager.cpp``) is still to be
re-merged.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

# numpy/Pillow are imported lazily so the pure-logic selftest and the
# non-image parsers run on a lean runner without them (the image *analysis*
# still needs them; a real run fails fast with a clear message).
_IMG = None


def image_deps():
    """Return ``(numpy, PIL.Image)``, importing them on first use."""
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


REPO_ROOT = Path(__file__).resolve().parent.parent

# Paths are overridable by flag (--game-dir/--game-exe/--host-exe) or env
# (RIDDICKVR_GAME_DIR/RIDDICKVR_GAME_EXE/RIDDICKVR_HOST_EXE) -- the branch's
# override support, kept in the #74 reconciliation.
GAME_DIR = Path(os.environ.get(
    "RIDDICKVR_GAME_DIR",
    r"F:\The Chronicles of Riddick - Assault on Dark Athena\System\Win32_x86"))
GAME_EXE = Path(os.environ.get("RIDDICKVR_GAME_EXE", str(GAME_DIR / "DarkAthena.exe")))
HOST_EXE = Path(os.environ.get(
    "RIDDICKVR_HOST_EXE", str(REPO_ROOT / "dist" / "riddickvr-host.exe")))
LOG_FILE = GAME_DIR / "riddick_vr.log"
DEFAULT_ARTIFACTS = REPO_ROOT / "tests" / "xrsim" / "artifacts"
DEFAULT_REPORT = REPO_ROOT / "tests" / "xrsim" / "report.json"

# GH #81: the hook the *game* loads is a deployed copy inside the game directory
# (`<game-dir>\opengl32.dll`), not the repo build the mock host is launched from
# (`dist/riddickvr-host.exe`). This loop must never deploy, so a live run
# normally exercises an *older* hook than the tree under test -- its identity is
# recorded (and, opt-in, graded) so no live result is attributed to the wrong
# build.
DEPLOYED_HOOK_NAME = "opengl32.dll"
BUILT_HOOK = Path(os.environ.get(
    "RIDDICKVR_BUILT_HOOK", str(REPO_ROOT / "dist" / DEPLOYED_HOOK_NAME)))

# The line may carry an optional p50/p95 tail (post-#68 pytest host) and an
# optional gameplay_only=1 suffix (the branch's gameplay-gated budget); both are
# tolerated so either host build parses. parse_budget still *requires* p50/p95.
FRAME_BUDGET_RE = re.compile(
    r"FrameBudget samples=(\d+) mean_ms=([\d.]+) max_ms=([\d.]+) "
    r"over_budget=(\d+) budget_ms=([\d.]+)"
    r"(?: p50_ms=([\d.]+) p95_ms=([\d.]+))?"
    r"(?: gameplay_only=1)?")

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

# GH #68 requirement 1: the mock host writes one line per *dumped* frame with the
# head/eye transform and projection used to render it (`src/host/eye_frame.h`),
# so a capture is self-describing in 3D -- the input GH #67 step 3 needs. Parsed
# here for the report; `capture_metadata_present` is *skipped* when the file is
# absent (a legacy capture or a host build predating #68), never failed.
#
# GH #77 / ADR-0463: the trailing `eye=runtime|sim|synthetic` token records the
# *provenance* of the four matrices. `runtime` = a real `xrLocateViews` call;
# `sim` = the mock host reconstructed the per-eye views a runtime would return for
# the frame's scripted head pose (it has no runtime to ask); `synthetic` = the
# constant placeholder baseline (identity rotation, +/- half-IPD, identical
# projections) that the hook actually consumes in mock mode, carrying no head
# rotation. The token is optional so a legacy capture still parses
# (source = "unknown").
EYE_FRAMES_FILE = "eye_frames.log"
EYE_FRAME_RE = re.compile(
    r"frame=(\d+)\s+hmdpos=([-\d.,eE]+)\s+hmdquat=([-\d.,eE]+)\s+"
    r"viewL=\[([-\d.eE ]+)\]\s+viewR=\[([-\d.eE ]+)\]\s+"
    r"projL=\[([-\d.eE ]+)\]\s+projR=\[([-\d.eE ]+)\](?:\s+eye=(\w+))?")
# GH #77: a scene-faithful eye view derives from the head pose (a runtime builds
# the eye view from `views[i].pose`, whose orientation equals the HMD's; the mock
# host reconstructs the same from the scripted pose). 1 degree is a generous
# tolerance for the wire format's 4-decimal rounding.
EYE_VIEW_ROTATION_TOL_DEG = 1.0
EYE_SCENE_FAITHFUL_SOURCES = ("runtime", "sim")
# The left/right eye origins must be separated by a plausible human IPD. Both a
# runtime capture and the mock reconstruction sit near 65 mm; the bound only
# catches a sign/scale/structure bug, not a calibration difference.
EYE_IPD_M_MIN, EYE_IPD_M_MAX = 0.03, 0.10

# GH #72 requirement 2: the mock host's content gate logs every gameplay frame
# it kept out of the graded window (all-black interop warm-up = `blank`; a
# byte-identical repeat of the last dumped frame = `frozen`). Parsed so the
# report can show the gate doing its job.
DUMP_SKIP_RE = re.compile(r"DumpGate skip frame=(\d+) reason=(blank|frozen)")

# GH #79 host-half capstone: the mock host records one `GateSample` line per
# capture attempt -- the content-gate verdict + the per-eye luma sample it took,
# whether a frame was written, and the hook's publish counter either side of the
# staging copies (`frames=before..after`; a move means the hook published while
# the pair was copied -- the race window behind the torn captures). The harness
# asserts the *saved* BMP matches the gate sample (both are two views of one
# staged copy, ADR-0465) and reports the straddle rate; that is GH #79's XRSIM
# acceptance, provable without the deploy-gated hook half.
GATE_SAMPLE_RE = re.compile(
    r"GateSample frame=(\d+) content=(live|blank|frozen) sampled=([01]) "
    r"written=([01]) luma_l=([\d.]+) luma_r=([\d.]+) "
    r"frames=(\d+)\.\.(\d+) straddled=([01])")
# The gate sample and the saved BMP are the same bytes; the only slack is the
# host's 2-decimal luma printout and the harness's independent mean.
CAPTURE_GATE_LUMA_TOL = 1.0

# GH #68 requirement 5 (comfort rules): the mock host prints a running summary of
# the hook-published locomotion comfort level (`comfortVignette`, Track C #9) and
# the locomotion input it published, so a run can assert the vignette is actually
# *applied* while the scripted scenario is moving/turning -- instead of needing a
# headset to notice a config/parse regression that disabled it. Parsed here; a
# line is absent on a pre-#68 host / an --evaluate-only run with no persisted host
# log, so the check is *skipped*, never failed, in that case.
COMFORT_VIGNETTE_RE = re.compile(
    r"ComfortVignette samples=(\d+) max=([\d.]+) mean=([\d.]+) above=(\d+) "
    r"locomotion=(\d+) max_locomotion=([\d.]+) threshold=([\d.]+)")

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
# GH #72 requirement 3: a *vertical* disparity is only meaningful when the
# alignment actually locked on. Two confidence signals select the verdict:
#   * a flat frame (reference-crop std below ``MIN_DY_CONTRAST``) has no vertical
#     texture to align; and
#   * an optimum pinned to the ``DY_SEARCH_BOUND`` edge means the tool cannot
#     quantify the offset -- the true minimum is at or beyond the window and a
#     degenerate landscape looks identical (measured: the dark gameplay frames
#     pin to the edge regardless of the window, and a *bright* frame whose
#     registration failed gave dy=-12 at a widened search with dx also on its
#     bound).
# Either one reports **inconclusive**, never failed: the render path applies only
# a horizontal half-IPD shift, so a real vertical offset has no mechanism and the
# measured figures there are the estimator failing to lock (measured crop-to-crop
# spread of 24 px on the failed bright frame; a steady dy=0 from every crop on a
# good frame). An interior optimum on a textured frame is a real, reportable
# defect.
MIN_DY_CONTRAST = 25.0
# The vertical search window, ``DY_SEARCH_BOUND`` either side. Wide enough that a
# real (if unlikely) offset stays interior and measurable, while a degenerate
# landscape just runs to the edge (and is reported inconclusive).
DY_SEARCH_BOUND = 8

# ------------------------------------------------- shift-only (synthetic) stereo
# GH #76 (epic #57): the mod's stereo is a *monoscopic* capture blitted to both
# eyes with a single half-IPD source-rect shift -- `stereo_renderer.cpp` says so
# verbatim ("monoscopic capture has no IPD parallax"; the only per-eye difference
# is `+/-halfIpdPx` on the blit source rect). One global horizontal shift
# therefore explains *every* region of the pair, so the disparity map is flat:
# there is no depth-dependent parallax to see. The existing stereo gates only
# assert that a horizontal disparity exists and that the eyes are not
# byte-identical, which a pure shift satisfies -- so "stereo correct" was never
# actually tested. The measurement below tests the *structure* of the
# disparity:
#   * tile the interior and re-align each tile locally around the global best dx;
#   * a shift-only pair has ~zero spread across the tiles (measured on the
#     shipped gameplay capture: 12/12 tiles agree exactly, spread 0 px, on every
#     retained frame);
#   * a genuine per-eye view has depth-dependent disparity, so the per-tile
#     shifts differ.
# Verified against the committed `tests/xrsim/artifacts` pair, not inferred.
PARALLAX_TILE_GRID = 4          # 4x4 interior tiles
PARALLAX_TILE_MIN_STD = 3.0     # skip flat tiles (no texture to align on)
PARALLAX_TILE_SEARCH_PX = 8     # local search either side of the global dx (full-res px)
PARALLAX_TILE_MIN = 6           # fewer textured tiles than this => inconclusive
SHIFT_ONLY_SPREAD_PX = 2        # <= this much spread => one global shift (no parallax)
PARALLAX_MIN_SPREAD_PX = 6      # >= this much spread => depth-dependent disparity


def tile_disparities(L, R, best_dx, downscale=1, grid=PARALLAX_TILE_GRID,
                     search_px=PARALLAX_TILE_SEARCH_PX):
    """Per-tile best horizontal shift around the global alignment.

    ``L``/``R`` are the (already downsampled) luminance arrays, ``best_dx`` the
    global best shift in downsampled pixels. Returns ``(tiles, skipped)`` where
    ``tiles`` is the list of per-tile shifts in **full-resolution** pixels and
    ``skipped`` counts the flat tiles excluded for lack of texture. A shift-only
    pair returns near-identical entries; a genuine stereo pair does not.
    """
    np, _ = image_deps()
    ds = max(1, int(downscale))
    h, w = L.shape
    s = max(1, int(round(search_px / ds)))
    # Inset the grid by the local search radius so every candidate shift stays in
    # bounds for every tile (a tile flush against an edge can only search one
    # way, which would fake a disparity spread).
    y_lo, y_hi = s, h - s
    x_lo, x_hi = s, w - s
    th, tw = (y_hi - y_lo) // grid, (x_hi - x_lo) // grid
    if th < 8 or tw < 8:
        return [], 0
    gdx = int(round(best_dx))
    tiles, skipped = [], 0
    for gy in range(grid):
        for gx in range(grid):
            y0 = y_lo + gy * th
            y1 = y0 + th
            x0 = x_lo + gx * tw
            x1 = x0 + tw
            ref = L[y0:y1, x0:x1]
            if float(ref.std()) < PARALLAX_TILE_MIN_STD:
                skipped += 1
                continue
            best, best_mse = None, float("inf")
            for dx in range(gdx - s, gdx + s + 1):
                rx0, rx1 = x0 + dx, x1 + dx
                if rx0 < 0 or rx1 > w:
                    continue
                mse = float(np.mean((ref - R[y0:y1, rx0:rx1]) ** 2))
                if mse < best_mse:
                    best, best_mse = dx, mse
            if best is not None:
                tiles.append(best * ds)
    return tiles, skipped


def classify_stereo_kind(metrics):
    """Classify one eye pair as ``parallax`` / ``shift-only`` / ``mono`` / ``unknown``.

    Pure (reads only the ``stereo_metrics`` dict) so it is testable without
    numpy/Pillow. ``shift-only`` means a single global horizontal shift explains
    the whole pair -- the current monoscopic-blit implementation -- while
    ``parallax`` means the per-tile disparity actually varies with depth.
    """
    if not metrics or metrics.get("read_error"):
        return "unknown"
    if metrics.get("identical"):
        return "mono"
    if metrics.get("best_dx") is None or metrics.get("best_dy") is None:
        return "unknown"
    if metrics.get("dy_at_boundary"):
        return "unknown"
    # GH #72 req 3's confidence rule, reused: a flat frame has no texture to
    # align on, so its per-tile shifts are estimator noise, not a measurement.
    contrast = metrics.get("contrast")
    if contrast is not None and contrast < MIN_DY_CONTRAST:
        return "unknown"
    tiles = metrics.get("tiles_used") or 0
    spread = metrics.get("dx_spread_px")
    if tiles < PARALLAX_TILE_MIN or spread is None:
        return "unknown"
    if spread <= SHIFT_ONLY_SPREAD_PX:
        return "shift-only"
    if spread >= PARALLAX_MIN_SPREAD_PX:
        return "parallax"
    return "unknown"


def stereo_kind_stats(checks):
    """Aggregate the per-frame ``stereo_kind`` labels into a run-level verdict.

    Reads the ``stereo_kind`` the stereo checks carry (attached in
    :func:`evaluate`). Gameplay frames are authoritative: a cinema frame is
    *deliberately* a flat, shift-free blit (GH #69), so it is counted but not
    used to call the run. Unmeasurable frames (``unknown``: too dark/flat,
    unaligned) are recorded but excluded from the verdict, so a dark stretch
    cannot turn a shift-only run into a false "mixed". The result is reported but
    never changes the verdict by itself -- `--require-parallax` turns it into a
    graded check.
    """
    frames = []
    seen = set()
    for c in checks:
        k = c.get("stereo_kind")
        if k is None:
            continue
        frame = c.get("frame")
        if frame in seen:
            continue
        seen.add(frame)
        frames.append({"frame": frame, "branch": c.get("branch"),
                       "kind": k, "dx": c.get("best_dx"),
                       "spread_px": c.get("dx_spread_px")})
    gameplay = [f for f in frames if f["branch"] == "gameplay"]
    used = gameplay or frames
    usable = [f for f in used if f["kind"] != "unknown"]
    kinds = [f["kind"] for f in usable]
    if not kinds:
        run_kind = "unknown"
    elif all(k == "parallax" for k in kinds):
        run_kind = "parallax"
    elif all(k in ("shift-only", "mono") for k in kinds):
        run_kind = "shift-only"
    else:
        run_kind = "mixed"
    spreads = [f["spread_px"] for f in usable if f["spread_px"] is not None]
    return {"kind": run_kind, "frames": len(frames), "gameplay_frames": len(gameplay),
            "measurable_frames": len(kinds), "tiles_grid": PARALLAX_TILE_GRID,
            "spread_px_max": (max(spreads) if spreads else None),
            "per_frame": frames}


def _best_align(L, R, y0, y1, x0, x1, search):
    """Best ``(dx, dy, mse)`` aligning the ``L`` crop ``[y0:y1, x0:x1]`` into ``R``.

    ``dy`` is searched over ``DY_SEARCH_BOUND`` either side (a correct frame has
    no vertical disparity, so the range only needs to catch a gross error).
    Windows that would leave the image are skipped, so the crop must be big
    enough for the search.
    """
    np, _ = image_deps()
    h, w = L.shape
    ref = L[y0:y1, x0:x1]
    best = (None, None, float("inf"))
    for dy in range(-DY_SEARCH_BOUND, DY_SEARCH_BOUND + 1):
        for dx in range(-search, search + 1):
            ry0, ry1 = y0 + dy, y1 + dy
            rx0, rx1 = x0 + dx, x1 + dx
            if ry0 < 0 or rx0 < 0 or ry1 > h or rx1 > w:
                continue
            mse = float(np.mean((ref - R[ry0:ry1, rx0:rx1]) ** 2))
            if mse < best[2]:
                best = (dx, dy, mse)
    return best


def stereo_metrics(left_path: Path, right_path: Path, search=48, downscale=1):
    """Best horizontal/vertical alignment of the two eye images.

    Returns dict with: non_black_l/r, best_dx, best_dy, residual (MSE at best),
    identical (bool), plus -- GH #72 requirement 3 -- ``contrast`` (reference-crop
    stddev) and ``dy_at_boundary`` (whether the vertical optimum sits on the
    search edge, i.e. cannot be quantified). The mod is a monoscopic blit with an
    IPD shift, so a correct frame has a small horizontal dx and ~zero dy.

    GH #76: also measures the *structure* of the disparity -- ``local_dx``
    (per-tile best shifts, full-res px), ``tiles_used``/``tiles_skipped`` and
    ``dx_spread_px`` (max-min of ``local_dx``). A single global shift (the
    current monoscopic blit) gives a flat map; a per-eye render does not. See
    :func:`classify_stereo_kind`.

    ``downscale`` (integer >= 1) block-subsamples both images before the search
    so multi-megapixel eye buffers can be analysed quickly; dx/dy are then in
    downscaled pixels (multiply by ``downscale`` for the full-res shift).

    A truncated/unreadable capture (the run is ended by ``terminate``, so the
    last dump can be partial) returns ``{"read_error": ...}`` instead of raising,
    so one bad frame cannot abort the whole harness report.
    """
    np, Image = image_deps()
    try:
        L = np.array(Image.open(left_path).convert("L"), dtype=np.float32)
        R = np.array(Image.open(right_path).convert("L"), dtype=np.float32)
    except (OSError, ValueError, SyntaxError) as exc:
        return {"read_error": str(exc)}
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
    # GH #72 requirement 3: contrast (texture) is the first confidence signal --
    # a flat frame has no vertical structure to align.
    out["contrast"] = float(L[y0:y1, x0:x1].std())
    best = _best_align(L, R, y0, y1, x0, x1, search)
    if best[0] is None:
        out.update(best_dx=None, best_dy=None, residual=None, identical=False,
                   note="no valid alignment window", dy_at_boundary=False)
        return out
    out["best_dx"], out["best_dy"], out["residual"] = best
    out["dy_at_boundary"] = abs(best[1]) == DY_SEARCH_BOUND
    # "Identical" = zero shift with a tiny residual (mono duplication).
    out["identical"] = (best[0] == 0 and best[1] == 0 and best[2] < 0.5)
    # GH #76: is the disparity a single global shift, or does it vary with depth?
    tiles, skipped = tile_disparities(L, R, best[0], downscale=ds)
    out["local_dx"] = tiles
    out["tiles_used"] = len(tiles)
    out["tiles_skipped"] = skipped
    out["dx_spread_px"] = (max(tiles) - min(tiles)) if tiles else None
    out["stereo_kind"] = classify_stereo_kind(out)
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
    contrast = metrics.get("contrast")
    tag = " [search boundary]" if metrics.get("dy_at_boundary") else ""
    if dy is None:
        chk("no_vertical_disparity", False, "dy not computed")
    elif abs(dy) <= 2:
        chk("no_vertical_disparity", True, f"dy={dy}{tag}")
    else:
        # GH #72 requirement 3: only *trust* a non-zero dy when the frame has
        # enough texture to align and the optimum is interior (quantifiable).
        # Otherwise the vertical figure is the estimator failing to lock, which
        # is reported inconclusive (a skip) -- never a defect.
        reasons = []
        if contrast is not None and contrast < MIN_DY_CONTRAST:
            reasons.append(f"low-contrast frame (std={contrast:.1f} < "
                           f"{MIN_DY_CONTRAST:.0f})")
        if metrics.get("dy_at_boundary"):
            reasons.append(f"optimum pinned to the +/-{DY_SEARCH_BOUND} search "
                           "edge (offset not quantifiable)")
        if reasons:
            checks.append({
                "check": "no_vertical_disparity", "ok": True, "skip": True,
                "detail": (f"inconclusive: dy={dy}{tag} -> "
                           f"{'; '.join(reasons)}; the vertical optimum is an "
                           "estimator artefact (GH #72 req 3)")})
        else:
            chk("no_vertical_disparity", False,
                f"dy={dy}{tag} (contrast std={contrast:.1f})" if contrast is not None
                else f"dy={dy}{tag}")
    if require_disparity:
        chk("horizontal_disparity_present", dx is not None and abs(dx) >= 1,
            f"dx={dx}")
    chk("not_mono_duplicate", not metrics.get("identical"),
        f"identical={metrics.get('identical')}")
    return ok, checks


def vertical_disparity_stats(checks):
    """GH #72 requirement 3: the verdict on the `dy` (vertical disparity) checks.

    ``ok``          -- frames where a vertical disparity was *measured* and not
                       found (dy within tolerance);
    ``inconclusive``-- low-contrast frames where the vertical optimum is an
                       estimator artefact, reported as a skip;
    ``failed``      -- high-contrast frames that measured a real vertical offset.
    A run that only ever reports ``inconclusive``/``ok`` has no evidence of a
    vertical stereo defect; a non-zero ``failed`` is a concrete defect on frames
    that *can* measure it.
    """
    ok = inconclusive = failed = 0
    for c in checks:
        if c.get("check") != "no_vertical_disparity":
            continue
        if c.get("skip"):
            inconclusive += 1
        elif c.get("ok"):
            ok += 1
        else:
            failed += 1
    return {"ok": ok, "inconclusive": inconclusive, "failed": failed}


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


def _parse_csv_floats(text):
    return [float(v) for v in text.split(",") if v != ""]


def read_eye_frames(artifacts):
    """GH #68 requirement 1: map frame number -> camera transform.

    Read from the mock host's ``eye_frames.log`` (``src/host/eye_frame.h``). Each
    record is the HMD pose, the per-eye view and projection matrices, and (GH
    #77) the provenance of those matrices. Returns ``{}`` when the file is absent
    (a legacy capture or a host build predating #68).
    """
    path = Path(artifacts) / EYE_FRAMES_FILE
    frames = {}
    if not path.is_file():
        return frames
    for line in path.read_text(errors="ignore").splitlines():
        m = EYE_FRAME_RE.search(line)
        if not m:
            continue
        try:
            frames[int(m.group(1))] = {
                "hmd_pos": _parse_csv_floats(m.group(2)),
                "hmd_quat": _parse_csv_floats(m.group(3)),
                "view_l": [float(v) for v in m.group(4).split()],
                "view_r": [float(v) for v in m.group(5).split()],
                "proj_l": [float(v) for v in m.group(6).split()],
                "proj_r": [float(v) for v in m.group(7).split()],
                # GH #77: provenance token; absent on a pre-#77 capture.
                "source": m.group(8) if m.group(8) else "unknown",
            }
        except ValueError:
            continue
    return frames


def _quat_yaw_deg(q):
    x, y, z, w = q
    return math.degrees(math.atan2(2.0 * (w * y + x * z),
                                   1.0 - 2.0 * (y * y + z * z)))


def _rot3_from_quat(q):
    """Column-major storage, returned row-major: matches the host's
    ``CreateViewFromPose`` rotation block (both eyes share the head orientation)."""
    x, y, z, w = q
    return [
        [1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w), 2.0 * (x * z + y * w)],
        [2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - x * w)],
        [2.0 * (x * z - y * w), 2.0 * (y * z + x * w), 1.0 - 2.0 * (x * x + y * y)],
    ]


def _rot3_from_view(m):
    """Row-major 3x3 rotation of a column-major 4x4 view matrix."""
    return [[m[0], m[4], m[8]],
            [m[1], m[5], m[9]],
            [m[2], m[6], m[10]]]


def _mat3_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)]
            for i in range(3)]


def _rot3_angle_deg(r):
    """Rotation angle (deg) of a row-major 3x3, computed stably.

    ``acos((trace-1)/2)`` loses most of its precision near identity (its
    derivative is infinite at 1), so a faithful-view comparison whose 4-decimal
    wire rounding leaves ~1e-5 matrix error would read as ~0.4 deg. The
    ``atan2`` form below is well conditioned across the whole range."""
    sin_part = 0.5 * math.sqrt(max(0.0,
        (r[2][1] - r[1][2]) ** 2 + (r[0][2] - r[2][0]) ** 2 +
        (r[1][0] - r[0][1]) ** 2))
    cos_part = max(-1.0, min(1.0, (r[0][0] + r[1][1] + r[2][2] - 1.0) * 0.5))
    return math.degrees(math.atan2(sin_part, cos_part))


def _view_head_rotation_deviation_deg(view, hmd_quat):
    """Angular distance (deg) between the recorded view's rotation and the head
    pose's rotation. A scene-faithful view (built from the eye pose, whose
    orientation equals the head's) has a world->eye rotation of ``R_head^T``, so
    ``R_view * R_head == I``; the host's identity placeholder deviates by the
    head's own rotation (GH #77)."""
    if len(view) != 16 or len(hmd_quat) != 4:
        return None
    r_view = _rot3_from_view(view)      # world -> eye rotation (R_head^T if faithful)
    r_head = _rot3_from_quat(hmd_quat)  # camera -> world rotation
    # rel = R_view * R_head; identity iff R_view == R_head^T (a true eye view).
    return round(_rot3_angle_deg(_mat3_mul(r_view, r_head)), 3)


def _view_translation_separation_m(view_l, view_r):
    """Distance (m) between the two view translations.

    A rigid per-eye pair has ``translation = -R^T * eye_origin`` with a shared
    rotation, so this equals the eye separation (the IPD) and is rotation
    invariant. Used to catch a sign/scale/structure bug in the recorded views."""
    if len(view_l) != 16 or len(view_r) != 16:
        return None
    dx = view_l[12] - view_r[12]
    dy = view_l[13] - view_r[13]
    dz = view_l[14] - view_r[14]
    return math.sqrt(dx * dx + dy * dy + dz * dz)


def eye_frame_stats(artifacts):
    """Summarise the per-dump camera transforms (GH #68 requirement 1).

    ``count`` is how many dumped frames carry a recorded transform, ``distinct``
    how many distinct HMD poses there were, and ``yaw_span_deg`` the peak-to-peak
    HMD yaw. A mock run oscillates the head, so a non-zero yaw span is a concrete
    "the capture window was not a frozen head" signal.

    GH #77 / ADR-0463 adds the *provenance* of the four matrices: ``source``
    (``runtime`` / ``sim`` / ``synthetic`` / ``unknown``, or ``mixed``),
    ``view_head_rotation_deviation_deg`` (max over frames of the view rotation's
    distance from the head pose -- ~0 for a faithful view, ~the head's own
    rotation for the placeholder baseline), ``view_lr_ipd_m`` (mean left/right
    view-translation separation, ~0.065 m for a plausible eye pair) and
    ``proj_lr_identical`` (the placeholder's two projections are equal).
    """
    frames = read_eye_frames(artifacts)
    if not frames:
        return {"count": 0, "distinct": 0, "yaw_span_deg": None,
                "source": None, "view_head_rotation_deviation_deg": None,
                "view_lr_ipd_m": None, "proj_lr_identical": None}
    yaws = []
    poses = set()
    sources = set()
    deviations = []
    ipds = []
    proj_identical = []
    for rec in frames.values():
        q = rec["hmd_quat"]
        if len(q) != 4:
            continue
        yaws.append(_quat_yaw_deg(q))
        poses.add(tuple(round(v, 3) for v in q) +
                  tuple(round(v, 3) for v in rec["hmd_pos"]))
        sources.add(rec.get("source", "unknown"))
        dev = _view_head_rotation_deviation_deg(rec["view_l"], q)
        if dev is not None:
            deviations.append(dev)
        ipd = _view_translation_separation_m(rec["view_l"], rec["view_r"])
        if ipd is not None:
            ipds.append(ipd)
        if len(rec["proj_l"]) == 16 and len(rec["proj_r"]) == 16:
            proj_identical.append(rec["proj_l"] == rec["proj_r"])
    span = (max(yaws) - min(yaws)) if yaws else None
    source = (sources.pop() if len(sources) == 1
              else ("mixed" if sources else "unknown"))
    return {"count": len(frames), "distinct": len(poses),
            "yaw_span_deg": round(span, 3) if span is not None else None,
            "source": source,
            "view_head_rotation_deviation_deg": (round(max(deviations), 3)
                                                 if deviations else None),
            "view_lr_ipd_m": (round(sum(ipds) / len(ipds), 4) if ipds else None),
            "proj_lr_identical": (all(proj_identical) if proj_identical else None)}


def frame_signature(path: Path):
    """Cheap 16x16 luma signature used to tell live frames from a static scene.

    Returns ``None`` for a truncated/unreadable capture so one partial frame
    cannot abort the harness (the run is ended by ``terminate``).
    """
    _, Image = image_deps()
    try:
        return Image.open(path).convert("L").resize((16, 16)).tobytes()
    except (OSError, ValueError, SyntaxError):
        return None


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
    sigs = [s for s in (frame_signature(p) for p in lefts) if s]
    if len(sigs) < 2:
        checks.append({"check": "scene_advanced", "ok": True, "skip": True,
                       "detail": f"only {len(sigs)} readable frame(s); cannot judge"})
        return checks
    distinct = len(set(sigs))
    detail = f"{distinct}/{len(sigs)} distinct frames"
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
    np, Image = image_deps()
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


def parse_gate_samples(host_log):
    """GH #79 host-half capstone: the mock host's per-attempt capture records.

    Returns a dict keyed by frame number (``content``, ``sampled``, ``written``,
    ``luma_l``, ``luma_r``, ``frames`` (before, after), ``straddled``). Empty for
    a pre-#79 host or an ``--evaluate-only`` run with no persisted host log.
    """
    out = {}
    for line in (host_log or "").splitlines():
        m = GATE_SAMPLE_RE.search(line)
        if not m:
            continue
        out[int(m.group(1))] = {
            "content": m.group(2),
            "sampled": m.group(3) == "1",
            "written": m.group(4) == "1",
            "luma_l": float(m.group(5)),
            "luma_r": float(m.group(6)),
            "frames": (int(m.group(7)), int(m.group(8))),
            "straddled": m.group(9) == "1",
        }
    return out


def saved_left_lumas(artifacts, frames=None):
    """Mean luma (0..255) of each saved LEFT-eye BMP, keyed by frame number.

    Recomputed from the file on disk, so it is independent of the gate's own
    sample -- the two agreeing proves the saved pixels are the copy the gate
    authorised (GH #79 host-half capstone). The mean is plain ``(R+G+B)/3`` over
    every pixel, matching the host's ``SampleEyeImage`` exactly (a Rec.601
    ``convert("L")`` would weight the channels differently and read a different
    number on a tinted frame). ``frames`` optionally restricts the decode.
    """
    np, Image = image_deps()
    out = {}
    for lp in sorted(Path(artifacts).glob("test_frame_*.bmp")):
        n = int(lp.stem.split("_")[2])
        if frames is not None and n not in frames:
            continue
        try:
            arr = np.asarray(Image.open(lp).convert("RGB"), dtype=np.float32)
        except Exception:
            continue
        out[n] = float(arr.mean())  # equal-channel weights == (R+G+B)/3
    return out


def capture_gate_check(gate_samples, saved_lumas, tol=CAPTURE_GATE_LUMA_TOL):
    """GH #79 host-half capstone: the saved BMP must match the gate's sample.

    Both come from one private staged copy (ADR-0465), so a frame whose recomputed
    luma disagrees with the gate sample means the gate and the write saw different
    bytes (the two-read regression #79 fixed). Only a *Live* verdict may be
    written. The hook-publish straddle rate is reported (the race window), not
    failed -- a straddle does not prove the pair tore.

    Returns ``(ok, record)``; skipped (never failed) when the host emitted no
    ``GateSample`` line (pre-#79 host) or wrote no frame.
    """
    if not gate_samples:
        return True, {"check": "capture_gate_matches_saved", "ok": True, "skip": True,
                      "detail": "no GateSample lines (pre-#79 host or evaluate-only)"}
    written = {n: g for n, g in gate_samples.items() if g["written"]}
    if not written:
        return True, {"check": "capture_gate_matches_saved", "ok": True, "skip": True,
                      "detail": f"no frame written of {len(gate_samples)} capture attempt(s)"}
    mismatches, non_live, checked = [], [], 0
    for n, g in sorted(written.items()):
        if g["content"] != "live":
            non_live.append(n)
        saved = saved_lumas.get(n)
        if not g["sampled"] or saved is None:
            continue
        checked += 1
        if abs(g["luma_l"] - saved) > tol:
            mismatches.append((n, g["luma_l"], saved))
    straddled = sorted(n for n, g in gate_samples.items() if g["straddled"])
    ok = not mismatches and not non_live
    detail = (f"{checked}/{len(written)} written frame(s) checked (tol {tol}); "
              f"{len(mismatches)} mismatch(es), {len(non_live)} non-live write(s); "
              f"{len(straddled)}/{len(gate_samples)} attempt(s) straddled a hook frame "
              "(GH #79 race)")
    if mismatches:
        n, gl, sv = mismatches[0]
        detail += f"; e.g. frame {n}: gate {gl:.2f} vs saved {sv:.2f}"
    if non_live:
        detail += f"; non-live written: {non_live[:3]}"
    return ok, {"check": "capture_gate_matches_saved", "ok": ok, "detail": detail,
                "checked": checked, "mismatches": len(mismatches),
                "non_live": len(non_live), "straddled": len(straddled),
                "attempts": len(gate_samples)}


def capture_gate_stats(gate_samples):
    """Report summary for the GH #79 capture-gate records (counts only)."""
    written = [g for g in gate_samples.values() if g["written"]]
    return {
        "attempts": len(gate_samples),
        "written": len(written),
        "sampled": sum(1 for g in gate_samples.values() if g["sampled"]),
        "straddled": sum(1 for g in gate_samples.values() if g["straddled"]),
        "blank": sum(1 for g in gate_samples.values() if g["content"] == "blank"),
        "frozen": sum(1 for g in gate_samples.values() if g["content"] == "frozen"),
    }


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
    np, Image = image_deps()
    try:
        arr = np.array(Image.open(left_path).convert("RGB"))
    except (OSError, ValueError, SyntaxError) as exc:
        return False, [{"check": "frame_readable", "ok": False,
                        "detail": f"unreadable capture: {exc}"}]
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


def parse_comfort(host_log):
    """GH #68 requirement 5: the run's locomotion comfort-vignette summary.

    Returns ``None`` when the host log carries no ``ComfortVignette`` line (a
    pre-#68 host, or an --evaluate-only re-analysis with no persisted host log),
    so callers *skip* -- never fail -- in that case. Otherwise the last line's
    ``{samples, max, mean, above, locomotion, max_locomotion, threshold}``.
    """
    rows = COMFORT_VIGNETTE_RE.findall(host_log or "")
    if not rows:
        return None
    samples, mx, mean, above, locomotion, max_loco, threshold = rows[-1]
    return {"samples": int(samples), "max": float(mx), "mean": float(mean),
            "above": int(above), "locomotion": int(locomotion),
            "max_locomotion": float(max_loco), "threshold": float(threshold)}


def comfort_vignette_check(comfort):
    """GH #68 requirement 5: did the locomotion comfort vignette actually apply?

    ``comfort`` is ``parse_comfort``'s dict (or ``None``). The vignette is only
    *expected* on frames where the run published move/turn input, so a run with no
    locomotion frame is reported as a skip (not a failure). With locomotion
    present, the published level must rise above the summary's own threshold,
    which proves the hook computed and published it (and the renderer's `>= 0.01`
    gate therefore opened) rather than the feature being silently disabled.
    """
    if comfort is None:
        return True, {"check": "comfort_vignette_applied", "ok": True, "skip": True,
                      "detail": "no ComfortVignette line (pre-#68 host, or "
                                "evaluate-only without a persisted host log)"}
    if comfort["locomotion"] == 0:
        return True, {"check": "comfort_vignette_applied", "ok": True, "skip": True,
                      "detail": "no locomotion input published; the vignette is "
                                "not expected"}
    ok = comfort["max_locomotion"] >= comfort["threshold"]
    return ok, {"check": "comfort_vignette_applied", "ok": ok,
                "detail": (f"max={comfort['max_locomotion']:.3f} while moving "
                           f"(threshold {comfort['threshold']:.3f}); "
                           f"{comfort['above']}/{comfort['samples']} frame(s) "
                           f"above, {comfort['locomotion']} locomotion frame(s)")}


# ------------------------------------------------------------------ frame budget
def parse_budget(host_log: str, budget_ms: float):
    """GH #68 requirement 2: assert the *frame-pacing* budget (ADR-0455).

    The mock host follows the game's uncapped swap rate, so a one-off level-load
    hitch shows up in ``mean_ms``/``max_ms`` without being a sustained 90 Hz
    regression. The budget is therefore asserted on **p50 and p95** -- the
    typical frame and the frame the user actually feels -- while ``max_ms`` and
    the drop count (``over_budget``) are still *reported* so a real stall stays
    visible. Requires a post-#68 host build (the line carries p50_ms/p95_ms).
    """
    rows = FRAME_BUDGET_RE.findall(host_log)
    if not rows:
        return False, {"check": "frame_budget_reported", "ok": False,
                       "detail": "no FrameBudget line from host"}
    n, mean, mx, over, budget, p50, p95 = rows[-1]
    n, over = int(n), int(over)
    mean, mx, budget = float(mean), float(mx), float(budget)
    if not p50 or not p95:
        return False, {"check": "frame_budget_reported", "ok": False,
                       "detail": "FrameBudget line lacks p50_ms/p95_ms "
                                 "(stale host build; rebuild riddickvr-host)"}
    p50, p95 = float(p50), float(p95)
    eff = budget_ms if budget_ms > 0.0 else budget
    rec = {"check": "frame_budget", "ok": (p50 <= eff and p95 <= eff),
           "p50_ms": p50, "p95_ms": p95, "mean_ms": mean, "max_ms": mx,
           "over_budget": over, "budget_ms": eff,
           "detail": (f"n={n} p50={p50:.2f} p95={p95:.2f} mean={mean:.2f} "
                      f"max={mx:.2f} over={over} budget={eff:.2f}")}
    return rec["ok"], rec


# ------------------------------------------------ hook build identity (GH #81)
# A live XRSIM run grades the hook the *game* loads, not the tree the host was
# launched from. The two are only the same after an interactive deploy -- which
# this loop must not perform -- so every result needs the hook's identity next to
# it: a deploy-gated `--sim-confirm` (#80) is inert against an older hook, and a
# stale-hook run of #67/#79's hook half cannot separate "the fix is wrong" from
# "the fix is not installed". `deployed_hook_matches_build` (opt-in, default
# reported-not-graded) is the acceptance instrument for "this run tested current
# source".
_IDENTITY_CACHE: dict = {}


def _mtime_utc(ts: float) -> str:
    return datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def file_identity(path):
    """sha256 + size + mtime of a file, or ``None`` when absent/unreadable.

    Cached on ``(path, size, mtime_ns)`` so re-analysing many frames of one run
    hashes each binary once; any content change moves the stat and misses.
    """
    p = Path(path)
    try:
        st = os.stat(p)
    except OSError:
        return None
    if not os.path.isfile(p):
        return None
    key = (str(p), st.st_size, st.st_mtime_ns)
    cached = _IDENTITY_CACHE.get(key)
    if cached is not None:
        return cached
    try:
        h = hashlib.sha256()
        with open(p, "rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                h.update(chunk)
    except OSError:
        return None
    rec = {"path": str(p), "sha256": h.hexdigest(), "bytes": st.st_size,
           "mtime_utc": _mtime_utc(st.st_mtime)}
    _IDENTITY_CACHE[key] = rec
    return rec


def deployed_hook_identity(game_dir, built_hook=None):
    """Identify the hook the game will load and the repo build to compare it to.

    ``matches_build`` is ``True`` (same sha256), ``False`` (different) or ``None``
    (nothing to compare -- the deployed hook or the build is absent).
    """
    deployed = file_identity(Path(game_dir) / DEPLOYED_HOOK_NAME)
    built = file_identity(Path(built_hook)) if built_hook else None
    if deployed is None or built is None:
        matches = None
    else:
        matches = built["sha256"] == deployed["sha256"]
    return {"deployed": deployed, "built": built,
            "deployed_present": deployed is not None, "matches_build": matches}


def format_hook_identity(ident) -> str:
    """One-line human summary of :func:`deployed_hook_identity`."""
    dep, built = ident.get("deployed"), ident.get("built")
    if dep is None:
        return (f"no {DEPLOYED_HOOK_NAME} in the game dir -- the hook build under "
                "test is unknown")
    verdict = {True: "MATCHES the repo build",
               False: "DIFFERS from the repo build (STALE hook under test)",
               None: "no repo build to compare"}[ident.get("matches_build")]
    line = (f"{dep['path']} sha256={dep['sha256'][:12]} {dep['bytes']} B "
            f"mtime={dep['mtime_utc']} -> {verdict}")
    if built is not None:
        line += (f" [build {built['path']} sha256={built['sha256'][:12]} "
                 f"{built['bytes']} B mtime={built['mtime_utc']}]")
    return line


def deployed_hook_check(ident, require_fresh=False):
    """GH #81: report the hook build under test; grade it only on request.

    Default: the check is *skipped*, because the mismatch is the loop's
    documented deploy gate and must not redden a normal run -- but the identity is
    still recorded in the report. ``--require-fresh-hook`` makes it the acceptance
    instrument for "this live run exercised the current build".
    """
    dep = ident.get("deployed")
    matches = ident.get("matches_build")
    if dep is None:
        detail = (f"no {DEPLOYED_HOOK_NAME} in the game dir; the hook build under "
                  "test is unknown")
    elif matches is True:
        detail = f"run exercised the current build: {format_hook_identity(ident)}"
    elif matches is False:
        detail = (f"run exercised a STALE hook: {format_hook_identity(ident)} -- "
                  "the deploy-gated change under test is not installed (GH #81)")
    else:
        detail = (f"{format_hook_identity(ident)} -- no repo build to compare "
                  "against, so current source cannot be shown to have been tested")
    if not require_fresh:
        detail = "reported only (pass --require-fresh-hook to grade): " + detail
    rec = {"check": "deployed_hook_matches_build", "ok": matches is True,
           "detail": detail, "deployed_hook": ident}
    if not require_fresh:
        rec["skip"] = True
    return rec


# ----------------------------------------------------- menu advance (GH #71)
# The scripted run stalls on the game's startup splash/menu: the mock host
# publishes controller state but *nothing reaches the game*, because (a)
# camera_hook.cpp's SendKey returns at SimModeNoInput() *before* its PostMessage
# and (b) gesture_tap.cpp's posting path needs s_gestureHwnd, which
# SetGestureGameHwnd is never called to set. The only *sim-safe* input route is
# the targeted window message gesture_tap.cpp already intends
# ("STILL post the targeted window message to the game so the harness can
#  advance menus without hijacking the user's desktop"): the hook never learns
# the HWND, but the harness knows the game's PID, so it can post the same
# messages itself. PostMessage only touches that window's queue -- it is NOT
# desktop input injection, so it stays inside the #68 simulator exception.
#
# MEASURED INEFFECTIVE (ADR-0466): this engine reads input via DirectInput
# (MSystem.dll) / Raw Input (RndrGL.dll), so it ignores posted window messages
# (21 confirm taps to the visible, foreground `MOS Win32/OpenGL` game window left
# it on its "PRESS ENTER" splash). The capability is therefore opt-in
# (`--advance-menu`, default off) and kept only as the harness half of a future
# hook-side bridge of the engine's input in simulator mode.
VK_RETURN = 0x0D
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_CHAR = 0x0102


def key_lparam(scan, is_down):
    """Win32 lParam for a key message (scan code in bits 16-23, repeat=1;
    key-up also sets the transition/previous-state bits)."""
    lp = ((scan & 0xFF) << 16) | 1
    if not is_down:
        lp |= (1 << 30) | (1 << 31)
    return lp


def confirm_key_messages(vk=VK_RETURN, scan=0):
    """The exact (message, wParam, lParam) sequence a confirm tap posts, in
    order -- mirroring gesture_tap.cpp's SendKeyTap (keydown, WM_CHAR for the
    text keys the engine routes, keyup)."""
    return [(WM_KEYDOWN, vk, key_lparam(scan, True)),
            (WM_CHAR, vk, key_lparam(scan, True)),
            (WM_KEYUP, vk, key_lparam(scan, False))]


def post_confirm_to_game_windows(pid, vk=VK_RETURN):
    """Post a confirm tap to every visible top-level window owned by ``pid``.

    Returns the number of windows reached (0 when none is found, or off
    Windows). Dependency-free ctypes; guarded so a headless/non-Windows runner
    degrades to a no-op instead of crashing.
    """
    if os.name != "nt" or not pid:
        return 0
    try:
        import ctypes
        from ctypes import wintypes
    except Exception:
        return 0
    try:
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.EnumWindows.argtypes = [ctypes.c_void_p, wintypes.LPARAM]
        user32.EnumWindows.restype = wintypes.BOOL
        user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND,
                                                    ctypes.POINTER(wintypes.DWORD)]
        user32.GetWindowThreadProcessId.restype = wintypes.DWORD
        user32.IsWindowVisible.argtypes = [wintypes.HWND]
        user32.IsWindowVisible.restype = wintypes.BOOL
        user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT,
                                        wintypes.WPARAM, wintypes.LPARAM]
        user32.PostMessageW.restype = wintypes.BOOL
    except Exception:
        return 0

    hwnds = []
    enum_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def _cb(hwnd, _lparam):
        wpid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value == int(pid) and user32.IsWindowVisible(hwnd):
            hwnds.append(hwnd)
        return True

    try:
        user32.EnumWindows(enum_proc(_cb), 0)
    except Exception:
        return 0

    msgs = confirm_key_messages(vk)
    reached = 0
    for hwnd in hwnds:
        ok = True
        for msg, wp, lp in msgs:
            if not user32.PostMessageW(hwnd, msg, wp, lp):
                ok = False
        if ok:
            reached += 1
    return reached


class GameMenuAdvancer:
    """Post a periodic confirm tap to the game window so a scripted run can
    advance the startup splash/menu (GH #71 requirement 1).

    Pure scheduling: it takes an injected ``poster`` callable (returning the
    number of game windows reached) and clock, so cadence / max-taps / stop are
    unit-testable without a Windows handle. There is no thread -- the
    orchestration loop drives ``poll``; at most one tap fires per ``poll`` (no
    catch-up burst after a stall), and ``poll`` reports whether it fired.
    """

    MIN_INTERVAL = 0.25

    def __init__(self, poster, interval_s=2.0, max_taps=0, clock=time.monotonic,
                 start=None):
        self._poster = poster
        self._clock = clock
        self.interval_s = max(self.MIN_INTERVAL, float(interval_s))
        self.max_taps = max(0, int(max_taps))
        self.taps = 0
        self.windows = 0
        self._stopped = False
        now = self._clock() if start is None else start
        self._next = now + self.interval_s

    @property
    def stopped(self):
        return self._stopped or (self.max_taps > 0 and self.taps >= self.max_taps)

    def stop(self):
        self._stopped = True

    def poll(self, now=None):
        """Fire one tap when due. Returns 1 if a tap was posted, else 0."""
        if self.stopped:
            return 0
        now = self._clock() if now is None else now
        if now < self._next:
            return 0
        try:
            reached = int(self._poster() or 0)
        except (TypeError, ValueError):
            reached = 0
        if reached > self.windows:
            self.windows = reached
        self.taps += 1
        self._next = now + self.interval_s
        return 1


# GH #71 requirement 1 stop signal: once the game reaches live gameplay we must
# stop confirming, or the confirm key would open the pause menu / act in-game.
# The startup splash is full-bleed-labelled (GH #72), so a bare full-bleed
# branch is not enough -- gameplay is the full-bleed branch that *follows* a
# cinema (menu) branch.
DISPLAY_MODE_RE = re.compile(r"DisplayMode:\s*(CINEMA|FULL-BLEED)")


def display_branch_sequence(text):
    """Ordered per-transition branch (True = cinema/menu, False = full-bleed)."""
    return [m.group(1) == "CINEMA" for m in DISPLAY_MODE_RE.finditer(text or "")]


def gameplay_reached_from_sequence(branch_seq):
    """True once a full-bleed branch follows a cinema (menu) branch."""
    seen_cinema = False
    for cinema in branch_seq:
        if cinema:
            seen_cinema = True
        elif seen_cinema:
            return True
    return False


# ------------------------------------------------------------------ orchestration
def game_command(game_exe, extra_args=None):
    """The game's argv: the exe plus any harness-supplied arguments.

    GH #71 requirement 1: the engine's console is id-style (``bind``/``exec``/
    ``echo``/``history``/``commands``; ``MSystem.dll``) and the game DLLs register
    ``loadgame``/``savegame``/``checkpoint``/``changemap``/``cg_newgame``/
    ``cg_loadlastvalidprofile`` (``GameClasses_Win32_x86.dll``,
    ``GameWorld_Win32_x86.dll``). Measured (ADR-0466 wake-64 addendum), the engine
    does **not** execute ``+``-prefixed console commands from its command line, so
    this pass-through alone cannot reach gameplay; it is kept as the exact handle
    the hook-side sim input bridge (GH #80) will use. ``--game-arg`` is repeatable
    so each token (``+loadgame``, ``checkpoint``) is passed verbatim, with no shell
    quoting.
    """
    cmd = [str(game_exe)]
    for a in (extra_args or []):
        if a:
            cmd.append(str(a))
    return cmd


def sim_confirm_env(route=None, max_taps=None, period_ms=None, hold_ms=None):
    """The GH #80 sim input bridge's opt-in environment for the *game* process.

    Both input-free routes to a live scene are measured dead (posted window
    messages -- ADR-0466; the engine's command line -- its wake-64 addendum), so
    the confirm has to be fabricated in-process on the engine's own device read
    path. The hook installs that bridge at attach time (ADR-0467/0468) and is
    inert unless ``RIDDICKVR_SIM`` (already set) *and* ``RIDDICKVR_SIM_CONFIRM``
    are present -- this helper is the harness's half of that two-token gate.

    ``route`` is the ``RIDDICKVR_SIM_CONFIRM`` token -- ``keyboard`` (DirectInput
    DIK_RETURN, the splash prompt), ``gamepad`` (XInput START|A) or ``both``.
    The cadence overrides default to the bridge's own 400 ms period / 100 ms hold
    / 300-tap budget; ``max_taps`` is the deterministic bound a scripted A/B
    usually wants. Returns ``{}`` when the bridge is not opted in.
    """
    if not route:
        return {}
    env = {"RIDDICKVR_SIM_CONFIRM": str(route)}
    if max_taps is not None:
        env["RIDDICKVR_SIM_CONFIRM_MAX_TAPS"] = str(int(max_taps))
    if period_ms is not None:
        env["RIDDICKVR_SIM_CONFIRM_PERIOD_MS"] = str(int(period_ms))
    if hold_ms is not None:
        env["RIDDICKVR_SIM_CONFIRM_HOLD_MS"] = str(int(hold_ms))
    return env


def build_host_cmd(args, artifacts: Path):
    """Build the mock-host command line for one scripted run.

    GH #69 requirement 1: scripted runs default to ``--dump-gameplay-only`` so
    the capture window is live 3D gameplay -- the frames that can actually
    validate stereo -- instead of the menu/cinema and all-zero interop warm-up
    frames (where identical eyes are correct). Pass ``--dump-branch any`` to
    keep the legacy capture-everything cadence.
    """
    host_exe = Path(getattr(args, "host_exe", None) or HOST_EXE)
    cmd = [str(host_exe), "--mock", "--auto-exit-with-game",
           "--dump-dir", str(artifacts), "--dump-interval", str(args.interval),
           "--max-frames", str(0 if getattr(args, "reach_gameplay", False) else args.frames)]
    if getattr(args, "dump_branch", "gameplay") == "gameplay":
        cmd.append("--dump-gameplay-only")
    if args.scenario:
        cmd += ["--scenario-file", str(Path(args.scenario).resolve())]
    if getattr(args, "scenario_immediate", False):
        cmd.append("--scenario-immediate")
    return cmd


def run_sim(args, artifacts: Path):
    game_dir = Path(getattr(args, "game_dir", None) or GAME_DIR)
    game_exe = Path(getattr(args, "game_exe", None) or (game_dir / "DarkAthena.exe"))
    log_file = game_dir / "riddick_vr.log"
    artifacts.mkdir(parents=True, exist_ok=True)
    for f in (list(artifacts.glob("*.bmp")) + list(artifacts.glob("*.png")) +
              list(artifacts.glob("frame_*.json")) +
              [artifacts / DISPLAY_LABEL_FILE, artifacts / EYE_FRAMES_FILE,
               artifacts / "host.log"]):
        try:
            f.unlink()
        except OSError:
            pass
    log_mark = log_file.stat().st_size if log_file.exists() else 0

    host_cmd = build_host_cmd(args, artifacts)

    env = dict(os.environ, RIDDICKVR_SIM="1")  # GH #68: no desktop input
    # GH #80: opt the hook's sim input bridge in (a no-op unless --sim-confirm).
    env.update(sim_confirm_env(getattr(args, "sim_confirm", None),
                               getattr(args, "sim_confirm_max_taps", None),
                               getattr(args, "sim_confirm_period_ms", None),
                               getattr(args, "sim_confirm_hold_ms", None)))
    print(f"[xrsim] host: {' '.join(host_cmd)}")
    host = subprocess.Popen(host_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1, env=env)
    time.sleep(1.5)
    game_cmd = game_command(game_exe, getattr(args, "game_arg", None))
    print(f"[xrsim] game: {' '.join(game_cmd)}")
    game = subprocess.Popen(game_cmd, cwd=str(game_dir), env=env)

    # GH #81: say which hook build this run will actually exercise. The game loads
    # the *deployed* `<game-dir>\opengl32.dll`, not this tree's build, so every
    # live result must be read next to this line.
    print("[xrsim] hook under test: " + format_hook_identity(deployed_hook_identity(
        game_dir, getattr(args, "built_hook", None) or BUILT_HOOK)))

    # GH #71 requirement 1: post the confirm keystroke to the game's own window
    # to *try* to advance the startup splash/menu. This is targeted window
    # messaging (never OS-global input) and is the sim-safe completion of
    # gesture_tap.cpp's intended route; it stops as soon as gameplay is reached.
    # Measured ineffective against this engine (ADR-0466), so opt-in only.
    advancer = None
    menu_stats = {"taps": 0, "windows": 0, "reached": False, "enabled": False}
    if getattr(args, "advance_menu", False):
        menu_stats["enabled"] = True
        advancer = GameMenuAdvancer(
            lambda: post_confirm_to_game_windows(game.pid),
            interval_s=float(getattr(args, "advance_interval", 2.0) or 2.0),
            max_taps=int(getattr(args, "advance_max_taps", 0) or 0))
        print(f"[xrsim] menu advance: confirm every {advancer.interval_s:.1f}s to "
              f"pid {game.pid} (GH #71)")

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
        if advancer is not None and advancer.poll() and not advancer.stopped:
            fresh_now = read_fresh_hook_log(log_file, log_mark)
            if gameplay_reached_from_sequence(display_branch_sequence(fresh_now)):
                advancer.stop()
                menu_stats["reached"] = True
                print(f"[xrsim] menu advance: stopped after {advancer.taps} tap(s) "
                      "-- gameplay branch reached")
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

    if advancer is not None:
        menu_stats["taps"] = advancer.taps
        menu_stats["windows"] = advancer.windows
        reach = "reached" if menu_stats["reached"] else "not reached"
        print(f"[xrsim] menu advance summary: taps={advancer.taps} "
              f"windows_reached={advancer.windows} gameplay {reach}")

    return "\n".join(host_log), log_mark, menu_stats


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
            if m.get("read_error"):
                checks.append({"group": "stereo", "check": f"frame_readable_{n}",
                               "ok": False, "frame": n, "branch": branch_name,
                               "detail": f"unreadable eye capture: {m['read_error']}"})
                continue
            _, sub = assert_stereo(m, require_disparity=args.require_disparity)
            for c in sub:
                rec = {**c, "group": "stereo", "frame": n, "branch": branch_name,
                       "stereo_kind": m.get("stereo_kind"),
                       "best_dx": m.get("best_dx"), "best_dy": m.get("best_dy"),
                       "dx_spread_px": m.get("dx_spread_px"),
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
    # GH #79 host-half capstone: the content gate and the saved BMP must agree.
    # Both come from one staged copy (ADR-0465); this is the harness-side proof
    # that the host half landed, provable without the deploy-gated hook half. A
    # pre-#79 host emits no GateSample line -> the check skips.
    gate_samples = parse_gate_samples(host_log)
    if lefts:
        written_frames = {n for n, g in gate_samples.items() if g["written"]}
        saved = saved_left_lumas(artifacts, frames=written_frames) if written_frames else {}
        ok_gate, gate_rec = capture_gate_check(gate_samples, saved)
        checks.append({**gate_rec, "group": "capture"})
    # GH #76 (epic #57): the mod's gameplay stereo is a monoscopic capture
    # blitted with ONE global horizontal shift, so the per-tile disparity spread
    # is zero -- no parallax (measured: 12/12 tiles agree on every retained
    # frame). The default run reports the structure as `stereo_kind` but does not
    # fail on it (the shift *is* the current, documented interim design).
    # `--require-parallax` (or a scenario's `"require_parallax": true`) makes it a
    # graded requirement -- the acceptance instrument for GH #76.
    require_parallax = bool(getattr(args, "require_parallax", False))
    if (not require_parallax) and args.scenario and Path(args.scenario).is_file():
        try:
            require_parallax = bool(json.loads(
                Path(args.scenario).read_text(encoding="utf-8")).get("require_parallax"))
        except (ValueError, OSError):
            require_parallax = False
    if require_parallax:
        kinds = stereo_kind_stats(checks)
        ok_parallax = kinds["kind"] == "parallax"
        checks.append({"group": "stereo", "check": "stereo_parallax_present",
                       "ok": ok_parallax,
                       "detail": (f"stereo_kind={kinds['kind']} "
                                  f"(gameplay frames={kinds['gameplay_frames']}, "
                                  f"max per-tile disparity spread="
                                  f"{kinds['spread_px_max']}); shifted-only eyes "
                                  "have no parallax (GH #76)")
                       if not ok_parallax else
                       (f"stereo_kind=parallax (per-tile disparity spread "
                        f"{kinds['spread_px_max']})")})
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
    # GH #68 requirement 1: a capture is only interpretable in 3D if the camera
    # transform that produced each frame was recorded. Skipped (not failed) when
    # absent, so legacy captures and pre-#68 hosts keep working.
    eye_stats = eye_frame_stats(artifacts)
    if eye_stats["count"]:
        checks.append({"group": "capture", "check": "capture_metadata_present",
                       "ok": True,
                       "detail": (f"{eye_stats['count']} frame(s) with camera "
                                  f"transform, {eye_stats['distinct']} distinct; "
                                  f"HMD yaw span {eye_stats['yaw_span_deg']} deg; "
                                  f"eye views={eye_stats['source']}")})
        # GH #77 / ADR-0463: the capture is only *really* self-describing when
        # the recorded per-eye matrices track the frame's head pose. That is true
        # for a real-headset capture (`eye=runtime`) and for the mock host's
        # reconstruction (`eye=sim`); the placeholder baseline (`eye=synthetic`)
        # carries no head rotation, so a run that records it is *skipped* with
        # the measured deviation as evidence rather than read as a real eye view.
        src = eye_stats["source"]
        dev = eye_stats["view_head_rotation_deviation_deg"]
        ipd = eye_stats["view_lr_ipd_m"]
        if src in EYE_SCENE_FAITHFUL_SOURCES:
            ipd_ok = ipd is not None and EYE_IPD_M_MIN <= ipd <= EYE_IPD_M_MAX
            ok = (dev is not None and dev <= EYE_VIEW_ROTATION_TOL_DEG) and ipd_ok
            checks.append({"group": "capture", "check": "eye_views_scene_faithful",
                           "ok": ok,
                           "detail": (f"{src} eye views track the head pose "
                                      f"(max view/head rotation deviation {dev} deg, "
                                      f"tol {EYE_VIEW_ROTATION_TOL_DEG}; L/R "
                                      f"separation {ipd} m, expected "
                                      f"{EYE_IPD_M_MIN}-{EYE_IPD_M_MAX})")})
        else:
            checks.append({"group": "capture", "check": "eye_views_scene_faithful",
                           "ok": True, "skip": True,
                           "detail": (f"eye views are not scene-faithful "
                                      f"(source={src}); the recorded view rotation "
                                      f"deviates from the head pose by up to {dev} "
                                      f"deg, so these matrices are a placeholder, "
                                      f"not the frame's real per-eye view (GH #77)")})
    else:
        checks.append({"group": "capture", "check": "capture_metadata_present",
                       "ok": True, "skip": True,
                       "detail": f"no {EYE_FRAMES_FILE}: per-frame camera "
                                 "transform unknown (legacy capture / pre-#68 host)"})
        checks.append({"group": "capture", "check": "eye_views_scene_faithful",
                       "ok": True, "skip": True,
                       "detail": f"no {EYE_FRAMES_FILE}: eye-view provenance "
                                 "unknown (legacy capture / pre-#77 host)"})
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
    # GH #68 requirement 5 (comfort rules): the locomotion comfort vignette must
    # actually apply while the scripted run is moving/turning. The mock host
    # summarises the hook-published level + the locomotion input it published; a
    # run with no locomotion frame (or a pre-#68 host) is skipped, not failed.
    _, comfort_rec = comfort_vignette_check(parse_comfort(host_log))
    checks.append({**comfort_rec, "group": "comfort"})
    if lefts:
        _, sub = comfort_checks(lefts[-1])
        for c in sub:
            checks.append({**c, "group": "comfort",
                           "detail": f"{c.get('detail','')} (frame {lefts[-1].name})"})
    # GH #81: identify the hook build this run actually exercised. The game loads
    # `<game-dir>\opengl32.dll` (a deployed copy this loop must not write), not the
    # repo build the mock host was launched from, so a live result is only
    # interpretable next to the hook's identity. Reported always; graded only with
    # `--require-fresh-hook`.
    eval_game_dir = Path(getattr(args, "game_dir", None) or GAME_DIR)
    eval_built_hook = getattr(args, "built_hook", None) or BUILT_HOOK
    checks.append({**deployed_hook_check(
        deployed_hook_identity(eval_game_dir, eval_built_hook),
        bool(getattr(args, "require_fresh_hook", False))), "group": "capture"})
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


def cmd_selftest(require_image_deps=False):
    """Validate the stereo/comfort/budget logic without the game (CI-safe).

    The pure parsers run without numpy/Pillow; the image cases are skipped when
    those are unavailable (``--require-image-deps`` turns a skip into a FAIL, so
    CI can demand full coverage).
    """
    results = {}
    ok_b, _ = parse_budget("FrameBudget samples=60 mean_ms=8.0 max_ms=40.0 over_budget=2 budget_ms=11.11 p50_ms=7.0 p95_ms=9.5", 11.11)
    ok_b2, _ = parse_budget("FrameBudget samples=60 mean_ms=30.0 max_ms=90.0 over_budget=50 budget_ms=11.11 p50_ms=20.0 p95_ms=40.0", 11.11)
    ok_b3, rec3 = parse_budget("FrameBudget samples=60 mean_ms=8.0 max_ms=9.0 over_budget=0 budget_ms=11.11 gameplay_only=1", 11.11)
    ok_b4, rec4 = parse_budget("FrameBudget samples=60 mean_ms=8.0 max_ms=9.0 over_budget=0 budget_ms=11.11 p50_ms=7.0 p95_ms=9.5 gameplay_only=1", 11.11)
    results["budget_ok_detected"] = ok_b
    results["budget_blow_detected"] = (not ok_b2)
    results["budget_requires_percentiles"] = (not ok_b3) and rec3["check"] == "frame_budget_reported"
    results["budget_accepts_gameplay_only_suffix"] = ok_b4 and rec4.get("p50_ms") == 7.0

    skipped = []
    if not have_image_deps():
        skipped = ["shifted_pair_passes", "identical_pair_fails",
                   "shift_only_pair_classified", "parallax_pair_classified"]
    else:
        np, Image = image_deps()
        import tempfile
        tmp = Path(tempfile.mkdtemp(prefix="xrsim_selftest_"))
        # shifted pair -> should pass
        base = (np.random.default_rng(7).random((64, 96, 3)) * 255).astype(np.uint8)
        Image.fromarray(base).save(tmp / "test_frame_0001.bmp")
        Image.fromarray(np.roll(base, 4, axis=1)).save(tmp / "right_0001.bmp")
        m = stereo_metrics(tmp / "test_frame_0001.bmp", tmp / "right_0001.bmp", search=20)
        ok_shift, _ = assert_stereo(m, require_disparity=True)
        # identical pair -> should fail (mono duplicate)
        Image.fromarray(base).save(tmp / "right_0002.bmp")
        Image.fromarray(base).save(tmp / "test_frame_0002.bmp")
        m2 = stereo_metrics(tmp / "test_frame_0002.bmp", tmp / "right_0002.bmp", search=20)
        ok_dup, _ = assert_stereo(m2, require_disparity=True)
        results["shifted_pair_passes"] = ok_shift
        results["identical_pair_fails"] = (not ok_dup)
        # GH #76: a single global shift is `shift-only`; a per-column disparity
        # ramp (depth-varying) is `parallax`.
        results["shift_only_pair_classified"] = (m.get("stereo_kind") == "shift-only")
        h, w = base.shape[:2]
        ramp = np.empty_like(base)
        for x in range(w):
            d = int(round(2 + 16.0 * x / max(1, w - 1)))
            ramp[:, x] = base[:, (x - d) % w]
        Image.fromarray(ramp).save(tmp / "right_0003.bmp")
        Image.fromarray(base).save(tmp / "test_frame_0003.bmp")
        m3 = stereo_metrics(tmp / "test_frame_0003.bmp", tmp / "right_0003.bmp", search=24)
        results["parallax_pair_classified"] = (m3.get("stereo_kind") == "parallax")

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


def cmd_evaluate_only(args):
    artifacts = Path(args.artifacts) if args.artifacts else DEFAULT_ARTIFACTS
    report = Path(args.report_file) if args.report_file else DEFAULT_REPORT
    lefts = sorted(artifacts.glob("test_frame_*.bmp"))
    if not lefts:
        print(f"[xrsim] evaluate-only: no captures in {artifacts}; nothing to do.")
        return 0
    game_dir = Path(getattr(args, "game_dir", None) or GAME_DIR)
    log_path = Path(args.hook_log) if args.hook_log else (game_dir / "riddick_vr.log")
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
                        "capture_gate": capture_gate_stats(parse_gate_samples(host_text)),
                        "viewmodel_probe": viewmodel_probe_stats(fresh),
                        "eye_frames": eye_frame_stats(artifacts),
                        "vertical_disparity": vertical_disparity_stats(checks),
                        "stereo_kind": stereo_kind_stats(checks),
                        "comfort_vignette": parse_comfort(host_text),
                        "deployed_hook": deployed_hook_identity(
                            game_dir, getattr(args, "built_hook", None) or BUILT_HOOK),
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
    ap.add_argument("--max-frame-ms", type=float, default=1000.0 / 90.0,
                    help="90 Hz frame-pacing budget; the check asserts p50 and p95 "
                         "against it (max/drops are still reported; GH #68/ADR-0455)")
    ap.add_argument("--no-require-disparity", dest="require_disparity",
                    action="store_false")
    ap.add_argument("--require-parallax", action="store_true",
                    help="grade the stereo structure: the eyes must be a real "
                         "per-eye render (depth-varying disparity), not one global "
                         "shift of a monoscopic capture (GH #76); a scenario may "
                         "set \"require_parallax\": true instead")
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
    ap.add_argument("--advance-menu", dest="advance_menu", action="store_true",
                    help="post the confirm keystroke to the game's own window to try "
                         "to advance the startup splash/menu (GH #71 req 1): targeted "
                         "window messages, never OS-global input. OFF by default -- "
                         "measured INEFFECTIVE against this engine, which reads input "
                         "via DirectInput (MSystem.dll) / Raw Input (RndrGL.dll), not "
                         "posted window messages (see the module docstring)")
    ap.add_argument("--no-advance-menu", dest="advance_menu", action="store_false",
                    help="never post game-window input (the default; GH #71)")
    ap.set_defaults(advance_menu=False)
    ap.add_argument("--advance-interval", type=float, default=2.0,
                    help="seconds between confirm taps while the splash/menu is up "
                         "(default 2.0; only with --advance-menu)")
    ap.add_argument("--advance-max-taps", type=int, default=0,
                    help="stop confirming after N taps (0 = bounded only by the run)")
    ap.add_argument("--artifacts", help="capture directory (default tests/xrsim/artifacts)")
    ap.add_argument("--report-file", help="report path (default tests/xrsim/report.json)")
    ap.add_argument("--hook-log", help="hook log to scan for markers (default: game log)")
    ap.add_argument("--game-dir", default=None,
                    help="game System\\Win32_x86 dir (env RIDDICKVR_GAME_DIR)")
    ap.add_argument("--game-exe", default=None,
                    help="game exe (env RIDDICKVR_GAME_EXE; default <game-dir>\\DarkAthena.exe)")
    ap.add_argument("--game-arg", action="append", default=None, metavar="TOKEN",
                    help="extra game command-line token, repeatable (GH #71 req 1); "
                         "e.g. --game-arg +loadgame --game-arg checkpoint. Leading-dash "
                         "tokens must use --game-arg=VALUE. Measured ineffective alone "
                         "(the engine ignores +console commands); the handle for the "
                         "hook-side sim bridge")
    ap.add_argument("--host-exe", default=None,
                    help="riddickvr-host.exe (env RIDDICKVR_HOST_EXE)")
    ap.add_argument("--shadow-install", default=None, metavar="SOURCE_ROOT",
                    help="run against a read-only *shadow* of a real install so the "
                         "loader picks the repo-built hook (GH #81): Content*/System "
                         "GL+Sound are directory junctions to SOURCE_ROOT, the program "
                         "dir is real copies with the built hook in the hook slot. "
                         "Nothing is written to the install. Sets --game-dir, so "
                         "--require-fresh-hook can finally grade a live run")
    ap.add_argument("--shadow-dir", default=None,
                    help="scratch directory for --shadow-install "
                         "(default %%TEMP%%\\riddickvr-shadow)")
    ap.add_argument("--built-hook", default=None,
                    help="path to the repo-built hook compared against the "
                         "deployed one (default dist\\opengl32.dll; env "
                         "RIDDICKVR_BUILT_HOOK) -- GH #81")
    ap.add_argument("--require-fresh-hook", dest="require_fresh_hook",
                    action="store_true",
                    help="grade the hook build under test (GH #81): FAIL unless the "
                         "run exercised the repo build, i.e. the game dir's "
                         "opengl32.dll matches the built artifact. The acceptance "
                         "instrument for a deploy-gated fix; OFF by default because "
                         "this loop must not deploy")
    ap.add_argument("--sim-confirm", nargs="?", const="both", default=None,
                    metavar="ROUTE",
                    help="opt the hook's simulator-only input bridge in (GH #80): "
                         "fabricate the confirm in-process on the engine's own "
                         "device path. ROUTE = keyboard (DirectInput DIK_RETURN; "
                         "the default) | gamepad (XInput START|A) | both. Sets "
                         "RIDDICKVR_SIM_CONFIRM for the game process; the bridge "
                         "is otherwise inert (ADR-0467/0468)")
    ap.add_argument("--sim-confirm-max-taps", type=int, default=None,
                    help="bound the fabricated confirm to N taps (GH #80); the "
                         "bridge default is 300")
    ap.add_argument("--sim-confirm-period-ms", type=int, default=None,
                    help="tap-to-tap spacing for the fabricated confirm (GH #80); "
                         "bridge default 400 ms")
    ap.add_argument("--sim-confirm-hold-ms", type=int, default=None,
                    help="how long one fabricated tap reads as down (GH #80); "
                         "bridge default 100 ms")
    ap.add_argument("--scenario-immediate", action="store_true",
                    help="start the scenario at host frame 0 instead of waiting for gameplay")
    ap.add_argument("--evaluate-only", action="store_true",
                    help="re-analyse existing captures; do not launch host/game")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--require-image-deps", action="store_true",
                    help="selftest: fail instead of skipping image cases without numpy/Pillow")
    args = ap.parse_args(argv)

    if args.interval is None:
        args.interval = (GAMEPLAY_DEFAULT_INTERVAL if args.reach_gameplay
                         else DEFAULT_DUMP_INTERVAL)
    if args.reach_gameplay:
        if args.duration is None:
            args.duration = GAMEPLAY_DEFAULT_DURATION
        args.timeout = max(args.timeout, int(args.duration) + 20)

    if args.selftest:
        return cmd_selftest(args.require_image_deps)
    if args.evaluate_only:
        return cmd_evaluate_only(args)
    if not have_image_deps():
        print("xrsim: numpy and Pillow are required for real runs "
              "(pip install numpy pillow)", file=sys.stderr)
        return 2

    # GH #81: an opt-in read-only shadow install. The loader resolves
    # `opengl32.dll` from the executable's directory, so mirroring the install
    # with junctions + a real program dir (and the repo build in the hook slot)
    # makes a live run exercise *current source* without writing to the install.
    if args.shadow_install:
        import shadow_install
        shadow_dest = (Path(args.shadow_dir) if args.shadow_dir
                       else Path(tempfile.gettempdir()) / "riddickvr-shadow")
        built = Path(args.built_hook) if args.built_hook else BUILT_HOOK
        manifest = shadow_install.build_shadow(args.shadow_install, shadow_dest, built)
        args.shadow_manifest = manifest
        args.game_dir = manifest["game_dir"]
        if not args.game_exe:
            args.game_exe = manifest["game_exe"]
        print("[xrsim] " + shadow_install.format_shadow(manifest))
        if not manifest["fresh"]:
            print("[xrsim] WARNING: the shadow's hook does not match the build "
                  "-- --require-fresh-hook will fail", file=sys.stderr)

    artifacts = Path(args.artifacts) if args.artifacts else DEFAULT_ARTIFACTS
    report = Path(args.report_file) if args.report_file else DEFAULT_REPORT
    host_log, log_mark, menu_stats = run_sim(args, artifacts)
    persist_host_log(artifacts, host_log)  # GH #69: survive the run

    # GH #69 requirement 4: the hook truncates its log on launch, so recover the
    # *current run's* section by its header rather than the pre-run byte mark
    # (which would slice away markers logged early in the new run).
    game_dir = Path(getattr(args, "game_dir", None) or GAME_DIR)
    log_file = game_dir / "riddick_vr.log"
    fresh = read_fresh_hook_log(log_file, log_mark)
    passed, checks = evaluate(args, artifacts, host_log, fresh)
    labels = read_display_branch_labels(artifacts)
    write_report(report, passed, checks,
                 extra={"mode": "run", "artifacts": str(artifacts),
                        "hook_log": str(log_file),
                        "hook_log_scope": "current-run section" if RUN_HEADER in fresh
                                          else "whole file",
                        "markers": marker_summary(checks),
                        "live_gameplay": gameplay_capture_stats(artifacts, labels),
                        "content_gate": content_gate_stats(host_log),
                        "capture_gate": capture_gate_stats(parse_gate_samples(host_log)),
                        "viewmodel_probe": viewmodel_probe_stats(fresh),
                        "eye_frames": eye_frame_stats(artifacts),
                        "vertical_disparity": vertical_disparity_stats(checks),
                        "stereo_kind": stereo_kind_stats(checks),
                        "comfort_vignette": parse_comfort(host_log),
                        "menu_advance": menu_stats,
                        "game_args": list(getattr(args, "game_arg", None) or []),
                        "sim_confirm": sim_confirm_env(
                            getattr(args, "sim_confirm", None),
                            getattr(args, "sim_confirm_max_taps", None),
                            getattr(args, "sim_confirm_period_ms", None),
                            getattr(args, "sim_confirm_hold_ms", None)) or None,
                        "deployed_hook": deployed_hook_identity(
                            game_dir, getattr(args, "built_hook", None) or BUILT_HOOK),
                        "shadow": getattr(args, "shadow_manifest", None),
                        "host_log": str(host_log_path(artifacts)),
                        "display_branch": {str(k): v for k, v in sorted(labels.items())}})
    print_summary(checks, report, passed)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
