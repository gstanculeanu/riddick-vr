# VR Scenario Testing

The repository does not ship a custom game map or a reliable checkpoint-warp
mechanism. Use the cell block/yard when available as the repeatable quiet test
area; the exact location depends on campaign progress. `tests/scenarios/manifest.json`
is the scenario inventory. It distinguishes the headless automated smoke test
from interactions that still require an in-game manual run.

## Automated smoke test

The current mock host synthesizes tracking, controller motion, and a small
reload/NanoMED/grab interaction loop. It does **not** simulate campaign state,
spawn targets, or prove that a game-side gesture was accepted.

```powershell
python tools/scenario_tests.py --check
python tools/scenario_tests.py --list
python tools/scenario_tests.py --run mock-smoke --frames 360 --timeout 120
python tools/scenario_tests.py --run reload --frames 300 --timeout 120
python tools/scenario_tests.py --run heal --frames 300 --timeout 120
python tools/scenario_tests.py --run climb-mantle --frames 330 --timeout 120
python tools/scenario_tests.py --run grab-throw --frames 120 --timeout 120
python tools/scenario_tests.py --run melee --frames 90 --timeout 120
python tools/scenario_tests.py --run grenade --frames 100 --timeout 120
python tools/scenario_tests.py --run drone --frames 150 --timeout 120
```

This delegates to `tools/semi_play.py`; it requires the local game install and
the built mock host. Scripted controller keyframes are available for reload,
healing, climbing, throwing, melee, grenade, and drone-disconnect profiles:

```powershell
python tools/semi_play.py --scenario reload --frames 300 --timeout 120
python tools/semi_play.py --scenario heal --frames 300 --timeout 120
python tools/semi_play.py --scenario climb --frames 330 --timeout 120
python tools/semi_play.py --scenario throw --frames 120 --timeout 120
python tools/semi_play.py --scenario melee --frames 90 --timeout 120
python tools/semi_play.py --scenario grenade --frames 100 --timeout 120
python tools/semi_play.py --scenario drone --frames 150 --timeout 120
```

These profiles drive mock poses only. A matching campaign object/NPC/station is
still required before gesture log markers can be expected. The scenario
validator does not treat synthetic controller input alone as proof that the
game accepted an action.

### Observation-only profiles (GH #75)

Some profiles frame the scene for inspection instead of driving an interaction:
`tools/scenarios/hands_view.json` holds both controllers idle in front of the
face so the native hands can be viewed. Such a profile has no hook marker to
assert, so it must opt out explicitly with `"observe": true`. `python
tools/scenario_tests.py --check` accepts a markerless profile only when that flag
is set; a profile with neither markers nor the opt-out is still rejected.
`tools/test_scenario_tests.py` guards both the opt-out and the shipped
`--check` gate.

## Manual scenarios and evidence

For a manual scenario, follow its `steps` in the manifest while the game is
running. Save a fresh hook log and at least one eye-frame capture. Record the
log's starting byte length before the attempt, then validate only new log text:

```powershell
$mark = (Get-Item '<GameFolder>\riddick_vr.log').Length
$frameMark = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
# Perform the scenario and capture an eye frame.
python tools/scenario_tests.py --validate reload `
  --log '<GameFolder>\riddick_vr.log' --since-byte $mark `
  --artifacts '<repo>\tests\artifacts' --frame-since $frameMark
```

The validator checks the scenario's log markers and that captured frame files
exist and are newer than `--frame-since`. A passing marker check proves the hook observed
the gesture; it does not replace visual inspection of the frame or in-headset
comfort testing.

**Caveat (GH #69, ADR-0446):** the hook opens `riddick_vr.log` with
`std::ios::out | std::ios::trunc` on every launch, so `--since-byte $mark` is
unreliable whenever the new run's log is shorter than `$mark` (the slice can be
empty, silently dropping markers). The XRSIM harness now recovers the
*current-run section* by its `=== Riddick VR Log Started at ... ===` header
(`tools/xrsim.py::read_fresh_hook_log` / `extract_run_section`); teaching
`scenario_tests.py --validate` and `semi_play.py` the same extraction is a
tracked follow-up.

