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

GH #87: a live run can end with the game exiting within ~2 s -- no hook log (or
one that stops during renderer init), the mock host logging ``Game Connected:
WAITING`` for the whole (short) run, and 0 captured frames -- *while the
identical re-run passes*. That is a false red, not a broken fix. So an attempt
whose game process ends before ``--min-live-seconds`` (default 5) with no fresh
hook log and no captured frame is an **early exit**: it is retried once
(``--max-launch-attempts``, default 2) and, if it repeats, reported as one
distinct ``game_started`` failure (report key ``launch``, with every attempt's
lived-seconds / frames / hook-log freshness) instead of grading the capture
checks -- so "the game did not start" can never be read as "the feature is
broken".

**Extension (autoloop wake 97):** the early exit is only **one** no-measurement
signature. A second was measured in wake 96 -- the game stayed **alive the whole
window** (47 s) with **0 frames and no hook log** while the identical re-run
passed; the old rule graded it ``started`` (it merely "stayed live") and its 0
frames produced a false red that was *not* retried. An attempt that stays live for
at least ``--min-live-seconds`` with no fresh hook log and no captured frame is now
its own state, ``no_evidence`` ("the hook never attached"), retried once like an
early exit and reported as one distinct ``game_started`` failure if it repeats. A
run with **any** evidence (a frame or a fresh hook log) is still ``started`` and is
never retried.

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