**Caveat (GH #71, ADR-0448):** the mock host follows the *game's* uncapped swap
rate (~250-300 FPS on the startup splash), so a frame-counted run is only a
fraction of the wall-clock the title needs to finish its multi-page splash and
load a level — a 180-frame run is ≈0.6 s and never leaves the splash. Use
`python tools/xrsim.py --reach-gameplay --duration 45` to drive a run into live
3D gameplay; it asserts a full-bleed scene was reached from the hook's `BarScan`
active-rect fill (the splash is letterboxed to ≈0.36 of the window). Because the
frame-indexed scenario keyframes then play far faster than 90 Hz, deferring
scenario playback until gameplay is reached is follow-up work (#71/#72), and the
`cinemaMode` branch bit still mislabels the static 2D splash pages as gameplay
(**addressed** by GH #72 requirement 1 / ADR-0459: an opt-in
`DisplayBranchColdStartLatch` demotes a clearly letterboxed frame — BarScan fill
`<= 0.75` — to cinema until the first full-bleed frame `>= 0.90` is ever
measured; **on by default** since the 2026-10-18 live A/B, ADR-0478).

**Caveat (GH #71 requirement 1, ADR-0466 — CORRECTED in wake 84 by ADR-0485):** a
mock run without input **stalls on the game's front-end "start card"** (the pulsing
THE CHRONICLES OF RIDDICK / ASSAULT ON DARK ATHENA page). `RIDDICKVR_SIM=1`
suppresses the hook's `SendInput` (input safety), and the earlier reading that the
engine "does not consume posted window messages" was **wrong**: the taps it posted
carried `lParam = 1`, i.e. scan code **0**, and `RndrGL.dll`'s window procedure
decodes a key from `lParam >> 16` — so the engine received key code 0 (an unmapped
key). Its pump is `GetMessageA(NULL,0,0)` → `TranslateMessage` →
`DispatchMessageA`, which *does* dispatch posted messages; Raw Input there is
**mouse-only** (one `RAWINPUTDEVICE`, page 1 / usage 2, `RIDEV_INPUTSINK`). Full
record: `docs/rea/reference/RndrGL_Input_Path.txt`.

With the real scan code (`MapVirtualKeyA(VK_RETURN) = 0x1C`) the same targeted
window message **does** advance the front-end: a replicated A/A/B/B (2 runs with
`python tools/xrsim.py --reach-gameplay --advance-menu`, 2 without) left the start
card at the ~6th tap into the `LOADING` page and then a live dark level, while both
no-tap controls sat on the card for the whole 45-60 s run (ADR-0485). The flag stays
**off by default** — it is a front-end driver, and the harness's gameplay branch
still cannot tell the card from a level (`scene_advanced` and
`gameplay_full_bleed_reached` pass on both, GH #72).

**Correction (wake 85, ADR-0486):** that ~6th-tap count is *not* a fixed floor the
engine imposes — it was the point at which the hook-log write buffer happened to
flush, because the tap loop's stop signal is satisfied by the engine's **own**
startup transition (`DisplayMode: CINEMA` → `FULL-BLEED` at frame 1). A run can
therefore stop after as few as 3 taps and stay on the card. The stop now also
requires `--advance-min-taps` (**default 8**) delivered taps first; with it, the
default run reaches the live level (card → transition at frame ~3300 → dark level
from frame ~3900).

**Update (wake 64, ADR-0466 addendum):** the *command-line* route is dead too.
The engine's console surface is real (`loadgame`/`savegame`/`changemap`/
`cg_newgame` in `Content/GameClasses_Win32_x86.dll` + `GameWorld_Win32_x86.dll`)
and savegames exist (`%APPDATA%/Starbreeze/Riddick/Content/Save/Gigel/`), but
`python tools/xrsim.py --game-arg +loadgame Checkpoint`, `--game-arg +quit`, and
`-env <cfg + COMMANDLINE=+quit> -nowriteenv` all left the run on the splash for
its full wall-clock budget — the engine does not execute `+`-prefixed console
commands from its command line. `--game-arg` is kept (repeatable, unit-tested
`game_command`, recorded as `game_args`) as the handle for the hook-side bridge
(GH #80): `MSystem.dll` resolves `dinput.dll!DirectInputCreateA` and
`xinput1_3.dll!XInputGetState` via `LoadLibrary`/`GetProcAddress`, so the bridge
must intercept `GetProcAddress`, not an IAT import.

**Update (wake 66, GH #80 req 1, ADR-0467/0468):** that hook-side bridge is now
implemented. The hook swaps `MSystem.dll`'s `KERNEL32!GetProcAddress` IAT entry
at `DLL_PROCESS_ATTACH` (before `CInputContext_DInput`'s constructor at
`0x1010b440` resolves the procs), wraps `XInputGetState` (gamepad: `START|A`) and
`DirectInputCreateA` → in-place `CreateDevice` (slot 3) → `GetDeviceState`
(slot 9) for `GUID_SysKeyboard` (`DIK_RETURN`). It is **off unless both**
`RIDDICKVR_SIM` and `RIDDICKVR_SIM_CONFIRM` are set (requirement 3). The harness
half is `python tools/xrsim.py --reach-gameplay --sim-confirm [keyboard|gamepad|both]`
(`--sim-confirm-max-taps`/`-period-ms`/`-hold-ms`; recorded as `sim_confirm`).
Live validation is no longer deploy-gated: use `--shadow-install` (ADR-0471) so
the run exercises the repo-built hook while the install stays untouched.

**Update (wake 67, GH #80 req 1):** the interceptor is now *executed* offline, so
it is no longer validated by inspection alone. `SimInputBridgeInstallOnModule`
is the module-agnostic mechanism (`SimInputBridgeInstallOnAttach` is exactly
`InstallOnModule(GetModuleHandleA("MSystem.dll"))`), and
`tests/sim_input_bridge_win32_tests.cpp` installs the bridge on *its own* module
and drives the engine's exact path — CTest `sim_input_bridge_win32_intercept`
(gate-on: `GetProcAddress` returns the wrappers, `XInputGetState` ORs `START|A`
into the engine's `XINPUT_STATE` and bumps the packet, `DirectInputCreateA`'s
`GUID_SysKeyboard` device gets slot 9 replaced; near-miss/other names pass
through) and `sim_input_bridge_win32_inert` (gate-off: every pointer is the real
one). It SKIPs without the input DLLs, and x86-only (the hook ships x86). This
proves the *mechanism*; the ADR-0467 ordering against the real constructor was
measured resolved on 2026-10-18 — a shadow run's startup report already carried
`inputProcRequests=2`, i.e. the `DllMain` swap **beats** `CInputContext_DInput`'s
constructor for both procs (see the wake-71 update below).

**Update (wake 71, GH #71 req 1 / #80 follow-up, ADR-0472):** the bridge now
*reports what it observed*, so the reach is decidable from a run instead of
inferred. It names every input proc the engine resolved through the swapped
`GetProcAddress` (`owned` by the route or merely `observed`), counts how often
the engine polled each wrapped entry point, counts the confirms it fabricated,
and records the real API's return code. A parseable `summary` line is emitted
**once from the deferred-init report** (so a poll whose own line preceded the
logger is still reported *by value*), **from the wrappers on a doubling poll
cadence** (`SimShouldLogPoll`: 1, 2, 4, … 128, then every 120 — two consecutive
logged counts are never more than a factor of two apart, so "never read" and
"read 12 times" cannot look alike; a harness run `TerminateProcess`es the game, so
`DLL_PROCESS_DETACH` never runs) and at detach; the harness parses it into the
`input_bridge` report key and prints one summary line, flagging
`xinput_connected` / `xinput_device_not_connected`. It is *reported*, not graded
(no criterion yet).

Measured 2026-10-18 with `--shadow-install … --require-fresh-hook
--sim-confirm both` (hook `sha256 38648e15…` = the build under review):
`inputProcRequests=2` — the engine resolved **both** `XInputGetState` and
`DirectInputCreateA` through the swapped `GetProcAddress`, and the bridge owned
both — yet `polls=0`, `taps=0`, `createDevicePatched=0`: the engine **never
calls** either entry point in a 120 s session, and the tightened build reports
that `polls=0` *by value* at deferred init (`summary … lastRealResult=none`), not
merely by an absent line. So fabricating the confirm on
those device read paths cannot advance the splash, and the reach must come from
another surface (`RndrGL.dll` imports Raw Input — ADR-0466 — or the in-process
console route of ADR-0466's wake-64 addendum). Also note the wake-70 reading
that this run's `route=keyboard` datum was `XInputGetState` is wrong:
`SimBridgeOwnsProcName` is route-scoped, so a keyboard-route run can only own
`DirectInputCreateA`.

**Update (wake 96, GH #71 req 1 / ADR-0497):** the posted-confirm route is **not**
dead — it was measured without an *active* window. A live A/B on the current build
(`--shadow-install`, `--require-fresh-hook`, `--dump-branch any`):

- control `--sim-confirm both` (bridge only, no activation route): stayed on the
  `PRESS ENTER` splash for the whole run, bridge `polls=0 taps=0
  createDevicePatched=0` with `procs[XInputGetState:owned,DirectInputCreateA:owned]`;
- treatment `--advance-menu --advance-min-taps 8 --sim-confirm both`:
  **reached live 3D gameplay** (62 gameplay / 8 cinema / **62 distinct** frames,
  last frame an in-level corridor) after **8** posted taps to **1** window.

The harness now **activates the game's own top-level window** first
(`SetForegroundWindow` under an `AttachThreadInput` handshake; window management
only, no synthesized input; `WINDOW_ACTIVATION_TIMEOUT_S=15 s`, 0.5 s cadence) and
records the observed result as the `window_activation` report key
(`attempted/found/foreground/hwnd/title/attempts`). In both runs activation
succeeded after 3 attempts — and in the control the window was foreground yet the
splash still did not advance, so activation alone is not sufficient; it is the
*precondition* for the posted messages the `--advance-menu` route sends. The
bridge's `polls=0` is unchanged and also true *during* gameplay, so the bridge is
not the reach mechanism; it stays opt-in and inert. Consequence: the reach command
is now `--reach-gameplay --duration 75 --advance-menu --advance-min-taps 8
--shadow-install <install>` (no `--sim-confirm` needed), which unblocks GH #67's
state question — sample `--ini ViewmodelObserve=1` *during* a reached level.

**Caveat (GH #72 requirement 3, ADR-0454):** the very dark live gameplay frames
(mean luma ≈13, reference-crop std ≈16) make the stereo diff's vertical
registration **unmeasurable** — `best_dy` pins to the search edge and moves with
the analysis crop/downscale (measured: dy 6/7/10/12/8 from five crops of one
frame). A *bright* frame can also fail to register (frame 2400: `dy` and `dx`
both on their bounds, 24 px crop spread). That is an alignment-estimator
artefact, not a stereo defect: the render path applies only a horizontal half-IPD
shift, and the same blit gives a steady `dy=0` from every crop on a good frame.
`tools/xrsim.py` therefore gates `no_vertical_disparity` on the frame's contrast
(`MIN_DY_CONTRAST`) **and** on the optimum not being pinned to the `±8` search
edge (`DY_SEARCH_BOUND`): an unquantifiable `dy` is reported **inconclusive** (a
skip), and the report's `vertical_disparity` key carries the `ok` /
`inconclusive` / `failed` tally. A textured frame with a real, interior offset
still fails. The `frame_budget` check asserts the 90 Hz budget on the
frame-pacing percentiles (**p50 and p95 <= 11.11 ms**), not on the arithmetic
mean or the single worst frame: the mock host follows the game's uncapped swap
rate, so a one-off level-load hitch (~760 ms) dominates `max_ms`/`mean_ms`
without being a sustained 90 Hz regression (ADR-0455). `max_ms` and the drop
count are still *reported* so a real stall stays visible.

**Stereo structure (GH #76, ADR-0461):** the gameplay stereo is *not* native. The
hook captures **one** backbuffer and blits it to both eyes with a single
`±(halfIpdPx + comfortPx)` source-rect shift (`src/hook/stereo_renderer.cpp:932-960`,
verbatim *"monoscopic capture has no IPD parallax"*), so the two eyes are related
by **one global horizontal translation** and the scene has no depth-dependent
parallax. The old gates (`horizontal_disparity_present`, `not_mono_duplicate`)
pass on such a pair, which is why the gap was invisible. `tools/xrsim.py` now
re-aligns a `4x4` interior grid of tiles locally and reports the disparity
*structure* as the `stereo_kind` report key (`parallax` / `shift-only` / `mono` /
`unknown`, with `spread_px_max` and `per_frame`). Measured on the shipped build:
**every textured tile agrees (spread 0 px)** on every bright gameplay frame, so
the run is labelled `shift-only`. `--require-parallax` (or a scenario's
`"require_parallax": true`) turns that into the graded check
`stereo_parallax_present` — it fails on a globally-shifted pair and is the
acceptance instrument for GH #76. It is **off by default** (the shift is the
current documented interim), so a default run's verdict is unchanged.

**Interim depth-based reprojection (GH #76, ADR-0501/0502, wakes 99-100):** the
issue's option 2 is implemented behind `StereoParallaxDibr=1` (default **off**):
the single uniform-shift blit is replaced by a `DibrCols x DibrRows` grid of
blits, each cell shifted by its own depth-derived disparity (`fPx * IPD / z`,
left `-disp/2` / right `+disp/2`), so disparity varies with depth. It **falls
back** to the legacy uniform shift whenever the reduced depth grid does not vary
(`>= 0.5 px`) **or** its maximum disparity is below the legacy half-IPD
convergence cue it replaces (`minSep = 2 * halfIpdPx`, ADR-0502), so neither a
missing nor a *far* depth can flatten the image. Live measurement on the reached
level: the window backbuffer's depth reads **uniformly far** (`2048.01 m`) at
`SwapBuffers`, and the engine's own FBO (`fbo=6`) is no better there; the
mid-frame **depth-only clear** (ADR-0439's viewmodel-window opener) is now
captured **before** the engine clears it (ADR-0502) — and that source is
**reliably present** (573/589 grid rebuilds `src=clear`) with real, depth-varying
content — but its nearest surface is only `~41 m` (`rawDepth min 0.905`, `z=[41.2
..471.2] m`, `fullDisp=[0.12..1.45] px`), i.e. it is a **far-only pass, not the
visible world pass** (whose measured near plane is 4.0 m). So the warp's
sub-2 px disparity is below the legacy cue and the `minSep` gate keeps the legacy
blit: `stereo_kind=shift-only` (`spread=0`), `not_mono_duplicate` and
`horizontal_disparity_present` PASS, and `--require-parallax` honestly FAILs.
Without the gate (measured) the applied far-depth warp collapsed the pair to a
mono duplicate (`identical=True`) — hence the gate.
**ADR-0503 (wake 101) closes the search for the near world depth:** the hook now
records every framebuffer id the engine allocates (`Hook_glGenFramebuffers` →
`GetKnownFramebufferIds`) and, at the depth-only clear (the one point a readback is
proven real), scans and logs each FBO's depth
(`DIBR fbo scan: fbo=.. bound=.. WxH status=0x.. rawDepth=[lo..hi]`). Live,
in-level gameplay (17 scans, `bound=` echoed each id so the reads are real): the
window and every engine FBO read uniformly `1.000` (far); **only `fbo=6`** has any
depth and it is the already-known far-only pass (`rawDepth [0.775..0.906]`, ≈ 23-41
m). A one-scan positive control confirms the read is real
(`fbo=6 rawDepth=[0.812..1.000]` beside the clear's own `depth_z=[13.2..81.8] m`).
So **no hook-readable buffer holds the near world pass** and the interim depth
reprojection has **no usable depth source** here; the scan is evidence-only (it
never rewrites the snapshot) and `StereoParallaxDibr=0` stays the default. GH #76's
remaining route is option (1), a second engine view — a separately scoped engine
render seam, not another depth readback. Also recorded:
`CameraHook::StabilizeViewMatrix` (the ADR-0406 "intended seam") is **dead code**
(no call sites); head-look is injected as mouse movement, so per-eye dual
rendering has no engine hook seam on this title.
**ADR-0504 (wake 102) opens option (1) with a concrete mechanism:** the engine
submits **eye-space** client-array geometry (`glDrawElements`; no `glDrawArrays`,
identity `GL_MODELVIEW`, ADR-0481/0482), so a per-eye camera is a *pure modelview
translation* of every vertex. `src/hook/stereo_replay.h` lands the tested replay
maths (eye-space offset `d_eye = R_head^T (eyePos - headPos)`, replay
`GL_MODELVIEW = translate(-d_eye)`, `ReplayViewResidual`) with a
scene-faithfulness gate: the mock host's **placeholder** per-eye baseline (origin
at the world origin; GH #77/ADR-0463) is refused and the head-local `+/-halfIpd`
fallback is used. The opt-in `StereoReplayProbe=1` (evidence only) logs it live —
`baseline=0.0650m src=fallback offL=(-0.0325,0,0) offR=(+0.0325,0,0) residual ~1.68`.
Option (1)'s remaining unit is a **draw recorder + replay** (separately scoped).
**ADR-0505 (wake 103; GH #76 / GH #90) applies that translation at the draw site
instead of replaying it.** ADR-0491 measured the array path to be *complete* (no
display lists, no `glDrawArrays`, no world-pass immediate mode), so at
`Hook_glDrawElements` the geometry is in hand and the fixed-function state that
will shade it is already current — a record-and-replay would only reconstruct what
is already there. With `StereoDualRender=1` (default **0**) `OnSceneDraw` re-issues
each eligible world draw once per eye into two per-eye colour+depth frames with
`glTranslatef(-offset_eye)`, and `OnSceneClear` mirrors the engine's own clear into
them and tracks the world span (colour clear/swap starts it; the mid-frame
depth-only clear ends it); at swap the eye pair *is* two real views
(`stereo_kind: parallax` is the acceptance, via `xrsim --require-parallax`).
Eligible = inside the world span **and** the sampled array bounds not near-field
(ADR-0490's separator), so the HUD, screen-space quads and the engine's viewmodel
are never eye-translated; a frame with too few eligible draws or a black eye
(`stereo_dual.h`) falls back to the shipped shifted capture. Logs
`DualRender: swap=.. eligible=.. skipped=.. coverL=.. coverR=.. use=..` plus the
span's sampled-scale census `scale=[unknown=.. near=.. mid=.. world=..]` and, from
ADR-0506, the rejection census `reject=.. rc=[nonfloat=.. noarray=.. zerocount=.. badsize=.. badindex=..]`
with the first rejected draw's `sig=[<reason> vtype=.. vsize=.. stride=.. ptr=.. itype=.. count=..]`
— which names *why* the pass lost draws (measured: all `noarray`, `GL_FLOAT`/3/stride 56
geometry read with a null client pointer, i.e. the world array is set out of the hook's sight).

**ADR-0507 (wake 105; GH #76 / GH #90) measures the array source.** A
`wglGetProcAddress` name census (each distinct resolved name logged once as
`GLProcCensus resolve: name=..`) plus forwarding wrappers for
`glBindBuffer[ARB]`/`glVertexAttribPointer[ARB]`/`glEnableVertexAttribArray[ARB]`
and an (expected-absent) `glInterleavedArrays` hook feed `src/hook/gl_entry_census.h`.
Per sampled interval the hook logs `GLProcCensus [..]: procs=.. names=.. arrayBuf=..
elemBuf=.. bindCalls=..(ary=.. elem=..) attribPtr=.. enableAttr=.. interleaved=..
attribLast=(..)`; the submitted-vertex line adds `ptrCalls=`/`ptrNull=`; and the
`DualRender:` rejection signature adds `buf=<n> bid=<id>` (the array-buffer binding
at the first rejection). Measured in-level (50 gameplay frames, full-bleed 1.00):
the engine resolves the whole core buffer-object API and calls it — 5.23M
`GL_ARRAY_BUFFER` + 10.70M `GL_ELEMENT_ARRAY_BUFFER` binds and 11.11M
`glVertexAttribPointerARB` calls over ~9.6k swaps — while 83% of its
`glVertexPointer` calls pass a **null** pointer, so ADR-0482's "no buffer object"
premise is falsified and the world position data is **not** in the fixed-function
client array the sampler reads (`buf=0` at the rejected draws). The sampler must
move to the generic-attribute + buffer path (ADR-0507 Next 1); the pass is
unchanged, opt-in (`StereoDualRender=0`), `use=0`, and `--require-parallax` still
fails.

**ADR-0508 (wake 106; GH #76 / GH #90) classifies by the array source.** The
per-eye pass no longer requires a *decoded* client array to re-issue a draw: an
undecodable draw that used the **generic-attribute path** is re-issued per eye,
because re-issuing needs no decoded vertices — the arrays and buffers are current
GL state at the draw hook. `gl_entry_census.h`'s `AttributePathCensus` is now a
**per-index** record (`enabled/size/type/stride` + the `GL_ARRAY_BUFFER` bound
*when the pointer was set*), `VertexSample` carries the stamped
`genericAttributeEnabled`/`genericPositionBufferId`, and the `GLProcCensus` line
adds `attrLive=(..)` while `DualRender:` adds `gen=/gbacked=/gsig=`. Every
*decodable* near-field draw (HUD/viewmodel) is still excluded, so screen-space
geometry is never eye-translated. Opt-in (`StereoDualRender=0` stays the default).

**ADR-0509 (wake 107; GH #76 / GH #90) reads the span's output state.** ADR-0508
moved the blocker: the span capture rose to 706/735 draws but the per-eye frames are
still refused, so the pass reads the engine's **output GL state** once per span at the
first eligible draw — `glColorMask`, depth mask, viewport, scissor box +
`GL_SCISSOR_TEST`, `GL_DEPTH_TEST`, `GL_BLEND`, i.e. the global state a re-issued draw
inherits — plus the eye frames' **mean luminance**. The `DualRender:` line gains
`luma=[L=… R=…]` and `sstate=[cap=… cmask=0x… dmask=… drawbuf=0x… vp=… sc=… scissor=…
dt=… blend=…]`. A fully-masked `cmask=0x0` (a depth/occlusion stage) means the pass
must move its span boundary; a live mask with sane viewport/scissor means the re-issue
itself is at fault. Evidence only; `StereoDualRender=0` stays the default.

**ADR-0510 (wake 108; GH #76 / GH #90) classifies at each draw.** ADR-0509's single
per-span read cannot tell a depth-only stage from a mask that turns back on, so the pass
now tracks the engine's `glColorMask` — `RndrGL`'s statically imported `glColorMask` is
IAT-hooked (`Hook_glColorMask` → `OpenGLProxy::NoteColourMask`, counted so a run states
the hook is live) — and gates the re-issue on it **per draw**: a draw submitted with the
colour mask fully off (the measured `cmask=0x0` depth stage) re-issues correctly but can
never colour an eye frame, so it is counted but not re-issued. `DualColourMaskWritesColour`
is RGB-only (an alpha-only mask writes nothing the eye-frame coverage reads), and
`SpanDrawCensus` splits its **eligible** draws into `eligibleColourOn`/`eligibleColourOff`
so one span says whether it ever turns colour back on. The `DualRender:` line gains
`cw=[on=… off=… calls=…]`. `m_dualEligible` counts only draws actually re-issued, so a
colour-masked span cannot pass the fail-safe gate. This is a correctness improvement over
ADR-0508 (which spent ~706 masked draws per eye) and stays opt-in
(`StereoDualRender=0`). Live (wake 108) the in-level span reads `cw=[on=547 off=159
calls=162194]` beside `sstate=[… cmask=0x0 …]`: **the span's opening depth prepass is
colour-masked (159 draws) but its colour world pass (547) is not** — so the span boundary
does **not** need to move and the remaining blocker is the re-issue (`coverL=0.289`,
`use=0`, the shipped shifted capture kept).

**Eye-view provenance (GH #77, ADR-0462/0463):** the `eye_frames.log` per-eye
view/projection matrices are **not** the matrices the hook consumes in a mock/sim
run. Mock mode never calls `xrLocateViews` (`src/host/openxr_manager.cpp:623-628`),
so the hook receives a constant synthetic baseline (identity rotation, `±halfIpd`,
`projL == projR`) that carries **no head rotation**. Each record now ends with
`eye=runtime` (real runtime), `eye=sim` (the host reconstructed the per-eye views
a runtime would return for the frame's scripted head pose, since it has no runtime
to ask) or `eye=synthetic` (the placeholder the hook actually consumed).
`tools/xrsim.py` reports `eye_frames.source`, the max angular deviation of the
recorded view rotation from the head pose and the left/right eye separation, and
grades `eye_views_scene_faithful` -- passing a `runtime` **or** `sim` capture that
tracks the head pose with a plausible IPD, and **skipping** (with the deviation as
evidence) a `synthetic`/legacy one. GH #67 step 3 must use the recorded head pose
(`hmdpos`/`hmdquat`) as the authoritative camera input.

**Comfort (GH #68 requirement 5, ADR-0456):** the mock host reads the
hook-published locomotion comfort level (`comfortVignette`) back every frame and
prints a `[Mock Host] ComfortVignette ...` summary carrying the max level reached
while it published move/turn input. `tools/xrsim.py` asserts
`comfort_vignette_applied` on it, so the vignette can be proven live without a
headset; the check is **skipped** (never failed) when the line or the locomotion
input is absent. Snap-turn/recenter and gross HUD clipping assertions are still
open in requirement 5 (they need hook-side signals and a deploy).

**Capture integrity (GH #79, ADR-0465):** the host reads each shared eye texture
once per capture — both eyes are staged back-to-back into one private copy that
the content gate samples and the BMP is written from — so the graded pixels and
the saved pixels are the same texels. The hook does not yet wait on the
frame-consumed event, so a blit that lands between the two adjacent copies can
still tear a pair; the hook half is deploy-gated. A pair that cannot be staged is
reported as `Capture skipped frame=N (eye staging failed)`.

**Capture-gate integrity (GH #79 host-half capstone, ADR-0469):** the host now
records, for every capture attempt, a `[Mock Host] GateSample` line (the gate's
per-eye luma sample, whether a frame was written, and the hook's
`clientFrameCount` either side of the staging copies) and mirrors it in the
`frame_####.json` sidecar under `"gate"`. `tools/xrsim.py` asserts
`capture_gate_matches_saved`: each saved BMP's mean luma (plain `(R+G+B)/3`, the
same definition the host samples) must match the gate sample within 1.0 luma, and
only a `Live` verdict may be written — the machine-checked form of GH #79's
acceptance, deploy-free. The `capture_gate` report key carries the straddle rate
(how often the hook published during a staging, i.e. the residual race window),
which is **reported, never failed**. A pre-#79 host / `--evaluate-only` run
without the lines **skips** the check.

**Hook build identity (GH #81, ADR-0470):** the game loads `opengl32.dll` from
its **own** `System\Win32_x86` directory, not the repo build the mock host is
launched from, and installing a fresh one is a *deploy* the autoloop must not
perform. Every run therefore records `deployed_hook` (path/sha256/bytes/mtime of
the deployed hook plus the `dist\opengl32.dll` build) in `report.json` and prints
one `[xrsim] hook under test: … -> MATCHES|DIFFERS …` line at launch. The
`deployed_hook_matches_build` check is **reported and skipped** by default and
**graded** with `--require-fresh-hook`, which fails unless the run exercised the
built hook — the acceptance instrument for "this live run tested current source"
on the deploy-gated ladder (#71/#72/#76/#79/#80). Measured 2026-10-18: the
deployed hook was `479232 B sha256 8514d35a…` vs the build
`488448 B sha256 14867cf3…`, i.e. a run grades a ~11 h older hook.

**Shadow install (GH #81, ADR-0471):** `--shadow-install <install-root>` mirrors a
real install **read-only** into a scratch tree — `Content*` and
`System\GL|Sound` are directory **junctions** (no admin, cross-volume), the
program directory `System\Win32_x86` becomes **real copies**, and the repo-built
`dist\opengl32.dll` is dropped into the hook slot. Because the loader resolves
`opengl32.dll` from the executable's directory, the game then runs the **current**
hook while every asset still resolves through the junction to the untouched
install. `--shadow-dir` overrides the scratch location (default
`%TEMP%\riddickvr-shadow-<pid>`, one per run — GH #83) and the report carries a
`shadow` key. This makes
`--require-fresh-hook` satisfiable without the forbidden deploy: measured
2026-10-18, `--shadow-install … --require-fresh-hook` **PASSED**, and the
previously deploy-gated `comfort_vignette_applied` went **PASS** (`max=1.000`)
where the stale-hook baseline was `max=0.000`; the install hashed byte-identical
before and after four runs.

**Live config tuning (GH #67, ADR-0480):** the hook resolves its config beside
its own module (`<dir-of-opengl32.dll>\riddick_vr.ini`, a real copy in the
shadow), so `--ini KEY=VALUE` (repeatable, requires `--shadow-install`) sets a key
in the **shadow's** ini — e.g. `--ini ViewmodelObserve=1` for the GH #67 probe,
`--ini SuppressEngineViewmodel=1` for GH #66. The run echoes the overrides
(manifest/`shadow` and report `ini_overrides`), and the game install is never
written. That is what makes an off-by-default hook flag reachable by a live run
without a deploy.

**First-person mesh capture (GH #67 step 1, ADR-0482/0483):** with
`--ini ViewmodelObserve=1` the hook samples the engine's submitted client-array
vertices at `glDrawElements` and logs the window's **largest** in-window draw
(the arms candidate) and the first outside draw (the World control), each with the
draw's index `count`. A 2026-10-18 run reads a 2520-vertex, camera-relative `fp`
mesh (`count=2520`, bbox ±0.45, byte-identical on all 16 sampled frames while the
head yaw varied 39.5°) against a 6-vertex world screen quad (`count=6`). Add
`--ini ViewmodelTrace=1` to also get the one-line pass signature
(`… d19 P? D d7 P? …`).

**The same sample is a plane, not the viewmodel (GH #67 step 1, ADR-0484).** The
`fp` record now also derives the sample's axis extents and grades
`viewmodel_candidate_is_volumetric`: the engine's first-person viewmodel is a
3-D mesh, so a planar in-window draw is a screen-space element and cannot be it.
The measured target sample is `min=(-0.450,-0.450,-0.050)
max=(0.450,0.450,0.000)` → `dz/dx = 0.056`, i.e. a flat plane on a 39.5°-varying
head pose — and the run's dumped "gameplay" frames are the game's
**start screen** (see GH #71), not gameplay. So the check FAILS until a real
in-game run supplies a volumetric sample; `viewmodel_pass_identified` (the window
has in-window draws) is unchanged and still PASSes.

**Correction (wake 86, ADR-0487):** "the frames are the start screen" was true of
the pre-wake-85 runs; with the tap floor (ADR-0486) a run reaches a live level.
Re-measured there, the depth-only-clear window opens (18 intervals, up to 4356
in-window draws) but its largest draw is still a **12-vertex plane at constant
`z = 6.25`** — so the window is real and still not the arms. The alternative
`--ini ViewmodelWindowOpener=2` (near-plane drop) **never opens**: every
projection load is `P?` (the hook's near extraction rejects this engine's
`glLoadMatrixf` matrix), so `arrayIn=0` in 21/21 intervals. The harness now
**fails** (never silently skips) when `ViewmodelObserve=1` ran but no draw fell
inside the window, so an inoperative opener is visible as a failed
identification.

**Correction (wake 87, ADR-0488/0489):** the `P?` above was the **hook's** bug,
not the engine's. This engine's projection is z-flipped (`m[11] == +1`,
`m[10] = +(f+n)/(f-n)`), so the classic `m[14]/(m[10]-1)` read **-far**
(measured `-2048.0078`) and rejected every load. `src/hook/projection_near.h` now
handles both orientations, and the pass trace reads real planes
(`P4.0 … P1.8 … P0.1` = world 4.0 / another pass 1.8 / near-plane-drop pass 0.1).
`--ini ViewmodelWindowOpener=2` now **opens** (3 intervals, up to 1322 in-window
draws). The raw matrix and both extractions are logged (`ViewmodelProbe proj`),
XRSIM grades `projection_near_extracted`, and the report carries
`viewmodel_projection`. **Neither opener makes the arms the largest in-window
draw**: on the same level the depth-only window sampled a 14688-count volumetric
mesh in 1/18 intervals and the near-drop window the same mesh in 1/3, while both
runs' *last* interval was a plane — so the strict
`viewmodel_candidate_is_volumetric` still FAILs and the new
`viewmodel_volumetric_candidate_sampled` reports the mesh observation without
claiming the pass was identified.

**Correction (wake 88, ADR-0490): the window-independent near-field route is
falsified.** ADR-0489's "Next" #1 (a discriminator that does not depend on the
window) was implemented as a *near-field* test: the interval's largest decodable
array draw whose sampled bounds are inside ±16 units of the origin, chosen
regardless of the window (`ViewmodelProbe submission near: … inside=..`; the
interval line gains `nearSample=0|1` so a pre-#0490 hook is skipped, not failed).
Two live runs on the reached level measured the candidate in **3 intervals
only** (`#1200`/`#1500`/`#1800`, the title/transition pages, best `count=44064`,
bounds ±0.45, volumetric, **0/3 inside the window**), and **no near-field draw at
all** in the 16 consecutive level intervals (`arrayIn=3600` each, ~2 M array
draws, `nearSample=0` every one). This engine submits the level's geometry in
**view space** (magnitudes ≳ 100; the level's in-window sample is at
`x ≈ 108..120`), so a near-field/object-space test cannot separate the viewmodel
from the world. XRSIM grades the claim
(`viewmodel_near_field_candidate_identifies_pass`, PASS only when a near-field
candidate covers at least as many intervals as the window had in-window draws and
is volumetric), and both runs FAIL (3/18, 3/19) — the honest verdict. The
remaining route to GH #67 step 1 is the engine-side identification of the
viewmodel pass.

**Correction (wake 89, ADR-0491): the census now covers the submission
*mechanism*, including the immediate-mode path every previous sample missed.** The
eight corrections above sampled only `glDrawElements` (client arrays) and
`glCallList` (display lists) — but the live 2009 `RndrGL.dll` also **imports the
immediate-mode path** (`glBegin`/`glEnd`/`glVertex3f`/`glVertex3fv`/`glVertex2f`/
`glVertex2i` among its 96 static `OPENGL32.dll` imports; re-measured read-only with
`python tools/gl_vocabulary_scan.py --imports`), and the reference's own open
question was "do the first-person arms use immediate mode?". A mesh submitted as
`glBegin(..) { glVertex3f(..) } glEnd()` emits **no** draw call, so no array or
list sample could ever have been the arms. `InstallRndrGLHooks` now IAT-hooks the
six entry points (observation only; forwarded verbatim, never suppressed, off
unless `ViewmodelObserve=1`) and `immediate_mode_observe.h` censuses the window:
blocks, 3-D vs 2-D vertices, and the window's largest block with exact bounds,
logged as `ViewmodelProbe immediate [#N]: …` plus a
`ViewmodelProbe submission fpimm:` candidate that shares the existing
planar-vs-volumetric shape gate. XRSIM grades
`viewmodel_in_window_immediate_geometry` (PASS on 3-D in-window immediate
vertices, FAIL when the census ran and there were none, SKIP on a pre-#0491 hook)
and `viewmodel_immediate_candidate_is_volumetric`. Finding immediate geometry in
the window removes the mechanism blind spot; it does **not** identify the arms.

**Correction (wake 90, ADR-0492): the viewmodel is an item *property*, not a GL
pass — the engine-side answer.** With every GL route measured dead (ADR-0484
planar candidate, ADR-0487 window opener, ADR-0490 near-field, ADR-0491 immediate
mode), the live **2009** `Content\GameClasses_Win32_x86.dll`
(`sha256 d7e81d7d…`) was scanned read-only. The engine's first-person viewmodel is
an item **property** `VIEWMODEL` plus an attach configuration
`ATTACHPOINT`/`ATTACHROTTRACK`, declared beside the class name
`CRPG_Object_Inventory` (`.rdata` VAs `0x1075a144`/`0x1075a150`/`0x1075a15c`); each
name has exactly **one** code xref, all in one neighbourhood (`0x10265a92`,
`0x10265b7a`, `0x10265b9d`), where `FUN_10265ab0` reads `ATTACHROTTRACK`/
`ATTACHPOINT` as **bool** properties (vtable slot `0x1b8`) into `+0x362`/`+0x35a`.
The 2009 RPG item-flag name table (VA `0x1082d728`) reproduces ADR-0435's bits with
`noviewmodel` = **bit 1**. Because the object/view transform is applied engine-side
to the submitted vertices (ADR-0481/0482), the arms reach GL as an ordinary
`glDrawElements` of an already-transformed, item-owned model — **no** GL signature
can isolate them, so the next step is the `VIEWMODEL` consumer / the engine's own FP
attach state, not another threshold. Recorded in
`docs/rea/reference/GameClasses2009_ViewmodelProperties.txt` (which also records
that the 9.0 MB 2009 module exceeds the provider's single-request analysis budget,
so the deep REA needs a persistent snapshot).

**Correction (wake 92, ADR-0493): the 2009 module *does* decompile — the 330 s
budget is a configurable deadline.** ADR-0492's "the 9.0 MB module exceeds the
provider's analysis budget" is corrected in place. `330_000` ms is only
`REA_GHIDRA_STARTUP_TIMEOUT_MS`'s **default**; with it raised to `1500000` two
`rea decompile` requests on the same module returned **exit 0** (~382 s / ~380 s
wall). `FUN_10265a70` (the sole `VIEWMODEL` xref) decompiles to
`{ FUN_10260940(); CStr::CStr("VIEWMODEL"); FUN_1026d790(); }`, and the callee
`0x100162e0` is **`CStr::CStr(const char*)`** (it installs `CStr::vftable`, zeroes
`[this+4]`, calls vtable slot `0x68`) — **not** the "name-keyed helper" the raw
scan read, so the literal is passed *by name* into `FUN_1026d790`. `--snapshot`
was measured **not** to be a cross-call analysis cache (the second call
re-imported and re-analysed; the provider pins
`import_mode = "ephemeral-source-immutable"`), so the "persist a snapshot" route
does not reduce cost; read-vs-set at the site is still open. Recorded in
`docs/rea/reference/GameClasses2009_ViewmodelDecompiled.txt`.

**Correction (wake 93, ADR-0494): the `VIEWMODEL` property is the *collectible*
class's model-load override, and the site is a READ — "the arms are this property"
is NOT supported.** ADR-0493's two open questions are answered read-only on the same
2009 module (three requests, `exit 0`, 390/378/369 s). `FUN_1026d790` is the
named-child copy helper (`name = arg->vtable[0x18]()`; `CRegistry::FindChild(registry,
name)`; the found child's value → destination `vtable[0x68]`), and the item model
loader `FUN_10260940` passes the *same* helper its own keys (`MODEL`,
`MODEL0`..`MODEL3`, `MODEL_DUMMY`, `PHYSMODEL`, `ATTACHMODEL`, `SOUND_*`, `ANIM_*`,
`ANIMGRAPH2`, `EFFECTLIST`, `PICKUPMSG`), so the direction is *definition → object*.
`rea xrefs 0x10265a70` returns exactly one incoming reference, the `.rdata` vtable
slot `0x10759ed4` (**slot 8** of the vtable at `0x10759eb4`), whose `-4` RTTI
(`0x107ab9d4` → TypeDescriptor `0x1082d888`) names the class
`CRPG_Object_Collectible` — **not** `CRPG_Object_Inventory` (that name's `.rdata`
proximity is string-pooling order; the `.data` class registry has Inventory as a
sibling of Collectible off `CRPG_Object`). Slot 8 is the model-load virtual:
implemented by `FUN_10260940` in `CRPG_Object_Item`/`CreatureAttack`/
`Telephone_Receiver` and **overridden** by `FUN_10265a70` in
`CRPG_Object_Collectible`; **no weapon-class vtable** (`Gun`/`Melee`/`Ulaks`/
`AngelusChain`) holds either function. So the engine-side search for the FP arms must
move off this property (the *item model* pipeline is still the place: the engine
names models by definition keys and routes them through the object's property
setters, not GL state). Recorded in
`docs/rea/reference/GameClasses2009_ViewmodelConsumer.txt`.


**Correction (wake 94, ADR-0495): the `FirstPerson`/`ThirdPerson` "view-mode
table" is a `CWObject_Matinee` script key, NOT the player FP body/arms — so
ADR-0494's "Next 1b" is closed.** ADR-0494 listed two candidate next steps for #67
step 1; one of them was to chase the `.data` block at `0x1083ff68`/`0x1083ff6c`
(ADR-0492 §4's "view-mode / camera-mode name list") to the camera/body-attach code.
A read-only static scan of the same 2009 module (no REA request this wake) measures
what that block is: `FirstPerson`/`ThirdPerson`/`KeepEndFade`/`NoSkip` — matinee
playback/camera keys. It has **exactly one** code reference, `push 0x1083ff68` at
`0x105ec068` (a 32-bit-keyed dispatch: `cmp edi, 0x4d68028a` / `cmp edi,
0x6b2e3218`), and that site is inside `FUN_105ebf40`, which is **slot 15** of the
vtable `0x1078c0e4` (its address `0x1078c120` appears exactly once in the image);
that vtable's `-4` RTTI CompleteObjectLocator `0x107b97e0` has TypeDescriptor
`0x10840238` = `.?AVCWObject_Matinee@@`, beside `.\WObj_Misc\WObj_Matinee.cpp`.
So the block is one matinee object's script key, reachable only through that single
push — there is no route from it to the player's first-person arms. The remaining
#67 step-1 candidates are the object property-setter target (destination
`vtable[0x68]`; ADR-0494's "1a") and the "is the viewmodel drawn at all?" state
question (GH #80 / wake 89). Recorded in
`docs/rea/reference/GameClasses2009_ViewmodeConsumer.txt`.


**Correction (wake 95, ADR-0496): the item-model destination `vtable[0x68]` is a
per-class virtual, and on the item class a second *property loader* — so ADR-0494's
"Next 1a" is NOT supported.** ADR-0494's other candidate for #67 step 1 was to chase
the destination `vtable[0x68]` (the slot `FUN_1026d790` hands the loaded model
reference to) to the model-instance creation. A read-only census of every MSVC RTTI
vtable in the same 2009 module (1,319 vtables, **874 distinct** slot-0x68 targets;
582 into `.text`, the rest into adjacent `.rdata` where the vtable is shorter than
27 slots) shows the slot is **per-class** — base `CRPG_Object` `0x1025df20`,
`_Weapon` `0x1024c320`, `_Inventory` `0x1024ebb0`, `_Char` `0x1024cdd0`, … — so there
is no single "model-instance setter". One read-only REA decompile
(`--procedure 0x10252d70`, `exit 0`, 380 s) shows what the item class's slot 0x68
is: `FUN_10252d70` calls the slot-8 item model loader `FUN_10260940()` and then
loads **SPAWN**, **PROJECTILE**, **SPAWNEFFECT**, **SUMMON_PROJECTILECLASS**,
**SUMMON_PROJECTILECLASS_PLAYER**, **GIVEONPICKUP**, **MELEE_HITSOUND** (the six
literals Ghidra left unnamed were recovered by local disassembly) — i.e. a *second
property loader* (`ret 0xC`), not a model-instance creator. The disassembly of
`FUN_1026d790` shows the value goes to a **separate** object's `vtable[0x68]` in a
one-argument virtual dispatch, so the slot *stores* the model reference; it does not
create the instance. With ADR-0495 (1b), **every named engine-side route of
ADR-0494 is exhausted**; the remaining route is the "is the viewmodel drawn at all?"
**state question** on the passive/unarmed level start (GH #80 / wake 89). Recorded
in `docs/rea/reference/GameClasses2009_ModelSetterSlot68.txt`.


**Launch reliability (GH #87, ADR-0479; extended by ADR-0498):** a live run can
measure nothing in two ways — a false red, either way, not a broken fix. (1) The
game ends within ~2 s — no hook log (or one that stops during renderer init), the
host logging `Game Connected: WAITING` for the whole (short) run, and 0 captured
frames — **while the identical re-run passes** (measured 2 of 3 fresh-shadow first
launches during the GH #72 A/B). (2) The game stays **live for the whole window**
with **0 frames and no hook log** (measured autoloop wake 96: 47 s alive, no hook
attach, identical re-run reached 62 distinct gameplay frames). An attempt with no
evidence (no captured frame **and** no fresh hook log) is retried once
(`--max-launch-attempts`, default 2) whether it ended early (`early_exit`) or
stayed live (`no_evidence`). If it repeats, the run reports **one** `game_started`
failure (report key `launch`, carrying every attempt's state / lived-seconds /
frames / hook-log freshness) and demotes the missing-capture FAILs to
reported-not-graded, so "nothing was measured" can never read as "the feature is
broken". A launch with evidence — including a short host-bounded `--frames` run
where the game is still alive — is never retried. `--evaluate-only` has no launch
record and grades exactly as before.

**In-level key drive (GH #67, ADR-0499):** the engine binds actions to **scan
codes** in the player profile
(`%APPDATA%\Starbreeze\Riddick\Content\Save\<profile>\_profile`,
`*ACTION_FORWARD 17`, `*ACTION_PRIMARYFIRE 240`, `*ACTION_WEAPON_SHIV 3`, …), and
`RndrGL.dll`'s window proc takes the scan code from the message's `lParam` — so
XRSIM can post a weapon-select / fire / move script to the game's own window with
`--drive-keys "WEAPON_PISTOL,PRIMARYFIRE,FORWARD:2000"` (opt-in; targeted
messaging, never desktop input). **Measured (wake 98): the drive is delivered
(`posted=16 windows=1`) but the in-level engine did not act on it** — no weapon,
no HUD/ammo change, no locomotion over the rest of the run, and a discriminating
`PAUSE` (Escape) post produced no pause menu — while the *same* route still walks
the front-end splash/menu (ADR-0485/ADR-0497). So "posting the profile's scan
codes drives in-level player action" is **not supported** on this target; the
route to an armed state moves to an engine-side input injection (ADR-0412's
`MSystem.dll` `QueueScanKey`/`CInputContextCore`) or a player-control state. See
`docs/adr/0499-…` and `docs/rea/reference/RndrGL_Input_Path.txt` §8.

**Artifacts (GH #73, ADR-0460):** a live run writes its per-eye captures to
`tests/xrsim/artifacts/` (~25 MB BMPs). Those are **evidence, not source** and
stay untracked — `.gitignore` excludes the directory and only the small,
diffable `tests/xrsim/report.json` is committed (regenerated from a live
`--reach-gameplay` run). `python tools/verify_test_artifacts.py` enforces this
(no `tests/xrsim/artifacts/*` may be git-tracked, the ignore rule must stay, and
`report.json` must exist and be valid JSON) and runs in the CI lane, so the
665 MB / 3-unique-blob over-commit that #73 requirement 1 removed cannot recur
silently.

## Current coverage and limits

The scripted profiles provide deterministic controller poses and velocities,
but they cannot load a campaign checkpoint or create targets. Reload, heal,
grenade, and drone results therefore still depend on the corresponding game
context. In particular, issue #40 still needs an in-headset visual check that
ammo digits fit the crop for each relevant weapon/resolution. Treat profile
marker failures as useful evidence, not proof that a gesture is broken when its
game preconditions were absent.