**CORRECTED (2026-10-18, wake 84).** The earlier reading -- "the engine ignores
posted window messages, because it reads DirectInput/Raw Input" -- is an artefact
of the *message the harness posted*, not of the message path. REA of the shipped
renderer (``docs/rea/reference/RndrGL_Input_Path.txt``) shows that ``RndrGL.dll``
-- the module that registers the game's window class (``MOS Win32/OpenGL``) and
runs its message pump -- **does** handle ``WM_KEYDOWN``/``WM_KEYUP``/``WM_CHAR``
(read: ``MapVirtualKeyA(vk, 0)``; the key code is taken from ``lParam >> 16``,
falling back to ``MapVirtualKeyA`` only when that field is the ``0x100``
sentinel), and its pump is ``GetMessageA(NULL, 0, 0, msg)`` ->
``TranslateMessage`` -> ``DispatchMessageA``, which delivers *posted* messages.
The 21 taps of the earlier attempt were posted with ``lParam = 1`` -- scan code
**0**, a key the engine cannot map. The harness now packs the real scan code
(``vk_to_scancode``), exactly as the hook's own ``gesture_tap.cpp`` does. The
route stays **off by default** (opt-in, GH #71) until a run measures it; the
report records it as ``menu_advance`` including the packed ``scan``. The taps
auto-stop once the hook's ``DisplayMode`` log shows the full-bleed branch *after*
a cinema/menu branch, **and** at least ``--advance-min-taps`` taps have been
delivered. **CORRECTED (wake 85):** the branch signal alone cannot bound the
taps. The engine emits its *own* startup transition as ``CINEMA`` -> ``FULL-BLEED``
at **frame 1** (measured, card-only run), so a bare branch check reads the
startup page as gameplay and stops the advance before it has been attempted --
wake 84 got 6 taps and this run only 3 purely through hook-log write buffering.
Wake 84 measured the card being left at the **5th-6th** tap, so the default
floor is 8. Tune with ``--advance-interval``/``--advance-max-taps``/
``--advance-min-taps`` (0 restores the branch-only stop).

**COMPLETED (wake 96, ADR-0497):** the posted-confirm route was still dead in
every prior wake for one more reason -- the game window was **not active**, and
the engine's pump only carries the key to the game when its own window is
foreground. The harness now activates the game's top-level window
(`SetForegroundWindow` under an ``AttachThreadInput`` handshake; window
management only, *no* synthesized input) before the taps, and records the
observed result as `window_activation` in the report. Measured live A/B on the
current build (``--shadow-install``, ``--require-fresh-hook``,
``--dump-branch any``): activation + ``--advance-menu --advance-min-taps 8``
reached **62 distinct live gameplay frames** (last frame an in-level corridor),
while activation + ``--sim-confirm both`` alone stayed on the ``PRESS ENTER``
splash with the bridge at ``polls=0`` (the engine resolves
``DirectInputCreateA``/``XInputGetState`` through the hook but never calls
either, on the splash *or* in a level). So the reach command is
``--reach-gameplay --duration 75 --advance-menu --advance-min-taps 8`` plus
``--shadow-install``; ``--sim-confirm`` is not the reach mechanism.

**Raw Input is mouse-only here (so it cannot carry a keyboard confirm):**
``RndrGL.dll`` calls ``RegisterRawInputDevices`` once with exactly **one**
``RAWINPUTDEVICE`` -- ``usUsagePage = 1`` / ``usUsage = 2`` (**mouse**),
``dwFlags = 0x100`` (``RIDEV_INPUTSINK``), ``hwndTarget`` = the game window --
and its ``WM_INPUT`` handler forwards only ``dwType == RIM_TYPEMOUSE`` to
``GetRawInputData``. There is no raw-input keyboard device to fabricate a confirm
on (recorded for the GH #80 route).

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
env it passed as ``sim_confirm``. The mechanism is in ADR-0468.

A live run is *isolated per run* (GH #83). The loop can have a second agent in
the same checkout when a runner is killed by a harness restart, and two live runs
then rebuild each other's shadow tree and hold the same ``dist`` binaries. So the
``--shadow-install`` scratch directory defaults to
``%TEMP%\riddickvr-shadow-<pid>`` (never one shared path), and a live-run lock
file (``%TEMP%\riddickvr-live.lock``) makes "at most one live harness" explicit:
a second run refuses (``--ignore-live-lock`` overrides on purpose). A lock whose
pid is gone is *stale* and is taken over, so a killed run never wedges the loop.

GH #71 req 1 / #80 follow-up: the bridge also *reports what it observed*, so a
run is decidable instead of inferred. Its log lines name every input proc the
engine resolved through the swapped ``GetProcAddress`` (``owned`` by the bridge
or merely ``observed``), count how often the engine polled the wrapped entry
point, count the confirms the bridge fabricated, and record the real API's
return code. The report carries the parsed result as ``input_bridge`` (a
periodic ``summary`` timeline survives the harness's ``TerminateProcess`` of the
game, which skips ``DLL_PROCESS_DETACH``); the run prints one summary line. A
real return of ``1167`` (``ERROR_DEVICE_NOT_CONNECTED``) is the measurement that
explains why a synthetic pad's *bytes* alone can be invisible to the engine.

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
``--ini KEY=VALUE`` (repeatable, requires ``--shadow-install``) sets a hook
config key in the **shadow's** program-dir ``riddick_vr.ini`` -- the file the
hook resolves beside its own module (``vr_config.cpp``). A deploy-free live run
can therefore flip an opt-in flag (e.g. ``--ini ViewmodelObserve=1`` for the GH
#67 viewmodel probe, or ``--ini SuppressEngineViewmodel=1`` for GH #66) and
record it in the run's ``shadow``/``ini_overrides``; the game install is never
written.

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
import atexit
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
# GH #67 step 1 (corrected, ADR-0481): the ARB vertex-program path -- where the
# engine's object/view transforms would live if it used one. `strings`/`binds`/
# `env` are interval counts; both env-parameter spellings are intercepted in the
# hook, so a zero env count is a statement about the engine, not about a name.
VIEWMODEL_PROGRAM_RE = re.compile(
    r"ViewmodelProbe program \[#(\d+)\]: strings=(\d+) binds=(\d+) env=(\d+) "
    r"lastProg=(\d+) strLen=(\d+) strHash=([0-9a-fA-F]+) matrix=([01]) base=(\d+)")
VIEWMODEL_PROGRAM_MATRIX_RE = re.compile(r"ViewmodelProbe program matrix:")
# GH #67 step 1 (second correction, ADR-0482): the submitted-vertex sample -- the
# surface the transform is actually applied on. `arrayIn`/`arrayOut` split the
# engine's `glDrawElements` calls by whether they fell inside the first-person
# window, `listIn`/`listOut` do the same for `glCallList` (a display list's
# vertices replay inside the driver and are NOT observable at the draw entry
# point), and `ptr*` is the client `glVertexPointer` state at the sample.
VIEWMODEL_SUBMISSION_RE = re.compile(
    r"ViewmodelProbe submission \[#(\d+)\]: arrayIn=(\d+) arrayOut=(\d+) "
    r"listIn=(\d+) listOut=(\d+) ptrSize=(\d+) ptrType=(\d+) ptrStride=(-?\d+) "
    r"inSample=([01]) outSample=([01])(?: nearSample=([01]))?")
# `fp` = the first-person window's largest decodable array draw (the arms
# candidate, ADR-0483); `world` = the first draw outside it (the control);
# `near` (ADR-0490) = the interval's largest decodable *near-field* array draw,
# window-independent, with a trailing ` inside=0|1` saying whether the window
# happened to be open at that draw. `count` (ADR-0483) is the draw's index count
# (a 4-6 vertex quad vs a real mesh); `n` is how many vertices were sampled.
# `valid=0` means the array was not a decodable float array (the type/stride is
# logged, not guessed). `count` and ` inside=` are optional so a pre-0483 /
# pre-0490 hook log still parses.
VIEWMODEL_SUBMISSION_SAMPLE_RE = re.compile(
    r"ViewmodelProbe submission (fp|world|near|fpimm): first=\(([-\d.]+),([-\d.]+),([-\d.]+)\) "
    r"min=\(([-\d.]+),([-\d.]+),([-\d.]+)\) max=\(([-\d.]+),([-\d.]+),([-\d.]+)\) "
    r"n=(\d+)(?: count=(\d+))? indexType=(\d+) vertexType=(\d+) valid=([01])"
    r"(?: inside=([01]))?")
# GH #67 step 1 (ninth correction, ADR-0491): the immediate-mode census. The
# submitted-vertex observer counts only glDrawElements/glCallList, so the shipped
# module's immediate-mode path (glBegin/glEnd + glVertex3f/glVertex3fv/glVertex2f/
# glVertex2i -- all measured in its static OPENGL32 import table) was never
# counted; a mesh submitted with glBegin..glEnd produced no sample at all. These
# tallies are per interval, split by window side: `v3In`/`v2In` are the 3-D and
# 2-D vertices whose block began inside the first-person window. `openIn` says an
# in-window block was still open at the sample; `sample=1` says the window held at
# least one block (its largest, `n`/bounds, is logged as the `fpimm` sample).
VIEWMODEL_IMMEDIATE_RE = re.compile(
    r"ViewmodelProbe immediate \[#(\d+)\]: beganIn=(\d+) beganOut=(\d+) endIn=(\d+) "
    r"endOut=(\d+) v3In=(\d+) v3Out=(\d+) v2In=(\d+) v2Out=(\d+) openIn=([01]) sample=([01])")
# GH #67 step 1 (sixth correction, ADR-0488): the projection matrix behind each
# sampled perspective load, so the engine's near-plane convention is auditable
# (the pass trace's `P?` was a hook-side extraction failure). `classic` is the
# previous `m14/(m10-1)`; on the engine's z-flipped matrix it is `-far`, which is
# why every load read `P?`. `near` is the (now corrected) extraction, 0 = unknown.
# `m10`/`m11`/`m14`/`m15` are column-major as `glLoadMatrixf` takes them.
VIEWMODEL_PROJECTION_RE = re.compile(
    r"ViewmodelProbe proj \[#(\d+)\]: m0=([-\d.]+) m5=([-\d.]+) m10=([-\d.]+) "
    r"m11=([-\d.]+) m14=([-\d.]+) m15=([-\d.]+) persp=([01]) "
    r"classic=([-\d.]+) near=([-\d.]+)")

_MATRIX_ROW_RE = re.compile(r"\[([-\d.eE ]+)\]")

# GH #71 requirement 1 / GH #80 follow-up: the sim input bridge's observation
# lines (src/hook/sim_input_bridge_win32.cpp). Every prior wake could only
# *infer* which input path the engine took and whether the fabricated confirm is
# ever read; these lines record it -- which of the input entry points the engine
# resolved through the swapped GetProcAddress (owned or merely observed), how
# often it polled the wrapped entry point, how many confirms the bridge
# fabricated, and the real API's last return code (a never-connected XInput
# device reports 1167 ERROR_DEVICE_NOT_CONNECTED, so the ORed bytes alone are
# invisible to the engine). Absent on a non-sim run or a pre-#80 hook.
INPUT_BRIDGE_GATE_RE = re.compile(
    r"XRSIM input bridge: gate on route=(\w+) installed=(\w+) getProcCalls=(\d+) "
    r"inputProcRequests=(\d+) createDevicePatched=(\d+) keyboardWrapped=(\d+)")
INPUT_BRIDGE_PROC_RE = re.compile(
    r"XRSIM input bridge: engine requested (\S+) \(owned=(yes|no)\)")
INPUT_BRIDGE_RESOLVED_RE = re.compile(
    r"XRSIM input bridge: input procs resolved so far: (.+)$")
INPUT_BRIDGE_FIRSTPOLL_RE = re.compile(
    r"XRSIM input bridge: XInputGetState first poll \(userIndex=(\d+) result=(\d+)\)")
INPUT_BRIDGE_RESULT_CHANGE_RE = re.compile(
    r"XRSIM input bridge: XInputGetState result changed -> (\d+) \(poll=(\d+)\)")
INPUT_BRIDGE_KEYBOARD_POLL_RE = re.compile(
    r"XRSIM input bridge: GetDeviceState (first poll|poll=(\d+))")
INPUT_BRIDGE_SUMMARY_RE = re.compile(
    r"XRSIM input bridge: summary route=(\w+) polls=(\d+) taps=(\d+) "
    r"keyboardPolls=(\d+) keyboardTaps=(\d+) lastRealResult=(none|\d+) "
    r"createDevicePatched=(\d+) keyboardWrapped=(\d+) getProcCalls=(\d+) "
    r"inputProcRequests=(\d+) procs\[(.*?)\]")
# XInput's ERROR_DEVICE_NOT_CONNECTED. The real call reporting this is why a
# synthetic pad's *bytes* cannot be seen by an engine that checks the pad first.
XINPUT_ERROR_DEVICE_NOT_CONNECTED = 1167
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


# GH #67 step 1 (fourth correction, ADR-0484): the geometric fingerprint of the
# window's largest decodable draw. The engine's first-person viewmodel is a 3-D
# mesh, so its submitted geometry cannot be *planar*; a screen-space overlay
# (a splash/post-process/HUD element) is a plane whose smallest axis extent is a
# small fraction of its largest. Measured on the 2009 target (wake 83): the
# "arms candidate" is `min=(-0.450,-0.450,-0.050) max=(0.450,0.450,0.000)`,
# i.e. dz/dx = 0.05/0.90 = 0.056 -- a plane, not arms. The ratio is the graded
# discriminator; 0.15 is ~2.7x the measured plane and far below any arm shape.
VIEWMODEL_CANDIDATE_MIN_THICKNESS_RATIO = 0.15


def viewmodel_candidate_geometry(sample):
    """GH #67 (ADR-0484): is the sampled in-window draw a 3-D mesh or a plane?

    Pure. ``sample`` is one parsed ``ViewmodelProbe submission fp`` record (or
    ``None``). Returns ``None`` when there is nothing to measure, else
    ``{"extent": [dx, dy, dz], "thickness_ratio": min/max, "volumetric": bool}``
    where ``volumetric`` is True only for a *valid* sample whose smallest axis
    extent is at least :data:`VIEWMODEL_CANDIDATE_MIN_THICKNESS_RATIO` of its
    largest. ``volumetric=False`` means the sampled draw is a screen-space
    element, so it cannot be the engine's first-person viewmodel.
    """
    if not sample:
        return None
    lo = sample.get("min")
    hi = sample.get("max")
    if not lo or not hi or len(lo) < 3 or len(hi) < 3:
        return None
    extent = [hi[i] - lo[i] for i in range(3)]
    largest = max(extent)
    smallest = min(extent)
    ratio = (smallest / largest) if largest > 0.0 else 0.0
    return {
        "extent": [round(e, 4) for e in extent],
        "largest_extent": round(largest, 4),
        "smallest_extent": round(smallest, 4),
        "thickness_ratio": round(ratio, 4),
        "volumetric": bool(sample.get("valid")) and largest > 0.0 and
                      ratio >= VIEWMODEL_CANDIDATE_MIN_THICKNESS_RATIO,
    }


def viewmodel_window_ran(probe):
    """GH #67 step 1 (fifth correction, ADR-0487): did the probe run at all?

    Pure. ``probe`` is :func:`viewmodel_probe_stats` output. The submission
    observer logs its interval tallies on every sampled frame whether or not the
    window ever opened, so a non-empty ``submission_lines`` proves
    ``ViewmodelObserve=1`` on a #0482+ hook. That distinction matters: with an
    opener that never fires (measured: ``ViewmodelWindowOpener=2`` on the live
    level -- ``arrayIn=0`` for every interval of a whole run) ``lines`` is empty,
    and treating that as "the probe was not run" silently hides a *failed*
    identification. Returns
    ``{"ran": bool, "intervals": int, "in_window_draws": int}``.
    """
    lines = (probe or {}).get("submission_lines") or []
    return {
        "ran": bool(lines),
        "intervals": len(lines),
        "in_window_draws": sum(int(ln.get("array_inside") or 0) for ln in lines),
    }


def viewmodel_projection_stats(log_text):
    """GH #67 step 1 (sixth correction, ADR-0488): the projections behind the probe.

    Pure. Each ``ViewmodelProbe proj`` line records one sampled perspective load's
    matrix depth row (``m10``/``m11``/``m14``/``m15``), the *classic* extraction
    ``m14/(m10-1)`` and the hook's extraction (``near``, 0 = unknown). Returns
    ``{"lines": [...], "perspective": int, "near_known": int, "distinct": int}``;
    an empty ``lines`` list means a pre-#0488 hook (or ``ViewmodelObserve=0``), so
    callers must *skip*, not fail.
    """
    lines = []
    for m in VIEWMODEL_PROJECTION_RE.finditer(log_text or ""):
        lines.append({
            "index": int(m.group(1)),
            "m0": float(m.group(2)), "m5": float(m.group(3)),
            "m10": float(m.group(4)), "m11": float(m.group(5)),
            "m14": float(m.group(6)), "m15": float(m.group(7)),
            "perspective": m.group(8) == "1",
            "classic": float(m.group(9)), "near": float(m.group(10)),
        })
    known = [ln for ln in lines if ln["perspective"] and ln["near"] > 0.0]
    return {
        "lines": lines,
        "perspective": sum(1 for ln in lines if ln["perspective"]),
        "near_known": len(known),
        # How many distinct (m10, m11, m14, near) tuples the run loaded: the count
        # of projection conventions present is the evidence a near-plane *drop*
        # between passes is even expressible (the NearPlaneDrop opener's premise).
        "distinct": len({(ln["m10"], ln["m11"], ln["m14"], ln["near"]) for ln in lines}),
    }


def viewmodel_near_field_coverage(probe):
    """GH #67 step 1 (eighth correction, ADR-0490): how far the window-independent
    near-field candidate covers the run.

    Pure. ``probe`` is :func:`viewmodel_probe_stats` output. Returns
    ``{"supported", "intervals", "in_window_intervals", "near_intervals"}``.
    ``supported`` is False for a pre-#0490 hook (no ``nearSample=`` marker on the
    interval line) so callers SKIP rather than fail. ``in_window_intervals`` counts
    the sampled intervals whose window had at least one in-window draw -- the
    available per-interval proxy for "the engine drew the first-person pass"; the
    discriminator claim under test is "the viewmodel *is* the near-field draw", so a
    run where the window drew in N intervals but a near-field candidate appeared in
    fewer than N of them does not support that claim.
    """
    lines = (probe or {}).get("submission_lines") or []
    return {
        "supported": any(ln.get("near_sampled") is not None for ln in lines),
        "intervals": len(lines),
        "in_window_intervals": sum(1 for ln in lines if int(ln.get("array_inside") or 0) > 0),
        "near_intervals": sum(1 for ln in lines if ln.get("near_sampled") == 1),
    }


def viewmodel_immediate_census(probe):
    """GH #67 step 1 (ninth correction, ADR-0491): the census of the first-person
    window's submission *mechanism*.

    Pure. ``probe`` is :func:`viewmodel_probe_stats` output. Returns the parsed
    immediate-mode tallies plus ``ran``: False for a pre-#0491 hook (no
    ``ViewmodelProbe immediate`` line) so callers SKIP rather than fail.
    ``vertex3_inside`` counts the 3-D immediate-mode vertices
    (``glVertex3f``/``glVertex3fv``) whose ``glBegin`` block opened *inside* the
    first-person window -- the mechanism question the reference left open ("if the
    first-person arms turn out to use immediate mode"), and one the array/list
    sampling could never answer because such a mesh emits no ``glDrawElements``.
    ``vertex2_inside`` is the 2-D path (HUD/screen-space), kept separate so "the
    window is 2-D immediate geometry" is distinguishable from "it is a 3-D mesh".
    """
    imm = (probe or {}).get("immediate")
    out = {"ran": bool(imm), "intervals": 0, "began_inside": 0, "began_outside": 0,
           "ends_inside": 0, "ends_outside": 0, "vertex3_inside": 0,
           "vertex3_outside": 0, "vertex2_inside": 0, "vertex2_outside": 0,
           "open_inside": 0, "sampled_intervals": 0, "fpimm": None}
    if imm:
        out.update(imm)
    return out


def viewmodel_probe_stats(log_text):
    """GH #67 step 2: summarise the observation-only first-person viewmodel probe.

    Read from the hook log (needs ``ViewmodelObserve=1``): the sampled frames'
    in/out-of-window draw counts and the GL_MODELVIEW captured at the first
    in-window draw. Returns ``{"lines": [...], "matrix": [...16 floats]|None}``;
    an empty ``lines`` list means the probe was not enabled / not present (a
    pre-#67 hook or a run without the flag), so callers must *skip*, not fail.

    ADR-0481 adds the ARB vertex-program tally (``program_lines`` plus a summed
    ``program`` dict) -- the *other* candidate for the transform source. It is
    present only from a hook that intercepts that path, so it too is additive.
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
    # GH #67 step 1 (corrected, ADR-0481): the ARB vertex-program tally, the
    # *other* candidate for "where the transforms are". Present only in a hook
    # built with the ARB-path observation (and only with ViewmodelObserve=1).
    program = []
    program_matrix = None
    for m in VIEWMODEL_PROGRAM_RE.finditer(text):
        program.append({
            "index": int(m.group(1)), "strings": int(m.group(2)),
            "binds": int(m.group(3)), "env": int(m.group(4)),
            "last_program": int(m.group(5)), "string_len": int(m.group(6)),
            "string_hash": m.group(7), "matrix_assembled": m.group(8) == "1",
            "matrix_base": int(m.group(9)),
        })
    if program:
        pm = VIEWMODEL_PROGRAM_MATRIX_RE.search(text)
        if pm:
            rows = _MATRIX_ROW_RE.findall(text[pm.end():])
            if len(rows) >= 4:
                try:
                    vals = [float(v) for row in rows[:4] for v in row.split()]
                except ValueError:
                    vals = []
                if len(vals) == 16:
                    program_matrix = vals
    # GH #67 step 1 (second correction, ADR-0482): the submitted-vertex sample.
    # `submission_lines` empty means the observation was not enabled / a pre-#0482
    # hook, so this too is additive (a legacy log yields zeros + null samples).
    submission = []
    for m in VIEWMODEL_SUBMISSION_RE.finditer(text):
        submission.append({
            "index": int(m.group(1)),
            "array_inside": int(m.group(2)), "array_outside": int(m.group(3)),
            "list_inside": int(m.group(4)), "list_outside": int(m.group(5)),
            "pointer_size": int(m.group(6)), "pointer_type": int(m.group(7)),
            "pointer_stride": int(m.group(8)),
            "inside_sampled": m.group(9) == "1", "outside_sampled": m.group(10) == "1",
            # GH #67 (ADR-0490): present only on a #0490+ hook -- it is the marker
            # that distinguishes "this hook reports a near-field candidate, and here
            # is whether it found one" from "this hook has no near-field candidate
            # at all" (a pre-#0490 build must be skipped, never failed).
            "near_sampled": int(m.group(11)) if m.group(11) is not None else None,
        })
    submission_samples = {}
    submission_sample_list = []
    for m in VIEWMODEL_SUBMISSION_SAMPLE_RE.finditer(text):
        sample = {
            "label": m.group(1),
            "first": [float(m.group(2)), float(m.group(3)), float(m.group(4))],
            "min": [float(m.group(5)), float(m.group(6)), float(m.group(7))],
            "max": [float(m.group(8)), float(m.group(9)), float(m.group(10))],
            "read": int(m.group(11)),
            "count": int(m.group(12)) if m.group(12) else None,
            "index_type": int(m.group(13)),
            "vertex_type": int(m.group(14)), "valid": m.group(15) == "1",
            # GH #67 (ADR-0490): `near` samples carry whether the window was open
            # at that draw (null on `fp`/`world`/pre-0490 logs).
            "inside_window": int(m.group(16)) if m.group(16) is not None else None,
        }
        # GH #67 (ADR-0484): the sample's geometry -- planar (a screen-space
        # element) or volumetric (a mesh, the required shape of an arms mesh).
        sample["geometry"] = viewmodel_candidate_geometry(sample)
        submission_sample_list.append(sample)
        # GH #67 (seventh correction, ADR-0489): the per-label slot keeps the LAST
        # sample (back-compatible), but every sampled interval is now retained --
        # a run samples one draw per 300-swap interval and the contents differ
        # between intervals (measured: the same near-plane-drop run sampled both a
        # 14688-count volumetric mesh and a 2520-count plane), so grading only the
        # last interval made the verdict depend on which interval was sampled last.
        submission_samples[m.group(1)] = sample
    # GH #67 step 1 (ninth correction, ADR-0491): the immediate-mode census. Absent
    # on a pre-#0491 hook / ViewmodelObserve=0, so this is additive too.
    immediate = []
    for m in VIEWMODEL_IMMEDIATE_RE.finditer(text):
        immediate.append({
            "index": int(m.group(1)),
            "began_inside": int(m.group(2)), "began_outside": int(m.group(3)),
            "ends_inside": int(m.group(4)), "ends_outside": int(m.group(5)),
            "vertex3_inside": int(m.group(6)), "vertex3_outside": int(m.group(7)),
            "vertex2_inside": int(m.group(8)), "vertex2_outside": int(m.group(9)),
            "open_inside": m.group(10) == "1", "sampled": m.group(11) == "1",
        })
    return {"lines": lines, "matrix": matrix, "pushes": pushes, "translates": translates,
            "program_lines": program,
            "program": {
                "strings": sum(p["strings"] for p in program),
                "binds": sum(p["binds"] for p in program),
                "env_params": sum(p["env"] for p in program),
                "matrix": program_matrix,
            } if program else None,
            "submission_lines": submission,
            "submission": {
                "array_inside": sum(s["array_inside"] for s in submission),
                "array_outside": sum(s["array_outside"] for s in submission),
                "list_inside": sum(s["list_inside"] for s in submission),
                "list_outside": sum(s["list_outside"] for s in submission),
                "fp": submission_samples.get("fp"),
                "world": submission_samples.get("world"),
                "near": submission_samples.get("near"),
                # GH #67 step 1 (ninth correction, ADR-0491): the immediate-mode
                # candidate (the window's largest glBegin..glEnd block, ADR-0483's
                # "largest mesh" in immediate form); None on a pre-#0491 hook.
                "fpimm": submission_samples.get("fpimm"),
                "samples": submission_sample_list,
            } if submission else None,
            "immediate_lines": immediate,
            "immediate": {
                "intervals": len(immediate),
                "began_inside": sum(x["began_inside"] for x in immediate),
                "began_outside": sum(x["began_outside"] for x in immediate),
                "ends_inside": sum(x["ends_inside"] for x in immediate),
                "ends_outside": sum(x["ends_outside"] for x in immediate),
                "vertex3_inside": sum(x["vertex3_inside"] for x in immediate),
                "vertex3_outside": sum(x["vertex3_outside"] for x in immediate),
                "vertex2_inside": sum(x["vertex2_inside"] for x in immediate),
                "vertex2_outside": sum(x["vertex2_outside"] for x in immediate),
                "open_inside": sum(1 for x in immediate if x["open_inside"]),
                "sampled_intervals": sum(1 for x in immediate if x["sampled"]),
                "fpimm": submission_samples.get("fpimm"),
            } if immediate else None}


def viewmodel_candidate_best(samples, label="fp"):
    """GH #67 step 1 (seventh correction, ADR-0489): the run's best first-person
    candidate, over *every* sampled interval.

    Pure. ``samples`` is the parsed ``submission.samples`` list (each entry
    carries ``label``/``count``/``geometry``); ``label`` selects which sample
    stream to grade (``fp`` = the in-window candidate, ``near`` = the
    window-independent near-field candidate of ADR-0490). Returns
    ``{"sampled", "volumetric", "best", "geometry", "last"}`` where ``best`` is
    the largest-count *volumetric* sample when any interval sampled one, else the
    last sample (so a plain run still reports its most recent observation). The
    graded shape question is "did the window ever hold a 3-D mesh", so it must not
    be decided by the arbitrary last interval of the run.
    """
    fps = [s for s in (samples or []) if s.get("label") == label]
    vols = [s for s in fps if (s.get("geometry") or {}).get("volumetric")]
    pick = max(vols, key=lambda s: s.get("count") or 0) if vols else (fps[-1] if fps else None)
    return {
        "sampled": len(fps),
        "volumetric": len(vols),
        "best": pick,
        "geometry": (pick or {}).get("geometry"),
        "last": fps[-1] if fps else None,
    }


# GH #67 step 1 (eighth correction, ADR-0490): the near-field bound the hook uses
# for the window-independent candidate. Mirrored here so the harness can re-check
# the logged bounds (a hook whose threshold disagreed would be visible, not
# silently trusted). Measured separation on the 2009 target: near-field meshes at
# ~±0.45, the view-space world/overlay draws at |x| ~ 1.6e3..3.5e3.
VIEWMODEL_NEAR_FIELD_MAX_ABS = 16.0


def viewmodel_sample_is_near_field(sample, max_abs=VIEWMODEL_NEAR_FIELD_MAX_ABS):
    """Pure. True when a *valid* sample's logged bounds lie entirely inside
    ``±max_abs`` on every axis -- the same predicate the hook applies to pick the
    window-independent candidate (ADR-0490)."""
    if not sample or not sample.get("valid"):
        return False
    lo = sample.get("min")
    hi = sample.get("max")
    if not lo or not hi or len(lo) < 3 or len(hi) < 3:
        return False
    return all(-max_abs <= lo[c] and hi[c] <= max_abs for c in range(3))



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


def parse_input_bridge(text):
    """GH #71 req 1 / #80 follow-up: the sim input bridge's observed activity.

    Collects the bridge's own log lines into a dict:

    ``gate``     -- the attach/deferred-init report (route, installed, the
                    GetProcAddress counters),
    ``procs``    -- ``{name: owned}`` for every input proc the engine requested
                    through the swapped GetProcAddress,
    ``polls``    -- the highest XInputGetState poll count seen,
    ``taps``     -- the number of fabricated XInput press edges,
    ``keyboard_polls`` / ``keyboard_taps`` -- the DirectInput route's equivalents,
    ``last_real_result`` -- the real XInput API's last return code (``1167``
                    ERROR_DEVICE_NOT_CONNECTED means the engine sees no pad, so
                    the ORed confirm bytes are never read),
    ``result_changes`` -- ``[[result, poll], ...]`` transitions of that code,
    ``summaries`` -- the parsed periodic ``summary`` records (a timeline),
    ``present``  -- whether the bridge logged anything at all.

    Empty (``present`` False) on a non-sim run, a pre-#80 hook, or an
    ``--evaluate-only`` run with no hook log: the report key is then ``None``.
    """
    out = {"gate": None, "procs": {}, "polls": 0, "taps": 0, "keyboard_polls": 0,
           "keyboard_taps": 0, "last_real_result": None, "result_changes": [],
           "summaries": [], "present": False}
    for line in (text or "").splitlines():
        m = INPUT_BRIDGE_GATE_RE.search(line)
        if m:
            out["present"] = True
            out["gate"] = {"route": m.group(1), "installed": m.group(2) == "yes",
                           "get_proc_calls": int(m.group(3)),
                           "input_proc_requests": int(m.group(4)),
                           "create_device_patched": m.group(5) == "1",
                           "keyboard_wrapped": m.group(6) == "1"}
            continue
        m = INPUT_BRIDGE_PROC_RE.search(line)
        if m:
            out["present"] = True
            out["procs"][m.group(1)] = m.group(2) == "yes"
            continue
        m = INPUT_BRIDGE_RESOLVED_RE.search(line)
        if m and m.group(1).strip() != "none":
            out["present"] = True
            for token in m.group(1).split(","):
                token = token.strip()
                if ":" in token:
                    name, state = token.rsplit(":", 1)
                    out["procs"][name] = state == "owned"
            continue
        m = INPUT_BRIDGE_FIRSTPOLL_RE.search(line)
        if m:
            out["present"] = True
            out["polls"] = max(out["polls"], 1)
            out["last_real_result"] = int(m.group(2))
            continue
        m = INPUT_BRIDGE_RESULT_CHANGE_RE.search(line)
        if m:
            out["present"] = True
            out["result_changes"].append([int(m.group(1)), int(m.group(2))])
            out["last_real_result"] = int(m.group(1))
            continue
        m = INPUT_BRIDGE_KEYBOARD_POLL_RE.search(line)
        if m:
            out["present"] = True
            n = 1 if m.group(1) == "first poll" else int(m.group(2))
            out["keyboard_polls"] = max(out["keyboard_polls"], n)
            continue
        m = INPUT_BRIDGE_SUMMARY_RE.search(line)
        if m:
            out["present"] = True
            rec = {"route": m.group(1), "polls": int(m.group(2)), "taps": int(m.group(3)),
                   "keyboard_polls": int(m.group(4)), "keyboard_taps": int(m.group(5)),
                   "last_real_result": None if m.group(6) == "none" else int(m.group(6)),
                   "create_device_patched": m.group(7) == "1",
                   "keyboard_wrapped": m.group(8) == "1",
                   "get_proc_calls": int(m.group(9)),
                   "input_proc_requests": int(m.group(10)),
                   "procs": [t for t in m.group(11).split(",") if t and t != "none"]}
            out["summaries"].append(rec)
            out["polls"] = max(out["polls"], rec["polls"])
            out["taps"] = max(out["taps"], rec["taps"])
            out["keyboard_polls"] = max(out["keyboard_polls"], rec["keyboard_polls"])
            out["keyboard_taps"] = max(out["keyboard_taps"], rec["keyboard_taps"])
            if rec["last_real_result"] is not None:
                out["last_real_result"] = rec["last_real_result"]
            for token in rec["procs"]:
                if ":" in token:
                    name, state = token.rsplit(":", 1)
                    out["procs"][name] = state == "owned"
    if not out["present"]:
        return None
    out["xinput_connected"] = (out["last_real_result"] == 0)
    out["xinput_device_not_connected"] = (
        out["last_real_result"] == XINPUT_ERROR_DEVICE_NOT_CONNECTED)
    return out


def input_bridge_stats(parsed):
    """One-line summary of :func:`parse_input_bridge` for a run's stdout."""
    if not parsed:
        return "input bridge: not opted in (no --sim-confirm lines)"
    procs = ",".join(sorted(parsed["procs"])) or "none"
    return (f"input bridge: polls={parsed['polls']} taps={parsed['taps']} "
            f"keyboard_polls={parsed['keyboard_polls']} keyboard_taps={parsed['keyboard_taps']} "
            f"lastRealResult={parsed['last_real_result']} procs={procs}")


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


# ----------------------------------------- live-run lock + isolation (GH #83)
# The autoloop can have more than one agent in one checkout: a runner killed by a
# harness restart leaves its `opencode run` child alive while the runner's mutex is
# free (GH #83). Two live XRSIM runs then collide -- the *shared* default shadow
# directory gets rebuilt under a run in progress (deleting its hook log), and two
# `DarkAthena.exe`/`riddickvr-host.exe` instances hold the same `dist` binaries.
# Two cheap, checkable rules remove that class of damage:
#   1. the shadow scratch directory is per run (keyed by this pid), so no run can
#      rebuild another run's tree; and
#   2. a live-run lock makes "at most one live harness" explicit -- a second run
#      refuses instead of racing, and a lock whose pid is gone is stale and taken
#      over, so a killed run never wedges the loop.
DEFAULT_LIVE_LOCK_NAME = "riddickvr-live.lock"


def default_shadow_dir(pid, root=None):
    """The per-run ``--shadow-install`` scratch directory (GH #83)."""
    base = Path(root) if root else Path(tempfile.gettempdir())
    return base / f"riddickvr-shadow-{int(pid)}"


def live_lock_path(root=None):
    base = Path(root) if root else Path(tempfile.gettempdir())
    return base / DEFAULT_LIVE_LOCK_NAME


def read_live_lock(path):
    """Parse the live-run lock; a missing / empty / half-written file reads None."""
    try:
        raw = Path(path).read_text(encoding="utf-8")
    except OSError:
        return None
    try:
        data = json.loads(raw)
    except ValueError:
        return None
    return data if isinstance(data, dict) else None


def live_lock_decision(lock, is_alive, pid):
    """Pure: classify the on-disk lock -> ``(state, other_pid)``.

    ``free``  -- no lock (or no usable pid in it): safe to take.
    ``owned`` -- the lock is already ours.
    ``stale`` -- the recorded holder is gone: safe to take over.
    ``held``  -- a live peer holds it: do not start a second live run.
    """
    if not lock:
        return "free", None
    try:
        other = int(lock.get("pid"))
    except (TypeError, ValueError):
        return "free", None
    if other == int(pid):
        return "owned", other
    return ("held" if is_alive(other) else "stale"), other


def process_alive(pid):
    """Best-effort liveness probe (only used to decide a *stale* lock)."""
    try:
        pid = int(pid)
    except (TypeError, ValueError):
        return False
    if pid <= 0:
        return False
    if os.name != "nt":
        try:
            os.kill(pid, 0)
        except OSError:
            return False
        return True
    import ctypes
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    handle = ctypes.windll.kernel32.OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return False
    ctypes.windll.kernel32.CloseHandle(handle)
    return True


def acquire_live_lock(path, pid, is_alive=None, started=None):
    """Take the live-run lock. Returns ``(ok, state, message)``.

    Writes the lock when it is free / stale / already ours; refuses (``ok`` False)
    when a live peer holds it.
    """
    is_alive = is_alive or process_alive
    path = Path(path)
    state, other = live_lock_decision(read_live_lock(path), is_alive, pid)
    if state == "held":
        return False, state, (
            f"another live XRSIM run holds {path} (pid {other}); only one live "
            "harness at a time (GH #83) -- wait for it or pass --ignore-live-lock")
    payload = {
        "pid": int(pid),
        "started": started or datetime.now(timezone.utc).isoformat(timespec="seconds"),
    }
    tmp = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp.write_text(json.dumps(payload), encoding="utf-8")
        os.replace(tmp, path)
    except OSError as exc:
        return False, state, f"cannot write the live-run lock {path}: {exc}"
    taken = "taken over (stale pid %s)" % other if state == "stale" else "acquired"
    return True, state, f"live lock {taken}: {path}"


def release_live_lock(path, pid):
    """Remove the lock only if we still own it (never clobber a takeover)."""
    lock = read_live_lock(path)
    try:
        other = int(lock.get("pid")) if lock else None
    except (TypeError, ValueError):
        other = None
    if other != int(pid):
        return False
    try:
        Path(path).unlink()
    except OSError:
        return False
    return True


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
# MEASURED (wake 84): this engine *does* dispatch posted window messages to its
# window procedure -- and its key handler takes the **scan code** from the
# message's lParam (`lParam >> 16 & 0x1ff`), using `MapVirtualKeyA(vk, 0)` only
# when that field is the `0x100` sentinel (see the module docstring and
# docs/rea/reference/RndrGL_Input_Path.txt). The earlier attempt posted
# `lParam = 1` (scan code 0), which the engine receives as key code 0 -- an
# unmapped key -- so "the engine ignores posted messages" was never measured.
# The tap now packs the real scan code, exactly as the hook's own
# src/hook/gesture_tap.cpp does (`MapVirtualKeyA(vk, MAPVK_VK_TO_VSC) << 16`).
# The capability stays opt-in (`--advance-menu`, default off) until a run
# measures it.
VK_RETURN = 0x0D
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_CHAR = 0x0102

# GH #71 req 1: what a real key message packs when lParam carries no scan code.
# The engine's window proc treats 0x100 as that sentinel (it then calls
# MapVirtualKeyA itself), so it is *not* the scan code to send.
KEY_LPARAM_NO_SCANCODE = 0x100
# Minimal VK -> scan code table for the confirm keys, used off Windows (or if the
# OS lookup is unavailable) so the message encoding stays pure and unit-testable.
KEY_SCANCODE_FALLBACK = {
    0x08: 0x0E,  # VK_BACK
    0x09: 0x0F,  # VK_TAB
    0x0D: 0x1C,  # VK_RETURN
    0x1B: 0x01,  # VK_ESCAPE
    0x20: 0x39,  # VK_SPACE
    0x25: 0x4B,  # VK_LEFT
    0x26: 0x48,  # VK_UP
    0x27: 0x4D,  # VK_RIGHT
    0x28: 0x50,  # VK_DOWN
}


def vk_to_scancode(vk):
    """Scan code for a virtual key, as the engine reads it from a key message.

    Uses ``MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)`` when the OS is available and
    falls back to ``KEY_SCANCODE_FALLBACK`` so the pure scheduling / encoding
    logic is testable off Windows. Returns 0 for an unmapped key (the value the
    engine cannot resolve to an action).
    """
    key = int(vk) & 0xFF
    if os.name == "nt":
        try:
            import ctypes
            scan = int(ctypes.WinDLL("user32").MapVirtualKeyW(key, 0))
            if scan:
                return scan & 0xFF
        except Exception:
            pass
    return KEY_SCANCODE_FALLBACK.get(key, 0)


def key_lparam(scan, is_down):
    """Win32 lParam for a key message (scan code in bits 16-23, repeat=1;
    key-up also sets the transition/previous-state bits)."""
    lp = ((scan & 0xFF) << 16) | 1
    if not is_down:
        lp |= (1 << 30) | (1 << 31)
    return lp


def confirm_key_messages(vk=VK_RETURN, scan=None):
    """The exact (message, wParam, lParam) sequence a confirm tap posts, in
    order -- mirroring gesture_tap.cpp's SendKeyTap (keydown, WM_CHAR for the
    text keys the engine routes, keyup).

    ``scan`` defaults to the real scan code for ``vk`` (``vk_to_scancode``);
    passing 0 reproduces the pre-wake-84 tap that the engine could not map.
    """
    if scan is None:
        scan = vk_to_scancode(vk)
    return [(WM_KEYDOWN, vk, key_lparam(scan, True)),
            (WM_CHAR, vk, key_lparam(scan, True)),
            (WM_KEYUP, vk, key_lparam(scan, False))]


def post_key_to_game_windows(pid, messages):
    """Post an ordered list of ``(message, wParam, lParam)`` to every visible
    top-level window owned by ``pid``.

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

    reached = 0
    for hwnd in hwnds:
        ok = True
        for msg, wp, lp in messages:
            if not user32.PostMessageW(hwnd, msg, wp, lp):
                ok = False
        if ok:
            reached += 1
    return reached


def post_confirm_to_game_windows(pid, vk=VK_RETURN):
    """Post a confirm tap to every visible top-level window owned by ``pid``."""
    return post_key_to_game_windows(pid, confirm_key_messages(vk))


def scan_key_message(scan, is_down):
    """The message one *scan-code* key edge posts.

    The engine's window proc resolves a key from the **message's own scan code**
    (``lParam >> 16 & 0x1ff``; ``RndrGL.dll`` ``0x10042817``), not from
    ``wParam`` -- so ``wParam`` carries no virtual key here; the scan code is the
    whole key. See ``docs/rea/reference/RndrGL_Input_Path.txt``.
    """
    return (WM_KEYDOWN if is_down else WM_KEYUP, 0, key_lparam(scan, is_down))


def post_scan_to_game_windows(pid, scan, is_down):
    """Post one scan-code key edge (down/up) to the game's own top-level windows.

    Same targeted window messaging as the confirm tap (never desktop input), so
    it stays inside the #68 simulator exception.
    """
    return post_key_to_game_windows(pid, [scan_key_message(scan, is_down)])


# --------------------------------------------- in-level key drive (GH #67/#80)
# The engine's window proc takes the scan code from the message's lParam (see
# docs/rea/reference/RndrGL_Input_Path.txt), and the *player profile* maps scan
# codes to actions, e.g.
#   %APPDATA%\Starbreeze\Riddick\Content\Save\<profile>\_profile
#     *ACTION_FORWARD 17        *ACTION_USE 18        *ACTION_JUMP 57
#     *ACTION_WEAPON_SHIV 3     *ACTION_WEAPON_CLUB 4 *ACTION_WEAPON_PISTOL 5
#     *ACTION_CYCLEWEAPON 47    *ACTION_PRIMARYFIRE 240 (mouse)
# So a scripted run can select a weapon / fire / move by posting the profile's
# own scan codes to the game window -- still targeted window messaging (never
# desktop input), so it stays inside the #68 simulator exception. This is the
# input route the #67 state question needs: reach an ARMED/FIGHTING state, the
# one in which the engine is known to draw its first-person arms, so the
# existing ViewmodelObserve probe can observe them.
DEFAULT_DRIVE_HOLD_MS = 120
DEFAULT_DRIVE_GAP_MS = 250
PROFILE_ACTION_PREFIX = "*ACTION_"


def parse_action_profile(text):
    """Parse a profile's ``*ACTION_<NAME> <scancode>`` lines.

    Returns ``{NAME: scancode}`` with upper-cased names (``FORWARD``,
    ``PRIMARYFIRE``, ``WEAPON_SHIV``, ...). Unparsable lines are skipped so a
    profile change cannot crash the runner.
    """
    actions = {}
    for line in (text or "").splitlines():
        line = line.strip()
        if not line.startswith(PROFILE_ACTION_PREFIX):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        name = parts[0][len(PROFILE_ACTION_PREFIX):].upper()
        try:
            scan = int(parts[1], 0)
        except ValueError:
            continue
        if name and 0 <= scan <= 0x1FF:
            actions[name] = scan
    return actions


def profile_action_path(explicit=None):
    """The player profile that maps actions to scan codes.

    ``explicit`` (``--action-profile``) wins; otherwise the first ``_profile``
    under ``%APPDATA%\\Starbreeze\\Riddick\\Content\\Save`` is used. Returns
    ``None`` when none exists (the drive then accepts raw scan codes only).
    """
    if explicit:
        return Path(explicit)
    base = (Path(os.environ.get("APPDATA", "")) / "Starbreeze" / "Riddick"
            / "Content" / "Save")
    try:
        candidates = sorted(base.glob("*/_profile"))
    except OSError:
        candidates = []
    return candidates[0] if candidates else None


def parse_drive_keys(spec, actions=None):
    """Parse a ``--drive-keys`` spec into ordered ``(scan, hold_ms)`` steps.

    Comma-separated; each step is ``SCAN`` or ``SCAN:HOLD_MS`` or an action name
    (``FORWARD``, ``PRIMARYFIRE``, ``WEAPON_SHIV``) resolved through ``actions``
    (``parse_action_profile``). Raises ``ValueError`` on an unparsable step so a
    typo fails loudly instead of posting the wrong key.
    """
    steps = []
    for raw in str(spec or "").split(","):
        item = raw.strip()
        if not item:
            continue
        name, _, hold_s = item.partition(":")
        name = name.strip()
        held = hold_s.strip()
        try:
            scan = int(name, 0)
        except ValueError:
            key = name.upper()
            if not actions or key not in actions:
                raise ValueError(f"unknown key {name!r} (not a scan code and not "
                                 f"in the action profile)")
            scan = actions[key]
        if not (0 <= scan <= 0x1FF):
            raise ValueError(f"scan code {scan} out of range (0..0x1FF)")
        hold = DEFAULT_DRIVE_HOLD_MS
        if held:
            try:
                hold = int(held, 0)
            except ValueError:
                raise ValueError(f"bad hold milliseconds {held!r} in {item!r}")
            if hold < 0:
                raise ValueError(f"negative hold milliseconds in {item!r}")
        steps.append((scan, hold))
    return steps


class KeyScriptDriver:
    """Post an ordered scan-code script to the game once gameplay is reached.

    Pure scheduling: an injected ``poster(scan, is_down) -> windows`` callable
    and clock make the down/hold/up/gap sequence unit-testable without a
    Windows handle. There is no thread -- the orchestration loop drives
    ``poll``; at most one key edge fires per ``poll``. Each step posts its
    key-down, holds for ``hold_ms``, then posts its key-up; steps are separated
    by ``gap_s``.
    """

    def __init__(self, poster, steps, gap_s=DEFAULT_DRIVE_GAP_MS / 1000.0,
                 clock=time.monotonic, start=None):
        self._poster = poster
        self._steps = [(int(s), int(h)) for s, h in steps]
        self.gap_s = max(0.0, float(gap_s))
        self._clock = clock
        self.posted = 0
        self.windows = 0
        self._i = 0
        self._down = True
        self._next = (self._clock() if start is None else start)

    @property
    def done(self):
        return self._i >= len(self._steps)

    @property
    def steps(self):
        return list(self._steps)

    def _post(self, scan, is_down):
        try:
            reached = int(self._poster(scan, is_down) or 0)
        except (TypeError, ValueError):
            reached = 0
        self.posted += 1
        if reached > self.windows:
            self.windows = reached

    def poll(self, now=None):
        """Fire one key edge when due. Returns 1 if a message was posted."""
        if self.done:
            return 0
        now = self._clock() if now is None else now
        if now < self._next:
            return 0
        scan, hold_ms = self._steps[self._i]
        if self._down:
            self._post(scan, True)
            self._down = False
            self._next = now + hold_ms / 1000.0
        else:
            self._post(scan, False)
            self._i += 1
            self._down = True
            self._next = now + self.gap_s
        return 1


# ------------------------------------------------- game-window activation (GH #80)
# Wake 96 measured the missing precondition for the simulator-only input bridge:
# the engine resolved BOTH ``DirectInputCreateA`` and ``XInputGetState`` through
# the hook's swapped ``GetProcAddress`` (``inputProcRequests=2``,
# ``procs[XInputGetState:owned,DirectInputCreateA:owned]``) yet never called
# either one (``polls=0 keyboardPolls=0 createDevicePatched=0``) -- so the
# fabricated confirm could never be read and the run stayed on ``PRESS ENTER``.
# A game that resolves its input procs and then never polls them is the
# signature of an *inactive* window: many engines skip device reads while they
# are not foreground. The harness launches the game from a background process,
# so activating the game's own top-level window (window management, NOT input)
# is the missing step. It is recorded in the report so a still-zero poll count
# can never be misread as "the bridge was not installed".
WINDOW_ACTIVATION_TIMEOUT_S = 15.0
WINDOW_ACTIVATION_RETRY_S = 0.5


def pick_game_window(windows, pid=None):
    """Pick the game's interactive top-level window from observed candidates.

    ``windows`` is an iterable of ``(hwnd, pid, visible, width, height, title)``.
    When ``pid`` is given, only that process's windows are eligible. The largest
    *visible* client area wins (the engine's render window), with the higher hwnd
    as a deterministic tie-break. Returns the winning tuple or None.
    Pure, so the choice is unit-tested without Win32.
    """
    best = None
    best_key = None
    for w in windows:
        _hwnd, wpid, visible, width, height, _title = w
        if pid is not None and int(wpid) != int(pid):
            continue
        if not visible or int(width) <= 0 or int(height) <= 0:
            continue
        key = (int(width) * int(height), int(_hwnd))
        if best_key is None or key > best_key:
            best, best_key = w, key
    return best


def enumerate_game_windows(pid):
    """Enumerate ``(hwnd, pid, visible, width, height, title)`` for a process.

    Dependency-free ctypes; a non-Windows/headless runner degrades to an empty
    list instead of raising.
    """
    if os.name != "nt" or not pid:
        return []
    try:
        import ctypes
        from ctypes import wintypes
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.EnumWindows.argtypes = [ctypes.c_void_p, wintypes.LPARAM]
        user32.EnumWindows.restype = wintypes.BOOL
        user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND,
                                                    ctypes.POINTER(wintypes.DWORD)]
        user32.GetWindowThreadProcessId.restype = wintypes.DWORD
        user32.IsWindowVisible.argtypes = [wintypes.HWND]
        user32.IsWindowVisible.restype = wintypes.BOOL
        user32.GetClientRect.argtypes = [wintypes.HWND,
                                         ctypes.POINTER(wintypes.RECT)]
        user32.GetClientRect.restype = wintypes.BOOL
        user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
        user32.GetWindowTextLengthW.restype = ctypes.c_int
        user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR,
                                          ctypes.c_int]
        user32.GetWindowTextW.restype = ctypes.c_int
    except Exception:
        return []

    found = []
    enum_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def _cb(hwnd, _lparam):
        wpid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value != int(pid):
            return True
        rect = wintypes.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(rect))
        n = max(0, user32.GetWindowTextLengthW(hwnd))
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        found.append((int(hwnd), int(wpid.value),
                      bool(user32.IsWindowVisible(hwnd)),
                      int(rect.right - rect.left), int(rect.bottom - rect.top),
                      buf.value))
        return True

    try:
        user32.EnumWindows(enum_proc(_cb), 0)
    except Exception:
        return []
    return found


def activate_game_window(pid):
    """Bring the game's own window to the foreground; return the observed state.

    Window management only -- no keyboard/mouse/OS input is injected. The
    ``AttachThreadInput`` handshake is what lets a background process transfer
    foreground ownership on Windows; every call is best-effort, and the returned
    ``foreground`` flag is re-read from the OS rather than assumed.
    """
    state = {"attempted": False, "pid": int(pid) if pid else None, "hwnd": None,
             "title": None, "width": 0, "height": 0, "was_foreground": None,
             "foreground": False, "set_foreground_ok": None, "attached": False,
             "found": False}
    if os.name != "nt" or not pid:
        return state
    window = pick_game_window(enumerate_game_windows(pid), pid)
    if window is None:
        return state
    hwnd, _wpid, _visible, width, height, title = window
    state.update({"attempted": True, "found": True, "hwnd": hwnd, "title": title,
                  "width": width, "height": height})
    try:
        import ctypes
        from ctypes import wintypes
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        user32.GetForegroundWindow.restype = wintypes.HWND
        user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND,
                                                    ctypes.POINTER(wintypes.DWORD)]
        user32.GetWindowThreadProcessId.restype = wintypes.DWORD
        user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
        user32.BringWindowToTop.argtypes = [wintypes.HWND]
        user32.SetForegroundWindow.argtypes = [wintypes.HWND]
        user32.SetForegroundWindow.restype = wintypes.BOOL
        user32.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD,
                                             wintypes.BOOL]
        user32.AttachThreadInput.restype = wintypes.BOOL
        kernel32.GetCurrentThreadId.restype = wintypes.DWORD

        fg = user32.GetForegroundWindow()
        state["was_foreground"] = bool(fg) and int(fg) == hwnd
        if state["was_foreground"]:
            state["foreground"] = True
            return state

        target_tid = user32.GetWindowThreadProcessId(hwnd, None)
        own_tid = kernel32.GetCurrentThreadId()
        if target_tid and own_tid and target_tid != own_tid:
            state["attached"] = bool(user32.AttachThreadInput(own_tid, target_tid,
                                                             True))
        try:
            user32.ShowWindow(hwnd, 5)  # SW_SHOW
            user32.BringWindowToTop(hwnd)
            state["set_foreground_ok"] = bool(user32.SetForegroundWindow(hwnd))
        finally:
            if state["attached"]:
                user32.AttachThreadInput(own_tid, target_tid, False)
        fg = user32.GetForegroundWindow()
        state["foreground"] = bool(fg) and int(fg) == hwnd
    except Exception:
        pass
    return state


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


# GH #71 req 1 (wake 85): the branch signal alone cannot bound the confirm taps.
# MEASURED (wake 85, card-only run): the engine emits its *own* startup transition
# as `DisplayMode: CINEMA ...` -> `3D Gameplay projection intercepted` ->
# `DisplayMode: FULL-BLEED ...` at **frame 1**, i.e. a cinema->full-bleed pair is
# in the log before any confirm has been delivered. So
# `gameplay_reached_from_sequence` is satisfied by the startup page, and the only
# reason wake 84 got 6 taps (and this run only 3) is hook-log write buffering --
# the stop is a race that cuts the advance off before the taps the wake-84
# measurement shows are needed (the card was left at the ~5th-6th tap, ~11-14 s at
# the default 2 s interval). Require the taps to have actually been delivered
# first: `ADVANCE_STOP_MIN_TAPS` (8) is > the 6 wake 84 measured, so the branch
# stop can never fire before the advance has had its taps.
ADVANCE_STOP_MIN_TAPS = 8


def advance_stop_decision(branch_seq, taps, min_taps=ADVANCE_STOP_MIN_TAPS):
    """Pure (GH #71 req 1, wake 85): may the confirm taps stop now?

    ``True`` only once at least ``min_taps`` taps have been delivered *and* the
    hook log shows a full-bleed branch after a cinema one. The tap-count floor
    exists because the engine's own startup transition already produces that
    branch pair (measured at frame 1, wake 85), so a bare branch check stops the
    advance before it has been attempted (wake 84: the card is left at tap 5-6).
    ``min_taps <= 0`` restores the pre-wake-85 behaviour (branch signal only).
    """
    return int(taps) >= int(min_taps) and gameplay_reached_from_sequence(branch_seq)


# ------------------------------------------------------------------ orchestration
# GH #87: a live run can end with DarkAthena.exe exiting within ~2 s -- no hook log
# (or one that stops during renderer init), the mock host logging
# `Game Connected: WAITING` for the whole (short) run, and 0 captured frames --
# *while the identical re-run passes*. That is not a measurement of the change
# under test, so an attempt that ends this way is retried once and, if it repeats,
# is reported as one distinct `game_started` failure instead of broken captures.
EARLY_EXIT_MIN_LIVE_SECONDS = 5.0
LAUNCH_MAX_ATTEMPTS = 2
# The checks that stay graded even when the game never started: the hook's
# on-disk identity is a fact about what *would* have been exercised, independent
# of whether this attempt reached a live scene.
LAUNCH_INDEPENDENT_CHECKS = ("deployed_hook_matches_build",)


def game_start_decision(game_exited, live_seconds, frames, hook_fresh,
                        min_live_seconds=EARLY_EXIT_MIN_LIVE_SECONDS):
    """Pure (GH #87 + extension): did the game actually start -- is this attempt a
    *measurement*?

    "Evidence" is a captured frame or a *fresh* hook log; without either, the hook
    never attached and no check on the capture is meaningful. Returns
    ``(state, detail)`` with ``state`` one of:

    * ``"started"`` -- the game produced evidence; grade normally.
    * ``"no_evidence"`` -- the game stayed live for at least ``min_live_seconds``
      but produced **no** captured frame and **no** fresh hook log: the hook never
      attached, so the run measured nothing. This is the **second** measured
      GH #87 signature (autoloop wake 96: the game lived 47 s with 0 frames and no
      hook log while the *identical* re-run passed). Without this state that
      attempt reads as a live scene that simply failed every check -- a false red.
    * ``"early_exit"`` -- the game process ended *before* ``min_live_seconds`` with
      no captured frame and no fresh hook log; the identical re-run passes, so the
      attempt is not evidence against the change under test.

    A harness-terminated run *shorter* than ``min_live_seconds`` with no evidence
    is still ``"started"``: a ``--frames`` run is bounded by frame count, not wall
    clock, so such a game may simply be too short to prove anything yet (the
    pre-extension behaviour for genuinely short runs is preserved).
    """
    evidence = bool(frames) or bool(hook_fresh)
    if evidence:
        if not game_exited:
            return "started", "game still live at run end"
        if frames:
            return ("started",
                    f"game exited after {live_seconds:.1f}s but captured {frames} "
                    f"frame(s)")
        return ("started",
                f"game exited after {live_seconds:.1f}s but wrote a fresh hook log")
    if live_seconds >= min_live_seconds:
        return ("no_evidence",
                f"game {'exited' if game_exited else 'live'} after "
                f"{live_seconds:.1f}s with no hook log and no captured frame: the "
                f"hook never attached, so the attempt is not a measurement "
                f"(min-live {min_live_seconds:.0f}s; GH #87 extension)")
    if not game_exited:
        return "started", "game still live at run end"
    return ("early_exit",
            f"game exited after {live_seconds:.1f}s with no hook log and no "
            f"captured frame (min-live {min_live_seconds:.0f}s; GH #87)")


def launch_retry_decision(state, attempt, max_attempts=LAUNCH_MAX_ATTEMPTS):
    """Pure (GH #87 + extension): retry an attempt that measured nothing.

    Both no-measurement states are retried while attempts remain -- a not-yet-final
    ``early_exit`` (game gone in ~2 s) *or* ``no_evidence`` (game live the whole
    window but the hook never attached). A real result is never retried, and the
    retry is bounded.
    """
    return (state in ("early_exit", "no_evidence")
            and attempt < max(int(max_attempts), 1))


def format_launch_summary(launch):
    """One line for the launch/attempt record (GH #87)."""
    if not launch:
        return "no launch recorded"
    attempts = launch.get("attempts") or []
    last = attempts[-1] if attempts else {}
    state = launch.get("state")
    if state is None:  # pre-extension launch records carry only the boolean
        state = "started" if launch.get("game_started") else "early_exit"
    label = {"started": "started", "no_evidence": "NO EVIDENCE",
             "early_exit": "DID NOT START"}.get(state, str(state))
    min_live = launch.get("min_live_seconds", EARLY_EXIT_MIN_LIVE_SECONDS)
    return (f"launch attempts={len(attempts)}/{launch.get('max_attempts')} "
            f"game {label}; last lived {last.get('live_seconds', 0):.1f}s, "
            f"{last.get('frames', 0)} frame(s), hook log "
            f"{'fresh' if last.get('hook_fresh') else 'absent'} "
            f"(min-live {min_live:.0f}s; GH #87)")


def demote_checks_for_no_game_start(checks, keep=LAUNCH_INDEPENDENT_CHECKS):
    """GH #87: an attempt the hook never attached to is not a measurement of the
    change under test, so its failing capture checks are *reported, not graded*.
    Returns the names of the demoted checks (the check list is modified in place)."""
    demoted = []
    for c in checks:
        if c.get("skip") or c.get("ok") or c.get("check") in keep:
            continue
        c["skip"] = True
        c["detail"] = (str(c.get("detail", "")) +
                       " (not graded: the attempt was not a measurement -- the hook "
                       "never attached; GH #87)").strip()
        demoted.append(c.get("check"))
    return demoted


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


def _run_attempt(args, artifacts: Path, attempt: int):
    """One host+game launch; the building block of :func:`run_sim` (GH #87).

    Returns ``(host_log, log_mark, menu_stats, info)`` where ``info`` records
    whether *this* attempt's game process ended on its own and how long it lived,
    so the caller can tell "the game did not start" from a real result.
    """
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
    # GH #87: a *fresh* hook log is evidence the game started. The log lives in the
    # game dir and outlives the attempt, so freshness is an mtime change, not
    # mere existence (a first attempt's log must not make a dead second look live).
    hook_existed = log_file.exists()
    hook_mtime = log_file.stat().st_mtime_ns if hook_existed else None

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
    game_start = time.time()

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
    menu_stats = {"taps": 0, "windows": 0, "reached": False, "enabled": False,
                  "vk": VK_RETURN, "scan": vk_to_scancode(VK_RETURN),
                  "min_taps": 0}
    if getattr(args, "advance_menu", False):
        menu_stats["enabled"] = True
        min_taps = int(getattr(args, "advance_min_taps", ADVANCE_STOP_MIN_TAPS)
                       or 0)
        menu_stats["min_taps"] = max(0, min_taps)
        advancer = GameMenuAdvancer(
            lambda: post_confirm_to_game_windows(game.pid),
            interval_s=float(getattr(args, "advance_interval", 2.0) or 2.0),
            max_taps=int(getattr(args, "advance_max_taps", 0) or 0))
        print(f"[xrsim] menu advance: confirm VK=0x{menu_stats['vk']:02X} "
              f"scan=0x{menu_stats['scan']:02X} every {advancer.interval_s:.1f}s "
              f"to pid {game.pid} (GH #71; stop after >= "
              f"{menu_stats['min_taps']} tap(s))")

    # GH #67/#80: drive the engine in-level with the profile's own scan codes once
    # gameplay is reached (weapon select / fire / move) so a run can reach the
    # armed/fighting state in which the engine draws its first-person arms, and
    # the existing ViewmodelObserve probe can observe them.
    drive_stats = {"enabled": False, "spec": None, "actions": {}, "steps": [],
                   "posted": 0, "windows": 0, "done": False, "started": False,
                   "profile": None}
    driver = None
    drive_start_after = float(getattr(args, "drive_after", None) or 12.0)
    # With an advancer we only know the level is loading once the gameplay stop
    # fires, so the drive waits *past* that; without one it waits from the start.
    drive_ready_at = float("inf") if getattr(args, "advance_menu", False) \
        else drive_start_after
    drive_spec = getattr(args, "drive_keys", None)
    if drive_spec:
        prof = profile_action_path(getattr(args, "action_profile", None))
        actions = {}
        if prof is not None and Path(prof).exists():
            try:
                actions = parse_action_profile(
                    Path(prof).read_text(encoding="utf-8", errors="replace"))
            except OSError:
                actions = {}
        drive_stats["profile"] = str(prof) if prof else None
        drive_stats["actions"] = actions
        try:
            steps = parse_drive_keys(drive_spec, actions)
        except ValueError as exc:
            print(f"[xrsim] key drive: ignoring --drive-keys {drive_spec!r}: {exc}")
            steps = []
        if steps:
            drive_stats.update(enabled=True, spec=str(drive_spec),
                               steps=[[s, h] for s, h in steps])
            gap_ms = float(getattr(args, "drive_gap_ms", DEFAULT_DRIVE_GAP_MS)
                           or 0)
            driver = KeyScriptDriver(
                lambda scan, down: post_scan_to_game_windows(game.pid, scan, down),
                steps, gap_s=gap_ms / 1000.0)
            print(f"[xrsim] key drive: {len(steps)} step(s) "
                  f"{[f'{s}:{h}ms' for s, h in steps]} "
                  f"profile={Path(prof).name if prof else '<none>'} (GH #67; "
                  f"scan codes from the action profile, targeted messaging)")

    # GH #80 wake-96: the engine resolves its input procs but does not poll them
    # until its own window is active. Activate it (window management only) and
    # let the OS say whether that succeeded; the result is measured, not assumed.
    window_stats = {"attempted": False, "pid": game.pid, "hwnd": None,
                    "title": None, "width": 0, "height": 0,
                    "was_foreground": None, "foreground": False,
                    "set_foreground_ok": None, "attached": False, "found": False,
                    "attempts": 0}
    win_deadline = time.time() + WINDOW_ACTIVATION_TIMEOUT_S
    win_next = time.time()

    duration = getattr(args, "duration", None)
    host_log = []
    start = time.time()
    while True:
        if host.poll() is not None or game.poll() is not None:
            break
        if not window_stats["foreground"] and time.time() >= win_next:
            win_next = time.time() + WINDOW_ACTIVATION_RETRY_S
            if time.time() <= win_deadline:
                observed = activate_game_window(game.pid)
                window_stats.update(observed)
                window_stats["attempts"] += 1
                if observed["found"] and window_stats["attempts"] == 1:
                    print("[xrsim] game window: hwnd={hwnd} title={title!r} "
                          "{width}x{height} foreground_before={was_foreground} "
                          "(GH #80; window activation only, no input)".format(**observed))
                if observed["foreground"]:
                    print(f"[xrsim] game window activated after "
                          f"{window_stats['attempts']} attempt(s): hwnd={observed['hwnd']} "
                          f"title={observed['title']!r}")
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
            if advance_stop_decision(display_branch_sequence(fresh_now),
                                     advancer.taps, menu_stats["min_taps"]):
                advancer.stop()
                menu_stats["reached"] = True
                drive_ready_at = min(drive_ready_at, elapsed + drive_start_after)
                print(f"[xrsim] menu advance: stopped after {advancer.taps} tap(s) "
                      "-- gameplay branch reached")
        # GH #67/#80: once in-level, drive the engine so it reaches an armed /
        # fighting state. With an advancer, wait --drive-after seconds past the
        # gameplay-reached stop (the level keeps loading after the last confirm);
        # without one, start after --drive-after seconds so a mis-set run does
        # not fire action keys into the splash/menu.
        if driver is not None and not drive_stats["done"] and elapsed >= drive_ready_at:
            drive_stats["started"] = True
            if driver.poll():
                drive_stats["posted"] = driver.posted
                drive_stats["windows"] = max(drive_stats["windows"],
                                             driver.windows)
                drive_stats["done"] = driver.done
                if driver.done:
                    print(f"[xrsim] key drive: script complete "
                          f"({driver.posted} messages to "
                          f"{driver.windows} window(s))")
        line = host.stdout.readline()
        if line:
            host_log.append(line.rstrip())
        else:
            time.sleep(0.03)

    # GH #87: the *game's* own exit (before the harness terminates it) is the
    # signal. A host-bounded/duration/timeout break leaves the game alive, so
    # `game_exited` is False and the attempt is never misread as an early exit.
    game_exit_code = game.poll()
    game_exited = game_exit_code is not None
    live_seconds = time.time() - game_start

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

    if driver is not None:
        drive_stats["posted"] = driver.posted
        drive_stats["windows"] = max(drive_stats["windows"], driver.windows)
        drive_stats["done"] = driver.done
        state = "started" if drive_stats["started"] else "not started"
        print(f"[xrsim] key drive summary: {state} posted={drive_stats['posted']} "
              f"windows={drive_stats['windows']} done={drive_stats['done']}")
    args.drive_stats = drive_stats

    if not window_stats["foreground"]:
        print("[xrsim] game window NOT foreground: hwnd={hwnd} found={found} "
              "attempts={attempts} set_foreground_ok={set_foreground_ok} "
              "(GH #80: the engine may skip device polls while inactive)".format(
                  **window_stats))

    frames = len(list(artifacts.glob("test_frame_*.bmp")))
    hook_fresh = bool(log_file.exists()) and (
        not hook_existed or log_file.stat().st_mtime_ns != hook_mtime)
    info = {"attempt": attempt, "game_pid": game.pid,
            "live_seconds": round(live_seconds, 2),
            "game_exited": game_exited, "game_exit_code": game_exit_code,
            "frames": frames, "hook_fresh": hook_fresh,
            "host_exited": host.poll() is not None}
    return "\n".join(host_log), log_mark, menu_stats, info, window_stats


def run_sim(args, artifacts: Path):
    """Launch host+game, retrying one early game exit (GH #87).

    Returns ``(host_log, log_mark, menu_stats, launch)``. ``launch`` records every
    attempt and whether the game actually started, so a downstream report can tell
    "the game never started" (not a measurement -> retried once) from "the game
    started and the feature is broken" (a real result).
    """
    min_live = float(getattr(args, "min_live_seconds", None)
                     or EARLY_EXIT_MIN_LIVE_SECONDS)
    max_attempts = max(1, int(getattr(args, "max_launch_attempts", None)
                              or LAUNCH_MAX_ATTEMPTS))
    attempts = []
    host_log, log_mark = "", 0
    menu_stats = {"taps": 0, "windows": 0, "reached": False, "enabled": False,
                  "vk": VK_RETURN, "scan": vk_to_scancode(VK_RETURN),
                  "min_taps": 0}
    window_stats = {"foreground": False}
    state, detail = "started", "game still live at run end"
    for i in range(1, max_attempts + 1):
        if i > 1:
            print(f"[xrsim] retrying launch (attempt {i}/{max_attempts}; GH #87)")
        host_log, log_mark, menu_stats, info, window_stats = _run_attempt(
            args, artifacts, i)
        state, detail = game_start_decision(
            info["game_exited"], info["live_seconds"], info["frames"],
            info["hook_fresh"], min_live)
        info["game_start_state"] = state
        info["detail"] = detail
        attempts.append(info)
        print(f"[xrsim] launch attempt {i}/{max_attempts}: {state} -- {detail}")
        if not launch_retry_decision(state, i, max_attempts):
            break
    launch = {"game_started": state == "started", "state": state,
              "attempts": attempts,
              "max_attempts": max_attempts, "min_live_seconds": min_live,
              "detail": detail, "summary": format_launch_summary(
                  {"game_started": state == "started", "state": state,
                   "attempts": attempts,
                   "max_attempts": max_attempts, "min_live_seconds": min_live})}
    return host_log, log_mark, menu_stats, launch, window_stats


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


def evaluate(args, artifacts: Path, host_log: str, fresh_log: str, launch=None):
    """Evaluate one run's evidence.

    ``host_log`` is the mock host's stdout (may be empty for --evaluate-only);
    ``fresh_log`` is the text of the hook log to scan for interaction markers.
    A check with ``"skip": True`` is reported but does not affect the verdict.
    ``launch`` (GH #87) is :func:`run_sim`'s attempt record; when it says the game
    never started, the capture checks are demoted to reported-not-graded and one
    distinct ``game_started`` failure stands in their place.
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
    probe_run = viewmodel_window_ran(probe)
    if probe["lines"]:
        opened = [p for p in probe["lines"] if p["inside"] > 0]
        max_inside = max((p["inside"] for p in probe["lines"]), default=0)
        detail = (f"{len(probe['lines'])} probe line(s), max in-window draws="
                  f"{max_inside}" + (", matrix sampled" if probe["matrix"] else ""))
        checks.append({"group": "viewmodel", "check": "viewmodel_pass_identified",
                       "ok": bool(opened), "detail": detail})
    elif probe_run["in_window_draws"] > 0:
        # The submission tallies are the ground truth for "the window opened":
        # an in-window draw exists even if the 300-swap summary line is absent
        # (a hand-crafted/partial log, or a run shorter than one sample).
        checks.append({"group": "viewmodel", "check": "viewmodel_pass_identified",
                       "ok": True,
                       "detail": (f"the probe ran ({probe_run['intervals']} interval(s)) and "
                                  f"counted {probe_run['in_window_draws']} in-window draw(s); "
                                  f"no swap summary line was captured")})
    elif probe_run["ran"]:
        # GH #67 step 1 (fifth correction, ADR-0487): the probe ran but the
        # configured opener never fired (no in-window draw in any interval).
        # That is a *failed* identification, not an absent probe -- e.g. the
        # measured NearPlaneDrop run (`arrayIn=0`, every projection load `P?`).
        checks.append({"group": "viewmodel", "check": "viewmodel_pass_identified",
                       "ok": False,
                       "detail": (f"the probe ran ({probe_run['intervals']} interval(s)) but "
                                  f"no draw fell inside the window (arrayIn=0): the "
                                  f"configured ViewmodelWindowOpener never opened it")})
    else:
        checks.append({"group": "viewmodel", "check": "viewmodel_pass_identified",
                       "ok": True, "skip": True,
                       "detail": "no ViewmodelProbe lines (ViewmodelObserve=0 or pre-#67 host)"})
    # GH #67 step 1 (ADR-0484/0487, unchanged): identifying the *window* is not
    # identifying the *viewmodel*. The last sampled interval's largest decodable
    # in-window draw must be a 3-D mesh; a planar sample is a screen-space
    # element (the measured target sample is min=(-0.450,-0.450,-0.050)
    # max=(0.450,0.450,0.000): dz/dx=0.056, a flat plane). Only graded when the
    # probe actually captured a first-person sample, so a run without
    # ViewmodelObserve=1 is unaffected.
    sub = probe.get("submission") or {}
    fp_sample = sub.get("fp")
    if fp_sample is not None:
        geo = fp_sample.get("geometry") or viewmodel_candidate_geometry(fp_sample)
        volumetric = bool(geo and geo["volumetric"])
        if geo:
            detail = (f"candidate extent={geo['extent']} "
                      f"thickness_ratio={geo['thickness_ratio']:.3f} "
                      f"(min {VIEWMODEL_CANDIDATE_MIN_THICKNESS_RATIO})@count="
                      f"{fp_sample.get('count')}")
        else:
            detail = "first-person sample carries no bounds"
        if not volumetric:
            detail += (" -- the in-window draw is planar/screen-space, so the "
                       "engine's first-person viewmodel pass was NOT identified")
        checks.append({"group": "viewmodel",
                       "check": "viewmodel_candidate_is_volumetric",
                       "ok": volumetric, "detail": detail})
    elif probe_run["ran"] and probe_run["in_window_draws"] == 0:
        # GH #67 step 1 (fifth correction, ADR-0487): the probe ran, the opener
        # never fired -> there is no in-window candidate at all. Grade that as a
        # failure so an inoperative opener cannot masquerade as "probe disabled".
        checks.append({"group": "viewmodel",
                       "check": "viewmodel_candidate_is_volumetric",
                       "ok": False,
                       "detail": ("the probe ran but the configured window opener never "
                                  "opened a window, so there is no in-window candidate "
                                  "to grade")})
    else:
        checks.append({"group": "viewmodel",
                       "check": "viewmodel_candidate_is_volumetric",
                       "ok": True, "skip": True,
                       "detail": "no first-person submission sample (ViewmodelObserve=0)"})
    # GH #67 step 1 (seventh correction, ADR-0489): the strict gate above is graded
    # on the *last* sampled interval (ADR-0484/0487 semantics, unchanged). That is
    # arbitrary -- a run samples one in-window draw per 300-swap interval and their
    # contents differ -- so the run also reports, as its own check, whether *any*
    # sampled interval's largest in-window draw was volumetric. Measured (wake 87):
    # the answer is yes for BOTH openers (1/18 intervals with the depth-only clear,
    # 1/3 with the near-plane drop, the same 14688-count mesh), so this is an
    # observation about the window's contents, NOT evidence that the window
    # isolates the arms. Graded whenever a sample exists so "no volumetric sample"
    # can never read as silence.
    best = viewmodel_candidate_best(sub.get("samples") or [])
    if best["sampled"]:
        pick = best["best"]
        geo = best["geometry"] or {}
        detail = (f"{best['volumetric']}/{best['sampled']} sampled interval(s) had a "
                  f"volumetric in-window draw; best count={pick.get('count')} "
                  f"extent={geo.get('extent')} ratio={geo.get('thickness_ratio')}")
        last = best["last"]
        if last is not None and last is not pick:
            lag = last.get("geometry") or {}
            detail += (f"; last interval count={last.get('count')} "
                       f"ratio={lag.get('thickness_ratio')}")
        checks.append({"group": "viewmodel",
                       "check": "viewmodel_volumetric_candidate_sampled",
                       "ok": best["volumetric"] > 0, "detail": detail})
    else:
        checks.append({"group": "viewmodel",
                       "check": "viewmodel_volumetric_candidate_sampled",
                       "ok": True, "skip": True,
                       "detail": "no first-person submission sample (ViewmodelObserve=0)"})

    # GH #67 step 1 (eighth correction, ADR-0490): the window *is not* what
    # isolates the arms -- two live runs measured the same 14688-count volumetric
    # mesh inside both openers (ADR-0489), so the run also tests a
    # **window-independent** discriminator: the interval's largest decodable
    # *near-field* array draw (label `near`, ADR-0490), regardless of the window,
    # tagged with whether the window happened to be open. The claim under test is
    # "the viewmodel *is* the near-field draw", so the check passes only when the
    # near-field candidate covers the intervals in which the window drew the
    # first-person pass AND is volumetric AND its logged bounds really are
    # near-field (the harness's own copy of the hook's constant). A pre-#0490 hook
    # has no `nearSample=` marker and is skipped, never failed.
    cov = viewmodel_near_field_coverage(probe)
    near_best = viewmodel_candidate_best(sub.get("samples") or [], "near")
    if not cov["supported"]:
        checks.append({"group": "viewmodel",
                       "check": "viewmodel_near_field_candidate_identifies_pass",
                       "ok": True, "skip": True,
                       "detail": ("no `nearSample=` marker (pre-#0490 hook or "
                                  "ViewmodelObserve=0)")})
    else:
        pick = near_best["best"]
        geo = near_best["geometry"] or {}
        near_field = viewmodel_sample_is_near_field(pick)
        volumetric = bool(geo.get("volumetric"))
        covers = cov["in_window_intervals"] > 0 and cov["near_intervals"] >= cov["in_window_intervals"]
        inside = sum(1 for s in (sub.get("samples") or [])
                     if s.get("label") == "near" and s.get("inside_window") == 1)
        detail = (f"{cov['near_intervals']}/{cov['in_window_intervals']} in-window interval(s) also "
                  f"produced a near-field candidate ({cov['intervals']} sampled interval(s); "
                  f"{inside}/{near_best['sampled']} near sample(s) landed inside the window); "
                  f"best count={pick.get('count') if pick else None} "
                  f"extent={geo.get('extent')} ratio={geo.get('thickness_ratio')}")
        if not covers:
            detail += (" -- the window drew in more intervals than the window-independent "
                       "near-field test found a candidate for, so 'the viewmodel is the "
                       "near-field draw' is NOT supported on this target")
        if near_best["sampled"] and not volumetric:
            detail += " -- the largest near-field draw is planar/screen-space"
        if near_best["sampled"] and not near_field:
            detail += (f" -- a near sample's logged bounds exceed the near-field bound "
                       f"(+/-{VIEWMODEL_NEAR_FIELD_MAX_ABS}), so the hook's threshold "
                       f"disagrees with the harness")
        checks.append({"group": "viewmodel",
                       "check": "viewmodel_near_field_candidate_identifies_pass",
                       "ok": covers and volumetric and near_field, "detail": detail})

    # GH #67 step 1 (ninth correction, ADR-0491): the *mechanism* census. Every
    # prior correction sampled only `glDrawElements` (client arrays) and
    # `glCallList` (display lists), but the shipped 2009 RndrGL.dll also **imports
    # the immediate-mode path** (measured: `glBegin`/`glEnd`/`glVertex3f`/
    # `glVertex3fv`/`glVertex2f`/`glVertex2i` among its 96 static OPENGL32 imports)
    # and the reference's own open question was "do the first-person arms use
    # immediate mode?". A mesh submitted with `glBegin(..) { glVertex3f(..) }
    # glEnd()` produces **no** `glDrawElements` call, so no array sample could ever
    # be the arms. This grades the census: PASS when the window held 3-D
    # immediate-mode vertices (the arms *could* be that geometry), FAIL when the
    # census ran and the window held none (the hypothesis is falsified for this
    # target, and the arms must be arrays/lists/absent). A pre-#0491 hook has no
    # census line and is SKIPPED, never failed.
    imm = viewmodel_immediate_census(probe)
    imm_samples = list(sub.get("samples") or [])
    if imm["fpimm"] and not any(s.get("label") == "fpimm" for s in imm_samples):
        # A log whose submission-interval line is absent (a short run) still carries
        # the immediate candidate, so grade it rather than silently skipping.
        imm_samples.append(imm["fpimm"])
    imm_best = viewmodel_candidate_best(imm_samples, "fpimm")
    if not imm["ran"]:
        checks.append({"group": "viewmodel",
                       "check": "viewmodel_in_window_immediate_geometry",
                       "ok": True, "skip": True,
                       "detail": ("no `ViewmodelProbe immediate` line (pre-#0491 hook "
                                  "or ViewmodelObserve=0)")})
        checks.append({"group": "viewmodel",
                       "check": "viewmodel_immediate_candidate_is_volumetric",
                       "ok": True, "skip": True,
                       "detail": ("no immediate-mode census (pre-#0491 hook or "
                                  "ViewmodelObserve=0)")})
    else:
        v3 = imm["vertex3_inside"]
        detail = (f"{imm['intervals']} sampled interval(s): in-window blocks="
                  f"{imm['began_inside']} (3-D vertices v3In={v3}, 2-D v2In="
                  f"{imm['vertex2_inside']}); outside blocks={imm['began_outside']} "
                  f"(v3Out={imm['vertex3_outside']} v2Out={imm['vertex2_outside']}); "
                  f"{imm['sampled_intervals']} interval(s) kept a block sample")
        if v3 == 0:
            detail += (" -- the first-person window held NO 3-D immediate-mode "
                       "vertices, so 'the arms are submitted with glBegin..glEnd' is "
                       "NOT supported on this target")
        checks.append({"group": "viewmodel",
                       "check": "viewmodel_in_window_immediate_geometry",
                       "ok": v3 > 0, "detail": detail})
        if imm_best["sampled"]:
            geo = imm_best["geometry"] or {}
            pick = imm_best["best"]
            vol_detail = (f"largest in-window block count={pick.get('count')} "
                          f"extent={geo.get('extent')} "
                          f"thickness_ratio={geo.get('thickness_ratio')}")
            if not geo.get("volumetric"):
                vol_detail += (" -- the window's largest immediate block is "
                               "planar/screen-space, so it is not the arms mesh")
            checks.append({"group": "viewmodel",
                           "check": "viewmodel_immediate_candidate_is_volumetric",
                           "ok": imm_best["volumetric"] > 0, "detail": vol_detail})
        else:
            checks.append({"group": "viewmodel",
                           "check": "viewmodel_immediate_candidate_is_volumetric",
                           "ok": True, "skip": True,
                           "detail": ("the census ran but no sample interval held an "
                                      "in-window glBegin..glEnd block, so there is no "
                                      "immediate candidate to grade")})

    # GH #67 step 1 (sixth correction, ADR-0488): the near-plane extraction the
    # NearPlaneDrop opener depends on. The pass trace read `P?` on every
    # projection load because the classic `m[14]/(m[10]-1)` returns -far on the
    # engine's z-flipped matrix (measured), so `worldNear_` was never set. Graded
    # only when the run carries the projection record (#0488+ hook with
    # ViewmodelObserve=1); a pre-#0488 hook has no such line and is skipped.
    proj = viewmodel_projection_stats(fresh_log)
    if proj["lines"]:
        first = proj["lines"][0]
        if proj["perspective"] == 0:
            checks.append({"group": "viewmodel", "check": "projection_near_extracted",
                           "ok": False,
                           "detail": (f"{len(proj['lines'])} projection record(s) but none "
                                      "classified perspective")})
        else:
            ok = proj["near_known"] > 0
            detail = (f"{proj['near_known']}/{proj['perspective']} perspective load(s) "
                      f"with a near plane, {proj['distinct']} distinct matrix/matrices; "
                      f"first m10={first['m10']:.5f} m11={first['m11']:.3f} "
                      f"m14={first['m14']:.5f} classic={first['classic']:.3f} "
                      f"near={first['near']:.4f}")
            if not ok:
                detail += (" -- the extraction still rejects the engine's matrix, so the "
                           "NearPlaneDrop opener cannot fire")
            checks.append({"group": "viewmodel", "check": "projection_near_extracted",
                           "ok": ok, "detail": detail})
    else:
        checks.append({"group": "viewmodel", "check": "projection_near_extracted",
                       "ok": True, "skip": True,
                       "detail": ("no ViewmodelProbe proj lines (pre-#0488 hook or "
                                  "ViewmodelObserve=0)")})
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
    # GH #87 (+ extension): is this attempt a measurement at all? Two signatures
    # are not: the game ending within a couple of seconds of launch (no hook log,
    # no frame), and the game staying live the whole window with no hook log and no
    # frame (the hook never attached). Either way the capture FAILs are demoted to
    # reported-not-graded and one distinct `game_started` failure stands in their
    # place, so "nothing was measured" can never read as "the fix is broken".
    launch = launch if launch is not None else getattr(args, "launch_stats", None)
    if launch is not None:
        started = bool(launch.get("game_started", True))
        rec = {"group": "launch", "check": "game_started", "ok": started,
               "state": launch.get("state"),
               "detail": format_launch_summary(launch)}
        if not started:
            rec["demoted_checks"] = demote_checks_for_no_game_start(checks)
        checks.append(rec)
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

    # GH #83: the live-run lock is a pure decision over (lock contents, liveness)
    # and the shadow dir is per run, so both are checkable without launching
    # anything -- and the takeover path proves a killed run cannot wedge the loop.
    results["live_lock_free_held_stale_owned"] = (
        live_lock_decision(None, lambda _p: True, 1) == ("free", None) and
        live_lock_decision({}, lambda _p: True, 1) == ("free", None) and
        live_lock_decision({"pid": "x"}, lambda _p: True, 1) == ("free", None) and
        live_lock_decision({"pid": 1}, lambda _p: True, 1) == ("owned", 1) and
        live_lock_decision({"pid": 2}, lambda _p: True, 1) == ("held", 2) and
        live_lock_decision({"pid": 2}, lambda _p: False, 1) == ("stale", 2))
    results["shadow_dir_is_per_run"] = (
        default_shadow_dir(11) != default_shadow_dir(22) and
        default_shadow_dir(11).name == "riddickvr-shadow-11")
    import tempfile as _tempfile  # the image block below rebinds `tempfile` locally
    with _tempfile.TemporaryDirectory() as lock_dir:
        lk = Path(lock_dir) / DEFAULT_LIVE_LOCK_NAME
        ok_l, s_l, _ = acquire_live_lock(lk, 101, is_alive=lambda _p: False)
        held, s_h, _ = acquire_live_lock(lk, 202, is_alive=lambda p: p == 101)
        ok_t, s_t, _ = acquire_live_lock(lk, 202, is_alive=lambda _p: False)
        results["live_lock_round_trip"] = (
            ok_l and s_l == "free" and not held and s_h == "held" and
            ok_t and s_t == "stale" and
            release_live_lock(lk, 101) is False and
            release_live_lock(lk, 202) is True and not lk.exists())

    # GH #87 + extension: an attempt with no evidence is not a measurement -> it is
    # retried once. Two signatures have no evidence: the game ending early (an early
    # exit) and the game staying live the whole window with the hook never attached
    # (no_evidence). Any evidence (a frame or a fresh hook log) is a real result and
    # must never be retried, and the retry is bounded.
    results["game_start_decision_states"] = (
        game_start_decision(True, 2.0, 0, False)[0] == "early_exit" and
        game_start_decision(True, 2.0, 1, False)[0] == "started" and
        game_start_decision(True, 2.0, 0, True)[0] == "started" and
        game_start_decision(True, 30.0, 0, False)[0] == "no_evidence" and
        game_start_decision(False, 30.0, 0, False)[0] == "no_evidence" and
        game_start_decision(False, 2.0, 0, False)[0] == "started")
    results["launch_retry_bounds_early_exit"] = (
        launch_retry_decision("early_exit", 1, 2) is True and
        launch_retry_decision("no_evidence", 1, 2) is True and
        launch_retry_decision("no_evidence", 2, 2) is False and
        launch_retry_decision("early_exit", 2, 2) is False and
        launch_retry_decision("started", 1, 2) is False)

    # GH #71 req 1 (wake 84): the confirm tap must pack the key's *scan code*
    # where the engine's window proc reads it (`lParam >> 16`). 0x100 is the
    # value that makes the engine call MapVirtualKeyA itself, so it must never
    # be sent as the scan code; the pre-wake-84 tap sent 0 (an unmapped key).
    down, char, up = confirm_key_messages()
    results["confirm_tap_packs_scancode"] = (
        vk_to_scancode(VK_RETURN) == 0x1C and
        all(((m[2] >> 16) & 0x1FF) == 0x1C for m in (down, char, up)) and
        ((down[2] >> 16) & 0x1FF) != 0 and
        ((down[2] >> 16) & 0x1FF) != KEY_LPARAM_NO_SCANCODE and
        key_lparam(0, True) == 1)

    # GH #71 req 1 (wake 85): the tap-count floor. The engine emits its own
    # startup transition as a cinema->full-bleed pair (frame 1, measured), so the
    # branch signal alone reads the startup page as gameplay and stops the advance
    # before it is attempted. The stop must wait for at least
    # ADVANCE_STOP_MIN_TAPS taps -- and that floor must exceed the 5-6 taps wake 84
    # measured as the point the start card is actually left.
    startup_pair = display_branch_sequence(
        "DisplayMode: CINEMA screen (menus/cinematics) (3Dfresh=0 menuCursor=1)\n"
        "DisplayMode: FULL-BLEED gameplay (3Dfresh=1 menuCursor=0)\n")
    results["advance_stop_requires_delivered_taps"] = (
        startup_pair == [True, False] and
        gameplay_reached_from_sequence(startup_pair) is True and
        advance_stop_decision(startup_pair, ADVANCE_STOP_MIN_TAPS - 1) is False and
        advance_stop_decision(startup_pair, ADVANCE_STOP_MIN_TAPS) is True and
        advance_stop_decision(startup_pair, ADVANCE_STOP_MIN_TAPS, 0) is True and
        advance_stop_decision([False], 99) is False and
        advance_stop_decision([], 99) is False and
        ADVANCE_STOP_MIN_TAPS >= 6)

    # GH #80 wake 96: the game-window activation target. The engine's render
    # window is the largest *visible* top-level window owned by its pid; an
    # invisible splash/helper window or another process's window must never be
    # selected, and the choice must be deterministic (largest area, then hwnd).
    wins = [(10, 4242, True, 1280, 605, "Riddick"),
            (11, 4242, False, 4000, 4000, "hidden helper"),
            (12, 9999, True, 5000, 5000, "other process"),
            (13, 4242, True, 320, 240, "small tooltip"),
            (14, 4242, True, 1280, 605, "Riddick (second)")]
    chosen = pick_game_window(wins, 4242)
    results["game_window_activation_target"] = (
        chosen == (14, 4242, True, 1280, 605, "Riddick (second)") and
        pick_game_window(wins, 9999) == (12, 9999, True, 5000, 5000,
                                         "other process") and
        pick_game_window([(10, 1, False, 800, 600, "hidden")], 1) is None and
        pick_game_window([], 1) is None and
        pick_game_window([(10, 1, True, 0, 600, "zero")], 1) is None)

    # GH #67 live tuning: `--ini` sets a hook flag in the *shadow's* riddick_vr.ini
    # (the file the hook resolves beside its module) so a deploy-free run can flip
    # it. The parser is pure and the merge is section/case-aware + idempotent -- the
    # two properties that make "this run used ViewmodelObserve=1" attributable.
    import shadow_install as _shadow
    parsed = _shadow.parse_ini_overrides(["ViewmodelObserve=1", "A= b=c"])
    rejected = False
    try:
        _shadow.parse_ini_overrides(["noequals"])
    except ValueError:
        rejected = True
    base_ini = "[VR]\nViewmodelObserve=0\nWeird=1\n[Other]\nViewmodelObserve=9\n"
    merged = _shadow.merge_ini(base_ini, {"ViewmodelObserve": "1", "A": "2"})
    results["ini_override_parse_and_merge"] = (
        parsed == {"ViewmodelObserve": "1", "A": " b=c"} and rejected and
        merged.count("ViewmodelObserve=1") == 1 and
        merged.count("ViewmodelObserve=9") == 1 and      # other section untouched
        "A=2" in merged and                              # missing key inserted
        _shadow.merge_ini(merged, {"ViewmodelObserve": "1", "A": "2"}) == merged)

    # GH #67 step 1 (second/third corrections, ADR-0482/0483): the submitted-vertex
    # parser must split array vs display-list draws by window side and decode the
    # sampled geometry -- the datum that decides where the transform actually is --
    # including the draw's index count (a quad vs a real mesh, ADR-0483).
    probe_text = (
        "ViewmodelProbe submission [#300]: arrayIn=3 arrayOut=40 listIn=1 listOut=12 "
        "ptrSize=3 ptrType=3590 ptrStride=0 inSample=1 outSample=1\n"
        "ViewmodelProbe submission fp: first=(0.250,-0.100,0.400) min=(-0.100,-0.200,0.300) "
        "max=(0.300,0.100,0.500) n=24 count=384 indexType=5121 vertexType=5126 valid=1\n"
        "ViewmodelProbe submission world: first=(100.000,2.000,-40.000) min=(90.000,0.000,-60.000) "
        "max=(140.000,8.000,-10.000) n=24 count=6 indexType=5121 vertexType=5126 valid=1\n")
    sub = viewmodel_probe_stats(probe_text)["submission"]
    results["viewmodel_submission_parsed"] = (
        sub is not None and sub["array_inside"] == 3 and sub["list_outside"] == 12 and
        sub["fp"]["first"] == [0.25, -0.1, 0.4] and sub["world"]["max"][0] == 140.0 and
        sub["fp"]["valid"] is True and sub["fp"]["count"] == 384 and sub["world"]["count"] == 6)

    # GH #67 step 1 (ADR-0484): identifying the *window* is not identifying the
    # *viewmodel*. The sample above is a real 3-D mesh (extent 0.4 x 0.3 x 0.2,
    # ratio 0.5) -> volumetric. The measured 2009-target sample is a plane
    # (min=(-0.450,-0.450,-0.050) max=(0.450,0.450,0.000): dz/dx=0.056) -> not.
    plane = {"min": [-0.450, -0.450, -0.050], "max": [0.450, 0.450, 0.000],
             "valid": True}
    box = {"min": [-0.100, -0.200, 0.300], "max": [0.300, 0.100, 0.500],
           "valid": True}
    invalid = {"min": [-0.1, -0.1, -0.1], "max": [0.1, 0.1, 0.1], "valid": False}
    results["viewmodel_candidate_geometry_planar_vs_volume"] = (
        viewmodel_candidate_geometry(None) is None and
        viewmodel_candidate_geometry({"min": [0, 0, 0]}) is None and
        viewmodel_candidate_geometry(plane)["volumetric"] is False and
        abs(viewmodel_candidate_geometry(box)["thickness_ratio"] - 0.5) < 1e-9 and
        viewmodel_candidate_geometry(box)["volumetric"] is True and
        viewmodel_candidate_geometry(invalid)["volumetric"] is False and
        viewmodel_candidate_geometry(plane)["extent"] == [0.9, 0.9, 0.05])

    # GH #67 step 1 (fifth correction, ADR-0487): a probe that ran but whose
    # opener never fired (submission tallies present, arrayIn=0) must be *visible*
    # as a ran-probe, not reported as "ViewmodelObserve=0 / pre-#67 host".
    never = ("ViewmodelProbe submission [#300]: arrayIn=0 arrayOut=900 listIn=0 "
             "listOut=0 ptrSize=3 ptrType=5126 ptrStride=0 inSample=0 outSample=1\n")
    ran = viewmodel_window_ran(viewmodel_probe_stats(never))
    absent = viewmodel_window_ran(viewmodel_probe_stats(""))
    results["viewmodel_opener_never_fired_is_visible"] = (
        ran["ran"] is True and ran["intervals"] == 1 and ran["in_window_draws"] == 0 and
        absent["ran"] is False and absent["intervals"] == 0)

    # GH #67 step 1 (sixth correction, ADR-0488): the near-plane extraction. The
    # engine's projection is z-flipped (m11 > 0), so the classic `m14/(m10-1)`
    # reads -far; the shipped hook must report a real near plane for both the
    # world pass (4.0) and the sampled near-plane-drop pass (0.1).
    proj_text = (
        "ViewmodelProbe proj [#300]: m0=0.63021 m5=-1.33333 m10=1.00391 "
        "m11=1.00000 m14=-8.01566 m15=0.00000 persp=1 classic=-2048.007812 "
        "near=4.000000\n"
        "ViewmodelProbe proj [#600]: m0=0.57748 m5=-1.22177 m10=1.00010 "
        "m11=1.00000 m14=-0.20001 m15=0.00000 persp=1 classic=-2048.600098 "
        "near=0.100000\n")
    ps = viewmodel_projection_stats(proj_text)
    results["viewmodel_projection_near_extracted"] = (
        ps["perspective"] == 2 and ps["near_known"] == 2 and ps["distinct"] == 2 and
        abs(ps["lines"][0]["near"] - 4.0) < 1e-6 and
        ps["lines"][0]["classic"] < 0.0 and
        abs(ps["lines"][1]["near"] - 0.1) < 1e-6)
    results["viewmodel_projection_absent_has_no_lines"] = (
        viewmodel_projection_stats("")["lines"] == [] and
        viewmodel_projection_stats("")["near_known"] == 0 and
        viewmodel_projection_stats("")["perspective"] == 0)
    # A rejected load (near=0) must not count as a known near plane, so the graded
    # check can fail an inoperative extraction rather than pass it silently.
    rejected = viewmodel_projection_stats(
        "ViewmodelProbe proj [#300]: m0=0.63021 m5=-1.33333 m10=1.00391 "
        "m11=1.00000 m14=-8.01566 m15=0.00000 persp=1 classic=-2048.007812 "
        "near=0.000000\n")
    results["viewmodel_projection_rejected_load_is_not_known"] = (
        rejected["perspective"] == 1 and rejected["near_known"] == 0)

    # GH #67 step 1 (seventh correction, ADR-0489): the shape verdict must be the
    # run's *best* sampled interval, not its last -- a run samples one draw per
    # interval and their contents differ (measured: one run sampled a 14688-count
    # volumetric mesh AND a 2520-count plane).
    mixed = viewmodel_probe_stats(
        "[INFO] ViewmodelProbe submission [#1500]: arrayIn=1322 arrayOut=27180 "
        "listIn=0 listOut=0 ptrSize=3 ptrType=5126 ptrStride=0 inSample=1 outSample=1\n"
        "[INFO] ViewmodelProbe submission fp: first=(0.450,-0.400,0.050) "
        "min=(-0.400,-0.445,0.000) max=(0.450,0.400,0.300) n=24 count=14688 "
        "indexType=5123 vertexType=5126 valid=1\n"
        "[INFO] ViewmodelProbe submission [#1800]: arrayIn=650 arrayOut=33513 "
        "listIn=0 listOut=0 ptrSize=3 ptrType=5126 ptrStride=0 inSample=1 outSample=1\n"
        "[INFO] ViewmodelProbe submission fp: first=(0.400,-0.400,0.000) "
        "min=(-0.450,-0.450,-0.050) max=(0.450,0.450,0.000) n=24 count=2520 "
        "indexType=5123 vertexType=5126 valid=1\n")
    best = viewmodel_candidate_best(mixed["submission"]["samples"])
    results["viewmodel_candidate_best_is_volumetric"] = (
        best["sampled"] == 2 and best["volumetric"] == 1 and
        best["best"]["count"] == 14688 and best["geometry"]["volumetric"] is True and
        best["last"]["count"] == 2520 and
        # The back-compatible per-label slot still holds the LAST sample.
        mixed["submission"]["fp"]["count"] == 2520 and
        len(mixed["submission"]["samples"]) == 2)
    planar_only = viewmodel_probe_stats(
        "[INFO] ViewmodelProbe submission [#300]: arrayIn=1 arrayOut=9 listIn=0 "
        "listOut=0 ptrSize=3 ptrType=5126 ptrStride=0 inSample=1 outSample=0\n"
        "[INFO] ViewmodelProbe submission fp: first=(0.400,-0.400,0.000) "
        "min=(-0.450,-0.450,-0.050) max=(0.450,0.450,0.000) n=24 count=2520 "
        "indexType=5123 vertexType=5126 valid=1\n")["submission"]["samples"]
    results["viewmodel_candidate_best_falls_back_to_last"] = (
        viewmodel_candidate_best(planar_only)["best"]["count"] == 2520 and
        viewmodel_candidate_best(planar_only)["volumetric"] == 0 and
        viewmodel_candidate_best([])["sampled"] == 0 and
        viewmodel_candidate_best([])["best"] is None)

    # GH #67 step 1 (eighth correction, ADR-0490): the window-independent near-field
    # candidate. The `near` label is parsed like `fp`/`world` plus its ` inside=`
    # tag, the near-field predicate mirrors the hook's constant, the best-of-run
    # selection is per-label (so the `fp` and `near` streams never mix), and the
    # `nearSample=` marker on the interval line separates a #0490+ hook from a
    # pre-0490 one (which must SKIP, not fail).
    near_text = (
        "[INFO] ViewmodelProbe submission [#300]: arrayIn=1 arrayOut=9 listIn=0 "
        "listOut=0 ptrSize=3 ptrType=5126 ptrStride=0 inSample=1 outSample=0 nearSample=1\n"
        "[INFO] ViewmodelProbe submission fp: first=(0.400,-0.400,0.000) "
        "min=(-0.450,-0.450,-0.050) max=(0.450,0.450,0.000) n=24 count=2520 "
        "indexType=5123 vertexType=5126 valid=1\n"
        "[INFO] ViewmodelProbe submission near: first=(0.450,-0.400,0.050) "
        "min=(-0.400,-0.445,0.000) max=(0.450,0.400,0.300) n=24 count=14688 "
        "indexType=5123 vertexType=5126 valid=1 inside=0\n"
        "[INFO] ViewmodelProbe submission [#600]: arrayIn=1 arrayOut=9 listIn=0 "
        "listOut=0 ptrSize=3 ptrType=5126 ptrStride=0 inSample=1 outSample=0 nearSample=0\n"
        "[INFO] ViewmodelProbe submission fp: first=(0.400,-0.400,0.000) "
        "min=(-0.450,-0.450,-0.050) max=(0.450,0.450,0.000) n=24 count=2520 "
        "indexType=5123 vertexType=5126 valid=1\n")
    nprobe = viewmodel_probe_stats(near_text)
    nsub = nprobe["submission"]
    nbest = viewmodel_candidate_best(nsub["samples"], "near")
    fbest = viewmodel_candidate_best(nsub["samples"], "fp")
    ncov = viewmodel_near_field_coverage(nprobe)
    results["viewmodel_near_field_sample_is_parsed_and_graded"] = (
        nsub["near"]["count"] == 14688 and nsub["near"]["inside_window"] == 0 and
        nbest["sampled"] == 1 and nbest["volumetric"] == 1 and
        nbest["best"]["count"] == 14688 and nbest["best"]["inside_window"] == 0 and
        # The two streams are selected independently (fp has two samples here).
        fbest["sampled"] == 2 and fbest["best"]["count"] == 2520 and
        viewmodel_sample_is_near_field(nbest["best"]) is True)
    # A #0490 hook covers the run only when the near-field test found a candidate in
    # every interval in which the window drew (measured: 3/15 on the live level, so
    # the claim is NOT supported); a pre-#0490 log has no marker at all.
    results["viewmodel_near_field_coverage_is_graded"] = (
        ncov == {"supported": True, "intervals": 2, "in_window_intervals": 2,
                 "near_intervals": 1} and
        viewmodel_near_field_coverage(viewmodel_probe_stats(""))["supported"] is False and
        viewmodel_near_field_coverage(viewmodel_probe_stats(
            "[INFO] ViewmodelProbe submission [#300]: arrayIn=1 arrayOut=9 listIn=0 "
            "listOut=0 ptrSize=3 ptrType=5126 ptrStride=0 inSample=1 outSample=0\n")
        )["supported"] is False)

    # A sample whose logged bounds exceed the near-field bound is rejected by the
    # predicate (the harness's copy of the hook's constant), so a disagreeing hook
    # cannot pass the graded check.
    far = {"min": [-3518.739, -1663.154, 2032.0], "max": [3518.739, 1663.154, 2032.0],
           "valid": True}
    results["viewmodel_near_field_bound_is_enforced"] = (
        viewmodel_sample_is_near_field(far) is False and
        viewmodel_sample_is_near_field({"min": [0.0, 0.0, 0.0], "max": [0.0, 0.0, 0.0],
                                        "valid": False}) is False and
        viewmodel_sample_is_near_field(None) is False and
        viewmodel_sample_is_near_field({"min": [-16.0, 0.0, 0.0], "max": [16.0, 16.0, 16.0],
                                        "valid": True}) is True)

    # GH #67 step 1 (ninth correction, ADR-0491): the immediate-mode census -- the
    # submission path (glBegin/glEnd + glVertex3f/glVertex2f) the shipped module
    # imports and no previous correction counted. The census line is parsed into the
    # probe, the window's largest block parses as the `fpimm` sample with the same
    # shape grade as `fp`/`near`, and a pre-#0491 log reports the census absent
    # (`ran` False) rather than zeros.
    imm_text = (
        "[INFO] ViewmodelProbe submission [#300]: arrayIn=0 arrayOut=900 listIn=0 "
        "listOut=0 ptrSize=3 ptrType=5126 ptrStride=0 inSample=0 outSample=1 "
        "nearSample=0\n"
        "[INFO] ViewmodelProbe immediate [#300]: beganIn=2 beganOut=5 endIn=2 endOut=5 "
        "v3In=122 v3Out=900 v2In=0 v2Out=0 openIn=0 sample=1\n"
        "[INFO] ViewmodelProbe submission fpimm: first=(0.100,0.200,0.050) "
        "min=(-0.400,-0.450,0.000) max=(0.450,0.400,0.300) n=120 count=120 "
        "indexType=0 vertexType=5126 valid=1\n"
        "[INFO] ViewmodelProbe immediate [#600]: beganIn=0 beganOut=4 endIn=0 endOut=4 "
        "v3In=0 v3Out=700 v2In=0 v2Out=0 openIn=0 sample=0\n")
    iprobe = viewmodel_probe_stats(imm_text)
    ic = viewmodel_immediate_census(iprobe)
    ibest = viewmodel_candidate_best(iprobe["submission"]["samples"], "fpimm")
    results["viewmodel_immediate_census_is_parsed"] = (
        ic["ran"] is True and ic["intervals"] == 2 and ic["began_inside"] == 2 and
        ic["began_outside"] == 9 and ic["vertex3_inside"] == 122 and
        ic["vertex3_outside"] == 1600 and ic["sampled_intervals"] == 1 and
        iprobe["submission"]["fpimm"]["count"] == 120 and
        ibest["sampled"] == 1 and ibest["volumetric"] == 1 and
        ibest["best"]["geometry"]["volumetric"] is True and
        viewmodel_immediate_census(viewmodel_probe_stats(""))["ran"] is False and
        viewmodel_immediate_census(viewmodel_probe_stats(""))["vertex3_inside"] == 0)

    # GH #67: the in-level key drive is a pure schedule over the profile's own
    # scan codes -- the parser resolves action names, and the driver posts each
    # step's key-down then its key-up (ordered, bounded, one edge per poll).
    profile_text = ("*ACTION_FORWARD 17\n*ACTION_USE 18\n"
                    "*ACTION_WEAPON_SHIV 3\n*ACTION_PRIMARYFIRE 240\n"
                    "*ACTION_JUMP 57\n")
    acts = parse_action_profile(profile_text)
    keys = parse_drive_keys("WEAPON_SHIV,PRIMARYFIRE:80,17:1500", acts)
    results["drive_keys_parse_profile"] = (
        acts.get("FORWARD") == 17 and acts.get("PRIMARYFIRE") == 240 and
        acts.get("WEAPON_SHIV") == 3 and
        keys == [(3, DEFAULT_DRIVE_HOLD_MS), (240, 80), (17, 1500)])
    posted = []
    drv = KeyScriptDriver(lambda scan, down: posted.append((scan, down)) or 1,
                          keys, gap_s=0.0, start=0.0)
    t = 0.0
    for _ in range(200):
        drv.poll(now=t)
        if drv.done:
            break
        t += 0.05
    results["drive_keys_driver_orders_edges"] = (
        drv.done and
        posted == [(3, True), (3, False), (240, True), (240, False),
                   (17, True), (17, False)] and drv.windows == 1 and
        parse_drive_keys("", acts) == [] and
        scan_key_message(240, True) == (WM_KEYDOWN, 0, key_lparam(240, True)))

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
                        "viewmodel_projection": viewmodel_projection_stats(fresh),
                        "eye_frames": eye_frame_stats(artifacts),
                        "vertical_disparity": vertical_disparity_stats(checks),
                        "stereo_kind": stereo_kind_stats(checks),
                        "comfort_vignette": parse_comfort(host_text),
                        "deployed_hook": deployed_hook_identity(
                            game_dir, getattr(args, "built_hook", None) or BUILT_HOOK),
                        "input_bridge": parse_input_bridge(fresh),
                        "window_activation": None,
                        "launch": None,
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
                         "window messages, never OS-global input. OFF by default "
                         "(opt-in until a run measures it). The tap packs the real "
                         "scan code, as the engine's window proc reads it from lParam "
                         "(see the module docstring / docs/rea/reference/"
                         "RndrGL_Input_Path.txt)")
    ap.add_argument("--no-advance-menu", dest="advance_menu", action="store_false",
                    help="never post game-window input (the default; GH #71)")
    ap.set_defaults(advance_menu=False)
    ap.add_argument("--advance-interval", type=float, default=2.0,
                    help="seconds between confirm taps while the splash/menu is up "
                         "(default 2.0; only with --advance-menu)")
    ap.add_argument("--advance-max-taps", type=int, default=0,
                    help="stop confirming after N taps (0 = bounded only by the run)")
    ap.add_argument("--advance-min-taps", type=int, default=ADVANCE_STOP_MIN_TAPS,
                    help="deliver at least N confirm taps before the gameplay-branch "
                         "stop may fire (GH #71 req 1: the engine's own startup "
                         "transition already emits a cinema->full-bleed pair, so a "
                         "bare branch check stops the advance before it is attempted; "
                         "default 8, 0 restores the pre-wake-85 branch-only stop)")
    ap.add_argument("--drive-keys", default=None, metavar="SPEC",
                    help="GH #67: once gameplay is reached, post this scan-code "
                         "script to the game window (targeted messaging, never "
                         "desktop input) to reach an armed/fighting state. "
                         "Comma-separated steps, each SCAN or SCAN:HOLD_MS or an "
                         "action name from the player profile (FORWARD, "
                         "PRIMARYFIRE, WEAPON_SHIV, ...). Example: "
                         "\"WEAPON_PISTOL,PRIMARYFIRE,PRIMARYFIRE,FORWARD:2000\"")
    ap.add_argument("--drive-gap-ms", type=int, default=DEFAULT_DRIVE_GAP_MS,
                    help=f"gap between --drive-keys steps (default {DEFAULT_DRIVE_GAP_MS})")
    ap.add_argument("--drive-after", type=float, default=12.0,
                    help="seconds before --drive-keys starts when there is no "
                         "--advance-menu to gate on (default 12)")
    ap.add_argument("--action-profile", default=None, metavar="PATH",
                    help="player profile that maps actions to scan codes "
                         "(default: the first _profile under the Starbreeze "
                         "Riddick Content Save folder in AppData)")
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
                         "(default %%TEMP%%\\riddickvr-shadow-<pid>, one per run "
                         "-- GH #83)")
    ap.add_argument("--ini", action="append", default=[], metavar="KEY=VALUE",
                    help="set a hook config key in the *shadow's* program-dir "
                         "riddick_vr.ini (the file the hook resolves beside its own "
                         "module), so a live run can flip a flag without deploying; "
                         "requires --shadow-install and never writes the game "
                         "install. Repeatable, e.g. --ini ViewmodelObserve=1 "
                         "--ini ViewmodelTrace=1 (GH #67 live tuning)")
    ap.add_argument("--ignore-live-lock", dest="ignore_live_lock",
                    action="store_true",
                    help="do not take the single-live-run lock (GH #83). Only for "
                         "a deliberate parallel probe: two live runs collide on the "
                         "shared dist\\ binaries and the game window")
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
    ap.add_argument("--min-live-seconds", type=float,
                    default=EARLY_EXIT_MIN_LIVE_SECONDS,
                    help="GH #87: an attempt with no hook log and no captured "
                         "frame that either ends on its own before this many "
                         "seconds (an early exit) or stays live at least this long "
                         "with the hook never attached (no evidence) is not a "
                         "measurement; it is retried once and, if it repeats, "
                         "reported as a distinct game_started failure "
                         f"(default {EARLY_EXIT_MIN_LIVE_SECONDS:g})")
    ap.add_argument("--max-launch-attempts", type=int, default=LAUNCH_MAX_ATTEMPTS,
                    help="GH #87: host+game launches per run; an attempt that "
                         f"measured nothing (early exit / no evidence) is retried "
                         f"once (default {LAUNCH_MAX_ATTEMPTS})")
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

    # GH #67 live tuning: ini overrides only make sense on the read-only shadow
    # (the hook reads `<dir-of-opengl32.dll>\riddick_vr.ini`). Refuse to write the
    # game install -- this loop must never touch it. Checked before the image-dep
    # probe so a misused flag fails the same way everywhere.
    if args.ini and not args.shadow_install:
        print("xrsim: --ini requires --shadow-install (refusing to write the game "
              "install's riddick_vr.ini)", file=sys.stderr)
        return 2

    if not have_image_deps():
        print("xrsim: numpy and Pillow are required for real runs "
              "(pip install numpy pillow)", file=sys.stderr)
        return 2

    # GH #83: at most one live harness at a time, and never a shared shadow tree.
    # The lock is taken before anything is built or launched (a shadow rebuild is
    # itself one of the collisions); a stale lock (dead pid) is taken over, and
    # the release is registered with atexit so every exit path drops it.
    live_lock = live_lock_path()
    live_lock_note = {"path": str(live_lock), "pid": os.getpid(),
                      "taken": not args.ignore_live_lock}
    if not args.ignore_live_lock:
        ok, _state, message = acquire_live_lock(live_lock, os.getpid())
        if not ok:
            print("xrsim: " + message, file=sys.stderr)
            return 3
        print("[xrsim] " + message)
        atexit.register(release_live_lock, live_lock, os.getpid())

    # GH #81: an opt-in read-only shadow install. The loader resolves
    # `opengl32.dll` from the executable's directory, so mirroring the install
    # with junctions + a real program dir (and the repo build in the hook slot)
    # makes a live run exercise *current source* without writing to the install.
    if args.shadow_install:
        import shadow_install
        shadow_dest = (Path(args.shadow_dir) if args.shadow_dir
                       else default_shadow_dir(os.getpid()))
        built = Path(args.built_hook) if args.built_hook else BUILT_HOOK
        ini_overrides = shadow_install.parse_ini_overrides(args.ini)
        manifest = shadow_install.build_shadow(args.shadow_install, shadow_dest, built,
                                               ini_overrides=ini_overrides)
        args.shadow_manifest = manifest
        args.ini_overrides = manifest.get("ini_overrides") or None
        args.game_dir = manifest["game_dir"]
        if not args.game_exe:
            args.game_exe = manifest["game_exe"]
        print("[xrsim] " + shadow_install.format_shadow(manifest))
        if not manifest["fresh"]:
            print("[xrsim] WARNING: the shadow's hook does not match the build "
                  "-- --require-fresh-hook will fail", file=sys.stderr)

    artifacts = Path(args.artifacts) if args.artifacts else DEFAULT_ARTIFACTS
    report = Path(args.report_file) if args.report_file else DEFAULT_REPORT
    host_log, log_mark, menu_stats, launch, window_stats = run_sim(args, artifacts)
    persist_host_log(artifacts, host_log)  # GH #69: survive the run

    # GH #69 requirement 4: the hook truncates its log on launch, so recover the
    # *current run's* section by its header rather than the pre-run byte mark
    # (which would slice away markers logged early in the new run).
    game_dir = Path(getattr(args, "game_dir", None) or GAME_DIR)
    log_file = game_dir / "riddick_vr.log"
    fresh = read_fresh_hook_log(log_file, log_mark)
    print("[xrsim] " + input_bridge_stats(parse_input_bridge(fresh)))
    passed, checks = evaluate(args, artifacts, host_log, fresh, launch=launch)
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
                        "viewmodel_projection": viewmodel_projection_stats(fresh),
                        "eye_frames": eye_frame_stats(artifacts),
                        "vertical_disparity": vertical_disparity_stats(checks),
                        "stereo_kind": stereo_kind_stats(checks),
                        "comfort_vignette": parse_comfort(host_log),
                        "menu_advance": menu_stats,
                        "window_activation": window_stats,
                        "key_drive": getattr(args, "drive_stats", None),
                        "game_args": list(getattr(args, "game_arg", None) or []),
                        "sim_confirm": sim_confirm_env(
                            getattr(args, "sim_confirm", None),
                            getattr(args, "sim_confirm_max_taps", None),
                            getattr(args, "sim_confirm_period_ms", None),
                            getattr(args, "sim_confirm_hold_ms", None)) or None,
                        "deployed_hook": deployed_hook_identity(
                            game_dir, getattr(args, "built_hook", None) or BUILT_HOOK),
                        "input_bridge": parse_input_bridge(fresh),
                        "shadow": getattr(args, "shadow_manifest", None),
                        "ini_overrides": getattr(args, "ini_overrides", None),
                        "live_lock": live_lock_note,
                        "launch": launch,
                        "host_log": str(host_log_path(artifacts)),
                        "display_branch": {str(k): v for k, v in sorted(labels.items())}})
    print_summary(checks, report, passed)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
