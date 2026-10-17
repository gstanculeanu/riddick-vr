# RiddickVR Autoloop — State

Durable, human-readable state written by each scheduled wake. Newest at top.

## Wake 65 — 2026-10-17 23:22 (scheduled loop)
- **Unit:** GH **#80 requirement 1** — decided *where* the simulator-only input
  bridge can legally install the `GetProcAddress` intercept, and landed the
  bridge's pure, unit-tested core (requirement 2 + the gate half of 3). Harness/
  hook-header + tests + ADR; **no deploy, no live behavior change**.
- **Why this unit:** #80 is the only remaining route to #71 req 1 (the reach that
  makes the #68 iteration vehicle useful), it is the explicit "Next" of wake 64,
  and the naive implementation (install from the hook's deferred init) was an
  unmeasured assumption that could waste future wakes.
- **Decisive finding (read-only RE of the shipped sim install, Ghidra 12.1.4 via
  `rea`; ADR-0467):** the engine resolves its input entry points **once, in a
  constructor** — `CInputContext_DInput::CInputContext_DInput` (MSystem.dll VA
  `0x1010b440`, body `0x1010b440–0x1010b52a`) does `LoadLibraryA("xinput1_3.dll")`
  + `"XInputGetState"` (strings VA `0x102fc7cc`/`0x102fc7bc`) and
  `LoadLibraryA("dinput.dll")` + `GetProcAddress("DirectInputCreateA")` (strings
  VA `0x102fc7b0`/`0x102fc79c`). The constructor is reached only through the
  input factory `FUN_1010b7f0` (no direct callers — indirect subsystem/factory
  registration), device creation is tied to `CSS_MSG_WIN32_NEWMAINWINDOW`
  (VA `0x102fc952`), and the renderer (hence this hook) is loaded as a *display
  context* (`CSystemWin32::DC_InitList` VA `0x100f69c0` registers `RndrGL.DLL`,
  string used at `0x100f6a4e`; RndrGL imports `OPENGL32.dll`, 96 thunks; the
  deployed hook's log shows its IAT installs landing in RndrGL's image).
  `DarkAthena.exe` statically imports `MSystem.dll`/`MCCDyn.dll`/`dvm.dll` but
  **not** `opengl32`/`RndrGL`, so `MSystem.dll` is mapped before any hook code.
- **Consequence (the correction):** the bridge must **not** install from the
  hook's deferred init (GH #27, first `wglCreateContext`) — a GL context needs a
  window HDC and the input context is part of the same window bring-up, so that
  seam can run *after* the procs were already resolved. Install from
  `DllMain(DLL_PROCESS_ATTACH)` as a loader-lock-safe, IO-free IAT swap on
  `MSystem.dll`'s `GetProcAddress` (or a thread started there whose only job is
  the swap), and *report* whether the install beat the constructor rather than
  assume it.
- **Change:**
  - `src/hook/sim_input_bridge.h` (new, pure, no Win32): two-token enable gate
    (`RIDDICKVR_SIM` + `RIDDICKVR_SIM_CONFIRM`, requirement 3), bounded tap
    cadence (`SimConfirmTick`/`SimConfirmPeriod`, edge-triggered, `maxTaps`),
    DirectInput keyboard encoding (`kDirectInputKeyboardBytes` 256,
    one byte per scan code `0x80`, `DIK_RETURN` 0x1C), `GUID_SysKeyboard`
    byte-match helper, XInput gamepad encoding
    (`ApplySimConfirmToGamepad` ORs START|A, `BumpSimPacketNumber`), and the two
    interceptor predicates (`SimBridgeOwnsProcName`, `SimBridgeIsKeyboardGuid`).
  - `tests/sim_input_bridge_tests.cpp` (new) + `CMakeLists.txt`
    (`sim_input_bridge_pure_core`, header added to the proxy target).
  - `docs/adr/0467-sim-input-bridge-install-point.md` (new) — the findings, the
    install-point decision, and the residual ordering risk.
  - `tools/verify_reported_fixes.py` — a GH #80 invariant (module + tests + ADR
    wired).
- **Evidence (offline):** `tools/build-all.ps1` Release, **no `-Deploy`** OK
  (x86 proxy + x64 host; `dist/opengl32.dll` byte-unchanged — no hook source
  changed); **CTest 22/22 on x86 AND x64** (incl. the new test); all 10
  `tools/test_*.py` PASS; `xrsim --selftest` PASS; `verify_reported_fixes` PASS
  (`#40..#80`), `verify_hook_init` PASS, `validate_addresses` PASS,
  `verify_test_artifacts` PASS, `scenario_tests.py --check` PASS. **No live
  sim run**: the pattern in this wake is a pure header + tests (byte-identical
  hook artifact), so a run would exercise nothing new; the live validation is
  the deploy-gated interception.
- **Blocker / honest limit:** live validation of the bridge remains deploy-gated
  (the sim install runs the stale 12:47 hook). Also still open: whether
  `CInputContext_DInput`'s constructor precedes the hook's `DllMain` on a real
  run — static xrefs cannot settle the indirect factory construction, so the
  interceptor must log its install outcome. Fallback if it is too late: the
  wake-64 in-process console route (`loadgame Checkpoint`), which needs no early
  install.
- **Next:** GH #80 req 1 — implement `SimInputBridge` (early install + the
  `XInputGetState` wrapper and/or `DirectInputCreateA`→`CreateDevice` vtable
  wrap → `GetDeviceState` for `GUID_SysKeyboard`), consuming the pure core;
  validate with the one interactive deploy. Otherwise the deploy-gated ladder is
  unchanged (#67 step 3, #72 req 1 A/B, #68 req 5, #79 hook half, #76 step 1).

## Wake 64 — 2026-10-17 22:24 (scheduled loop)
- **Unit:** GH **#71 requirement 1** (the top blocker — the reach makes the #68
  iteration vehicle useless for live validation) — tested the *second* input-free
  route (the engine's own console/command line) to a **decidable negative**, and
  landed the harness capability (`--game-arg`) that is the handle for the
  remaining route. Harness + tests + tools + docs + a new issue; **no C++/asset
  change, no build, no deploy**.
- **Why this unit:** #68/#71 is the top open item, every player-visible ladder
  item is deploy-gated, and wake 63 left route (b) as "no CLI switch and no save".
  This wake read the *sim* install (F:) read-only and found that claim was wrong,
  then measured whether the engine's console can be driven from the command line.
- **New decisive findings (measured / read-only, not inferred):**
  1. **Savegames DO exist** (wake 63 said none):
     `%APPDATA%\Starbreeze\Riddick\Content\Save\Gigel\{01.Escape, Checkpoint,
     _filler000, _profile}` (`GAME_LASTVALIDPROFILE=Gigel`). Route (b) is not void
     for lack of a save.
  2. **The engine's console surface is real**: `MSystem.dll` registers the
     id-style console (`exec`/`echo`/`history`/`commands`/`bind*`, and
     `CConsole::ExecuteString`); the game logic lives in
     `Content\GameClasses_Win32_x86.dll` + `Content\GameWorld_Win32_x86.dll`,
     which name `loadgame`, `savegame`, `checkpoint`, `changemap`,
     `cg_newgame`/`cg_goto_newgame`, `cg_loadlastvalidprofile`, and
     `"Have to select a valid profile before loading a map"`.
  3. **But the command line does not execute them** (three wall-clock-bounded
     `--game-arg` runs, each left on the `PRESS ENTER` splash for the full
     budget): `+loadgame Checkpoint` (60 s), `+quit` (25 s — would exit at once
     *if* the engine ran `+` console commands), and `-env <copy of
     Environment.cfg + COMMANDLINE=+quit> -nowriteenv` (20 s — MSystem documents
     `"CommandLine is overrided by value in environment file."`). A posted
     command line cannot drive this engine.
  4. **The input DLLs are runtime-resolved**: `MSystem.dll` loads
     `dinput.dll`+`DirectInputCreateA` and `xinput1_3.dll`+`XInputGetState` via
     `LoadLibrary`/`GetProcAddress` (`.\Input\Win32_DInput\MInput_DI.cpp`,
     `CInputContext_DInput`), so a bridge must intercept `GetProcAddress` in
     `MSystem.dll`, **not** an IAT import of those DLLs.
- **Change:**
  - `tools/xrsim.py`: `game_command(game_exe, extra_args)` (pure) + a repeatable
    `--game-arg TOKEN` (leading-dash tokens via `--game-arg=VALUE`), forwarded to
    the game `Popen`; the tokens are recorded in the report as `game_args`; the
    docstring documents the measured negative and the hook-side handoff.
  - `tools/test_xrsim.py`: +3 tests (verbatim tokens, bare-exe/empty-token
    defaults, both `--game-arg` forms parse); `_args` gains `game_arg`.
  - `tools/verify_reported_fixes.py`: a new GH #71 req-1 invariant (helper +
    flag + report key + test wired).
  - `docs/adr/0466-...md`: a **wake-64 addendum** (savegames exist; console
    surface; the three measured command-line negatives; the runtime-resolved
    input DLLs) + a `docs/vr-scenarios.md` update note.
  - **Filed GH #80** — `[FEATURE] Simulator-only input bridge: hook the engine's
    DirectInput/XInput so XRSIM can reach gameplay` (the concrete next slice:
    intercept `GetProcAddress` in `MSystem.dll`; wrap `XInputGetState` and/or
    `DirectInputCreateA` → `CreateDevice`(vtable slot 3) →
    `GetDeviceState`(slot 9); a pure sim-only schedule; inert without
    `RIDDICKVR_SIM=1`).
- **Evidence (offline):** `python tools/test_xrsim.py` PASS (all, incl. the 3
  new); `python tools/xrsim.py --selftest` PASS; `verify_reported_fixes.py` PASS
  (`#40..#79`); `scenario_tests.py --check`, `verify_hook_init`,
  `validate_addresses`, `verify_test_artifacts`, all `tools/test_*.py` PASS. **No
  C++ changed → no `build-all.ps1`/CTest** (documented).
- **Evidence (live, allowed exception `RIDDICKVR_SIM=1`):** the three
  `--game-arg` probes above (full-duration runs on the splash; the `-env` cfg was
  a temp file, the install untouched). The committed `tests/xrsim/report.json`
  baseline is unchanged (probes wrote to `%TEMP%`).
- **Blocker / honest limit (the loud one):** GH #71 req 1 is **still not
  achievable offline**. Both non-hook routes are now measured dead (posted
  messages — wake 63; command line — this wake), so the *only* remaining path is
  the hook-side sim input bridge (GH #80), which is deploy-gated: the sim install
  runs the stale 12:47 hook, and deploying is a hard rule. Route (b) survives only
  as an in-process call (the hook executing a console command), i.e. route (a)
  with a different injection point.
- **Next:** GH #80 — implement the sim-only input bridge (pure schedule +
  `GetProcAddress` interception) offline; validate with the one interactive deploy.
  The deploy-gated ladder is otherwise unchanged (#67 step 3, #72 req 1 A/B, #68
  req 5, #79 hook half).

## Wake 63 — 2026-10-17 21:55 (scheduled loop)
- **Unit:** GH **#71 requirement 1** — established the *decidable* root cause of the
  XRSIM reach failure and encoded the sim-safe attempt + its negative result.
  Harness + tests + docs; **no C++/asset change, no build, no deploy**.
- **Why this unit:** #71/#68 is the top open item, every player-visible ladder
  item is deploy-gated, and the reach failure makes the iteration vehicle useless
  for live validation. Wake 62 proposed a hook-side fix ("let `camera_hook`
  `SendKey` post before the `SimModeNoInput()` return, and wire
  `SetGestureGameHwnd`"). This wake **tested that mechanism** (in the harness,
  where the HWND problem does not exist) and found it **cannot work**.
- **Decisive findings (measured, not inferred):**
  1. `python tools/xrsim.py --reach-gameplay --duration 45 --advance-menu` (real
     window messaging, no OS input): the game window was found
     (`class='MOS Win32/OpenGL'`, title *The Chronicles of Riddick: Assault on
     Dark Athena*, **visible and foreground**, pid match) and received **21
     VK_RETURN taps** (each `WM_KEYDOWN`+`WM_CHAR`+`WM_KEYUP`) — the game stayed
     on its splash; the log's only `DisplayMode` line is
     `CINEMA screen ... (menuCursor=1)`.
  2. A captured frame (`--dump-branch any`) renders the literal **"PRESS ENTER"**
     title plate, unchanged across the run.
  3. **Binary import scan (ASCII import names):** `MSystem.dll` imports
     **DirectInput** (and XInput); `RndrGL.dll` imports **Raw Input**
     (`RegisterRawInputDevices`, `GetRawInputData`) and `PeekMessage`; no module
     imports `GetAsyncKeyState`/`GetKeyboardState`. Both DirectInput and Raw
     Input are **device-sourced** — fed only by OS-level `SendInput`, exactly what
     `RIDDICKVR_SIM=1` suppresses (the deployed hook logs
     `XRSIM: RIDDICKVR_SIM=1 - desktop input injection disabled`).
  4. No start-at-level route: `-map` in the engine DLLs is a `mip-map` /
     `environment-mapping` false positive, and there is no savegame on the box.
- **Change:**
  - `tools/xrsim.py`: `post_confirm_to_game_windows(pid)` + `confirm_key_messages`
    (the exact posted key triple, ctypes, no-op off Windows) and
    `GameMenuAdvancer` (pure cadence/max-taps/stop, injected poster + clock, no
    thread, no catch-up burst); `display_branch_sequence` /
    `gameplay_reached_from_sequence` (stop signal = a full-bleed branch *after* a
    cinema branch, because the splash is full-bleed-mislabelled per GH #72);
    `--advance-menu` (**default OFF**) / `--advance-interval` /
    `--advance-max-taps`; the outcome is recorded in the report as `menu_advance`.
  - `tools/test_xrsim.py`: +7 tests (key-message lParam, cadence/max-taps, stop,
    no-catch-up, the menu→gameplay transition rule, the pid-0 no-op, flag parse).
  - `tools/verify_reported_fixes.py`: a GH #71 req-1 invariant (capability +
    opt-in default + report key + test wired).
  - `docs/adr/0466-engine-input-is-directinput-rawinput-so-the-sim-cannot-post-past-the-splash.md`
    (new) + a `docs/vr-scenarios.md` caveat.
- **Evidence (offline):** `python tools/test_xrsim.py` PASS (all cases incl. the 7
  new); `python tools/xrsim.py --selftest` PASS; all 10 `tools/test_*.py` PASS;
  `verify_test_artifacts` + `verify_hook_init` + `validate_addresses` +
  `verify_reported_fixes` + `scenario_tests.py --check` PASS. **No C++ changed →
  no `build-all.ps1`/CTest** (documented). Committed `tests/xrsim/report.json`
  restored after the probe runs overwrote it (the committed wake-61 baseline is
  unchanged).
- **Blocker / honest limit (the loud one):** **GH #71 requirement 1 is not
  achievable offline under the hard rules.** The engine ignores posted window
  messages, and OS-level `SendInput` is forbidden (input safety). Viable routes,
  both out of this wake's scope: (a) a hook-side, simulator-only **input bridge**
  (hook `GetRawInputData` / `IDirectInputDevice8::GetDeviceState` and inject the
  scripted confirm state) — deploy-gated REA work; (b) launch directly into a
  level/savegame — no CLI switch and no save exist. Wake 62's proposed hook change
  (posting window messages) would **not** have worked.
- **Next:** the reach is the loop's biggest blocker; the highest-value item is
  (a) — REA the engine's DirectInput/Raw-Input read path and bridge it in sim mode
  (ADR-0466), which also finally lets the harness validate the deploy-gated
  player-visible fixes (#67 step 3, #64/#61 passes, #76 step 1). Otherwise the
  offline candidates stay thin (all ladder items are deploy-gated). `--advance-menu`
  is landed off-by-default as the harness half of route (a).

## Wake 62 — 2026-10-17 21:29 (scheduled loop)
- **Unit:** GH **#79 host half** (wake 61's stated Next) — the XRSIM capture now
  reads each shared eye texture **once**, as a back-to-back pair, and runs both
  the content gate and the BMP write over that one private copy; the sampler and
  the BMP layout are pure helpers pinned by a new CTest. Host C++ + tests + tools
  + docs; **no hook change, no asset change, no deploy**.
- **Why this unit:** #79 is the newest open item and its *host* half is the one
  the issue explicitly offers as loop-safe (`Alternatively/additionally
  host-side`), unlike the deploy-gated `WaitForFrameConsumed`. It removes the
  mechanism that made the gate probe and the saved BMP disagree (frame 540) and
  that read the two eyes at different times (frame 810's global vertical offset,
  frame 1770's intra-image tear).
- **Change:**
  - `src/host/eye_sample.h` (new, pure): `SampleEyeImage` = the full-image mean
    luma + the 16x16 FNV-1a frozen-page signature, extracted from the removed
    `SampleEyeContent` and now run over the staged copy; honours a padded
    `RowPitch`.
  - `src/host/bmp_writer.h` (new, pure): `BmpFileHeader` (54 bytes,
    `static_assert`), `MakeBmpHeader` (top-down), `ConvertRgbaRowToBgra`.
  - `src/host/openxr_manager.{h,cpp}`: `StagedEyeImages` + `StageEyePair` —
    create both private staging textures, `CopyResource` left then right
    *adjacent*, then map both; `WriteBmpFromMapped` writes from the mapped
    private rows. `SaveTextureToBMP` (its own per-eye staging) is deleted. The
    dump block stages once (`haveStaged`), samples + saves from it, and reports
    `Capture skipped frame=N (eye staging failed)` if the pair cannot be staged
    instead of falling back to a second read. The `ArtifactCapture capture_ms`
    line (#78) now covers the staging-failure case too.
  - `tests/eye_capture_tests.cpp` (new CTest `eye_capture_helpers`, x86 + x64):
    header layout/orientation, the R/B row swizzle, padded-`RowPitch` mean luma,
    alpha-invariance, signature determinism + change, and that the saved row's
    texels are the sampled row's texels.
  - `tools/verify_reported_fixes.py`: the GH #72 req-2 invariant updated to
    `StageEyePair`/`SampleEyeImage`, plus a GH #79 invariant (staging, the two
    adjacent copies, no `SampleEyeContent(`/`SaveTextureToBMP(`, the CTest
    wired); summary now `#75-79`.
  - `docs/adr/0465-eye-capture-single-staged-pair.md` (new) + ADR-0449 amended +
    `src/host/README.md` + `docs/vr-scenarios.md`.
- **Evidence (offline):** `build-all.ps1 -Config Release` OK (**no `-Deploy`**);
  CTest **21/21 PASS on x64 AND x86** (new `eye_capture_helpers`); all 11
  `tools/test_*.py` PASS; `xrsim.py --selftest` PASS; `verify_test_artifacts` +
  `verify_hook_init` + `validate_addresses` + `verify_reported_fixes` +
  `scenario_tests.py --check` PASS.
- **Evidence (live, allowed exception `RIDDICKVR_SIM=1`, fresh
  `dist\riddickvr-host.exe`; in-game hook stale at 12:47 as expected):** the new
  path was exercised end-to-end with `--dump-branch any --frames 1500
  --interval 60 --downscale 4` — **26 captures written from the staged pair**
  (0 `eye staging failed`), `capture_metadata_present` PASS (26 transforms, eye
  views=sim, dev 0.009°), and a decoded BMP is valid (`BM`, 26 837 046 =
  54 + 2496*2688*4, off 54, w 2496, h -2688, bpp 32). `capture_ms` for the new
  path: **n=26 min 83.6 mean 92.7 max 132.1 ms**, vs wake 61's old path
  **n=83 min 104.6 mean 112.0 max 153.9 ms** (same 26.8 MB/eye write; content
  does not change the capture cost, the intervals differ 60 vs 30).
- **Blocker / honest limit (the loud one):** I could **not** re-measure the
  gameplay-only window the issue asks for, because **this session's sim no longer
  reaches live gameplay at all**: 4 `--reach-gameplay` runs (90/120/180/240 s,
  up to **75 750** host frames) produced **0 gameplay frames** — the hook's only
  `DisplayMode` line is `CINEMA screen … (3Dfresh=0 menuCursor=1)`, i.e. the game
  is sitting at a **cursor menu** that never advances. `--dump-gameplay-only`
  therefore gates out every frame, so `frame_budget`/`frames_captured`/
  `gameplay_full_bleed_reached` FAIL and the 3 GH #79 checks cannot be graded.
  This is **not** caused by this change: the capture path only runs once the
  published branch is gameplay, and it never was (the change is downstream of the
  gate). New diagnosis on **GH #71**: in sim mode *no input can reach the game* —
  `camera_hook.cpp`'s `SendKey` returns at `SimModeNoInput()` **before** its
  `PostMessage`, and `gesture_tap.cpp`'s posting path needs `s_gestureHwnd`,
  which **`SetGestureGameHwnd` is never called to set** (only declared/defined).
  So the harness's only route to gameplay is the game auto-advancing its splash →
  intro, which this session's menu does not. Related, still open: the hook never
  calls `WaitForFrameConsumed` (#79 hook half, deploy-gated).
- **Next:** the host half of #79 is landed; the hook `WaitForFrameConsumed` half
  plus the sim input posting (`SetGestureGameHwnd`, and letting `camera_hook`'s
  sim `SendKey` post) are both hook changes that need the one interactive deploy.
  Offline, the next highest-value item is #76 step 1 (per-eye view/projection),
  also deploy-gated. If an offline slice is wanted instead: extend the harness to
  make the reach failure loud/attributed (it already is) or pick up #61's
  `.xtc` diffuse decode.

## Wake 61 - 2026-10-17 21:35 (scheduled loop)
- **Unit:** filed **GH #78** and fixed it - the XRSIM frame budget (GH #68
  req 2) was measuring the *harness's own artifact capture* as a 90 Hz render
  regression, leaving the committed baseline permanently red for a reason that
  is not a game/hook cost. Host + harness evidence + docs; **no asset change, no
  deploy**.
- **Why this unit:** #68 is the top open item and the report's only FAIL was
  `frame_budget`; wake 60 recorded it as "the known sim frame_budget" and moved
  on. Auditing the sampling found the harness grading itself.
- **Decisive finding (measured, not inferred):** the mock host writes two
  **26.8 MB** BMPs (2496x2688 x 32 bpp) plus `display_branch.log`,
  `eye_frames.log` and `frame_####.json` every `--dump-interval` frames, on the
  same thread and inside the loop iteration whose delta is sampled. Fresh
  `tests/xrsim/artifacts/host.log`: `p50_ms=3.50` (game/hook cadence) while
  `p95_ms` oscillated with the dump cadence (`35.33, 5.71, 21.03, ... 16.89,
  ... 104.83`) and `over_budget` incremented 1-2 per 30-frame window. The new
  `ArtifactCapture` log measured the capture at **112.0 ms mean**
  (104.6-153.9 ms, 83 dumps) - every excluded sample was real harness work.
- **Change:**
  - `src/host/frame_time_stats.h`: `FrameTimeExcludingHostWork(loopDeltaMs,
    hostWorkMs)` = `max(0, delta - work)`, with the sample documented as the
    rendered-frame cadence; a delta that is entirely host work returns 0 so the
    caller skips it instead of recording a fake fast frame.
  - `src/host/openxr_manager.cpp`: times the whole capture-decision block
    (content sample + BMPs + sidecars) and carries `lastHostWorkMs` into the
    next budget sample; the raw `dtMs < 1000` idle-gap guard is unchanged, so
    real stalls (level load, shader compile) stay in `max_ms`/`over_budget`.
    Every dump now logs `[Mock Host] ArtifactCapture frame=N capture_ms=X` so
    the exclusion is visible.
  - `tests/frame_time_tests.cpp`: subtraction, no-work pass-through, clamp to 0,
    plus a fixture that reproduces the old failure (3.5 ms cadence with 104 ms
    captures on >5% of frames fails p95) and passes after the measured capture
    is removed - while the sustained-regression fixture still fails.
  - `docs/adr/0464-frame-budget-excludes-host-capture.md` (new),
    `src/host/README.md`, GH #78 invariant in `tools/verify_reported_fixes.py`
    (summary now `#75-78`).
- **Evidence:** build-all Release (**no `-Deploy`**) OK; CTest **20/20 on x64
  AND x86**; all 10 `tools/test_*.py` + `xrsim.py --selftest` +
  `verify_test_artifacts` + `verify_hook_init` + `validate_addresses` +
  `verify_reported_fixes` + `scenario_tests.py --check` PASS. **Live**
  (`--reach-gameplay --duration 90 --interval 30 --downscale 4`): 83 gameplay
  frames, `frame_budget` **PASS** `n=2700 p50=1.90 p95=4.96 max=86.22` (before
  the fix the same run's tail read `p50=3.51 p95=104.83`); `eye views=sim`,
  `eye_views_scene_faithful` PASS (0.009 deg / 0.065 m); comfort vignette PASS.
  `tests/xrsim/report.json` regenerated and **committed** (mode=run, 83
  gameplay frames).
- **The regenerated report exposed a second, bigger defect (filed GH #79):**
  3 checks fail on 2 of 83 frames - `frame 540` has a **black right eye**
  (`meanL=40.53 meanR=0`) even though `EyePairIsBlank` had just sampled it as
  non-blank, and `frames 810/1770` fail `no_vertical_disparity` (`dy=-3/+7`).
  A quadrant probe shows a **global** vertical offset on 810 (top -5/-6,
  bottom -3, center -4 at downscale 4) and an **intra-image tear** on 1770
  (top dy=0, bottom/center dy=8) - not content parallax. Root cause found in
  the code: the host calls `SignalFrameConsumed()` but the hook **never calls
  `WaitForFrameConsumed()`**, so the hook overwrites the shared eye textures
  while the host reads them. This also corrupts #76's `stereo_kind` (5 frames
  labelled `parallax`, impossible for the mono blit).
- **Blocker / honest limit:** the fix for #79's hook half is deploy-gated (the
  sim still runs the 12:47 build). The committed report is therefore honestly
  red on those 3 capture-integrity checks; the old 10-frame baseline masked
  them. #78's own check is green and the exclusion is measured/logged.
- **Next:** #79 (hook waits on the consumed event before the next eye blit;
  host staging-copy fallback), then #76 step 1 (per-eye view/projection). The
  loop-wide deploy gate is unchanged (#67 step 3 / #72 req 1 A/B / #68 req 5 /
  #79 hook half).

## Wake 60 — 2026-10-17 21:05 (scheduled loop)
- **Unit:** GH **#77 capstone** (wake 59's stated Next) — the mock/sim host now
  **reconstructs** the per-eye views a runtime *would* return for each dumped
  frame's scripted head pose and tags them `eye=sim`, so a headless run produces
  scene-faithful eye data and the `eye_views_scene_faithful` graded branch finally
  runs (it previously only ever skipped). Host + harness + tests + docs; **no
  asset change, no deploy, shared state untouched**.
- **Why this unit:** every player-visible ladder item is code-complete and
  deploy-gated, #68 is the top open item, and the wake-59 record was honest but
  left the graded branch reachable only on a headset.
- **Change:**
  - `src/host/eye_frame.h`: new pure helpers `QuatToRowMajor3x3`,
    `ViewMatrixFromPose` (the rigid inverse; `CreateViewFromPose` now forwards to
    it) and `ComputeEyeViewFromHeadPose(headPos, headQuat, halfIpd, rightEye)`;
    `EyeFrameSample.viewsFromRuntime` (bool) → `viewSource ∈ {Runtime, Simulated,
    Synthetic}`; the wire token is now `eye=runtime|sim|synthetic`.
  - `src/host/openxr_manager.cpp`: the mock dump writes `eye=sim` and reconstructs
    `viewL/viewR` from the frame's own `state.hmdPose`; `frame_####.json`
    `leftEyeView/rightEyeView/"eyeViews"` follow. **The published shared-state
    eye views are unchanged** — the hook still consumes the placeholder baseline,
    so the sim's rendering / procedural-hand overlay do not move.
  - `tools/xrsim.py`: `EYE_SCENE_FAITHFUL_SOURCES = ("runtime","sim")`; the
    `eye_frames` key gains `view_lr_ipd_m`; the check grades rotation deviation
    (≤ 1°) **and** eye separation (0.03–0.10 m) so a broken reconstruction fails
    instead of passing vacuously; `synthetic`/legacy still skips.
  - **Found + fixed two latent ADR-0462 numerical bugs** (first time the check ran
    against host-convention views):
    1. the deviation used `R_view * R_head^T` (= `R_head^-2`) — the host stores
       `R_head^T`, so it must be `R_view * R_head`; the old unit-test fixture
       stored `R_head` (transposed) and hid it. Fixed + a convention-pinning test.
    2. `acos((trace-1)/2)` is ill-conditioned near identity, so the 4-decimal wire
       rounding read as **0.436 deg**; replaced with the `atan2(‖skew‖/2, …)` form
       → **0.009 deg** live, so the now-graded sim check cannot flake at 1°.
  - Tests: `tests/eye_frame_tests.cpp` (source tokens + reconstruction
    rotation/translation/IPD), `tools/test_xrsim.py` (+`test_eye_view_scene_faithful_grades_sim`,
    +`test_view_head_deviation_uses_transpose_convention`, fixture now stores the
    host convention), `tools/verify_reported_fixes.py` #77 invariant rewritten;
    `docs/adr/0463-simulated-eye-views-in-the-record.md` (new) + ADR-0462 amended +
    `src/host/README.md` + `docs/vr-scenarios.md`.
- **Evidence:** `build-all.ps1 -Config Release` OK (**no `-Deploy`**); CTest
  **20/20 on x64 AND x86**. All 10 `tools/test_*.py` + `xrsim.py --selftest` +
  `verify_test_artifacts` + `verify_hook_init` + `validate_addresses` +
  `verify_reported_fixes` + `scenario_tests.py --check` (8 test cases, 12 scripted,
  1 observation-only) PASS. **Live** (`RIDDICKVR_SIM=1`,
  `--reach-gameplay --duration 90 --interval 30 --downscale 4`): 27 gameplay
  frames, 0 cinema/blank, all stereo PASS, `capture_metadata_present` … `eye
  views=sim`, and `eye_views_scene_faithful` **PASS** (`deviation 0.009 deg, tol
  1.0; L/R separation 0.065 m`). The run's only FAIL is the known sim-install
  `frame_budget` (p95 104.8 ms at the sim's ~10 FPS) — not this change. Did **not**
  commit a regenerated `report.json` (the committed wake-58 baseline is untouched;
  the captures stay in the gitignored artifacts dir).
- **Blocker / honest limit:** `eye=sim` is a reconstruction, so the pass proves the
  harness path + the reconstruction's structure, **not** that the sim rendered a
  head-relative stereo pair (GH #76's hook work). `eye=runtime` still needs a
  headset; `synthetic` is now only the real-session `xrLocateViews` fallback.
- **Next:** same deploy gate (#67 step 3 `ViewmodelObserve=1`, #72 req 1 A/B,
  #68 req 5 snap-turn/HUD, GH #76 per-eye rendering). With the eye record now
  faithful in sim, the offline candidates are thin; #76 step 1 (per-eye
  view/projection) remains the highest-value hook item.

## Wake 59 — 2026-10-17 20:35 (scheduled loop)
- **Unit:** filed **GH #77** and landed its first slice — the XRSIM eye-frame
  record (`eye_frames.log`, #68 req 1) presented the host's **synthetic** eye
  baseline as "the per-eye view/projection that produced the dumped frame"; the
  harness now tags each record's provenance and stops treating a placeholder as a
  real eye view. Host + harness + docs + tests; **no asset change, no deploy**.
- **Why this unit:** every ladder item (#67 step 3, #64, #61, #65, #62, #63) is
  code-complete and deploy-gated, and #68 is the top open item — the prompt says
  to *extend it to cover whatever you are fixing*. Auditing #68 req 1 found the
  record's central claim is false in every mock run.
- **Decisive finding (read-only + live, not inferred):** in mock mode the host
  never calls `xrLocateViews`. `Initialize()` returns early
  (`openxr_manager.cpp:623-628`), so `UpdateTracking`'s real branch (`:823`,
  `:1007`) never runs; the mock `RunLoop` fills `left/rightEyeView` +
  `left/rightEyeProjection` with a **constant synthetic baseline**
  (`:1679-1697`: identity rotation, `m[12] = -/+ 0.0325`, `projL == projR`), and
  writes those verbatim to `eye_frames.log` (`:1993-1996`) / `frame_####.json`
  (`:2028-2030`). Observed file: `viewL`/`viewR` rotation = identity on every
  line while `hmdquat` changes with the scripted keyframe → the "eye view" has
  **no head rotation** and is independent of the head. `eye_frame.h:3-13` and the
  `xrsim.py` docstring overstated it.
- **Change:**
  - `src/host/eye_frame.h`: `EyeFrameSample.viewsFromRuntime`; every line now ends
    `eye=runtime|synthetic` (`FormatEyeFrameLine`), header comment corrected.
  - `src/host/openxr_manager.{h,cpp}`: `m_eyeViewsFromRuntime` set `true` only in
    the `xrLocateViews` success branch (`:1013-1017`), `false` in the fallback
    (`:1030`); the dump site records it in the log and as `"eyeViews"` in
    `frame_####.json` (+ the missing `rightEyeProjection`).
  - `tools/xrsim.py`: parse the optional token (`source`, default `unknown`);
    `eye_frame_stats` reports `source`, `view_head_rotation_deviation_deg`
    (`_view_head_rotation_deviation_deg`: angle between `viewL`'s rotation and the
    head pose — ~0 faithful, the head's own rotation for the identity baseline),
    `proj_lr_identical`; `capture_metadata_present` detail now says
    `eye views=<source>`; new graded check `eye_views_scene_faithful` — **passes**
    a `runtime` capture whose views track the head pose (tol 1°), **fails** a
    `runtime`-labelled but inconsistent one, **skips** (with the deviation as
    detail) a `synthetic`/`unknown` one. No default-run verdict change.
  - `tests/eye_frame_tests.cpp`: 8 fields, `eye=` token, both values, determinism.
  - `tools/test_xrsim.py`: +3 tests (provenance parse + deviation; skip-synthetic;
    grade-runtime incl. the fail case); 2 existing eye-frame tests extended.
  - `tools/verify_reported_fixes.py`: GH #77 invariant; summary now `#75-77`.
  - `docs/adr/0462-eye-frame-view-provenance.md` (new) + a `docs/vr-scenarios.md`
    note.
- **Evidence:** `build-all.ps1` (no deploy) OK; CTest **20/20 on x86 AND x64**.
  All 10 `tools/test_*.py` + `xrsim.py --selftest` + `verify_test_artifacts` +
  `verify_hook_init` + `validate_addresses` + `verify_reported_fixes` +
  `scenario_tests.py --check` (8 test cases / 12 scripted / 1 observation-only)
  PASS. **Live:** `python tools/xrsim.py --reach-gameplay --duration 180
  --interval 60 --downscale 4` reached gameplay and the report shows
  `capture_metadata_present: … eye views=synthetic` and
  `eye_views_scene_faithful [SKIP]: … deviates … by up to 20.907 deg … placeholder
  … (GH #77)` — the measured 20.9° is exactly the scripted head yaw the identity
  baseline ignores.
- **Blocker / honest limit:** the `eye=runtime` graded branch is only exercised on
  a real headset, so the sim run proves the *skip* + the deviation, not the pass.
  I did **not** commit a regenerated `report.json`: the clean 180 s run's only
  extra FAIL was a truncated in-flight final capture (harness reports it, never
  crashes), and the committed wake-58 baseline is a clean 10-frame live capture —
  the new check skips on its token-less `eye_frames` by design
  (`test_eye_frame_check_skipped_without_file`), so the baseline stays coherent and
  the diff stays focused. Loop-wide deploy gate unchanged (#67 step 3, #72 req 1
  A/B, #68 req 5).
- **Next:** the record is honest; the loudest remaining offline thread is that the
  mock never produces a scene-faithful eye view at all (`eye=synthetic` everywhere)
  — a real capstone would derive the per-eye view from the scripted head pose in
  the *record* (not the shared state) so a sim run can grade `runtime`-equivalent
  data. Otherwise the ladder is still deploy-gated (#67 step 3).

## Wake 58 — 2026-10-17 20:10 (scheduled loop)
- **Unit:** filed **GH #76** (native stereo is a shifted monoscopic capture — epic
  #57 **definition-of-done item 1**, previously untracked: the epic table listed it
  as *"new"* and no issue existed) and landed its first, fully offline slice: the
  XRSIM harness now **measures the structure of the stereo disparity** instead of
  only asserting that one exists. Tooling + docs + report only; **no C++/asset
  change, no build, no deploy**.
- **Why this unit:** every player-visible ladder item (#67 step 3, #64, #61, #65,
  #62, #63) is code-complete and blocked on one hook deploy this loop must not
  perform, and #46/#52/#53/#54 are landed; #68 (iteration vehicle) is the top
  open item and the prompt says to *extend it to cover whatever you are fixing*.
  Auditing the epic's gap list found the single biggest VR gap untracked.
- **Decisive finding (read-only + live, not inferred):** the gameplay stereo is
  **one global horizontal shift of a monoscopic capture**. `stereo_renderer.cpp`
  `:811-812` captures the backbuffer once and blits it into both eye FBOs; `:932-960`
  says verbatim *"monoscopic capture has no IPD parallax (both eyes blit the same
  backbuffer). Apply a small half-IPD horizontal shift per eye"* — the only per-eye
  difference is `+/- (halfIpdPx + comfortPx)` on the blit source rect. Measured:
  a 4×4 interior tile re-alignment finds the **same** shift in **every** textured
  tile (spread **0 px**) on every bright gameplay frame — prior committed captures
  all `-92` / all `-56` full-res px; a fresh live `--reach-gameplay` re-run the same
  (`shift-only`). A genuine per-eye view would have depth-varying per-tile shifts.
- **Change:**
  - `tools/xrsim.py`: `tile_disparities()` (per-tile local re-alignment; the grid
    is **inset** by the search radius — an earlier version placed tiles flush to
    the edges, where only one shift direction is in bounds and the constraint faked
    a spread); `classify_stereo_kind()` (`parallax` >= 6 px spread / `shift-only`
    <= 2 / `mono` / `unknown`, reusing the ADR-0454 contrast-confidence rule so a
    dark frame is refused, never mis-called); `stereo_kind_stats()` (run verdict
    over the **measurable** gameplay frames; cinema is deliberately flat, GH #69);
    the verdict is the new `stereo_kind` report key; new `--require-parallax` flag
    (and scenario `"require_parallax": true`) adds the graded check
    `stereo_parallax_present`. **Default off** — the shift is the documented
    interim, so a default run's verdict is byte-for-byte unchanged.
  - `tools/test_xrsim.py`: +8 tests (shift-only, parallax, mono, flat→unknown, the
    aggregation/dedupe/unknown rules, the opt-in gate end-to-end, the scenario
    field, the report key); 2 new `--selftest` image cases.
  - `tools/verify_reported_fixes.py`: a GH #76 invariant; summary now `#75-76`.
  - `docs/adr/0461-stereo-is-a-shifted-monoscopic-capture.md` (new) + a
    `docs/vr-scenarios.md` note.
  - `tests/xrsim/report.json`: regenerated from a live `--reach-gameplay
    --duration 60 --interval 60 --downscale 4` run (mode=run, **60 checks**, 10
    gameplay frames) — it now carries `stereo_kind: {kind: shift-only,
    measurable_frames: 4, spread_px_max: 0}`.
- **Evidence:** live run reached gameplay (10 frames, 0 blank) and the only FAIL is
  the known sim-install `frame_budget` (p50 ~104 ms at the sim's ~10 FPS); all
  stereo gates PASS yet `stereo_kind = shift-only` — i.e. the old gates could not
  see the gap, which is the point. `--require-parallax` on those captures **fails**
  naming the shift; it passes on a synthetic per-column disparity ramp. All 10
  `tools/test_*.py` + `xrsim.py --selftest` + `verify_test_artifacts` +
  `verify_hook_init` + `validate_addresses` + `verify_reported_fixes` +
  `scenario_tests.py --check` (8 test cases / 12 scripted / 1 observation-only)
  PASS. No C++ changed → no `build-all` run.
- **Blocker / honest limit:** the *renderer* is not fixed here — GH #76 needs
  per-eye view/projection (or a depth-based reprojection), which is a hook change
  validated by one deploy (and a headset for the visual). Also honest: on this
  scripted run only 4 of 10 gameplay frames are bright enough to measure the
  disparity structure (the rest are dark → `unknown`), so the run verdict is taken
  over the measurable frames and the count is reported.
- **Next:** GH #76 step 1 (per-eye camera offset / two 3D renders) is the highest
  value now that it is tracked+measured, but it needs the deploy. Otherwise the
  remaining offline candidates are thin; the ladder is still deploy-gated
  (#67 step 3, #72 req 1 A/B, #68 req 5 snap-turn/HUD).

## Wake 57 — 2026-10-17 19:49 (scheduled loop)
- **Unit:** GH **#73 requirement 3** — add a guard so the large XRSIM eye-capture
  BMPs cannot be silently re-added to git, and record the decision in an ADR.
  Tooling + docs only; **no C++/asset change, no build, no deploy**.
- **Why this unit:** #73 was the newest open item whose remaining requirements
  were all offline/loop-safe (no deploy). Requirement 1 (`git rm --cached`,
  `20b6995`) and requirement 2 (`report.json` regenerated from a live
  `--reach-gameplay` run during the #74 reconciliation, wakes 52–53) are already
  on `main`; only req 3 remained. It is also the top-of-list #68/#73 harness
  hygiene work that the standing ladder names when no deploy is possible.
- **Change:**
  - `tools/verify_test_artifacts.py` (new, dependency-free, headless): FAILS
    (exit 1) unless (a) `git ls-files tests/xrsim/artifacts` is empty — no
    capture tracked, any extension; (b) `.gitignore` still excludes
    `tests/xrsim/artifacts/`; (c) `tests/xrsim/report.json` exists and is valid
    JSON (the diffable live baseline). SKIPs with exit 0 when git is absent or
    the tree is not a work tree, so a lean runner is never broken.
  - `.github/workflows/vr-worker.yml`: new CI step "XRSIM eye captures stay
    untracked (GH #73)" running the guard, right after the GH #68 XRSIM step.
  - `tools/verify_reported_fixes.py`: a GH #73 invariant (guard exists + consults
    `git ls-files`; `.gitignore` rule present; CI lane invokes it), and the
    summary line now reads `#71-73`.
  - `docs/adr/0460-xrsim-eye-captures-untracked.md` (new): context (26 BMPs /
    665 MB / **3 unique blobs** committed by `b97c4a8` before the ignore rule),
    decision (captures are evidence, only `report.json` is committed; a guard +
    invariant make a re-add loud), consequences.
  - `docs/vr-scenarios.md`: an "Artifacts (GH #73, ADR-0460)" note in the XRSIM
    section.
- **Evidence:** `python tools/verify_test_artifacts.py` → exit 0 at HEAD.
  **Positive control:** `git add -f tests/xrsim/artifacts/right_0720.bmp` then the
  guard → **exit 1** naming the path (then `git rm --cached` restored; tree
  clean). `verify_reported_fixes.py` (with the new invariant) PASS;
  `scenario_tests.py --check` → `manifest valid (8 test cases, 12 scripted
  profiles, 1 observation-only)` exit 0; `verify_hook_init` + `validate_addresses`
  PASS; all 10 `tools/test_*.py` PASS; `xrsim.py --selftest` PASS. Confirmed the
  committed `tests/xrsim/report.json` is the live union baseline (`mode=run`, 40
  checks, single FAIL = the known sim-install `frame_budget` at ~10 FPS), i.e.
  #73 req 2 is satisfied. Commit `176f4fe` (guard + wiring + ADR) then this
  state commit.
- **Blocker / honest limit:** none for this unit. The loop-wide deploy gate is
  unchanged (#67 step 3, #72 req 1 A/B, #68 req 5 `comfort_vignette_applied`
  snap-turn/recenter + HUD-clipping all still need one interactive hook deploy,
  which this loop must not perform).
- **Next:** with #73 req 1/2/3 all landed, #73 is code-complete (can be closed
  once the ADR-0460 decision is accepted). Remaining offline candidates: a fresh
  #68 slice needs hook signals (snap-turn/recenter, gross HUD clipping) so it is
  deploy-gated; otherwise the top deploy-gated item is #67 step 3
  (`DriveEngineViewmodel=1` + `ViewmodelObserve=1`). #64/#61 native-hand passes
  also want a headset.

## Wake 56 — 2026-10-17 19:34 (scheduled loop)
- **Unit:** GH **#72 requirement 1** — the published display branch
  (`cinemaMode`) labelled the engine's **static 2D startup pages** (legal/ESRB)
  as `FULL-BLEED gameplay`, so the XRSIM gameplay-only dump gate and the
  per-frame stereo skip graded a frozen splash. Implemented the fix shape the
  issue itself records (ADR-0458-era note): a **cold-start full-bleed latch**
  driven by the hook's own `BarScan` active-rect fill, behind a **default-off**
  INI knob. Hook C++ + config + tests/tools/docs; **no deploy, no game/asset
  change**.
- **Why this unit:** it is the *last open requirement* of #72 and #48 called it
  "the top remaining item" of the #68 iteration vehicle; the standing prompt
  ranks #68 (iteration vehicle) above the player-visible hand items, and
  #67 step 3 / #64 / #61 / #65 / #62 / #63 are all hook changes whose live
  validation needs one interactive deploy this loop must not perform. The
  ladder is now: #72.1 (this) → the same deploy.
- **Change:**
  - `src/hook/display_branch.h` (new, pure/dependency-free):
    `ComputeDisplayBranch(has3d, menuCursor, activeFill, haveFill, cfg, state)`.
    Decisive thresholds `fullBleedFill=0.90` / `letterboxFill=0.75` with a *hold
    band* between them (the smoothed scan cannot flicker the branch). The
    `fullBleedSeen` latch is set only by a **3D, non-menu** full-bleed frame and
    is never reset, so the demotion fires only *cold-start* — GH #44 (real
    gameplay with engine-baked bars) is unaffected after the first gameplay
    frame. Legacy signals (`!Has3DThisFrame`, sustained menu cursor) unchanged.
  - `src/hook/stereo_renderer.cpp`: the branch decision is now
    `ComputeDisplayBranch(...)`. With the latch **on** the BarScan rect is
    measured *before* the decision and in *every* frame (otherwise a demoted
    page would stop scanning and latch forever — the reason the naive fix would
    hang); with it **off** the scan stays exactly where it was (gameplay branch
    only) and the verdict is byte-for-byte the legacy one. The `DisplayMode`
    line gains `latch= fill= fullBleed=` for the live A/B.
  - `vr_config.{h,cpp}` + `riddick_vr.ini` + `tools/riddick_vr.ini.example`:
    `DisplayBranchColdStartLatch` (default 0) parsed/logged/documented.
  - `tests/display_branch_tests.cpp` (new) + CTest
    `display_branch_cold_start_latch` (x86 **and** x64): latch-off legacy;
    no-fill legacy; splash demoted; splash never sets the latch; first
    full-bleed latches and later bars stay gameplay (#44); hold band; in-between
    fill does not demote without a prior verdict; non-3D/menu never latch.
  - `tools/verify_reported_fixes.py`: the #43 invariant updated to the helper
    call, plus a new GH #72 req-1 invariant (helper + knob + shipped INI +
    test wired).
  - `docs/adr/0459-display-branch-cold-start-latch.md` (new); `src/hook/README.md`
    section; `docs/vr-scenarios.md` #72 note annotated as **addressed**.
- **Evidence (offline):** `build-all.ps1 -Config Release` OK (x64 host + x86
  `opengl32.dll`; **no `-Deploy`**); CTest **20/20 PASS on x64 AND x86** (new
  `display_branch_cold_start_latch`); all 10 `tools/test_*.py` PASS;
  `xrsim.py --selftest` PASS; `validate_addresses` + `verify_hook_init` +
  `verify_reported_fixes` + `scenario_tests.py --check` (8 test cases, 12
  scripted profiles, 1 observation-only) PASS. Commit `8f4d824`.
- **Blocker / honest limit:** the change is **hook-side and off by default**, so
  it cannot be live-validated by this loop (the sim install's hook is the 12:47
  build; deploying is a hard rule). No live run was attempted *because* it would
  exercise the stale hook, not this code. The one live A/B owed: set
  `DisplayBranchColdStartLatch=1`, run
  `python tools/xrsim.py --reach-gameplay --duration 60`, and confirm from
  `display_branch.log` + `DisplayMode: ... latch=1 fill=.. fullBleed=..` that the
  splash pages are cinema and every retained frame is full-bleed gameplay, and
  whether a pre-gameplay full-bleed page sets the latch early (the documented
  failure mode). Thresholds 0.90/0.75 are empirical (splash 0.31-0.36 vs
  gameplay 1.00).
- **Next:** #72 is now code-complete (req 2 + req 3 landed earlier, req 1 here)
  pending that deploy. Remaining deploy-gated ladder, in priority order:
  #67 steps 3-4 (`DriveEngineViewmodel=1` + `ViewmodelObserve=1`), #64/#61
  headset passes, #65/#62 threshold validation, `comfort_vignette_applied`. If
  no deploy is possible, the next offline candidates are #73 req 3 (guard the
  XRSIM BMPs against re-tracking + ADR) or a fresh #68 slice (snap-turn/recenter
  + gross HUD clipping need hook signals).

## Wake 55 — 2026-10-17 19:22 (scheduled loop)
- **Unit:** GH **#75** — `python tools/scenario_tests.py --check` (the CI scenario
  lane *and* the loop's own validation step) was **red on `main`**: `hands_view.json`
  is an idle inspection pose with no interactive gesture, so it has no hook marker,
  yet the validator required non-empty `markers`/`markers_any`. Tooling + docs only;
  **no C++/asset change, no build, no deploy**.
- **Why this unit:** #75 is the newest open item, it blocks a mandatory step of every
  wake's validation, and it is deploy-free (unlike #67 step 3 / #72 req 1, which
  need a hook deploy the loop is forbidden to do).
- **Change (opt-out, option 1 in the issue):**
  - `tools/scenario_tests.py`: a new optional boolean `"observe"` (non-bool →
    hard error) skips **only** the non-empty-markers requirement; every other check
    still runs (name/stem, keys, required fields, increasing frames, marker fields,
    declared markers present in `camera_hook.cpp`, scripted profile exists) and a
    profile with neither markers nor the opt-out is still rejected. `--check` now
    prints the observation-only count.
  - `tools/scenarios/hands_view.json`: `"observe": true` (keeps its empty `markers`).
  - `tools/test_scenario_tests.py` (new, headless): synthetic workspace covering the
    opt-out pass, markerless-without-opt-out rejection, non-bool `observe`,
    marker-absent-from-hook-source, missing scripted profile, non-increasing
    keyframes — plus a final case that runs the **shipped** `--check` (the #75 guard).
  - `tools/verify_reported_fixes.py`: a GH #75 invariant (opt-out handler + flag +
    new test exist).
  - `.github/workflows/vr-worker.yml`: the CI lane now runs `test_scenario_tests.py`
    before `scenario_tests.py --check`.
  - `docs/adr/0458-observation-only-scenario-profiles.md` (new); `docs/vr-scenarios.md`
    section; ADR-0457's red note annotated as resolved.
- **Evidence:** `scenario_tests.py --check` → **`manifest valid (8 test cases, 12
  scripted profiles, 1 observation-only)`** exit 0. `tools/test_scenario_tests.py`
  8/8 PASS; all 8 other `tools/test_*.py` exit 0; `test_xrsim.py` + `xrsim.py
  --selftest` PASS; `validate_addresses` + `verify_hook_init` +
  `verify_reported_fixes` (with the new invariant) PASS. Commit `d3dabf0`.
- **Also checked (#74, no change needed):** the wake 52–54 union is intact at HEAD —
  `tools/xrsim.py`/`src/host/openxr_manager.cpp`/`tools/verify_reported_fixes.py`
  contain `--reach-gameplay`, `display_branch`, `eye_frames`, `p50_ms`,
  `comfort_vignette` (xrsim) and `eye_frames.log` (host) and the #68/#71/#72
  invariants, so #74's code requirement is met; the only remaining #74/"#67" item is
  the deploy-gated live probe (#67 step 3).
- **Blocker:** none for this unit. The standing loop-wide limit is unchanged: #67
  step 3 / #72 req 1 / #68 req 5 (`comfort_vignette_applied`) still need one
  interactive hook deploy, which the loop must not perform.
- **Next:** now that `--check` is green, the top player-visible work is GH **#67**
  step 3 (needs a deploy) — otherwise advance #64 (`NativeHand: LEFT/RIGHT DRAWN`
  one-time per-hand draw log is in) or #61 (native hand `.xtc` diffuse), both of
  which can be extended and unit-tested without a deploy. #74 can be closed after one
  interactive deploy confirms the restored hook probe live.

## Wake 54 — 2026-10-17 19:14 (scheduled loop)
- **Unit:** GH **#74**, the remaining **hook half** (wake 53's stated Next) — finish
  the union so the GH #67 step-1/step-2 viewmodel machinery is restored *beside*
  PR #70's `track`/trace/`NearPlaneDrop` window and controller drive. Hook C++ +
  config + tests/tools/docs; **no deploy, no game/asset change**.
- **Why this unit:** #74 is the top open item and the two earlier wakes restored
  only its Python and host halves. The hook half was the last red: at HEAD
  `src/hook/opengl_proxy.cpp` had `s_modelviewTracker` gone and all four
  `NoteModelview*` entry points as **no-op linker stubs**, and
  `viewmodel_suppressor.h` had no `observe` mode — while `tools/xrsim.py`,
  `tools/test_xrsim.py`, `src/hook/README.md`, ADR-0450/0451 and the
  `gl_matrix_tracker` CTest still assumed them. GH #67 step 3 has no input without
  this restore.
- **Change (union):**
  - `viewmodel_suppressor.h`: `ViewmodelSuppressionConfig.observe` +
    `ViewmodelObservationSnapshot` + `NoteObservedDraw`/`SetObservedSample`/
    `ComputeSnapshot`/`NoteObservedPush`/`NoteObservedTranslate`/`Observing`
    restored; a private `Tracking()` (`Active() || observe`) now drives the window
    state machine so suppression, trace/drive and observation share one boundary
    (only `enabled` can skip a draw).
  - `opengl_proxy.cpp`: `s_modelviewTracker` + real `NoteModelview*` bodies
    restored; `wglMakeCurrent` resets the tracker; `BuildViewmodelSuppressionConfig`
    sets `cfg.observe`; the swap handler emits `ViewmodelProbe [#N]: inside=..
    outside=.. sample=.. pushes=.. translates=..` + the matrix line; the
    `GL_MODELVIEW` load feeds the tracker with the **effective** matrix (the drive
    correction when drive is active, else `m`); `ShouldSuppressDrawCall` records
    the observation before the unchanged suppression decision. The `viewmodelTrace`
    line and the drive branch are untouched.
  - `vr_config.{h,cpp}` + `riddick_vr.ini` + `tools/riddick_vr.ini.example`:
    `ViewmodelObserve` (default 0) is read/parsed/documented again; the config log
    line gains `observe=%d`.
  - `tests/viewmodel_suppressor_tests.cpp`: the GH #67 observation cases restored
    on top of the branch's `track`/trace/near-plane cases (open on depth-clear,
    in/out tallies, first-in-window sample, push/translate counting,
    observe-with-suppression, inert-without-observe).
  - `tools/verify_reported_fixes.py`: the GH #67 step-2 + step-1 invariants
    re-added; `xrsim_tool` now read once near the top so the earlier block resolves
    it.
  - `docs/adr/0457-viewmodel-hook-union-restores-gh67-observation.md` (new).
- **Evidence:** `build-all.ps1 -Config Release` OK (x86 `opengl32.dll` + x64 host;
  **no `-Deploy`**); CTest **19/19 PASS on x64 AND x86**; all 9 `tools/test_*.py`
  PASS; `xrsim.py --selftest` PASS; `validate_addresses` + `verify_hook_init` +
  `verify_reported_fixes` (with the re-added #67 invariants) PASS. Commit
  `8c873f6`.
- **Blocker / honest limit:** the restore is **hook-side and off by default**
  (`ViewmodelObserve=0`), so it cannot be live-validated by this loop — the probe
  only appears in a hook build deployed to the game install, and deploying is
  forbidden (the sim install's hook is stale). The tracker is forwarding-only: it
  changes only what `GetLastViewMatrix()` reports, never what the engine renders.
  `scenario_tests.py --check` is still red on `hands_view.json` (GH **#75**,
  pre-existing, unchanged).
- **Next:** with the hook half reconciled, #74 is essentially closed pending one
  interactive deploy: set `ViewmodelObserve=1` and read
  `ViewmodelProbe [#N]: ... pushes=.. translates=..` + the matrix from a 3D
  gameplay frame (confirms the pass boundary + the composed viewmodel transform),
  which is GH #67 step 3's input. Otherwise #75 (make the scenario validator
  tolerate an idle inspection profile), or regenerate `tests/xrsim/report.json`
  once a deterministic gameplay reach exists (#71/#72).
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 53 — 2026-10-17 19:05 (scheduled loop)
- **Unit:** GH **#74**, the **host half** (wake 52's stated Next) — reconcile the
  pr-70 regression so the repo's host-side XRSIM capabilities (GH #68/#71/#72)
  are back *and* the branch's conveniences are kept (the issue's preferred
  "union"). Host C++ + CMake + tests/tools/docs; **no deploy, no game/asset
  change**.
- **Why this unit:** #74 is the top open item and wake 52 landed only the Python
  half (`tools/xrsim.py` + its test). The union's host half
  (`src/host/openxr_manager.cpp`) and the `verify_reported_fixes.py` invariants
  were still missing, so a live run's `frame_budget`/`display_branch`/
  `eye_frames`/`content_gate`/`comfort` signals had no host to emit them and no
  invariant to guard them.
- **Change (union):**
  - `src/host/openxr_manager.cpp`: restored the dump-gate banner; per-dump
    `display_branch.log` (GH #69) + `eye_frames.log` pose/projection (GH #68
    req 1); the content gate (GH #72 req 2); the p50/p95 `FrameBudget`
    (GH #68 req 2) **with** the branch's `gameplay_only=1` suffix + cinema
    gating; the `ComfortVignette` summary (GH #68 req 5). Kept the branch's
    gameplay-gated scenario start (`--scenario-immediate`, `m_scenarioStartFrame`
    /`m_gameplayStreak`) and the per-frame `frame_*.json` sidecar.
  - `src/host/dump_gate.h` + `tests/dump_gate_tests.cpp`: **both-eye rule** —
    `EyePairIsBlank`; a frame with one empty eye is `Blank`, not graded.
  - `CMakeLists.txt`: re-added the host CTests (`dump_gate_gameplay_only`,
    `eye_frame_record`, `frame_time_percentiles`, `comfort_vignette_summary`) +
    `gl_matrix_tracker` + `native_hand_skin_mapping` + the
    `src/hook/gl_matrix_tracker.h` source entry.
  - `tools/verify_reported_fixes.py`: re-added the #69/#71/#72/#68-req1/2/5/#61
    invariants (union with the branch's #63/#66/#67 block). One string updated to
    the shipped harness: `read_fresh_hook_log(log_file, log_mark)` (the restored
    harness names the path `log_file`, not a `LOG_FILE` constant).
  - `docs/adr/0449-...md`, `src/host/README.md`: document the both-eye rule.
- **Change (found while live-validating — the both-eye gate):** the restored gate
  sampled only the left eye, so a gameplay-labelled frame whose **right** eye was
  still all-black (interop warm-up / level transition) entered the graded window
  and produced a spurious stereo FAIL — measured live: **frame 1320,
  `meanL=35.62 meanR=0`** (`both_eyes_non_black` + `interop_produced_content`).
  `EyePairIsBlank` skips such frames; unit-tested.
- **Evidence:**
  - **Live** (allowed exception, `RIDDICKVR_SIM=1`, fresh `dist\riddickvr-host.exe`;
    the F: sim install's deployed hook is the 12:47 build — unchanged hook source
    this wake):
    - `--reach-gameplay --duration 40 --interval 60 --downscale 4`: **97 gameplay
      frames / 0 cinema / 0 blank**, all 97 stereo groups PASS, `scene_advanced`
      PASS, `gameplay_window_has_content` PASS (0 blank of 97),
      `gameplay_full_bleed_reached` PASS (BarScan fill 1.00),
      `capture_metadata_present` PASS (97 transforms, yaw span 39.98°),
      `comfort_vignette_applied` PASS (max=1.000), `frame_budget` PASS on
      `--evaluate-only` (n=6000 p50=2.49 p95=4.69, budget 11.11). The only FAILs
      were the frame-1320 empty-eye frame (fixed above).
    - A second graded run (`--duration 60 --interval 60`): **6 gameplay frames**,
      all stereo PASS, `display_branch` all gameplay, `eye_frames` 6 distinct.
      Its single FAIL is `frame_budget` (**n=330 p50=96.2 p95=106.5** — the sim
      install renders in-game at ~10 FPS on this machine, so the *live* 90 Hz
      cadence genuinely misses; an honest performance result, not a wiring bug).
      `tests/xrsim/report.json` now holds this run (the first **union-format**
      baseline; the committed one was the legacy 586-line capture).
  - Offline: `build-all.ps1 -Config Release` OK (x64 host + x86 hook; **no
    `-Deploy`**); CTest **19/19 PASS on x64 AND x86**; all 9 `tools/test_*.py` +
    `validate_addresses` + `verify_hook_init` + `verify_reported_fixes` +
    `xrsim.py --selftest` PASS. Commit `3de4585`.
- **Blocker / honest limit:** the runtime install is **nondeterministic about
  reaching non-cinema gameplay** in a 40-60 s window: the 45 s run captured 0
  frames (the hook published `cinemaMode=1` throughout — the open #71/#72 issue),
  while the 40 s/60 s runs captured 97/6. This is *not* a host-half defect (the
  banner, the comfort line, and the p50/p95 budget line all appear; the log shows
  `glLoadMatrixf [3D VR PROJECTION]` yet `cinemaMode` stays latched). **#71
  requirement 1 / #72 requirement 1 remain the reach-gameplay signature and are
  hook-side.**
- **Remaining #74 gaps (hook-side, deliberate non-overreach):** the merge also
  replaced `src/hook/opengl_proxy.cpp` (`NoteModelview*` are now **no-op stubs**;
  `s_modelviewTracker` gone) and `src/hook/viewmodel_suppressor.h` (no
  `ViewmodelObservationSnapshot`/`ComputeSnapshot`/`observe`), so GH #67
  step 1 (software modelview stack wired) and step 2 (viewmodel observation
  probe) are still clobbered; their invariants are intentionally **not** re-added
  yet. The `gl_matrix_tracker` header/CTest survive and are re-registered, but the
  proxy does not feed the tracker.
- **Second finding (pre-existing):** `python tools/scenario_tests.py --check` is
  still red on `tools/scenarios/hands_view.json` (GH **#75**, empty markers) —
  unchanged by this wake.
- **Next:** re-merge the hook half of #74 — start from
  `git show f5fd7f7:src/hook/opengl_proxy.cpp` +
  `src/hook/viewmodel_suppressor.h` and keep pr-70's `viewmodel_drive`
  (ADR-0446/0447) as the union, then re-add the #67 step1/2 invariants; or fix
  #75 (make the scenario validator tolerate an idle inspection profile). Then
  regenerate `tests/xrsim/report.json` with a deterministic gameplay reach
  (#71/#72).
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 52 — 2026-10-17 18:17 (scheduled loop)
- **Unit:** GH **#74** (the critical harness regression filed wake 51) — **Python
  half reconciled.** Restored the autoloop's `tools/xrsim.py` from `f5fd7f7` as
  the canonical #68/#69/#71/#72 harness and layered PR #70's conveniences back on
  (the issue's *preferred* "union" path), so the repo's own
  **`python tools/test_xrsim.py` is green again**.
- **Why this unit:** #74 is the top open item and it was a *concrete, verifiable
  red*, not just a design question: at HEAD `python tools/test_xrsim.py` **failed**
  (`AssertionError ... "interop_produced_content" in names`) because the pr-70
  merge replaced `tools/xrsim.py` but **not** its 834-line test — the test asserted
  an API that no longer existed. `.github/workflows/vr-worker.yml:63` runs exactly
  this, so `main` was red. A verbatim restore (`git checkout f5fd7f7 --
  tools/xrsim.py`) was measured to turn it green, confirming the test/impl pair.
- **Change (tools only; no C++/build/deploy):**
  - `tools/xrsim.py`: autoloop harness restored + pr-70 union patches —
    `--game-dir/--game-exe/--host-exe` (+ `RIDDICKVR_GAME_DIR/EXE/HOST_EXE` env),
    `--scenario-immediate` forwarded, the gameplay-gated `gameplay_only=1`
    FrameBudget suffix tolerated (p50/p95 remain the asserted fields), stale
    `frame_*.json` sidecar cleanup, and lazy numpy/Pillow (`image_deps`) with
    `--require-image-deps`; the docstring records the union and the missing host half.
  - `tools/test_xrsim.py`: `_args` gains the union attrs + 4 tests (host-exe
    override, `--scenario-immediate`, `gameplay_only` suffix + stale-host,
    selftest arg) → **63/63**.
  - `tests/xrsim/report.json`: reverted to the committed autoloop baseline (the
    dirty working-tree copy was a HEAD-format run output).
- **Evidence:** `python tools/test_xrsim.py` **PASS 63/63**; `python tools/xrsim.py
  --selftest` **PASS** (incl. new `budget_requires_percentiles` /
  `budget_accepts_gameplay_only_suffix`); all 9 `tools/test_*.py` PASS;
  `validate_addresses` PASS; `verify_hook_init` PASS; `verify_reported_fixes`
  PASS. No C++ changed → no `build-all.ps1`/CTest (documented).
- **Blocker / honest limit (the other half of #74):** `src/host/openxr_manager.cpp`
  is still the pr-70 version, so the host does **not** emit the p50/p95
  FrameBudget, the `ComfortVignette` summary, `eye_frames.log` /
  `display_branch.log`, or the DumpGate content gate (their helper headers +
  CTests survive). A *live* XRSIM run's `frame_budget` is therefore an honest
  `frame_budget_reported` FAIL ("stale host build") and the branch / capture-meta
  / comfort checks skip until the host glue is re-merged.
  `tools/verify_reported_fixes.py` also still lacks the #68/#71/#72 invariants.
- **Second finding (pre-existing, not from this change):** `python
  tools/scenario_tests.py --check` **FAILS** on `tools/scenarios/hands_view.json`
  (added by `133a2d8` with `"markers": []`; the validator requires non-empty
  markers, and its markers must appear literally in `src/hook/camera_hook.cpp` —
  an idle inspection pose has none). Filed as GH **#75**.
- **Next:** re-merge the **host half** of #74 — start from
  `git show f5fd7f7:src/host/openxr_manager.cpp` and keep pr-70's
  `gameplay_only=1` budget gating + cinema-streak/scenario-start + `133a2d8`'s
  sim-mode window messages; then re-add the #68/#71/#72 invariants to
  `tools/verify_reported_fixes.py`; then regenerate `tests/xrsim/report.json` live
  (#73.2).
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 51 — 2026-10-17 12:40 (scheduled loop; began 2026-10-16 22:08)
- **Unit (two parts):** (a) GH **#73** — stop tracking the 26 legacy XRSIM
  eye-capture BMPs (safe, independent cleanup, **landed**); (b) found + filed GH
  **#74** — a **critical regression** that blocks the harness: a concurrent session
  merged `pr-70` into `main` mid-wake and **replaced the autoloop's XRSIM harness
  and host assertions with a parallel #68 implementation**.
- **Why:** I started on the "make the committed XRSIM baseline honest" unit
  (the committed `report.json` is a legacy evaluate-only capture, and 26 × 25.6 MB
  BMPs are tracked against the repo's own `.gitignore`). While validating it, a
  live `--reach-gameplay` run proved the harness is fine and the committed
  baseline is stale; then `tools/xrsim.py` began rejecting `--reach-gameplay` —
  the tree had moved. Investigation found the regression.
- **Probe evidence (pre-regression, fresh host, `RIDDICKVR_SIM=1`, allowed
  exception):** a real `--reach-gameplay --duration 30 --interval 300` run gave
  **5 distinct gameplay frames**, `gameplay_full_bleed_reached` PASS (fill 1.00),
  `gameplay_window_has_content` PASS (0 blank), all stereo gates PASS
  (`dx=-48…+36`, `identical=False`), `capture_metadata_present` PASS (yaw span
  21.3°), `frame_budget` PASS (p50=2.28 p95=4.90, budget 11.11) — **exactly one
  real FAIL**, `comfort_vignette_applied` (the deploy-gated wake-50 hook fix).
  This is the honest baseline the committed `report.json` does *not* show.
- **Regression (#74) — measured, not inferred.** Merge `e6c6c6e` (parents
  `f5fd7f7` + branch `b84753e`) took the **branch's** versions of three files the
  autoloop had advanced (wakes 45–50). At HEAD: `tools/xrsim.py` (+300/−953) has
  **0** refs to `--reach-gameplay`, `--duration`, `display_branch`, `eye_frames`,
  `content_gate`, `p50_ms`, `comfort_vignette`, `gameplay_full_bleed`, and no
  `--evaluate-only`/`--hook-log`; `src/host/openxr_manager.cpp` (+57/−240) has
  **0** `ComfortVignette` / `eye_frames.log` refs; `tools/verify_reported_fixes.py`
  (+45/−229) has **0** `#68/#71/#72`/`comfort_vignette`/`eye_frames` invariants.
  The lost versions are intact at `f5fd7f7`. The branch *did* add real things that
  must be kept in any fix: `--game-dir/--game-exe/--host-exe`, numpy-free
  selftest, `--scenario-immediate`, gameplay-gated scenario start +
  `gameplay_only=1` FrameBudget, per-frame `frame_%04u.json` sidecar, and
  `133a2d8` targeted window-message sim advance (no OS input injection). ADRs
  0447–0456 + `src/host/README.md` + `docs/vr-scenarios.md` now describe code that
  is gone.
- **Change landed (this wake):** `git rm --cached -r tests/xrsim/artifacts` (26
  BMPs → 0 tracked; files kept on disk; `git check-ignore --no-index` now matches
  `.gitignore:38`). Commit `20b6995`; 26 deletions, nothing else.
- **Deliberately NOT done:** I did **not** unilaterally re-merge the divergent
  `xrsim.py` (a 953-line-different second implementation) — choosing between the
  two parallel #68 designs is a design decision for the interactive session /
  maintainer, and a wrong union would clobber the branch's work. Filing #74 with
  the exact recovery path (`git show f5fd7f7:<file>`) is the bounded, safe action.
- **Blocker:** #74 (harness reconciliation). Until then, `report.json` cannot be
  honestly regenerated (HEAD's `xrsim.py` has no `--reach-gameplay`/
  `--evaluate-only`), so #73 requirement 2 is deferred.
- **Next:** reconcile #74 (union preferred, else supersede ADRs 0447–0456 and the
  docs); then regenerate `tests/xrsim/report.json` (#73.2).
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 50 — 2026-10-16 22:05 (scheduled loop)
- **Unit:** GH **#68 requirement 5 (comfort rules), first slice** — assert the
  locomotion comfort vignette is actually *applied*. Building the assertion
  immediately **found and fixed a real publish defect**: the hook computes
  `comfortVignette` (Track C #9) but `IpcBridgeClient::WriteState` never copied it
  to shared memory, so it was structurally unobservable host-side. Host C++ +
  tools/tests/docs + a **one-line hook fix** (`src/bridge/ipc_bridge.cpp`); no
  game/asset change, **no deploy**.
- **Why this unit:** wake 49's Next was "#72 req 1 via one interactive deploy,
  else the next host-side #68 gap". #72 req 1 / #67 step 3 / #64 / #63 / #62 / #65
  are all hook-side and need a deploy this loop must not do (the sim install's
  hook is stale at 17:56). #68 requirement 5 ("comfort vignette applied during
  scripted movement/turning") was the least-implemented requirement and is
  host/tools-side, so it is the top item that can be *finished* this wake.
- **Root cause (measured, not inferred):** the mock host reads `clientState` back
  every frame, but the hook publishes hook→host fields **by hand** in
  `IpcBridgeClient::WriteState` (`src/bridge/ipc_bridge.cpp`) — an explicit subset
  that omitted `comfortVignette`, the only hook→host field of
  `RiddickVRSharedState` it did not copy (`IpcBridgeHost::ReadState` copies the
  whole struct, so the host read the never-written offset = 0.0). Live run:
  `[Mock Host] ComfortVignette samples=12990 max=0.000 mean=0.000 above=0
  locomotion=12929 max_locomotion=0.000 threshold=0.050` — 12 929 of 12 990
  frames published locomotion input (the default mock walks + turns) yet the level
  stayed 0.0 the whole run. The vignette *is* applied in-headset
  (`StereoRenderer` reads the hook's *local* `m_sharedState`, which
  `CameraHook::Update` sets); only the host-side publish was broken.
- **Change (host C++ + tools/tests/docs + one hook line; no deploy):**
  - **Fix:** `IpcBridgeClient::WriteState` now copies
    `m_pSharedState->comfortVignette = state.comfortVignette;`.
  - `src/host/comfort_vignette.h` (new, pure/dependency-free):
    `ComfortVignetteStats` + `AccumulateComfort` (clamp, max/mean/`above`, and the
    separate locomotion-frame tally + `maxWhileLocomotion`) +
    `FormatComfortVignetteLine`; `kComfortVignetteMin=0.05`,
    `kComfortLocomotionMag=0.2`.
  - `src/host/openxr_manager.cpp`: the mock loop reads `clientState.comfortVignette`
    back every frame, classifies the frame's *published* thumbstick input, and
    prints `[Mock Host] ComfortVignette samples=.. max=.. mean=.. above=..
    locomotion=.. max_locomotion=.. threshold=..` every 30 frames.
  - `tools/xrsim.py`: `COMFORT_VIGNETTE_RE`, `parse_comfort`,
    `comfort_vignette_check` → a graded `comfort` check `comfort_vignette_applied`
    (FAILs when the level never rose while locomotion was published; **skipped**
    when the line is absent — pre-#68 host / evaluate-only — or no locomotion was
    published) + a `comfort_vignette` report key.
  - `tests/comfort_vignette_tests.cpp` (new CTest `comfort_vignette_summary`,
    x86+x64) pins the accumulation + the exact wire line;
    `tools/test_xrsim.py` **+5 → 59** (parse, applied/failed, skip paths, evaluate
    wiring, report key); `tools/verify_reported_fixes.py` (new #68 req-5 invariant,
    incl. the publish line); `src/host/README.md`, `docs/vr-scenarios.md`,
    `docs/adr/0456-xrsim-comfort-vignette-assertion.md` (new).
- **Evidence:**
  - **Live** (allowed exception, `RIDDICKVR_SIM=1`, fresh `dist\riddickvr-host.exe`,
    in-game hook stale as expected): `--reach-gameplay --duration 45 --interval 300`
    reached live gameplay (43 gameplay frames, `gameplay_window_has_content` PASS,
    `gameplay_full_bleed` reached) and the **defect reproduced** — the host log has
    433 `ComfortVignette` lines, all `max=0.000` while `locomotion` climbed to
    12929. `--evaluate-only --downscale 4` → the report's single FAIL is
    `comfort_vignette_applied` (`max=0.000 while moving (threshold 0.050); 0/12990
    frame(s) above, 12929 locomotion frame(s)`), `comfort_vignette` key present,
    everything else PASS/expected-skip. (This also *replaced* a stale, messy
    committed baseline — 26 legacy `both_eyes_non_black` FAILs — with a clean
    single-FAIL one.)
  - Offline: `build-all.ps1 -Config Release` OK (x86 `opengl32.dll` **rebuilt with
    the publish fix** + x64 host; **no `-Deploy`**); CTest **15/15 PASS on x86 AND
    x64** (new `comfort_vignette_summary`); all 9 `tools/test_*.py` PASS;
    `xrsim.py --selftest` PASS; `validate_addresses` PASS; `verify_hook_init` PASS;
    `scenario_tests.py --check` PASS; `verify_reported_fixes.py` PASS.
- **Blocker / honest limit:** the **fix is hook-side and cannot be live-validated
  by this loop** — the game-dir hook is the 17:56 build and deploying is forbidden,
  so `tests/xrsim/report.json` stays `passed: false` with exactly one known FAIL
  (`comfort_vignette_applied`) until one interactive deploy installs the rebuilt
  `dist\opengl32.dll`. This is the same boundary GH #67 step 3 / #72 req 1 hit. The
  check asserts the *published level* (what the renderer gates on); measuring the
  actual peripheral dimming in the captured eye image is follow-up. #72 req 1 (the
  BarScan branch signal) is still the top open hook item.
- **Next:** one interactive deploy (`dist\opengl32.dll` + `riddickvr-host.exe`) turns
  `comfort_vignette_applied` green and can carry `ViewmodelObserve=1` (#67 step 1/2/3)
  + the #72-req-1 branch fix in the same sitting; otherwise the next host-side #68
  gap (snap-turn/recenter or HUD-clipping assertions — both need hook signals).
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 49 — 2026-10-16 21:43 (scheduled loop)
- **Unit:** GH **#68 requirement 2 (second half)** — make the frame-time budget a
  *frame-pacing* assertion: report **p50/p95** (plus max + drops) and assert the
  90 Hz budget on p50/p95 instead of `mean`/`max`. Host + tooling only; **no
  hook change, no deploy**.
- **Why this unit:** wake 48's Next named exactly this as the top item that can
  be *finished* without a deploy (the top hook items — #72 req 1, #67 step 3,
  #64, #63, #62, #65 — all need a headset deploy this loop must not do, and the
  sim install's hook is stale at 17:56). It is the last red check on a live run
  (ADR-0447/0448/0454), and it is host-side ⇒ live-validatable with the freshly
  built `dist\riddickvr-host.exe` via the allowed sim exception (#68).
- **Root cause (measured, not inferred):** the host's frame loop is gated on the
  *game's* uncapped swap rate, so a single level-load stall dominates
  `max_ms`/`mean_ms`. Live `FrameBudget samples=5400 mean_ms=3.32 max_ms=763.26
  over_budget=123 budget_ms=11.11` (ADR-0448) — the old
  `mean <= budget && max <= 1.5*budget` rule fails on that one-off 763 ms hitch
  (a load artefact, not an 11.11 ms render cost), so `frame_budget` was a
  permanent red FAIL excluded from every "all green" claim.
- **Change (host C++ + tools/tests/docs; no hook/asset change):**
  - `src/host/frame_time_stats.h` (new, pure/dependency-free): `PercentileMs`
    (nearest-rank → always a real sample), `SummarizeFrameTimes`,
    `FormatFrameBudgetLine`, `kMaxFrameTimeSamples` (1<<20 safety bound).
  - `src/host/openxr_manager.cpp`: the 30-frame `FrameBudget` line now ends with
    `p50_ms=.. p95_ms=..`; `samples/mean_ms/max_ms/over_budget` keep their
    previous cumulative meaning (p50/p95 come from the bounded sample window).
  - `tools/xrsim.py`: `FRAME_BUDGET_RE` grows an optional `p50_ms/p95_ms` tail;
    `parse_budget` asserts `p50 <= budget and p95 <= budget` (arg-governed),
    still reports `max_ms`/`over_budget` and puts `p50_ms`/`p95_ms` in the
    record; a line *without* percentiles is a `frame_budget_reported` FAIL
    ("stale host build"), never silently passed.
  - `tests/frame_time_tests.cpp` (new CTest `frame_time_percentiles`, x86+x64)
    pins the percentile definition, "hitch is not a regression", the drop count
    and the exact wire line; `tools/test_xrsim.py` (+1 → 54) pins pass/fail and
    the legacy/stale-host path; `tools/verify_reported_fixes.py` (new #68 req 2
    invariant); `src/host/README.md`, `docs/vr-scenarios.md`,
    `docs/adr/0455-xrsim-frame-pacing-percentile-budget.md` (new).
- **Evidence:**
  - **Live** (allowed exception, `RIDDICKVR_SIM=1`, fresh `dist\riddickvr-host.exe`,
    scratch deleted): `--reach-gameplay --duration 45 --interval 300`, then
    `--evaluate-only --downscale 4`:
    - the run's final host line is
      `[Mock Host] FrameBudget samples=12900 mean_ms=3.26 max_ms=762.84
      over_budget=249 budget_ms=11.11 p50_ms=2.38 p95_ms=4.62`;
    - report `frame_budget` **ok=true** (`p50=2.38 p95=4.62 mean=3.26
      max=762.84 over=249 budget=11.11`) — the 763 ms hitch is still *reported*
      and no longer fails; **XRSIM RESULT: PASS** (43 gameplay frames, full-bleed
      reached, `gameplay_window_has_content` PASS, `capture_metadata_present`
      PASS, all 43 stereo gates PASS, all `no_vertical_disparity` inconclusive
      per ADR-0454). Previously this run's only FAIL was `frame_budget`.
  - Offline: CTest **14/14 x64 + 14/14 x86** (incl. new `frame_time_percentiles`);
    all 9 `tools/test_*.py` PASS; `xrsim.py --selftest` PASS; `validate_addresses`
    PASS; `verify_hook_init` PASS; `scenario_tests.py --check` PASS;
    `verify_reported_fixes.py` PASS. `build-all.ps1` (no `-Deploy`) OK
    (`dist\riddickvr-host.exe` x64 + `dist\opengl32.dll` x86 rebuilt).
- **Blocker / honest limit:** `max_ms` is **not** asserted — a one-off stall is
  reported but does not fail; only a *sustained* regression (p95 > budget) does.
  This measures the host/game frame loop the simulator drives, not a headset
  present-time; the real 90 Hz pipeline still needs the headset. #72 req 1 (the
  BarScan branch signal) is still the top open hook item and needs one
  interactive deploy (sim hook stale at 17:56).
- **Next:** #72 req 1 via one interactive deploy (`ViewmodelObserve=1` + the
  branch fix together — now also carries #67 step 1's modelview capture and
  #67 step 3's camera-relative re-projection), else the next host-side #68 gap.

## Wake 48 — 2026-10-16 21:17 (scheduled loop)
- **Unit:** GH **#72 requirement 3** — resolve the `dy=±4` vertical-disparity
  question (an explicit #72 acceptance criterion) and make the harness report it
  honestly. Tools/tests/docs only; **no hook change, no build, no deploy**.
- **Why this unit:** wake 47's Next named #72 req 1 as the top remaining item but
  "a hook change (blocked on the same deploy)", and the sim install's hook is
  stale (17:56) so no hook change can be live-validated this loop. #72's *other*
  unmet criterion ("the `dy=±4` question is resolved with a stated verdict") is
  fully decidable host/tools-side and live-validatable with the fresh
  `dist\riddickvr-host.exe`, so it is the top item this wake can *finish*.
- **Decisive finding — verdict: ARTEFACT, not a stereo defect** (four controlled
  measurements; live, one `--dump-branch any` run so both branches share the blit
  path, plus a graded gameplay-only run):
  1. Bright frames are a rock-steady `dy=0` from **all five** disjoint crops (dy
     span 0), invariant to downscale.
  2. Dark frames: `best_dy` = **+6/+3/+2/+1** at downscale **1/2/4/8** (frame
     3600) and crops **6/7/10/12/8** (span 6); frame 10200's crops flip `dx`
     sign. A geometric offset cannot do either.
  3. The optimum **pins to the search edge** on all 15 dark frames; widening the
     search does not find an interior minimum (a `±12` search still gives `-12`).
  4. It is **not** only darkness: the graded run's bright frame 2400 (std 53) had
     **both** `dy=-4` and `dx=-48` on their bounds, a 24 px crop spread, and
     residual MSE ~6900 vs ~25 for a good frame — the registration failed to
     lock, so a brightness threshold alone would have mis-failed it.
  Mechanism: the renderer applies only a horizontal half-IPD shift (GH #12); a
  real vertical offset has no mechanism and would be brightness-independent.
- **Change (tools/tests/docs; no C++/asset/deploy):**
  - `tools/xrsim.py`: `stereo_metrics` refactored around a pure `_best_align(...)`
    and now returns `contrast` (reference-crop stddev) + `dy_at_boundary`;
    `DY_SEARCH_BOUND` 4 → **8** (a *real* offset stays interior/measurable while a
    degenerate landscape still runs to the edge); `MIN_DY_CONTRAST = 25.0`
    (measured separation: dark 15-17 vs textured 47-100). `assert_stereo` keeps
    `no_vertical_disparity` a FAIL only when the offset is **quantifiable**
    (contrast ≥ 25 **and** interior); otherwise it is reported **inconclusive**
    (skip) with the reason. New `vertical_disparity_stats()` + a
    `vertical_disparity` report key (`ok`/`inconclusive`/`failed`) on both report
    paths.
  - `tools/test_xrsim.py` (+9 → **53**): contrast+boundary reporting, a textured
    interior offset still fails, a bright edge-pinned offset is inconclusive, a
    low-contrast offset is inconclusive, a flat dark pair is inconclusive at the
    edge, the summary counts, and three synthetic-verdict cases.
  - `tools/verify_reported_fixes.py` (new #72 req 3 invariant),
    `docs/vr-scenarios.md` caveat,
    `docs/adr/0454-vertical-disparity-artifact-contrast-gate.md` (new),
    `tests/xrsim/report.json` (regenerated `--evaluate-only --downscale 4`; also
    picks up wake 47's `eye_frames`/`capture_metadata_present`, which had not
    been regenerated then).
- **Evidence:**
  - **Live** (allowed exception, `RIDDICKVR_SIM=1`, fresh `dist\riddickvr-host.exe`,
    scratch deleted):
    - `--reach-gameplay --duration 40 --interval 1200 --downscale 4` (graded
      gameplay-only, end-to-end with the final code): `vertical_disparity`
      **`{ok: 2, inconclusive: 8, failed: 0}`** — **no vertical-stereo defect**;
      `gameplay_full_bleed_reached` PASS (fill 1.00), `gameplay_window_has_content`
      PASS (0 blank), `scene_advanced` PASS 10/10 distinct, `capture_metadata_present`
      PASS 10 transforms. Only FAIL = `frame_budget` (pre-existing mock cadence,
      ADR-0448).
    - `--dump-branch any` run re-analysed (bright + dark in one capture):
      **`{ok: 6, inconclusive: 14, failed: 0}`** — the 6 bright frames measure
      `dy=0`, all 14 dark frames inconclusive (before this change the same capture
      failed on all 15 dark frames).
    - A gameplay-only capture (42 frames) that mis-failed the dark frames **and**
      the bright frame 2400 now gives **`{ok: 11, inconclusive: 30, failed: 0}`**.
  - Offline: `test_xrsim.py` **53/53 PASS**; `xrsim.py --selftest` PASS; all 9
    `tools/test_*.py` + `validate_addresses` + `verify_hook_init` +
    `verify_reported_fixes` + `scenario_tests.py --check` PASS. No C++/asset
    changed → no `build-all.ps1`/CTest (documented).
- **Blocker / honest limit:** this resolves the *measurement* (the estimator
  cannot quantify `dy` on these frames, and the renderer has no vertical
  mechanism) — **not** a headset confirmation of the game's own dark frames.
  `MIN_DY_CONTRAST` is empirical; an offset ≥ 8 px is reported inconclusive ("out
  of range"), not failed. **#72 req 1 (the BarScan branch signal) is still the top
  open item and is a hook change that needs one interactive deploy** (the sim hook
  is stale at 17:56).
- **Next:** #72 req 1 via one interactive deploy (`ViewmodelObserve=1` + the
  branch fix together), else **#68 requirement 2's remaining half — p50/p95
  frame-time percentiles + a p95-based budget assertion** (the last red check on a
  live run; spec-listed and host-side, no deploy).
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 47 — 2026-10-16 20:45 (scheduled loop)
- **Unit:** GH **#68 requirement 1 (second half)** — make every XRSIM capture
  *self-describing in 3D*: the mock host now records the **head/eye pose +
  projection** it published for each dumped frame (`eye_frames.log`), the harness
  parses it, and the report carries `eye_frames` + a `capture_metadata_present`
  check. While live-validating this I hit a **second, concrete defect** and fixed
  it: a truncated/unreadable eye BMP crashed the whole harness (no report at all),
  which is exactly what a real run produces (the run ends by `terminate`).
- **Why this unit (top items are deploy-blocked):** #67 step 3, #64, #63, #62,
  #65 are hook-side and need a headset deploy this loop must not do; wake 46's
  Next said so explicitly. #68 is the standing "iteration vehicle" and its
  requirement 1 ("dump both eyes **and record the eye pose/projection used for
  each frame**") was half-unmet — the missing half is precisely the camera
  transform GH #67 step 3 needs to make a captured `GL_MODELVIEW`
  camera-relative. Host-side ⇒ live-validatable with the freshly built host.
- **Change (host C++ + tool + tests/docs; no hook change, no deploy):**
  - `src/host/eye_frame.h` (new): pure, dependency-free
    `FormatEyeFrameLine(frame, EyeFrameSample)` — one line per dumped frame:
    `frame=N hmdpos=x,y,z hmdquat=x,y,z,w viewL=[16] viewR=[16] projL=[16]
    projR=[16]` (column-major, space-separated matrices, 4-decimal pose).
  - `src/host/openxr_manager.cpp`: on each **dumped** frame, write the published
    `state.hmdPose` + `leftEyeView/rightEyeView` + `leftEyeProjection/
    rightEyeProjection` to `<dump-dir>\eye_frames.log` (same truncate-on-first-
    dump / append-after discipline as `display_branch.log`).
  - `tools/xrsim.py`: `EYE_FRAMES_FILE` + `EYE_FRAME_RE`; `read_eye_frames()`
    (`{}` when absent) + `eye_frame_stats()` (`count`, `distinct`, `yaw_span_deg`);
    a `capture_metadata_present` check (**skipped**, never failed, when absent so
    legacy captures keep working); `eye_frames` in both report paths; the dump
    cleanup clears `eye_frames.log` too.
  - **Robustness fix (found live):** `stereo_metrics` returns
    `{"read_error": ...}` (and `frame_signature` returns `None`,
    `comfort_checks` a failing check) for a truncated capture instead of raising;
    `evaluate` reports a failing `frame_readable_<N>` for that frame. Before this,
    the run that ended in `terminate` produced **no report at all** (PIL
    `image file is truncated`).
  - `tests/eye_frame_tests.cpp` (new) + CTest `eye_frame_record` (x86 **and**
    x64): pins the exact wire format (7 fields, 3/4-value pose, four 16-float
    matrices round-trip) + determinism.
  - `tools/test_xrsim.py` **+5 → 44**: eye-frame parse + yaw span, metadata check
    skipped-without / present-with, the report key, and the truncated-capture
    robustness path.
  - `tools/verify_reported_fixes.py` (new #68 invariant),
    `src/host/README.md`, `docs/adr/0453-per-frame-camera-transform-capture.md`
    (new).
- **Evidence:**
  - **Live** (`python tools/xrsim.py --reach-gameplay --duration 25 --interval
    300`, allowed exception, `RIDDICKVR_SIM=1`, **fresh `dist\riddickvr-host.exe`**,
    scratch artifacts, deleted after): reached live gameplay, **21 dumped frames
    each with a camera transform** — `eye_frames.log` lines are real (e.g.
    `frame=300 hmdpos=0.0000,1.7000,0.0000 hmdquat=0.0331,-0.0995,0.0033,0.9945
    viewL=[... -0.0325 ...] viewR=[... +0.0325 ...] ...`) with the HMD quaternion
    changing every frame. `--evaluate-only --downscale 4` →
    `eye_frames {count 21, distinct 21, yaw_span_deg 39.511}` (matches the mock's
    ±20° oscillation), `capture_metadata_present` **PASS** `21 frame(s) with
    camera transform, 21 distinct; HMD yaw span 39.511 deg`, and the truncated
    last frame is now a reported `frame_readable_6600` FAIL instead of a crash
    (the report was written; before the fix the harness died with
    `OSError: image file is truncated`).
  - Offline: `python tools/test_xrsim.py` **44/44 PASS**; `xrsim.py --selftest`
    PASS; all 9 `tools/test_*.py` + `validate_addresses` + `verify_hook_init` +
    `verify_reported_fixes` + `scenario_tests.py --check` PASS.
    `build-all.ps1 -Config Release` OK (x86 `opengl32.dll` + x64 host; **no
    `-Deploy`**); CTest **13/13 PASS** on **x86 and x64** (new `eye_frame_record`).
- **Blocker / honest limit:** the metadata records the **host-published**
  transform (the transform the hook injects). It is host-side and
  live-validated; the hook-side `GL_MODELVIEW` capture (ADR-0450/0451) and the
  step-3 re-expression it enables still need one interactive **deploy** + headset
  run (the loop must not deploy, and the sim install's hook is stale at 17:56).
  The live run also re-shows the pre-existing "very dark gameplay frame ⇒
  `no_vertical_disparity` dy=-4" registration artifact (wake 43/#72) — not
  claimed as a stereo defect.
- **Next:** one interactive deploy enables `ViewmodelObserve=1` (+ `NativeHands=1`,
  `NativeHandTexture=1`) so the FP-pass modelview capture lands with an
  `eye_frames` transform attached → #67 step 3 re-expresses it in head/controller
  space. Otherwise #72 req 1 (the BarScan branch signal) is the top remaining
  #68-adjacent item and is a hook change (blocked on the same deploy).
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 46 — 2026-10-16 20:27 (scheduled loop)
- **Unit:** GH **#61** (real skin diffuse on the native hands) — the standing
  prompt says "the baked diffuse decodes white; fix or keep the skin-tone
  fallback". I tested that premise instead of trusting it: **it is false**, so
  the fix is to **stop disabling the (correct) texture** and pin the fact with a
  regression test. No GL/deploy; generated-asset + tests/docs/config only.
- **Why this unit (top items are deploy-blocked):** #67/#64/#65/#62/#63 are all
  hook-side and need a headset deploy this loop must not do (#67 step 3 still
  needs a validated capture; wake 45 handed it to an interactive session). #61
  had a concrete, *offline-decidable* question and a false premise.
- **Decisive finding (offline, read-only):** commit `52c2101` disabled the
  texture claiming "the baked NativeHandTexture decoded white". Decoding the
  shipped `Content/Models.xtc` `Riddick_ArmScar_D` mip 1 (256×512 DXT1) gives a
  whole-surface **mean RGB ≈ (164.0,130.6,114.7) → mean luma ≈ 139 with
  `0/131072` near-white texels**; the native arm meshes' own UVs span nearly the
  full atlas and sample mean luma **127–147** with stddev 42–55 and **0
  near-white** samples (both V-flip variants). The asset is byte-identical to
  its first commit (`280e058`, checksum `0x05308AE2`). The washed-out/white look
  was the **0.25 fade translucency** that the *same* commit fixed (`f = 1.0f`).
- **Change (config + hook docs + tests/tools + ADR):**
  - `riddick_vr.ini`: `NativeHandTexture=0 → 1` (matches the compiled default,
    `tools/riddick_vr.ini.example`, and the documented recommendation) + comment
    citing the verification.
  - `tests/native_hand_skin_tests.cpp` (new) + CTest `native_hand_skin_mapping`
    (CMake): decodes the in-tree asset with `bc1.h` and asserts skin-tone /
    non-white / non-flat for the whole surface *and* at the arm meshes' own UVs
    (both V-flips), plus non-degenerate UV spans.
  - `tools/test_xtc_bake.py`: +`test_committed_asset_is_skin_toned` (CI lane, no
    game install) mirroring the invariant on the committed headers.
  - `tools/verify_reported_fixes.py`: new #61 invariant (shipped INI keeps
    `NativeHandTexture=1`; the regression test stays wired).
  - `src/hook/native_hand_texture.h` + `src/hook/README.md` +
    `docs/adr/0452-native-hand-skin-diffuse-verified.md` (new).
- **Validation:** `build-all.ps1 -Config Release` OK (x86 hook + x64 host; **no
  `-Deploy`**); CTest **12/12 PASS on x86 AND x64** (new `native_hand_skin_mapping`);
  all 10 `tools/test_*.py` + `validate_addresses` + `verify_hook_init` +
  `verify_reported_fixes` + `scenario_tests.py --check` PASS.
- **Blocker / honest limit:** **not headset-validated** — the *rendered* result
  still needs one in-headset confirm (as does `NativeHandTextureVFlip`). The live
  game-dir INI also still has `NativeHandTexture=0` and the deployed hook is
  stale (17:56); this loop must not write the game install, so the running build
  keeps the flat tone until an interactive session deploys + sets the INI. What
  is now settled offline is the *claim* that justified disabling it.
- **Next:** one interactive deploy with `NativeHandTexture=1` (flip `VFlip` if
  the seam is wrong) closes #61; then #64's remaining "flat/2D" is texture-side
  and should resolve with it. Otherwise continue #67 step 3 (needs the capture).
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 45 — 2026-10-16 20:15 (scheduled loop)
- **Unit:** GH **#67 step 1 (remainder)** — make the draw-time `GL_MODELVIEW`
  capture the **effective** matrix by mirroring the fixed-function modelview stack
  (`glLoadMatrixf` + `glPushMatrix`/`glTranslatef`/`glPopMatrix`). Hook C++ +
  tests/docs/tools; off-path (forwarding-only); **no deploy, no game/asset change**.
- Why this unit: wake 44 landed the step-2 observation probe (ADR-0450) but its
  captured matrix came from `s_lastView`, which is written **only** by
  `Hook_glLoadMatrixf` (`src/hook/opengl_proxy.cpp:406`). REA already recorded
  (`docs/rea/reference/RndrGL_GL_Path.txt` §3) that RndrGL does **not** import
  `glMultMatrixf`/`glRotatef`; it composes object transforms from `glLoadMatrixf` +
  `glPushMatrix` + `glTranslatef` + `glPopMatrix`. So a viewmodel pass that pushed a
  sub-transform was captured as the last *loaded base*, not `base * translate(...)`
  — step 3 would have built on a wrong matrix. This is #67 plan step 1's stated
  "add hooks for the matrix concatenation calls", and it is a prerequisite for the
  step-2 probe to be meaningful. It is the same class of bounded, offline-verifiable
  unit the loop landed for FK (#58) and placement (#64).
- Change (code/tests/docs/tools; no deploy):
  - `src/hook/gl_matrix_tracker.h` (new): header-only, dependency-free
    `GlMatrixTracker` — column-major, bounded stack (`kMaxDepth=32`), `LoadMatrixf`/
    `LoadIdentity`/`Multiply`/`Translatef` (`M' = M*T`, column 3)/`Push`/`Pop`;
    overflow & underflow ignored (never out of range).
  - `src/hook/opengl_proxy.{h,cpp}`: `s_modelviewTracker`; `Hook_glLoadMatrixf`
    (MODELVIEW) feeds it and re-publishes `s_lastView`; new
    `NoteModelviewLoadIdentity/Push/Pop/Translatef` (no-op outside `GL_MODELVIEW`;
    push/translate also tick the observation counters); reset to identity on
    `wglMakeCurrent`. `GetLastViewMatrix()` is now the effective modelview.
  - `src/hook/dllmain.cpp`: `InstallRndrGLHooks` IAT-hooks `OPENGL32.dll!glLoadIdentity`,
    `!glPushMatrix`, `!glPopMatrix`, `!glTranslatef` (same mechanism as
    `glDrawElements`/`glCallList`) → `NoteModelview*` then forward verbatim. Never
    suppresses.
  - `src/hook/viewmodel_suppressor.h`: observation snapshot gains `pushesInside`/
    `translatesInside` (`NoteObservedPush`/`NoteObservedTranslate`; no-ops unless
    observe+window-open) — direct in-game evidence that the pass composes.
  - `src/hook/opengl_proxy.cpp` probe line →
    `ViewmodelProbe [#N]: inside=.. outside=.. sample=.. pushes=.. translates=..`.
  - `tools/xrsim.py`: parse the optional `pushes`/`translates` (backward compatible —
    a pre-step-1 line leaves them `None`) + aggregate them in `viewmodel_probe`.
  - `tools/test_xrsim.py` (+1 → **39**): composition parse + backward compatibility.
  - `tests/gl_matrix_tracker_tests.cpp` (new) + CTest `gl_matrix_tracker`; CMake
    source list + test. `tests/viewmodel_suppressor_tests.cpp` (+compose counters,
    inert without observe / outside the window).
  - `tools/verify_reported_fixes.py` (new GH #67 step-1 invariant),
    `src/hook/README.md`, `docs/rea/reference/RndrGL_GL_Path.txt` (§3 note),
    `docs/adr/0451-software-modelview-stack-for-draw-time-capture.md` (new),
    `tests/xrsim/report.json` (regenerated `--evaluate-only --downscale 4`;
    `viewmodel_probe` +`pushes`/`translates`, minimal diff).
- Validation: `build-all.ps1 -Config Release` OK (x86 `opengl32.dll` + x64 host; no
  `-Deploy`); CTest **11/11 PASS** on **x86 and x64** (new `gl_matrix_tracker`);
  `test_xrsim.py` **39/39**; `xrsim.py --selftest` PASS; all 9 `tools/test_*.py` +
  `validate_addresses` + `verify_hook_init` + `verify_reported_fixes` +
  `scenario_tests.py --check` PASS.
- Blocker: **not validated live** — like the step-2 probe, the composed capture only
  appears in a hook build deployed to the game install, and this loop must not
  deploy. The tracker is forwarding-only (it changes nothing the engine renders), so
  landing it is inert until a headset run reads it. **Hand to an interactive
  session:** `ViewmodelObserve=1`, one frame in 3D gameplay, read
  `ViewmodelProbe [#N]: ... pushes=.. translates=..` + `ViewmodelProbe matrix:` —
  non-zero `pushes`/`translates` confirm the pass composes and the matrix is now the
  effective one.
- **Next:** with a validated capture (matrix + pushes/translates) #67 step 3
  re-expresses the modelview in head/controller space; alternatively #63 suppresses
  the same pass via the unchanged window. Both are hook changes needing one
  interactive deploy.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 44 — 2026-10-16 19:57 (scheduled loop)
- **Unit:** GH **#67 step 2** (identify the first-person viewmodel pass) — land an
  **observation-only** probe that reuses the GH #66 depth-only-clear window but
  **never skips a draw**, so the engine's own first-person arms/weapon can be
  identified (and their transform read) before they are driven from the
  controller (#67) or suppressed (#63). Hook C++ + tools/tests/docs; off by
  default; **no deploy, no game/asset change**.
- Why this unit: wake 43's Next was "#72 req 1 via interactive deploy, else #67
  step 2". #72 req 1 is a hook change blocked on a deploy this loop must not do,
  so I took #67 step 2. The remaining #67 work is *identify* → *re-express in
  head/controller space* → *stop the overlay*; identification is the prerequisite
  and is where the missing evidence is.
- Design (observation-only, subordinate to suppression): the SAME provisional
  boundary as #66 (mid-frame depth-only clear opens; projection load / swap
  closes). The state machine records, per sampled frame, how many intercepted
  draws fell **inside vs outside** the window and the `GL_MODELVIEW` in effect at
  the **first in-window draw** (the pass loads one viewmodel-relative matrix and
  draws its submeshes; a later world/HUD matrix must not overwrite it). The
  suppression decision is unchanged and evaluated **last**.
- Change (hook C++ + tools/tests/docs; no deploy):
  - `src/hook/viewmodel_suppressor.h`: `ViewmodelSuppressionConfig::observe`
    (default false); `ViewmodelObservationSnapshot`; `NoteObservedDraw()`,
    `SetObservedSample()`, `ComputeSnapshot()`, `Observing()`.
  - `src/hook/opengl_proxy.cpp`: `BuildViewmodelSuppressionConfig()` sets
    `observe` from `ViewmodelObserve`; `ShouldSuppressDrawCall()` records the
    tallies + first in-window modelview (from `s_lastView`) **before** the
    unchanged suppress decision; logs `ViewmodelProbe [#N]: inside=.. outside=..
    sample=..` + a `ViewmodelProbe matrix: [..]x4` line every 300 presented
    frames.
  - `src/hook/vr_config.{h,cpp}`: `ViewmodelObserve` (default 0) + INI + the
    config log line; `riddick_vr.ini` + `tools/riddick_vr.ini.example`.
  - `tools/xrsim.py`: `viewmodel_probe_stats()` + `viewmodel_probe` report key;
    graded `viewmodel_pass_identified` (**skip** when the probe was not run, fail
    when it ran but no draw fell inside the window).
  - `tests/viewmodel_suppressor_tests.cpp` (+observation cases, incl. "observe
    never suppresses even with suppression on"), `tools/test_xrsim.py` (+3 →
    **38**), `tools/verify_reported_fixes.py` (#67 invariant),
    `src/hook/README.md`, `docs/adr/0450-viewmodel-pass-observation-probe.md`
    (new), `tests/xrsim/report.json` (regenerated `--evaluate-only --downscale 4`;
    +1 key).
- Validation: `build-all -Config Release` OK (x86 `opengl32.dll` + x64 host; no
  `-Deploy`); CTest **10/10 PASS** on **x86 and x64** (`viewmodel_suppressor_window`
  now covers observation); `test_xrsim.py` **38/38**; `xrsim.py --selftest` PASS;
  all 9 `tools/test_*.py` + `validate_addresses` + `verify_hook_init` +
  `verify_reported_fixes` + `scenario_tests.py --check` PASS.
- Blocker: **not validated live** — `ViewmodelProbe` only appears in a hook build
  deployed to the game install, and this loop must not deploy (same boundary
  wake 43 hit for #72 req 1). The feature is off by default and observation-only,
  so landing it changes nothing until an interactive session flips
  `ViewmodelObserve=1`. **Hand to an interactive session:** set
  `ViewmodelObserve=1`, run one frame in 3D gameplay, read
  `ViewmodelProbe [#N]: inside=.. outside=..` + the matrix from `riddick_vr.log`.
- **Ops note (honest):** `tools/autoloop/logs/STATE.log` is gitignored and was
  accidentally **overwritten** (not appended) this wake. It was reconstructed
  from the per-run logs (`run-*.log` / `run-*.err.log`, which echo the prior
  summaries): 45 lines, wakes 2-44. Wakes 1 and 20, and some early-wake bodies,
  could not be recovered (their run logs have rotated out) — the full narrative
  for every wake remains in `STATE.md` above.
- **Next:** either (a) an interactive deploy enables `ViewmodelObserve=1` so the
  pass boundary + viewmodel transform are captured, then #67 step 3 re-expresses
  the matrix in head/controller space; or (b) continue #72 req 1 (branch signal)
  in the same interactive deploy (both are hook changes needing one deploy).
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 43 — 2026-10-16 19:43 (scheduled loop)
- **Unit:** GH **#72 requirement 2** (the "cold-start content gate" half of
  wake 42's stated Next) — keep the all-black GL↔D3D11 interop warm-up **and** a
  frozen (byte-identical) static page out of the XRSIM **graded** window. Host
  C++ + pure helper + tools/tests/docs; **no hook change, no deploy, no
  game/asset change**.
- **Finding (live, before→after, `--frames 600 --interval 60`):** the old
  gameplay-only window opened on **2 all-black** frames + 1 near-black frame,
  producing spurious `both_eyes_non_black` / `interop_produced_content` /
  `no_vertical_disparity` FAILs; after the content gate the window is 6 live
  frames, `blank_gameplay` **2 → 0**, and **every** retained frame is
  `dx=-25 dy=0` PASS. Host log: `DumpGate skip frame=120|180|240 reason=blank`;
  `content_gate {blank_skipped: 3, frozen_skipped: 0}`. No live frame dropped.
- **BONUS — requirement 3 resolved (artifact, not defect):** a full
  `--reach-gameplay --duration 30 --interval 300` run (27 gameplay/0 cinema,
  `gameplay_window_has_content` PASS 0 blank of 27, `gameplay_full_bleed_reached`
  PASS fill 1.00) shows the `dy=±4` vertical "disparity" appears **only on very
  dark frames** (mean luma 2.5–13 → `dy=+3/−4`) and is exactly **0 on every
  bright frame** (mean 35–107). A real misalignment would be
  brightness-independent and constant-signed → the stereo-alignment estimator on
  a flat low-contrast MSE landscape. (Also seen before: gating the all-black frame
  removed its spurious `dy=-4`.)
- Change (host C++ + tools/tests/docs; no deploy):
  - `src/host/dump_gate.h`: `RiddickVR::FrameContent {Live,Blank,Frozen}`;
    `ShouldDumpFrame(..., content = Live)` keeps only `Live` content in
    gameplay-only mode (legacy mode unchanged; content gate subordinate to the
    branch gate).
  - `src/host/openxr_manager.{h,cpp}`: `SampleEyeContent()` (**full-image** mean
    luma + 16×16 FNV signature), `kBlankMeanLuma = 1.0` (matches the harness
    rule), `m_lastDumpSignature`/`m_hasLastDumpSignature`, the gate call, the
    `DumpGate skip frame=N reason=blank|frozen` log + a banner line.
  - `tools/xrsim.py`: `content_gate_stats()` + `content_gate` report key; new
    gameplay-only graded check `gameplay_window_has_content`. `tools/test_xrsim.py`
    (+3 → **35**). `tests/dump_gate_tests.cpp` (+content cases).
  - `tools/verify_reported_fixes.py` (new #72 invariant; #69 invariant updated to
    the new call signature), `src/host/README.md`,
    `docs/adr/0449-xrsim-graded-window-content-gate.md` (new),
    `tests/xrsim/report.json` (regenerated `--evaluate-only --downscale 4`; +1 key).
- Validation: `build-all -Config Release` OK (x86 `opengl32.dll` + x64 host; no
  deploy); CTest **10/10 PASS x86 AND x64**; all 9 `tools/test_*.py` +
  `validate_addresses` + `verify_hook_init` + `verify_reported_fixes` +
  `scenario_tests.py --check` PASS.
- Blocker: **#72 requirement 1 (the branch signal) is a hook change** — the
  startup splash is labelled gameplay because a perspective projection is loaded,
  and the fix (BarScan active-rect fill; mind #44) can only be validated live by
  **deploying** the 32-bit hook, which this loop must not do. Recorded on #72 with
  the decoded evidence (splash fill ≈0.31 vs gameplay 1.00) and a design note
  (cold-start latch vs threshold; #44 baked-bars risk).
- **Next:** #72 req 1 is a hook change blocked on deploy — hand to an interactive
  session (A/B knob, default off), else continue **#67 step 2** (identify the
  first-person viewmodel pass) now that a capture can reach live 3D gameplay.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 42 — 2026-10-16 19:25 (scheduled loop)
- **Unit:** GH **#71 requirement 1** (the top "Next" from wake 41) — make a
  scripted XRSIM run actually reach live, advancing 3D gameplay, and prove it.
  Tools/docs + tests only; **no C++/asset change, no build, no deploy**.
- **Decisive finding (corrects ADR-0447 and the #71 premise): the mock run never
  "stalled" — the window was too short.** The mock host's loop is gated on the
  game's frame-ready signal, so it advances at the **game's uncapped swap rate**;
  the trivial startup splash renders at **~250-300 FPS** (`OnSwapBuffers: Frame
  1283 (256.4 FPS)`, host `mean_ms≈3.2-3.3`). So a 180-frame run is ≈**0.6 s** and
  wake 41's 900-frame probe ≈3 s of game time — far short of the splash + level
  load. At **5400 frames (≈16 s)** the run reaches live 3D gameplay.
- Change (tools/docs/tests; no game/asset change):
  - `tools/xrsim.py`: **`--reach-gameplay`** runs the host with an **unlimited**
    frame count (`--max-frames 0`) bounded by wall-clock **`--duration`** (default
    60 s; `--interval` defaults to 300 for a startup-length run), because a frame
    budget cannot express wall-clock startup time. New `active_rect_fill()` (parse
    the hook's `BarScan` active-rect from the current-run log) drives the graded
    **`gameplay_full_bleed_reached`** check (need fill ≥ 0.9): the splash is
    letterboxed (~0.36), gameplay fills the window (1.00). New
    `gameplay_capture_stats()` + a `live_gameplay` report key
    (gameplay/cinema/unknown/**blank**/distinct) so the graded window's content is
    visible. Deliberately **not** the `cinemaMode` bit (which mislabels the splash).
  - `tools/test_xrsim.py` (+4 → **32**): unlimited reach command; `BarScan` fill
    parsing (0.36 splash / 1.00 gameplay / None); gameplay/blank/distinct stats;
    the opt-in full-bleed gate + report key.
  - `tools/verify_reported_fixes.py` (new GH #71 invariant);
    `docs/adr/0448-xrsim-reach-gameplay-window.md` (new); `docs/vr-scenarios.md`
    caveat; `tests/xrsim/report.json` (regenerated `--evaluate-only --downscale 4`;
    +1 key only).
- Evidence (LIVE, `RIDDICKVR_SIM=1`, scratch artifacts, deleted after):
  - `python tools/xrsim.py --frames 5400 --interval 300 --dump-branch any`:
    `scene_advanced` **PASS 15/19 distinct**; `gameplay_captures_present` PASS 18
    gameplay/1 cinema; all gameplay frames non-black with `dx=-31..-23`,
    `not_mono_duplicate` PASS. Rendered: legal page → ESRB page → a dark interior
    with the **native VR hand** bottom-left + HUD top-left. Hook `BarScan`: splash
    `(0,304)-(648,605)`/`(336,80)-(944,477)` of `(1280,605)` ≈0.36, later
    `snap to (0,0)-(1280,605)` = 1.00.
  - `python tools/xrsim.py --reach-gameplay --duration 40 --interval 300`:
    `gameplay_full_bleed_reached` **PASS (max fill=1.00)**; `scene_advanced`
    **PASS 35/38**; `gameplay_captures_present` PASS 38/0/0; `live_gameplay`
    `{gameplay 38, blank 0, distinct 35}`. Only FAIL = `frame_budget`
    (mock-cadence artefact).
  - Offline: `test_xrsim.py` **32/32**; `xrsim.py --selftest` PASS;
    `verify_reported_fixes` PASS; all 8 `tools/test_*.py` + `validate_addresses` +
    `verify_hook_init` + `scenario_tests.py --check` PASS. No C++/asset changed →
    no `build-all.ps1`/CTest (documented).
- Blocker: **not full VR portability.** #71 requirement 1 is now met *structurally*
  (full-bleed scene reached) but the `cinemaMode` bit still mislabels the static 2D
  splash as gameplay (req 2) and blank warm-up frames are reported, not skipped
  (req 3). Filed **GH #72** with the decoded evidence (BarScan fill 0.36 vs 1.00).
  New recorded risk: live gameplay frames register **`dy=±4`** in the stereo diff on
  very dark scenes (mean luma ≈2.5-13) — likely a low-contrast registration
  artifact, **not claimed** as a stereo defect until reproduced on a brighter frame
  (#72).
- **Next:** #72 — fix the branch signal (use the BarScan active-rect fill; mind
  #44) + the cold-start content gate; then **#67 step 2** (identify the
  first-person viewmodel pass) on top of a real gameplay capture; defer scenario
  playback until gameplay is reached (#71) so scripted interaction runs at ~90 Hz.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 41 — 2026-10-16 19:00 (scheduled loop)
- **Unit:** GH **#69 requirement 1** (the last open item; wake 40's stated Next) —
  land the **gameplay-only XRSIM dump gate** so the capture window is the branch
  whose stereo is meaningful, then live-validate it. One unit (host C++ + tool +
  tests + docs); off by default; no deploy; no game/asset change.
- Finding 1 (gate works, decisive, measured): before, the mock host dumped on a
  fixed cadence so a 180-frame scripted run captured **5 cinema + 8 gameplay**
  frames (the cinema frames, incl. the all-zero interop warm-up, have identical
  eyes *correctly*). After gating on the hook-published branch: **5 gameplay / 0
  cinema / 0 unknown of 5**, every frame `dx=-14 dy=0 identical=False`, markers
  still PASS.
- Finding 2 (req 1 acceptance **not** met — the residual is re-scoped): the
  gameplay-only window is a **static startup-splash page**, not live gameplay.
  `scene_advanced` **FAIL 1/5**, and a `--frames 900 --interval 150` probe is
  still **1/6 distinct** (all dumps byte-identical). Rendered, the dump is the
  **legal/attribution text page** (`© 2009 Atari…`); with A taps it advances only
  to the **ESRB "MATURE 17+" page**. The hook logs
  `DisplayMode: FULL-BLEED gameplay (3Dfresh=1 menuCursor=0)` for those static 2D
  pages → `cinemaMode` (the only branch bit) **mislabels them as 3D gameplay**.
  The built-in loop (which *does* tap Button A every 60 frames) stalls on the
  same ESRB page, so the stall is **not scenario-specific**. The experimental
  melee A-tap scenario edit was **reverted** (it did not reach gameplay and the
  built-in loop proves it is not the cause).
- Change (host C++ + tools/tests/docs; off by default; no deploy):
  - `src/host/dump_gate.h` (new): `RiddickVR::ShouldDumpFrame(dumpDue,
    dumpGameplayOnly, stateRead, cinemaMode)` — legacy when off; when on, capture
    only a *known* gameplay branch (`stateRead && !cinemaMode`), never an unread
    state.
  - `tests/dump_gate_tests.cpp` (new) + CTest `dump_gate_gameplay_only` (built
    and run on **x86 and x64**): cadence, legacy pass-through, gameplay-only
    semantics, unknown-branch rejection.
  - `src/host/openxr_manager.{h,cpp}`: `m_dumpGameplayOnly` + `SetDumpGameplayOnly`;
    the mock loop calls the gate and the `display_branch.log` header/trunc is now
    keyed on the first **dumped** frame (`dumps == 0`), so it is written even when
    the early frames are skipped; a `[Host] Dump gate: gameplay-only` line.
  - `src/host/main.cpp`: `--dump-gameplay-only` (help text + `SetDumpGameplayOnly`).
  - `tools/xrsim.py`: extracted `build_host_cmd(...)`; `--dump-branch
    {gameplay,any}` (default `gameplay`) → scripted runs pass
    `--dump-gameplay-only`; `--dump-branch any` keeps the legacy cadence.
  - `tools/test_xrsim.py` (+2 → **27**): default requests the flag and threads the
    scenario; `any` omits it.
  - `tools/verify_reported_fixes.py` (extended #69 invariant),
    `src/host/README.md`, `docs/adr/0447-xrsim-gameplay-only-dump-gate.md` (new).
- Evidence:
  - **Live** `python tools/xrsim.py --scenario tools/scenarios/melee.json
    --timeout 150` (allowed exception; `RIDDICKVR_SIM=1`; scratch artifacts):
    `gameplay_captures_present` **PASS — 5 gameplay / 0 cinema / 0 unknown of 5**;
    all 5 `dx=-14 dy=0 identical=False`; `display_branch.log` all `cinema=0`;
    markers PASS; `scene_advanced` FAIL 1/5; `frame_budget` FAIL (mock cadence).
  - Probes: `--frames 900 --interval 150` → 6 gameplay/0 cinema, 1/6 distinct; the
    built-in loop `--frames 600 --interval 60` → 9 gameplay/0 cinema, 3/9 distinct,
    first 2 dumps all-black (interop warm-up), content = the same splash page.
  - Offline: `python tools/test_xrsim.py` **27/27 PASS**; `xrsim.py --selftest`
    PASS; `scenario_tests.py --check` PASS; all 9 `tools/test_*.py` +
    `validate_addresses` + `verify_hook_init` + `verify_reported_fixes` PASS.
  - `tools/build-all.ps1 -Config Release` OK (x86 `opengl32.dll` + x64 host);
    CTest **10/10 PASS** x86 **and** x64 (new `dump_gate_gameplay_only`).
- Blocker: **not** full VR portability — req 1's *mechanism* is landed but its
  *acceptance* is blocked: the mock run never leaves the startup splash and
  `cinemaMode` mislabels static 2D splash pages as gameplay. Filed **GH #71**
  (reach a live, advancing 3D scene + fix the branch signal; related to #43).
- **Next:** #71 — get a scripted run into live, advancing 3D gameplay (fresh
  input pacing / longer window / direct level start) and make the branch signal
  exclude the static splash; then **#67 step 2** (identify the first-person
  viewmodel pass) on top of a real gameplay capture.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 40 — 2026-10-16 18:40 (scheduled loop)
- **Unit:** GH **#69** (top vehicle #68's residual requirement 4) — make the
  scenario **interaction markers reproducible** under the mock. Two independent
  defects found and fixed; all offline-tested and one live-validated run. One
  unit (harness + scenario), no C++/asset change, no deploy.
- Finding 1 (harness defect, decisive, measured): `tools/xrsim.py` used a
  **byte offset** as the run boundary — `log_mark = LOG_FILE.stat().st_size`
  *before* launch, then `LOG_FILE.read_bytes()[log_mark:]`. But
  `Logger::Initialize` (`src/hook/logging.cpp:24`) opens `riddick_vr.log` with
  `std::ios::out | std::ios::trunc`, so **every launch rewrites the file from
  offset 0**. A pre-run byte mark is not a run boundary: when the previous run's
  log was longer than the new one the slice is empty and markers logged early in
  the new run vanish. Measured on this wake's run: the wake-39 log is **4108 B**,
  the new (truncated) log is **3404 B** with `COMBAT MODE entered` at byte
  **2292** and `RIOT BATON ARC JAB` at byte **2673** → the old `[4108:]` slice is
  **empty**. That is exactly why wakes 38/39 recorded "markers do not fire".
- Finding 2 (scenario defect, decisive, from the wake-39 log): the old
  `melee.json` held **both** triggers (0.90) through the punch, so block stayed
  engaged (`Combat State: AIM / BLOCK engaged (Left Trigger)`) and the forward
  thrust hit the **parry** branch (`isForwardParry` @4400) *before* the
  trajectory jab — `RIOT BATON ARC JAB` was unreachable, and the log proves it
  (`Physical Gesture: PARRY / COUNTER-STRIKE THRUST`). The scenario also let
  `ApplyScenarioFrame` overwrite the mock's built-in forward walk (keyframe
  `stickLY` defaults to 0).
- Change (tools/tests/docs/scenario; **no game/asset change**):
  - `tools/xrsim.py`: `RUN_HEADER` (`"=== Riddick VR Log Started at"`),
    `extract_run_section(text)` (text after the **last** run header; whole text
    when absent), `read_fresh_hook_log(path, log_mark=0)` (header first; byte-mark
    fallback only for a header-less producer), `marker_summary(checks)`; `run_sim`
    and `--evaluate-only` both use it; report gains `markers` + `hook_log_scope`.
  - `tools/scenarios/melee.json` (re-scripted so it drives what it asserts):
    walk forward (`stickLY 0.85`), hold both triggers → combat stance, **release
    the block** (`ltrig 0.10`, `rtrig` 0.90 for `isBatonArcJab`), then a fast
    forward right-hand jab (`rz −0.26 → −0.42` over 3 frames ≈ 4.8 m/s); grips
    0.50 so the baton *discharge* branches cannot co-opt the log.
  - `tools/test_xrsim.py` (+4 → **25**): last-run section chosen; a larger
    pre-run byte mark is shown to **drop** the markers while
    `read_fresh_hook_log` keeps them; header-less fallback; `--evaluate-only`
    with `--hook-log`/`--scenario` reports the marker map and still fails an
    un-fired marker.
  - `tools/verify_reported_fixes.py` (extended #69 invariant: the truncating
    writer + header-based extraction), `docs/vr-scenarios.md` caveat,
    `docs/adr/0446-xrsim-current-run-hook-log.md` (new),
    `tests/xrsim/report.json` (regenerated baseline; +2 keys only).
- Evidence:
  - **Live** `python tools/xrsim.py --scenario tools/scenarios/melee.json
    --timeout 150` (allowed exception; `RIDDICKVR_SIM=1`; scratch artifacts):
    `marker:COMBAT MODE entered` **PASS**, `marker:RIOT BATON ARC JAB` **PASS**
    (both from `hook log run section (3404 chars)`, scope `current-run
    section`); `gameplay_captures_present` **PASS — 8 gameplay / 5 cinema / 0
    unknown of 13**; the 8 gameplay frames `dx=-14, dy=0, identical=False`. The
    hook log shows the intended chain: BLOCK engaged → `COMBAT MODE entered` →
    3D gameplay → `EXPLORATION MODE restored` → `Physical Gesture: RIOT BATON ARC
    JAB detected (Vz: 4.80 m/s)`.
  - Offline: `python tools/test_xrsim.py` **25/25 PASS**; `xrsim.py --selftest`
    PASS; `scenario_tests.py --check` PASS; all 9 `tools/test_*.py` +
    `validate_addresses` + `verify_hook_init` + `verify_reported_fixes` PASS; no
    C++/asset changed → no `build-all.ps1`/CTest (documented).
- Blocker: none. Residual #69 gaps (unchanged, **not** this change): requirement
  1 (run the dump window gameplay-dominated) is still open — the transition is
  still cinema 1–60 / gameplay 75–180; the concrete mechanism is host-side (the
  mock host now reads `cinemaMode` per frame, so it can gate dumps on gameplay,
  which would also drop the all-zero warmup frames 1/15/30). Frame 15 `dy=-4`,
  frames 1/15/30 all-zero, and the mock `FrameBudget` fail (mean=15.92
  max=755.38 @11.11 ms, a mock cadence artefact) are pre-existing.
  `semi_play.py`/`self_test_pipeline.py`/`scenario_tests.py --validate` still
  slice by byte mark (documented follow-up).
- **Next:** #69 req 1 — gate mock dumps on the now-published `cinemaMode` (host
  C++ + `build-all.ps1` + reboot the vehicle) so the dump window is
  gameplay-only; then **#67 step 2** (identify the first-person viewmodel pass)
  on top of the reproducible capture.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 39 — 2026-10-16 18:21 (scheduled loop)
- **Unit:** GH **#68** (the XRSIM iteration vehicle, top priority) — do wake 38's
  stated "Next": give every capture a **per-frame display-branch label** (cinema
  vs gameplay) and **persist the mock host stdout**, so an identical-eye capture
  is *attributed* not guessed. Closes #69 requirements 2 and 3 and its acceptance
  ("report states, per frame, whether it was cinema or gameplay"). One unit.
- Finding (decisive, from the code + live run): the harness could not interpret
  its own captures. `stereo_renderer.cpp` deliberately blits both eyes with the
  **same** source rect on the *cinema* branch (no IPD shift) and a half-IPD shift
  on the *gameplay* branch, but nothing recorded which branch produced a dumped
  frame — so "identical eyes" was unjudgeable (wake 38 had to guess). The bit the
  host needs is already published: `StereoRenderer::m_sharedState.cinemaMode`
  (`riddick_vr_protocol.h:89`) read by the mock loop via `ipcHost.ReadState`.
- Change (host C++ + tools/tests/docs; **no game/asset change**):
  - `src/host/openxr_manager.cpp`: hoist the per-frame state read
    (`stateRead && clientState.cinemaMode`); the capture line now carries
    `cinema=<0|1>`, and each dumped frame appends one machine-readable line to
    `<dump-dir>\display_branch.log`
    (`frame=NNNN cinema=<0|1> left=<ok|err> right=<ok|err>`).
  - `tools/xrsim.py`: `read_display_branch_labels()` (parse the label file;
    absent frame ⇒ `unknown`, never assumed gameplay); `persist_host_log()` +
    `host_log_path()` (mock stdout → `<artifacts>\host.log`, stale files cleared
    each run); every stereo check is now tagged `branch=`; on a **cinema** frame
    the two gameplay-only gates (`horizontal_disparity_present`,
    `not_mono_duplicate`) are reported **skipped** (identical eyes are correct
    there) while `both_eyes_non_black`/`interop_produced_content` still apply;
    new `gameplay_captures_present` check (fails when the label file exists and
    **no** frame is gameplay ⇒ a menu-only run cannot pass); `--evaluate-only`
    reuses `<artifacts>\host.log` when present; the report records `host_log` and
    the per-frame `display_branch` map.
  - `tools/test_xrsim.py` (+5 → **21**): label parsing, cinema identical eyes
    skipped-not-failed + cinema-only run fails, gameplay identical eyes still
    fail, the gameplay-presence gate, `--evaluate-only` reads a persisted host log.
  - `tools/verify_reported_fixes.py` (new GH #69 invariant),
    `src/host/README.md`, `docs/adr/0445-xrsim-per-frame-display-branch-label.md`
    (new), `tests/xrsim/report.json` (regenerated baseline).
- Evidence:
  - **Live** `python tools/xrsim.py --scenario tools/scenarios/melee.json
    --timeout 150` (allowed exception; `RIDDICKVR_SIM=1`) reached 3D gameplay:
    `gameplay_captures_present` **PASS — 8 gameplay / 5 cinema / 0 unknown of
    13**; the 8 gameplay frames `dx=-14, dy=0, identical=False`; `display_branch.log`
    shows the transition live (`frame=1..60 cinema=1`, `75..180 cinema=0`);
    `<artifacts>\host.log` persisted (`FrameBudget samples=180 mean_ms=17.42
    max_ms=899.54 over=30 budget=11.11`).
  - Offline: `python tools/test_xrsim.py` **21/21 PASS**; `xrsim.py --selftest`
    PASS; `--evaluate-only --downscale 4` on the committed captures regenerated
    `tests/xrsim/report.json` (branch unknown ⇒ `gameplay_captures_present`
    skipped, not retro-failed); `build-all.ps1 -Config Release` OK (x86
    `opengl32.dll` + x64 host); CTest **9/9 PASS** x86 **and** x64; all 9
    `tools/test_*.py` + `validate_addresses` + `verify_hook_init` +
    `verify_reported_fixes` + `scenario_tests.py --check` PASS.
- Blocker: none. Residual #69 gaps are now **visible and attributed** rather than
  guessed: frames 1/15/30 are all-zero eye buffers (interop warmup, flagged by
  `interop_produced_content`), the melee markers (`COMBAT MODE entered`,
  `RIOT BATON ARC JAB`) still do not fire under the mock, and one transition
  cinema frame (60) showed `dy=-4`.
- **Next:** #69 req 1 — make a scripted run reach live gameplay *earlier* (drive
  past the loading/warmup so the all-zero interop frames shrink) and wire the
  scenario's interaction markers to the mock; then **#67 step 2** (identify the
  first-person viewmodel pass) on top of the now-attributable capture.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 38 — 2026-10-16 18:20 (scheduled loop)
- **Unit:** GH **#68** (the XRSIM iteration vehicle) — make the harness
  **trustworthy and re-runnable without a headset**, then use it to **measure
  stereo correctness live**. Opened **GH #69** for the residual capture gap. One
  concrete unit.
- Context (concurrent writer — re-checked mid-wake): the interactive session
  landed `tools/xrsim.py` (#68) at 17:57, then committed `b97c4a8`
  (`chore(autoloop): prioritize #68 (xrsim) as the iteration vehicle; allow xrsim
  sim runs`) at 17:59 **during this wake**. That commit (a) added all 26 capture
  BMPs (~700 MB) + a `report.json` to git, and (b) **rewrote `prompt.md` to make
  XRSIM the top iteration vehicle and explicitly allow `python tools/xrsim.py`**
  (the one input-injection exception; `RIDDICKVR_SIM=1` suppresses `SendInput`).
  My wake began on the pre-`b97c4a8` prompt (game launch forbidden); after the
  edit I used the now-allowed harness. The 700 MB baseline BMPs are **tracked**, so
  the new `.gitignore` rule only guards *future*, newly-named dumps.
- Finding (decisive, from the on-disk captures — no game launch): all **13**
  `test_frame_*` / `right_*` pairs are **byte-identical** (`np.array_equal == True`,
  `max|L-R| == 0`) and only **3 distinct** images exist across the run (luma
  means 0.000 / 7.644 / 4.975; frames 1/15/30/135 all-zero). The hook log for the
  same run is healthy (`Hardware Interop: SUCCESS`, two distinct handles
  0xC0001E82/0xC0000982, `Stereo Eye Framebuffers ... LeftFBO: 1 RightFBO: 2`,
  `OnSwapBuffers: Frame 195`), and shows the run entered `DisplayMode: CINEMA
  screen ... (3Dfresh=0 menuCursor=1)`.
- Analysis — observation vs inference: the *cinema* branch
  (`src/hook/stereo_renderer.cpp:885-888`) deliberately blits both eyes with the
  **same** source rect ("flat screen: no IPD shift"), so identical eyes are
  *expected* for a 2D menu frame; the *gameplay* branch
  (`stereo_renderer.cpp:898-926`) applies a half-IPD source shift that **must**
  make the eyes differ. Nothing recorded which branch produced a dumped frame, so
  the old capture alone could not distinguish "menu frame (correct)" from
  "gameplay frame with no stereo (defect)".
- **Resolved live (same wake, harness run — now prompt-allowed):**
  `python tools/xrsim.py --scenario tools/scenarios/melee.json` (twice) reaches a
  live scene and the eye pairs then **differ**: `dx=-14 px`, `dy=0`,
  `identical=False`, `not_mono_duplicate` PASS on the live frames. **The mod's
  stereo path DOES produce horizontal disparity with zero vertical disparity on
  live frames** — so the earlier identical-eye result was a **static/menu capture,
  not a stereo defect**. The harness also now re-runs end to end and reports the
  host `FrameBudget` (mean 16.10 ms, max 780.51 ms, 28/180 over the 11.11 ms 90 Hz
  budget — a mock-host/startup cadence artefact, not a measured render cost).
- Change (tools/CI/docs only; no C++/asset change → no build/CTest):
  - `tools/xrsim.py`: new **`--evaluate-only`** (re-analyse existing captures on
    disk with no host/game launch, no input synthesis) that writes
    `tests/xrsim/report.json`; new **`--downscale`** (block-subsample the search
    for multi-megapixel eye buffers), `--artifacts`/`--report-file`/`--hook-log`
    overrides, and **`skip`-semantics** so a missing host log does not fabricate a
    frame-budget failure. New checks: `interop_produced_content` (fires on
    all-zero eye buffers) and `capture:scene_advanced` (frozen-capture detection).
  - `tools/test_xrsim.py` (new): 16 self-contained tests for the analysis half —
    shift/identical/vertical/downscale registration, all-black interop
    attribution, comfort rules, budget parsing, `--evaluate-only` exit codes and
    report file, static-capture flagging, marker counting; **SKIPs cleanly (exit
    0) if numpy/Pillow are absent**.
  - `.gitignore`: ignore `tests/xrsim/artifacts/` for **future** dumps (the 26
    baseline BMPs are already tracked by `b97c4a8`); `report.json` stays tracked.
  - `.github/workflows/vr-worker.yml`: pip-install numpy+pillow, run
    `tools/test_xrsim.py` and `tools/xrsim.py --selftest` (the CI-safe half the
    #68 spec asked for), before the hook-init step.
- Evidence:
  - `python tools/test_xrsim.py` PASS (16/16); `python tools/xrsim.py --selftest`
    PASS; all 9 `tools/test_*.py` + `validate_addresses` + `verify_hook_init` +
    `verify_reported_fixes` + `scenario_tests.py --check` PASS.
  - `python tools/xrsim.py --evaluate-only --downscale 4` on the committed
    captures → `passed=false` (stereo 39 ok / 30 fail; capture `scene_advanced`
    3/13).
  - **Live:** `python tools/xrsim.py --scenario tools/scenarios/melee.json` (×2,
    scratch `--artifacts`) → live frames `dx=-14, dy=0, identical=False` (stereo
    PASS); early frames all-zero (interop warmup) and one identical burst (menu);
    `FrameBudget n=180 mean=16.10 max=780.51 over=28/180 @11.11 ms`.
  - No C++/asset change → no `build-all`/CTest.
- Blocker: none. Caveat (corrected): the earlier "identical eyes" was a **static
  capture, not a stereo defect** — the mod produces disparity live. Residual gaps
  tracked in **#69**: early all-zero frames, menu bursts, no per-frame
  display-branch label, host stdout not persisted, and the melee scenario's
  interaction markers (`COMBAT MODE entered`, `RIOT BATON ARC JAB`) not firing in
  mock. Next: land the per-frame `cinemaMode` dump label + host-log persistence,
  then #67 step 2 (FP viewmodel pass) on top of the now-working capture.

## Wake 37 — 2026-10-16 17:46 (scheduled loop)
- **Unit:** GH **#64** (→ #58/#46) — fix the **native-hand placement** so it is
  side-symmetric and pose-stable, and correct the unsupported "file geometry IS
  the `NODES` bind pose" record left by wake 36. One concrete, offline-verifiable
  defect, with the record correction as an honest rider.
- Finding 1 (placement defect, decisive, from the committed AABBs): ADR-0427
  derived the arm's long axis as the mesh AABB's **largest extent**. That holds
  for the file geometry but not for the ADR-0443 **posed** variant: the posed
  right arm's AABB becomes `x 7.93 / y 7.82 / z 8.25` (nearly cubic), so the
  largest-extent rule picks **Z**, and the placement becomes `up = model Y`,
  `backward = -model Z`, anchor `z = 54.61` — the right hand rotated ~90° about
  the longitudinal axis, mis-anchored, and no longer the mirror of the left
  (left stays `long_axis = Y`, basis `X(-1,0,0) Y(0,0,1) Z(0,1,0)`, anchor
  `y = -26.07`). A ~4% extent difference flips the whole frame. Enabled by
  `NativeHandPose=1` (default off), but shipped code.
- Finding 2 (record correction, decisive): `tools/xmd_fk.py --check`'s
  "the file geometry IS the bind pose" claim is a **tautology**. At bind every FK
  world map is the identity, so the weighted blend equals `position·Σw` and
  `pose_drift_max ≡ 0` for *any* stored geometry — it tests the recipe, not the
  content, and cannot detect a cluster stored in a different reference pose.
  ADR-0424's direct measurement stands and reproduces: `xmd_pose` relations
  `c1↔c6 = 26.582`, `c2↔c6 = 27.273` (arms vs body) vs `c0↔c6 = 0.931`
  (legs↔body), arm/body AABBs disjoint. The ADR-0441 recipe stays valid (it
  mirrors the engine, which applies the same deltas to the stored geometry); only
  the wording is wrong.
- Change (code/asset/tests/docs):
  - `tools/xmd_bake.py`: new `_long_axis()`; bakes `arm_axis` (model axis 0/1/2
    of the file-geometry long direction) into the record, the emitted
    initializer and the manifest; docstring corrected.
  - `src/hook/native_hand_mesh.h`: `int arm_axis` field; the provenance comment
    rewritten to the measured truth (the ADR-0442 check is tautological;
    ADR-0424 stands).
  - `src/hook/native_hand_pose.h`: new
    `ComputeNativeHandPlacementAlongAxis(min,max,isLeft,long_axis,scale)`;
    `ComputeNativeHandPlacement` keeps its signature and now selects the largest
    extent and delegates.
  - `src/hook/vr_hand_renderer.cpp`: `DrawNativeHand` uses `mesh->arm_axis` when
    `0..2`, else the legacy heuristic.
  - `src/hook/native_hand_mesh_data.h` (regenerated; 4 meshes / 1548 verts / 2610
    tris + 870 posed; `arm_axis 1` on all four).
  - `tests/native_hand_pose_tests.cpp`: explicit-axis override, out-of-range /
    zero-extent rejection, and **both arms, bind and posed, mirror-symmetric**
    (backward is ±model Y, never flipped to Z).
  - `tests/native_hand_mesh_tests.cpp`: `arm_axis` frozen per mesh + "is the
    file-geometry long axis".
  - `tools/test_xmd_bake.py` (+2): baked axis recorded; `_long_axis` ties.
  - `tools/test_xmd_fk.py` (+1): `test_bind_check_is_tautological` — a geometry
    shifted >5 units from every bone still yields `pose_drift_max < 1e-12`, so
    `--check` would not flag it.
  - `tools/verify_reported_fixes.py`: GH #64 invariant (baked axis + the
    corrected "BIND-POSE PROVENANCE" note).
  - `src/hook/README.md`; `docs/adr/0444-native-hand-placement-baked-axis.md`
    (new); ADR-0443 gets a correction pointer.
- Evidence (read-only, 2004 EFBB): `python tools/xmd_pose.py --model
  .../Riddick_B05.xmd` (relations above); `tools/xmd_bake.py --cpp … --manifest …`
  wrote 4 meshes and `--verify` → OK; `build-all.ps1 -Config Release` OK (x86
  `opengl32.dll` + x64 host); CTest **9/9 PASS** on x86 **and** x64; all 8
  `tools/test_*.py` + `validate_addresses` + `verify_hook_init` +
  `verify_reported_fixes` + `scenario_tests.py --check` PASS.
- Blocker: none. Caveat (not claimed): `arm_axis` fixes the frame's *stability
  and symmetry*, not its absolute orientation — no headset was used, so
  `NativeHandYaw/Pitch/Roll`/scale/offsets still need on-headset tuning;
  `NativeHands`/`NativeHandPose` stay off by default; ADR-0424's raised
  reference pose (the missing per-cluster transform, GH #59) is still open.
- **Next:** headset A/B `NativeHands=1` + `NativeHandPose=0/1` with the placement
  knobs (the two hands are now mirror-consistent), then land the GH #34 runtime
  read of the live `poseDesc` matrix vector and cross-check it against `xmd_fk`;
  or chase the per-cluster reference transform that reconciles ADR-0424's raised
  arm pose with the `NODES` bind.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 36 — 2026-10-16 17:22 (scheduled loop)
- **Unit:** GH **#58** (→ #46/#64) — pin the **first-person arm pose source** and
  make the offline pose *drawable*: bake a posed variant of the native hand
  asset from the engine's own first-person clips and select it behind an INI
  flag. No game process, no runtime read (GH #34 still open).
- Finding 1 (pose source RESOLVED): the first-person arms are driven by the
  `FPS*.xsa` clips in `Content/Anim/Main` (`FPSAssault`/`FPSPistol`/`FPSMini`/
  `FPSGrenade`/`FPSMelee`). Decisive cross-check: `Riddick_B05`'s 70-node
  `NODES` skeleton uses rotation slots `0..69`, and every `FPS*` rotation window
  covers them (`FPSPistol`/`FPSAssault` = 76, others 71-72); posing the model
  from `FPSPistol:Pis_MhStd_Dwn` resolves **all** slots with **no mapping
  notes**. No separate first-person arm model exists under
  `Content/Models/Characters/Riddick/` — the clips key the same Riddick skeleton
  the baked arm clusters are skinned to (cluster 1 bones
  `[1,8,9,12,13,14,16,17,18,38..52]`, cluster 2 `[9,17..19,21,53..67]`).
- Finding 2 (canonical frame): the clips are all actions (aim/hit/equip/throw);
  the most neutral shipped frame is the **melee unequip end** —
  `FPSMelee:Mel_ShaUnEquip` last key (`KEYFRAMETIMES[17] = 0.3334 s`) moves the
  left arm 10.47 and the right 12.01 model units, vs 27.3/34.9 for
  `Pis_MhStd_Dwn` and 32.6/42.2 for `Min_StdClo`.
- Change (code/asset/tests/docs):
  - `tools/xmd_bake.py`: `--anim/--sequence/--pose-key` (+ `--no-pose`).
    `load_pose()` resolves the canonical `POSE_SPEC` (pinned by KEYFRAMETIMES
    **index**, so the bake is deterministic) and `_apply_pose()` evaluates the
    ADR-0441 recipe via `tools/xmd_fk.py` (`sample_sequence` → `world_matrices`
    → `skin_positions`/`skin_normals`) for every **skinned** cluster. The header
    gains `<ident>_Posed_Vertices[]`, the posed AABB and the clip provenance
    (name/sha256/size/time); the manifest gains a `pose` block +
    `totals.posed_vertices` + per-mesh `pose_notes`.
  - `src/hook/native_hand_mesh.h`: optional `posed_vertices` / `pose_source` /
    `posed_bounds_min|max` + `has_pose()`. Also corrected the ADR-0424 "large
    baked reference pose" note — ADR-0441/0442 prove the file geometry *is* the
    bind pose (the arms merely rest spread along ±Y).
  - `src/hook/native_hand_mesh_data.h` (regenerated): 4 meshes / 1548 verts /
    2610 tris + **870 posed vertices**; `Pa2_Hand*` are unskinned → correctly
    unposed. `--verify` confirms the committed header is a fresh bake.
  - `src/hook/vr_config.{h,cpp}`: `nativeHandPose` (`[VR] NativeHandPose`,
    default **0**) + the INI log line.
  - `vr_hand_renderer.cpp` `DrawNativeHand`: selects the posed arrays **and**
    the posed AABB when `NativeHandPose` is on and a pose exists; bind geometry
    otherwise (unchanged default).
  - `tests/native_hand_mesh_tests.cpp`: posed topology/finite/unit-normal/bounds
    checks, both arms must carry a pose that differs >1 unit from bind, the
    unskinned Pa2 clusters must not.
  - `tools/test_xmd_bake.py`: **+4** tests (emitted pose == `xmd_fk`'s skinning
    of the same clip/key to 1e-9, ≥1-unit displacement, unskinned skip, CLI +
    `--verify` + bad sequence/key error paths, `--no-pose`).
  - `riddick_vr.ini`, `tools/riddick_vr.ini.example`, `src/hook/README.md`,
    `tools/verify_reported_fixes.py` (new #58 invariant),
    `docs/rea/mos-xmd-format.md`, `docs/adr/0443-first-person-arm-pose-source.md`.
- Evidence (read-only, 2004 EFBB):
  - `python tools/xmd_bake.py --cpp src/hook/native_hand_mesh_data.h
    --manifest docs/rea/reference/native_hand_mesh.json` → 4 meshes / 1548 verts
    / 2610 tris / 870 posed; provenance in the header
    `FPSMelee.xsa:Mel_ShaUnEquip@key17=0.333400s`, clip sha256
    `d6fbb681…c6ef7e` size 21068; `--verify` → OK.
  - `tools/build-all.ps1 -Config Release` OK (x86 `opengl32.dll` + x64 host);
    CTest **9/9 PASS** on x86 **and** x64 (`native_hand_mesh_asset` now covers
    the pose); all 8 `tools/test_*.py` + `validate_addresses` +
    `verify_hook_init` + `verify_reported_fixes` + `scenario_tests.py --check`
    PASS.
- Blocker: none for landing. Caveat (not claimed): the pose is **static** and
  **unvalidated** — `NativeHandPose` defaults off; the placement is still the
  provisional ADR-0427 AABB/basis mapping (the bind pose carries no bone
  *orientation*, so no authoritative hand-bone frame can be derived offline);
  `.xsa` sampling stays the nlerp/lerp approximation (ADR-0422); the
  QUATERNIONS-pool movement slot is reported as zero translation. Player-visible
  #64/#61/#65/#62/#63 still need a headset pass.
- **Next:** headset A/B `NativeHandPose=0/1` (one key) together with the
  placement knobs, or land the **GH #34** runtime read of the live `poseDesc`
  matrix vector and cross-check it against `xmd_fk` (the offline and runtime
  paths are now independently comparable). Re-bake another clip/weapon pose with
  `--anim/--sequence/--pose-key` if the melee-rest frame is not the right fit.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 35 — 2026-10-16 17:00 (scheduled loop)
- **Unit:** GH **#58** (→ #46/#60) — take wake 34's "Next" and make the offline FK
  recipe *executable*: land `tools/xmd_fk.py`, the offline forward-kinematics +
  skinning evaluator for `.xmd` models, and confirm ADR-0441 over the whole
  shipped corpus. No C++/asset change (the native draw stays rigid, ADR-0436).
- Change (tool/tests/docs/CI):
  - `tools/xmd_fk.py` (new, dependency-free): the ADR-0441 algebra as code —
    `quat_to_mat3`, `local_from_bind(bindpos,quat,move)` = `v ↦ R·(v−bindpos)+
    bindpos+t`, `world_matrices` (`world=parent·local`, cycle-guarded),
    `skin_positions`/`skin_normals` (influence **weighted sum**, rotation-only
    normals), and `pose_from_sequence` which maps a clip's `rotation`/`movement`
    window slots to each node's `rotation_slot`/`movement_slot` (ADR-0422) and
    samples via `xsa_anim`. CLI: `--bind`, `--anim/--sequence/--time`,
    `--joints`, `--obj` (posed Wavefront), `--check`, `--inventory`, `--json`.
  - `tools/test_xmd_fk.py` (new): **10** synthetic-container tests (no game).
  - `.github/workflows/vr-worker.yml` runs the new test;
    `docs/adr/0442-offline-fk-skinning-recipe.md` (new) + an "Implemented offline
    (ADR-0442)" paragraph in `docs/rea/mos-xmd-format.md`.
- Evidence (read-only, 2004 EFBB):
  - **Bind == file geometry corpus-wide (decisive for ADR-0441):**
    `xmd_fk.py --inventory <game>\Content\Models --check` → **268 models / 3454
    clusters / 3103 skinned / 915508 vertices**, worst bind **pose** drift
    **1.42e-14**, worst weight error 3.92e-3, **0 problems**. The file mesh *is*
    the bind pose everywhere (no bind-inverse, no load-time rebase).
  - `Riddick_B05.xmd`: per-cluster bind drift ≤ 7.95e-15.
  - A real clip poses the arms: `--cluster 1 --anim Content\Anim\Main\Use.xsa
    --sequence use_pad_rid --time 0.2` → 12.4 model-unit displacement on the left
    arm (≈0.4 m at 0.033), no mapping notes; posed OBJ written (526 verts).
  - Design honesty: weights are **not** renormalised (the engine sums the maps);
    `pose_drift_max = |blend − pos·Σw|` isolates the *pose* from the weights so
    the corpus check tests the RE, not the content; the QUATERNIONS-pool movement
    slot (TRACK_FLAG_MOVEMENT) is reported, not guessed.
  - All `tools/test_*.py` (8 incl. the new one), `validate_addresses`,
    `verify_hook_init`, `verify_reported_fixes`, `scenario_tests.py --check` PASS.
    No C++/asset changed → no `build-all.ps1`/CTest rerun.
- Blocker: none. Caveat (not claimed): the draw path is unchanged (rigid,
  ADR-0436) and no headset was used; `.xsa` sampling stays the nlerp/lerp
  approximation (ADR-0422), not the engine's Hermite spline; the FP-arm **pose
  source** (which clip/predicate) and the runtime matrix vector (GH #34) remain
  unidentified.
- **Next:** pick the first-person arm pose source (GH #58 — inventory
  `Content\Anim\Main` for the Riddick FP/weapon clips keyed to `Riddick_B05`'s
  arm bones) and drive a skinned variant of `DrawNativeHand` from `xmd_fk`-style
  deltas behind a flag; or land the GH #34 runtime read and cross-check it against
  `xmd_fk`. Player-visible #64/#61/#65/#62/#63 still need a headset pass.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 34 — 2026-10-16 16:42 (scheduled loop)
- **Unit:** GH **#60** step 1 (RE, read-only) — resolve the ADR-0440 "Next probe":
  does the draw's `poseDesc+0x14` matrix list alias the FK output `+0x30`/`+0x34`,
  and is there a load-time vertex rebase or a bind-inverse step? Result: **both
  halves answered; the ADR-0440 (a)/(b) dichotomy was false.**
- Finding 1 (aliasing RESOLVED): `FUN_1001de10` (the descriptor array (re)sizer,
  called by the FK builder `FUN_10021650`) **caches the FK arrays' data pointers**:
  `poseDesc+0x30 = *(*(poseDesc+0x14)+0x18)` (world) and
  `poseDesc+0x34 = *(*(poseDesc+0x0c)+0x18)` (local); on any length change both are
  re-initialised to identity (12 floats) and the `+0x1c` weight list is zeroed. The
  draw (`FUN_1002dcc0`) consumes exactly `*(*(poseDesc+0x14)+0x18) + bone*0x30`, so
  **the skin matrix list IS the FK output** (`*(poseDesc+0x30)`), bit-identical.
- Finding 2 (no bind-inverse): the skinning's per-bone fetch
  `FUN_10022f90(provider, bone)` returns `matrices[palette[bone]]` (or `matrices
  [bone]`) — a raw 0x30-byte fetch, no arithmetic. `provider` is the `FUN_100238a0`
  bundle `{+0x00 world array, +0x04 palette, +0x08 n}`. Option "(b) a bind-inverse
  between the FK output and the consumed list" is **dead**.
- Finding 3 (plain affine): the single-influence branch of `FUN_100254e0` is
  `out = v·R + T` (translation added directly); multi-influence is the weight-sum of
  the same maps. **No bind subtraction anywhere.**
- Finding 4 (decisive): `FUN_1001f340` overwrites the local translation to
  `bindpos − R·bindpos + t` ⇒ local affine `v ↦ R·(v − bindpos) + bindpos + t`
  (rotate about the bone's bind position). At bind (identity quats, t=0) every local
  → identity and the root parent is identity, so **every world → identity**: the
  "world" matrices are **bind-relative deltas**, applied directly to the
  **model-space file geometry**. Hence there is *neither* a load-time rebase *nor* a
  bind-inverse — and the model-space `Riddick_B05` refboxes render correctly.
- Finding 5: the vertex accessor `FUN_1007ee20(mesh,variant)` returns the raw
  per-variant position buffer (`*(listobj+0x18)`; streamed via `FUN_1007c6c0` when
  non-resident) — no transform.
- Change (docs/tool, no C++/asset): wake-34 addendum in
  `docs/rea/reference/MXR_SkeletonInstance_WorldMatrices.txt`; new
  `docs/adr/0441-fk-world-matrices-are-bind-relative-deltas.md`; the ADR-0440
  "Next probe" in `docs/rea/mos-xmd-format.md` answered + closed; new
  `tools/autoloop/extract_pseudo.py` (focus a `rea function --format json` capture
  to its pseudocode — handles the utf-8-BOM / utf-16 captures the loop produces).
- Evidence: `rea function MXR.dll` on `0x1001de10`, `0x100254e0`, `0x10022f90`,
  `0x1007ee20` → `tools/autoloop/logs/wake34_func_*.json|.pseudo.txt`; plus the
  wake-33 `FUN_1001f340` capture. Provider Ghidra 12.1.4 (MXR.dll sha256 `06424f55…`).
  No C++/asset changed → no `build-all.ps1`/CTest; `verify_reported_fixes`,
  `verify_hook_init`, `scenario_tests.py --check`, `test_mos_xmd`,
  `validate_addresses` all PASS. Moved the stray untracked `mxr` REA dump (a
  duplicate of the wake-20 `CreateMatrixIW` xrefs query) into
  `tools/autoloop/logs/wake20_stray_mxr_xrefs.json` (gitignored).
- Blocker: none — #60 step 1 is now fully answered. Caveat (inference, not
  exhaustive): the **weighted/connected** local branch (`connect != −1`,
  `weight != 0`) applies a weight-blended bind-offset correction whose exact algebra
  was not expanded (verified identity-at-bind for the zero-weight/unconnected
  branch only). No runtime read was taken (that is still GH #34).
- **Next:** the offline skinned native hand recipe is now pinned — FK from `NODES` +
  a `.xsa` pose → per-bone bind→animated **deltas**, applied to the model-space
  `.xmd` geometry, with **no** per-cluster bind transform to find. Concretely:
  drive the ADR-0436 native draw from `tools/xmd_pose.py`-style deltas for a
  rest/animated hand (bounded, offline, testable without the game), or land the
  GH #34 runtime read of the `poseDesc+0x14` list.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 33 — 2026-10-16 16:21 (scheduled loop)
- **Unit:** GH **#60** step 1 (RE, read-only) — identify the owner class + builder
  of the live per-bone matrix array and decide `world·bind_inverse` vs raw
  `world` (the "Next probe" left by ADR-0430).
- Finding (decisive for #60 step 1): the builder is the `CXR_SkeletonInstance`
  **forward kinematics** chain `FUN_10021650` (a **virtual** method: its code
  pointer sits at file `0x14354C` in `.rdata`, immediately before
  `"Obsolete function"`/`".\XRSkeleton.cpp"`/`"EvalAnim"`/`"NODEINDICES"`/`"NODES"`;
  RTTI `.?AVCXR_SkeletonInstance@@` @ `0x1016a4c0`) → `FUN_10020b50` (evaluate the
  local pose: quaternion pool → `poseDesc+0x24`, translation pool → `+0x2c`) →
  `FUN_1001f340` (tree walk). It composes
  **`world[node] = world[parent] · local[node]`** via `FUN_1001db60` (3×4 concat,
  algebra verified) and stores **raw world** matrices at `*(poseDesc+0x30)+node*
  0x30` (local at `*(poseDesc+0x34)+node*0x30`). **No bind/reference/inverse is
  composed anywhere in the chain.** `FUN_10021650` also validates a caller root
  4×4 (finite) and pre-multiplies it into the pose (`flags` modes). Node records
  are 0x20 B (`this+0x14` list; `this+0xc` = child-index list): pos `[0..2]`,
  child start `short @+0x0e`, child count `byte @+0x10`, connect `short @+0x12`,
  rotation slot `short @+0x1c`, movement slot `short @+0x1e`. The draw
  (`FUN_1002dcc0`) consumes `*(*(poseDesc+0x14)+0x18)+bone*0x30` where
  `poseDesc = modelInstance[0x94]` (override `*(arg6+0x3c)`) and `bone` = the
  per-cluster bone index.
- Tension recorded (NOT resolved): `Riddick_B05` skinned clusters are authored in
  **model space** (`--mesh` refboxes: arm clusters `y −25.4..6.4`, `z 32.8..52.5`,
  matching the `NODES`/live extents) — so *if* the mesh buffer handed to
  `FUN_100254e0` is that data, the consumed matrix must be `world·bind_inverse`;
  the FK output is raw `world`. Either the engine rebases the vertex buffer to
  bone-local at load, or a bind-inverse step exists between the FK output and the
  consumed `poseDesc+0x14` list (which was not resolved to alias `+0x30`/`+0x34`).
- Change (docs only, no C++/asset): new
  `docs/rea/reference/MXR_SkeletonInstance_WorldMatrices.txt` and
  `docs/adr/0440-skeleton-instance-world-matrix-builder.md`;
  `docs/rea/mos-xmd-format.md` provider "Next probe" answered + re-pointed.
- Evidence: `wake33_search_skeleton_*`, `wake33_func_10021650/10020b50/1001f340/
  1001db60.*`, `wake33_a2o_vtable.json`, `wake33_b05_skeleton.json`,
  `wake33_b05_mesh.json` under `tools/autoloop/logs/`. No C++/asset changed → no
  `build-all.ps1`/CTest; `test_mos_xmd`, `test_mos_xtc`, `test_xmd_export`,
  `test_xmd_bake`, `test_xmd_pose`, `test_xsa_anim`, `test_xtc_bake`,
  `validate_addresses`, `verify_hook_init`, `verify_reported_fixes`,
  `scenario_tests.py --check` all PASS. (`python -m pytest tools` errors at
  collection — pre-existing harness/path quirk; the loop runs the scripts
  directly, which PASS.)
- Blocker: the `poseDesc+0x14` vs `+0x30`/`+0x34` aliasing and the model-space vs
  bone-local question need either more RE or a runtime read (GH #34). **Next:**
  find the writer of `poseDesc+0x14` (or the load-time vertex rebase); then a
  skinned native hand can be built offline as FK(`NODES`+`.xsa`) with a known
  bind step.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 32 — 2026-10-16 15:27 (scheduled loop)
- **Unit:** GH **#66** (approach 2) — land the per-draw infrastructure to suppress
  the engine's own first-person viewmodel, plus the RE evidence for *which* draw
  entry points the engine actually uses. Off by default; merged and tested.
- Finding (decisive, read-only, all 4 RndrGL builds incl. x86): **`RndrGL.dll` is
  the only shipped module that imports OpenGL** (MXR/MSystem/MCCDyn/SbzEngine/
  GameClasses/GameWorld import none). Its 3D mesh path is **`glDrawElements`**;
  **`glDrawArrays` is imported by no shipped module** (the proxy's forwarder is
  dead for this title). It also imports `glCallList`, but **no** module imports
  the list builders (`glNewList`/`glGenLists`/`glEndList`) nor the GL-metafile
  playback names — `glCallList`'s origin is an open question (recorded). It does
  **not** import `glDepthRange`/`glMultMatrixf`/`glRotatef`, so the depth-bias
  trick is not used and depth-range is not a usable signal.
- Change (code/tests/docs):
  - `src/hook/viewmodel_suppressor.h` (new, header-only, dependency-free):
    `ViewmodelSuppressor` + `ViewmodelWindowOpener`. Frame boundary = full colour
    clear + swap; a **depth-only** clear opens the window (PROVISIONAL opener);
    the next projection load or the swap closes it. Inert (pure `false`) when
    disabled.
  - `dllmain.cpp`: `InstallRndrGLHooks` now IAT-hooks `OPENGL32.dll!
    glDrawElements` + `glCallList` in `RndrGL.dll` (same `IATHook` as
    `GDI32!SwapBuffers`), forwarding to the **saved original** (the proxy's own
    forwarder) when the window is closed → no export/`.def`/forwarder change.
  - `opengl_proxy.{h,cpp}`: drive the window from the already-intercepted
    `glClear` (colour vs depth-only), `glLoadMatrixf`/`glFrustum`/`glOrtho`
    (projection load) and `Hook_wglSwapBuffers`; expose
    `ShouldSuppressDrawCall()`; log per-frame tallies every 300 swaps.
  - `vr_config.{h,cpp}`: `SuppressEngineViewmodel=0` (default off) +
    `ViewmodelDepthClearWindow=1`; re-read on each `glClear` (live tuning).
  - `tests/viewmodel_suppressor_tests.cpp` + CTest `viewmodel_suppressor_window`;
    `CMakeLists.txt`; `riddick_vr.ini` + `tools/riddick_vr.ini.example`;
    `src/hook/README.md`; `tools/verify_reported_fixes.py` #66 invariant;
    `docs/rea/reference/RndrGL_GL_Path.txt`;
    `docs/adr/0439-per-draw-viewmodel-suppression.md`.
- Evidence: `build-all.ps1 -Config Release` OK (x86 `opengl32.dll` + x64 host);
  CTest **9/9 PASS** x86 **and** x64 (new `viewmodel_suppressor_window`); all
  `tools/test_*.py` PASS; `verify_reported_fixes`, `scenario_tests.py --check`,
  `verify_hook_init`, `validate_addresses` all PASS.
- Blocker: **not validated in-headset** — the depth-only-clear signature is a
  hypothesis; the per-frame counters exist to confirm it in one run. Default off,
  so no behaviour change ships.
- **Next:** run once with `SuppressEngineViewmodel=1`, check the
  `Viewmodel suppression [#N]: draws=N suppressed=M` log against the FP pass;
  add a camera-space / projection-fingerprint opener if the depth-clear boundary
  proves unreliable. Then #61 UV V-flip and #64 orientation headset passes.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 31 — 2026-10-16 15:00 (scheduled loop)
- **Unit:** GH **#63** (duplicate hands) — resolve the last open approach-1
  unknown (the `noviewmodel` **draw gate**) and re-direct the issue. Result:
  **negative** (no draw gate exists), so #63 moves to approach 2.
- Finding 1 (real evidence, read-only; Ghidra 12.1.4 on
  `Content/GameClasses_Win32_x86.dll`, sha256 `1898b736…`): the engine **sets and
  clears the `noviewmodel` bit itself**. `FUN_100e6aa0`
  (`0x100e6aa0..0x100e6b3c`, reached only via the vtable at VA `0x101c67e0`,
  slot `+0x40`) is a per-frame item update: `flags |= 0x2` once a saturating
  counter `+0x212` exceeds byte threshold `+0x210`; `flags &= ~0x2` once `+0x212`
  exceeds `+0x211` **and** `*(short*)(+0x204) != 0`; both transitions call
  `FUN_100dcb90(this, 0x1056, 0, -1, -1)` (anim property/event). So the bit is a
  runtime *output* of an item state machine, not a boolean draw-gate input.
- Finding 2 (decisive, #63): a raw-byte scan of **every** shipped x86 module
  (GameClasses, GameWorld, MXR, MSystem, MCCDyn, RndrGL, `SbzEngine.exe`) finds
  **no direct mask reader** of `[+0x14c] & 2`:
  - exactly one immediate bit-1 test on `+0x14c` anywhere — `0x10037111
    test byte [eax+0x14c],2`, a *different* object's field (player-start/
    camera-entity selection, ADR-0435);
  - `test al,2` / `test eax,2` / `and reg,2` / `bt reg,1` within 40 B of any
    `+0x14c` read occur **only inside `FUN_100e6aa0` itself** (172 read sites in
    GameClasses);
  - no `(flags >> 1) & 1` form near a `+0x14c` read.
  Documented false positives: `0x1013F898` (a `JNZ` rel32 operand) and
  `0x10052C84` (bytes of `CMP dword [esi+0x1a8],0`).
- Change (docs only, no C++/asset): new
  `docs/rea/reference/GameClasses_NoviewmodelRuntime.txt` (toggler pseudocode +
  assembly, the exhaustive scan method/results, the false positives, and the
  inference/`not claimed` list) and
  `docs/adr/0438-noviewmodel-engine-toggled-no-draw-gate.md`. ADR-0435 and
  `GameClasses_RPG_ItemFlags.txt` get a correction pointer. New GH issue **#66**
  for approach 2; comment on #63.
- Decision: **approach 1 is out** — a forced `flags |= 2` has no reader and the
  engine's own `FUN_100e6aa0` would clear it each frame. **Approach 2**
  (per-draw suppression) is feasible: the hook is an `opengl32.dll` proxy that
  already *exports* `glBegin`/`glDrawArrays`/`glDrawElements`
  (`src/hook/opengl32.def:13,75,77`) but only forwards them.
- Evidence: `wake31_func_100E6AF3.json`, `wake31_func_1013F898.json`,
  `wake31_xrefs_100e6aa0.json` under `tools/autoloop/logs/`; module-wide mask
  scan scripts (`scan14c_all.py`, `scan_bit1*.py`). No C++/asset changed → no
  `build-all.ps1`/CTest; `verify_reported_fixes` and `scenario_tests.py --check`
  PASS.
- Blocker: none for landing. **Next:** start **#66** (approach 2) — add
  per-draw interception in `OpenGLProxy` for the already-exported
  `glBegin`/`glDrawArrays`/`glDrawElements` and search for a reliable
  viewmodel-pass signature. Only if that fails, trace whether the equipped
  item's `+0x14c` is *copied* and compared (a copy-based consumer cannot be
  excluded by a mask scan).
- Branch: committed on `main`, pushed to `origin/main` (HEAD was already
  `origin/main` at wake start — no concurrent writer).

## Wake 30 — 2026-10-16 14:54 (scheduled loop)
- **Unit:** GH **#65** (punch/attack on swing) — do the issue's *remaining* work:
  expose the strike velocity thresholds in the INI. Extracted the inline
  punch/hook/uppercut/cross-slash classifiers out of `UpdateMeleeCounters` into
  a shared, dependency-free, unit-tested header so sensitivity is headset-tunable
  without a rebuild.
- Finding (read-only, `src/hook/camera_hook.cpp`): after `e858809` the four
  strike booleans still used hard-coded numbers
  (`forwardVz < -1.20 && handZ < -0.10`, `|hookVx| > 1.35 && Vz < -0.15`,
  `uppercutVy > 1.35 && handY < -0.10 && Vz < -0.20`, `crossVx 1.10`), so any
  tuning required a rebuild; the issue explicitly asks for INI exposure.
- Change (code/tests/docs):
  - `src/hook/combat_strike.h` (new, header-only): `StrikeThresholds` (8 numbers,
    defaults = the e858809 first pass), `StrikeVelocity`, `StrikeIntent`, and
    `ClassifyStrike(...)` returning the four classifiers + the combined `strike`
    gate. Comparisons are strict (`<`/`>`) as before.
  - `camera_hook.cpp`: `GetStrikeThresholds()` from `g_vrConfig`; the classifier
    block now calls `ClassifyStrike(...)`. Kept the guard-volume computation
    (ADR-0434 / GH #62 shared-envelope source invariant) and the ADR-0403 note —
    strikes are deliberately **not** suppressed inside the guard volume.
  - `vr_config.{h,cpp}`: 8 INI keys `StrikeForwardVz/StrikeForwardMaxZ/
    StrikeHookVx/StrikeHookVz/StrikeUppercutVy/StrikeUppercutMaxY/
    StrikeUppercutVz/StrikeCrossVx`, each clamped (a typo cannot make strikes
    impossible/fire on idle); INI log line extended.
  - `tests/combat_strike_tests.cpp` (new) + CTest `combat_strike_thresholds`:
    defaults, strict boundaries, per-hand symmetry, combined OR gate, the
    **ADR-0403 guard-raise regression** (raising both hands into guard is never a
    strike), and that tuning the struct changes the outcome (the INI path).
  - `CMakeLists.txt`, `riddick_vr.ini`, `tools/riddick_vr.ini.example`,
    `src/hook/README.md`, `tools/verify_reported_fixes.py` (new #65 invariant),
    `docs/adr/0437-swing-driven-strike-thresholds.md` (new).
- Evidence: `build-all.ps1 -Config Release` OK (x86 `opengl32.dll` + x64 host);
  CTest **8/8 PASS** on x86 **and** x64 (new `combat_strike_thresholds`);
  `verify_reported_fixes`, `scenario_tests.py --check`, `validate_addresses`,
  `verify_hook_init`, `test_mos_xmd`, `test_mos_xtc`, `test_xmd_bake`,
  `test_xmd_export`, `test_xmd_pose`, `test_xsa_anim`, `test_xtc_bake` all PASS.
- Blocker: **not validated in-headset** — whether the defaults feel right on a
  real Quest swing (and never fire on a guard raise) is a hardware step; this
  wake only makes the numbers tunable and pins their semantics. Rigid overlay
  path unchanged (native hand skinned draw = GH #58/#60).
- **Next:** headset pass on `Strike*` (and the #62 `Guard*` window / #64
  `NativeHand*` knobs); then the remaining player-visible items #63 (suppress the
  engine FP viewmodel — RE still needs the draw gate) and #61 UV validation.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 29 — 2026-10-16 14:44 (scheduled loop)
- **Unit:** GH **#64** (native hands flat/2D, out of place, stray procedural
  blocks) — make the native draw read as **solid lit 3D**, remove the floating
  overlay blocks, and expose an on-headset **orientation** correction.
- Finding (read-only, `src/hook/vr_hand_renderer.cpp`): `DrawNativeHand` computed
  a hand-rolled **two-sided abs-lambert** (`shade = 0.35 + 0.65*|N·L|`, `|.|`
  kills the terminator) *on top of* the fixed-function `GL_LIGHTING`/`GL_LIGHT0`
  `RenderHands` already enables → double-shaded and flat-looking; and it drew with
  **backface culling on** (`GL_CULL_FACE`+`GL_FRONT_FACE(GL_CW)`), which the
  baked `.xmd` open shell does not respect → missing/single-sided faces. The four
  hand-local **interaction boxes** were drawn unconditionally, so they floated as
  stray blocks on the native mesh.
- Change (code/tests/docs):
  - `DrawNativeHand`: disable `GL_CULL_FACE` + `glLightModeli(GL_LIGHT_MODEL_TWO_SIDE,
    GL_TRUE)` for the native batch only (restored inline); **removed the
    abs-lambert**, letting the shipped unit normals drive real smooth
    `GL_LIGHTING`; tint goes **white when textured** (MODULATE) so the GH #61
    diffuse shows at full strength, flat skin tone only for the untextured
    fallback. New core GL entry point `glLightModeli` added to the dispatch.
  - `DrawTacticalHand`: `const bool native = DrawNativeHand(side);` gates **both**
    the procedural hand *and* the interaction boxes → no stray blocks on the
    native path. Upper-arm cloth intentionally kept (native clusters cover only
    the forearm + hand).
  - `native_hand_pose.h`: `RotateNativeHandPlacement(p, yaw, pitch, roll)` — a
    hand-local orientation correction (degrees; `R = Ry·Rx·Rz`, identity default)
    applied in `MapNativeHandPoint`/`MapNativeHandNormal`. INI keys
    `NativeHandYaw/Pitch/Roll` (`vr_config`, clamped ±180°) so the provisional
    ADR-0427 basis can be corrected on a headset without code changes.
  - `tests/native_hand_pose_tests.cpp`: rotation-composition, orthonormality,
    `R * base` equality, unit-length normal, pitch/roll checks.
  - `tools/verify_reported_fixes.py` (#64 invariant), `src/hook/README.md`,
    `riddick_vr.ini`, `tools/riddick_vr.ini.example`,
    `docs/adr/0436-native-hand-solid-3d-and-overlay-cleanup.md` (new).
- Evidence: `build-all.ps1 -Config Release` OK (x86 `opengl32.dll` + x64 host);
  CTest **7/7 PASS** on x86 **and** x64 (`native_hand_pose_mapping` now covers the
  rotation); `validate_addresses`, `test_mos_xmd`, `test_xmd_export`,
  `test_xmd_bake`, `test_xsa_anim`, `test_xmd_pose`, `test_mos_xtc`,
  `test_xtc_bake`, `verify_hook_init`, `verify_reported_fixes`,
  `scenario_tests.py --check` all PASS.
- Blocker: **not validated in-headset** — the orientation correction defaults to 0
  (the inferred basis); `NativeHandYaw/Pitch/Roll` + scale/offset/V-flip need
  headset tuning. Rigid draw only (skinning = GH #58/#60). **Next:** tune the
  native-hand knobs on hardware; re-anchor the interaction visuals to the native
  hand frame; then #61 headset UV check.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 28 — 2026-10-16 14:26 (scheduled loop)
- **Unit:** GH **#63** (duplicate hands) — RE **approach 1** precondition: pin
  the RPG item `*flags` encoding so the engine's `noviewmodel` flag can be
  written at runtime.
- Finding (read-only, REA/Ghidra 12.1.4 on
  `Content/GameClasses_Win32_x86.dll`, sha256 `1898b736…`, 2004 EFBB build):
  - `*flags` is parsed by the `CRPG_Object_Item` property registrar
    `FUN_100dfbd0` (`0x100dfbd0..0x100e0b31`); the FLAGS branch
    (`"FLAGS"` @0x100e0568) does
    `*(uint *)(item + 0x14c) |= FUN_100095a0(&PTR_s_wallcollision_101f7600)`
    (mask injected at `0x100e057a`). → **flag bitfield = `CRPG_Object_Item + 0x14c` (u32)**.
  - `FUN_100095a0 → FUN_10009160` is the generic, reused name-set→bitmask
    translator: it walks a **NULL-terminated** `char*` table and sets
    `mask |= 1 << index` per matched `a+b+c` token (numeric tokens OR raw).
    → **bit = 1 << (0-based name index)**.
  - Table base `0x101f7600` (`PTR_s_wallcollision_101f7600`), **32 entries**,
    NULL at `0x101f7680`: `[0] wallcollision, [1] noviewmodel, [2]
    playerlaserbeam, [3] __equipped__, [4] rpgmodelflags, [5] aiming, …`.
    → **`noviewmodel` = bit 1 = `0x00000002`**.
  - Shipped-content cross-check: `Content/Registry/RpgAmmo.xrg`
    `renderquantity+noviewmodel+needequippedlink+unselectable` →
    `0x00100000 | 0x2 | 0x00010000 | 0x100 = 0x00110102`.
  - Reader lead (NOT proven to be the draw gate): the only direct `& 2` read of
    `+0x14c` found is `0x10037111  F6 80 4C 01 00 00 02` (`test byte [eax+0x14c],2`)
    inside `FUN_10037020` (`0x10037020..0x100372bc`, sole caller `FUN_10040aa0`),
    whose bits tested (0x1/0x2/0x20 = wallcollision/noviewmodel/aiming) match the
    table — but its surrounding role is player-start/camera-entity selection.
  - No `test dword [reg+0x14c],2` (F7 /8x) exists; the F6 byte forms for modrm
    `0x81..0x87` were not exhaustively scanned (search-bytes re-imports Ghidra
    ~35 s/pattern).
- Change (docs only, no C++/asset): new
  `docs/rea/reference/GameClasses_RPG_ItemFlags.txt` (full 32-bit table + the
  parser loop + reader lead) and `docs/adr/0435-rpg-item-flags-noviewmodel-bit.md`.
  No code/asset change → no build/CTest.
- **Repair (main was red):** between this wake's start and its end a concurrent
  writer landed `e858809` (GH #65 punch fix) + `147d7b8` (prompt re-prioritize;
  new issues **#64/#65**) but did **not** refresh `tools/verify_reported_fixes.py`.
  #65 removed the `combatStance.active && !isClimbingActive` melee chord literal,
  so the `#52/#54` stance invariant failed on `main`. Updated that one assertion
  to pin #65's intent (melee is swing-driven; the two-hand stance still gates the
  guard *pose*) — `verify_reported_fixes` is green again. (Note: concurrent
  writer → re-check `git log` before pushing.)
- Evidence: `wake28_*` under `tools/autoloop/logs/` (ev_f32fcf56 FLAGS registrar,
  ev_bf358cea TranslateFlags wrapper, ev_36fdf50a core scanner, ev_11cf38db table
  bytes, ev_70ebc13f name bytes, ev_ee5b4a11 +0x14c reader, ev_b8c39329 table
  xref). Retired: run `rea`/Ghidra **serially** — one function/search per call.
- Blocker: the **viewmodel draw gate** (consumer of the bit) and the live
  **equipped-item pointer** are still unidentified, so approach 1 cannot be
  implemented yet. **Next:** decompile `FUN_10040aa0` (sole caller of the reader)
  to classify `FUN_10037020`, and hunt the first-person weapon/arm draw path for
  a `[+0x14c] & 2` test; else fall back to #63 approach 2.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 27 — 2026-10-16 12:19 (scheduled loop)
- **Unit:** GH **#62** (physical block/attack) — make the block a **front-of-HMD
  hand pose** and stop the guard from masking swings: extract the guard window
  into a shared, INI-tunable, unit-tested envelope used by *both* the guard-pose
  detector and the strike gate.
- Finding (read-only, `src/hook/camera_hook.cpp`): the guard had **two different
  volumes**. The pose detector (added in `05ae8a6`) used a tight front-of-face
  window `y ∈ [-0.16, 0.32]`, `z ∈ [-0.60, -0.12]`, span `< 0.50`; the **strike
  gate** still used the old loose volume `y ∈ [-0.42, 0.32]`, `z ∈ [-0.68,
  -0.05]`, span `< 0.56`, and `isMeleeStrike` was masked by `&& !inGuardVolume`.
  The old `y` floor sits **below the view** and its `z` ceiling reaches **inside**
  it, so a forward/hook swing thrown from a raised guard landed in the loose
  volume and never fired — the issue's "raising the hands to guard can mask or
  cancel attacks".
- Change (code/tools/docs):
  - `src/hook/combat_guard.h` (new, header-only, dependency-free): `GuardEnvelope`
    (head-local metres; `+Y` up, `-Z` forward) + `GuardHandInWindow` /
    `InGuardEnvelope` (two-hand + `maxHandSpan` predicate; sign-agnostic span).
  - `src/hook/vr_config.{h,cpp}`: `guardYMin=-0.16`, `guardYMax=0.32`,
    `guardZMin=-0.60`, `guardZMax=-0.12`, `guardMaxHandSpan=0.50` — INI keys
    `GuardYMin/GuardYMax/GuardZMin/GuardZMax/GuardMaxHandSpan` (clamped; an
    inverted window is rejected). Defaults **reproduce the existing pose window
    exactly**, so the block pose is unchanged; only the strike gate narrows.
  - `src/hook/camera_hook.cpp`: `GetGuardEnvelope()` (from `g_vrConfig`); the
    strike gate now uses `InGuardEnvelope(...)`; `chinLevel`/`inFrontOfFace`/
    `handsClose` derive from the envelope fields (still front-of-face, never
    below the view). The holster-gate `inGuardPose` predicate is left as-is.
  - `tests/combat_guard_tests.cpp` (new) + CTest `combat_guard_envelope`:
    boundary inclusivity, the **low-hold rejection** (#62 regression), view-plane
    and reach rejections, one-hand-out, span bound (strict/sign-agnostic), tuned
    envelope, inverted window.
  - `CMakeLists.txt`, `riddick_vr.ini`, `tools/riddick_vr.ini.example`,
    `src/hook/README.md`, `tools/verify_reported_fixes.py` (new shared-envelope
    invariant), `docs/adr/0434-front-of-hmd-block-guard-envelope.md` (new).
- Evidence: `build-all.ps1 -Config Release` OK (x86 `opengl32.dll` manifest +
  x64 host); CTest **7/7 PASS** on x86 **and** x64 (new `combat_guard_envelope`);
  `test_mos_xmd`, `test_xmd_export`, `test_xmd_bake`, `test_xmd_pose`,
  `test_xsa_anim`, `test_mos_xtc`, `test_xtc_bake`, `validate_addresses`,
  `verify_hook_init`, `verify_reported_fixes`, `scenario_tests.py --check` all
  PASS.
- **#63 lead (RE, read-only; not this wake's unit):** `noviewmodel` is a **shipped
  per-item RPG flag**, not an engine-only token. `Content/Registry/RpgAmmo.xrg`
  has e.g. `*flags renderquantity+noviewmodel+needequippedlink+unselectable`;
  only **9/60** shipped `*flags` lines carry it (all in `RpgAmmo.xrg`); every
  `RpgWeapons.xrg` weapon and pickup/template lacks it. The GameClasses flag-name
  table also holds `noviewmodel`/`rpgmodelflags`/`usethirdperson` next to
  `aiming`/`wallcollision` (`GameClasses_Win32_x86.dll` VA `0x101c527c`, sha256
  `1898b736…`; only data xref at `0x101f7604`). So viewmodel suppression is
  **data-driven** (option 1 of #63) — see comment on #63. (Retired: three
  parallel `rea`/Ghidra runs contend and hit the 330 s provider timeout; run them
  one at a time.)
- Blocker: #62 acceptance ("only the VR/native hands visible … validated
  in-headset") still needs a headset; the guard defaults are the ADR-0400 window
  and must be tuned on hardware. **Next:** tune `Guard*` on-headset; #63 needs
  the engine-side consumer of the `noviewmodel` bit (approach 1) or a per-draw
  viewmodel signature (approach 2) since we cannot write game content.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 26 — 2026-10-16 12:20 (scheduled loop)
- **Unit:** GH **#61** (native-hand texture) — close ADR-0432's "Next": **bind
  the real `Riddick_ArmScar_D` diffuse** on `VRHandRenderer::DrawNativeHand`,
  shipped as a compile-in DXT1 asset (no 685 MB runtime file I/O).
- Change (code/tools/docs):
  - `tools/xtc_bake.py` (new): reads an image's mip out of a `.xtc` via
    `mos_xtc`, requires a **compressed DXT1** mip, bakes the BC1 blocks
    **verbatim** into a C++ header plus decoded reference pixels + a full-RGBA
    checksum; `--cpp/--manifest/--verify/--json`; canonical set =
    `Riddick_ArmScar_D` **mip 1** (256x512).
  - `src/hook/native_hand_texture_data.h` (generated, 411 KB): 65536 DXT1
    block bytes, `sha256 21c57eca…021e`, `rgba_checksum 0x05308AE2`;
    `docs/rea/reference/native_hand_texture.json`.
  - `src/hook/bc1.h` (new): `Bc1DecodeBlock`/`Bc1DecodeSurface` — integer 5/6/5
    expansion + integer palette interpolation, byte-for-byte the same maths as
    `mos_xtc.bc1_decode`.
  - `src/hook/native_hand_texture.h` (new): `NativeHandTexture` +
    `FindNativeHandTexture`, header-only.
  - `src/hook/vr_hand_renderer.{h,cpp}`: `EnsureNativeHandTexture()` lazily
    creates one `GL_TEXTURE_2D` (RGBA8 upload, no GL extension), binds it for
    the native triangles only with `GL_MODULATE` (fade + lambert preserved) and
    emits `glTexCoord2f`; `GL_TEXTURE_BIT` added to the `glPushAttrib` mask; the
    flat skin tone stays the fallback. New GL dispatch:
    `glGenTextures/glBindTexture/glTexImage2D/glTexParameteri/glTexEnvi/
    glTexCoord2f`.
  - `src/hook/vr_config.{h,cpp}`: `NativeHandTexture` (default 1),
    `NativeHandTextureVFlip` (default 0, **provisional**).
  - `tests/native_hand_texture_tests.cpp` (new) + CTest
    `native_hand_texture_asset`; `CMakeLists.txt`, `riddick_vr.ini`,
    `tools/riddick_vr.ini.example`, `src/hook/README.md`,
    `docs/adr/0433-native-hand-texture-asset.md` (new; 0427/0432 cross-refs),
    `docs/rea/mos-xmd-format.md`, `.github/workflows/vr-worker.yml`.
- Evidence: `xtc_bake.py` wrote 1 texture / 65536 block bytes and `--verify` ->
  OK; `mos_xtc --decode Riddick_ArmScar_D` viewed (skin + stitched scar);
  `build-all.ps1 -Config Release` OK (x86 `opengl32.dll` + x64 host); CTest
  **6/6 PASS** on x86 **and** x64 (new test proves the C++ BC1 decoder
  reproduces the Python reference: full-surface checksum + 8 sample pixels, plus
  hand-computed blocks and edge-clip no-overrun); `test_xtc_bake.py` (6), all
  other python verifiers + `scenario_tests.py --check` PASS.
- Blocker: none. **Next:** headset validation of UV orientation
  (`NativeHandTextureVFlip`) + ADR-0427 placement; optionally bake mip 0
  (`--mip 0`) for full res and add the `_N`/`_S` (raw/DXT) maps once the
  raw/linear channel order is decoded. Keep `NativeHands`/`NativeHandTexture`
  as fallback-gated flags.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 25 — 2026-10-16 11:18 (scheduled loop)
- **Unit:** GH **#61** (native-hand texture) — decode the shipped **`.xtc`
  packed block**: recover the per-image **mip chain** and its **S3TC codec**, so
  the real Riddick skin diffuse can be read out for the native-hand draw path.
- Finding 1 (real evidence, read-only): the block at `desc[9]` is
  `u32 mip_offset[mip_count]` (absolute file offsets of each mip's header)
  followed by one `[header + pixel data]` record per mip. `mip_count` is read
  back as `(mip_offset[0] - block_offset) / 4` (the table is exactly `4*n`).
  Header is **`0x2C` when compressed, `0x1C` when raw**; the chain invariant
  `offset[i] + header_len + mip_bytes == offset[i+1]` holds on **887/901**
  (Models), **244/246** (GUI), **3787/3797** (Levels). Fields: `+0x04`
  colours (`0x5000`/`0`), `+0x08` `mip_bytes|0x10` (compressed), `+0x0C`/`+0x10`
  w/h, `+0x1C` **codec**, `+0x20` `mip_bytes`; a `mip_bytes == 0` mip is *empty*.
- Finding 2 (decisive): the codec field `+0x1C` correlates **1:1** with the
  stored size and is standard S3TC — **`0` = DXT1/BC1** (`w*h/2`), **`2` =
  DXT3/BC2**, **`4` = DXT5/BC3** (both `w*h`). Verified **visually**: BC1
  `Riddick_ArmScar_D`/`Riddick_B03_D` (skin + stitched arm scar), BC3
  `Pa2_inmate_H04_D` (inmate head), BC2 `SKYMOUNT_0` (snowy skybox). Block order
  is the usual S3TC order (alpha half first: the swap yields garbage).
  Counts: Models `{DXT1:455, DXT5:128}` base mips; Levels adds `DXT3:290`.
- Finding 3 (engine, read-only; Ghidra 12.1.4 via `rea`): `rea search
  MSystem.dll "XTC" --kind procedures` names `CTextureContainer_VirtualXTC2::
  ReadTexture` (`0x1009b280`) / `GetTextureMipMap` (`0x1009b760`) /
  `CImage::ReadHeader`, `".\Raster\MTextureContainerXTC2.cpp"`,
  `"Invalid mipmap requested: %d/%d/%d, texture %s"`. `GetTextureMipMap`
  validates the level against a per-image `min_mip`/`max_mip` nibble
  (`descriptor+0x19`) and returns a `CImage` (`0x30` stride). Mirrored
  statically; nothing executed.
- Change (tools/docs; **no C++**, so no `build-all.ps1`/CTest rerun):
  - `tools/mos_xtc.py`: `XtcMip`; `Xtc.read_mips`/`try_read_mips`; `bc1_decode`
    / `bc2_decode` / `bc3_decode` (+`_rgb565`/`_bc1_base`); `Xtc.decode`;
    `write_png` (stdlib-only). CLI `--mips NAME`, `--decode NAME --out FILE.png
    [--mip N]`; `summary()` adds `mip_chain_ok/bad_images` +
    `mip_codec_histogram`.
  - `tools/test_mos_xtc.py`: **+6** tests (BC1/BC2/BC3 on known blocks, empty
    mip, raw 32-bit, PNG write, chain-break) -> **18**; `build_xtc` now emits
    real packed mip blocks.
  - `docs/adr/0432-xtc-mip-chain-and-s3tc-codec.md` (new); ADR-0431 follow-up
    marked resolved; `docs/rea/mos-xmd-format.md` `.xtc` item 4 rewritten.
- Evidence: `mos_xtc.py Models.xtc --mips Riddick_ArmScar_D` -> 11 parsed
  (`512x1024 DXT1 262144B` .. `1x1 DXT1 8B`); `--decode Riddick_ArmScar_D
  --out armscar.png` -> correct skin/scar texture (viewed), `--decode
  Pa2_inmate_H04_D` (DXT5 head), `mos_xtc.py Levels.xtc --decode SKYMOUNT_0`
  (DXT3 sky); `--check` GUI/Models/Levels -> ok (246/901/3797);
  `test_mos_xtc.py` (18) PASS; `test_mos_xmd.py`, `test_xmd_export.py`,
  `test_xmd_bake.py`, `test_xmd_pose.py`, `test_xsa_anim.py`,
  `validate_addresses.py`, `verify_hook_init.py`, `verify_reported_fixes.py`,
  `scenario_tests.py --check` all PASS.
- Blocker: none. **Next:** feed `bc1_decode(Riddick_ArmScar_D)` into
  `VRHandRenderer::DrawNativeHand` (GL texture upload or a small in-tree baked
  asset like ADR-0420's mesh bake); the raw/linear channel order (needed for
  normal maps) and the reduced-mip-chain images stay open.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 24 — 2026-10-16 10:55 (scheduled loop)
- **Unit:** GH **#61** (native-hand texture) — decode the shipped **`.xtc`
  texture packs** and build a read-only reader, so the real Riddick skin
  diffuse can be extracted for the native hand draw path.
- Finding 1 (real evidence, read-only): a `.xtc` is the **same `CDataFile`
  tree** as `.xmd` (identical `MOS DATAFILE2.0` magic) written by the engine's
  **`VirtualXTC`** texture container — `MSystem.dll` holds
  `CTextureContainer_VirtualXTC2` / `VirtualXTC`, `?AddFromXTC@...`,
  `XTXCompile`, `".\Raster\MTextureContainerXTC2.cpp"`, and
  `"Invalid XTC. (IMAGELIST was not a directory.)"` / `"Invalid XTC. (No
  IMAGELIST)"` (raw printable-string scan).
- Finding 2 (container): root record at 0 with `next_sibling` = node-table
  offset **near EOF** and `data_offset` = **0** (this is why `mos_xmd.py`
  rejected `.xtc`). Exactly **two** contiguous nodes: `TEXTURES` (payload = the
  packed texture-data blob, at file `0x30`) and `IMAGEDIRECTORY5` (payload =
  the image directory, the last region before the node table). The two payloads
  tile `[0x30, node_start)` exactly. Concrete: GUI `0x30+0x1127322` /
  `0x1127352+0x5634`, Models `0x30+0x28DDD11B` / `0x28DDD14B+0x12DC0`, Levels
  `0x30+0x5AE40437` / `0x5AE40467+0x4D7EA`.
- Finding 3 (directory): `IMAGEDIRECTORY5` is a flat list of framed records
  `u32 marker=0xFFFFFFFF; u32 0; u32 name_len; char name[name_len]; 0x20 pad;
  u32 desc[14+]`. Records tile the payload exactly: **246/901/3797** (GUI/
  Models/Levels). The node `count` (+0x28) is **248/914/3811** — 2-14 higher;
  the framing is authoritative (recorded as an open question, not guessed).
- Finding 4 (descriptor, pinned & cross-checked over **4944/4944** records):
  `desc[1]` = mip count = `log2(max(w,h))+1`; `desc[2]`=`0x300`;
  `desc[3]`=`0x5000` colour / `0` linear; `desc[4]` = base-mip bytes | `0x10`
  when block-compressed; `desc[5]/[6]` = width/height (powers of two);
  `desc[9]` = **absolute file offset** of the packed block (strictly increasing,
  inside `TEXTURES`); `desc[10]` = `0x102` colour / `0x4300102` linear.
  `Riddick_ArmScar_D` = 512x1024, 11 mips, colour, compressed, base `0x40000`
  (DXT1-sized), block@`0x16CDEABE` (+350072 B); `riddick_armscar_N` = 512x1024
  raw `0x200000` (=512*1024*4).
- Finding 5 (block framing): a block starts with a `u32` mip-offset table
  (`mip_levels` absolute offsets), then one `[0x2C-byte header + data]` record
  per mip; the mip deltas match the exact DXT1 mip-chain sizes. **The pixel
  codec is not decoded yet.**
- Change (code/tools/docs; **no C++**):
  - `tools/mos_xtc.py` (new): read-only `.xtc` reader — container, framed
    directory, pinned descriptor; `--list`, `--find NAME`,
    `--extract NAME --out FILE`, `--json`, `--check`. Seeks (handles the 1.5 GB
    `Levels.xtc` without loading it).
  - `tools/test_mos_xtc.py` (new): 12 self-contained synthetic-container tests.
  - `.github/workflows/vr-worker.yml`: run `test_mos_xtc.py`.
  - `docs/adr/0431-xtc-texture-pack-container.md` (new) + `.xtc` section in
    `docs/rea/mos-xmd-format.md`.
- **Validation fix (pre-existing regressions from the interactive session, not
  this unit):** commits `f4cdd20` (#44 cursor->cinema debounce, 10:40:37) and
  `05ae8a6` (#62 pose-based guard, 10:47:06) changed the code but left
  `tools/verify_reported_fixes.py` asserting the old literals, so the suite was
  red. Updated the two stale assertions to the current expressions
  (`... || s_cursorMenu;` and the pose-based `guardPoseDetected =
  !isClimbingActive && !s_twoHandedStabilizationActive && chinLevel && ...`);
  the invariants' intent is preserved.
- Evidence: `python tools/mos_xtc.py --check <GUI|Models|Levels>.xtc` -> ok
  (246/901/3797); `python tools/test_mos_xtc.py` PASS (12); `--find
  Riddick_ArmScar_D` -> 512x1024 mips=11; `test_mos_xmd.py`, `test_xmd_pose.py`,
  `test_xmd_bake.py`, `test_xmd_export.py`, `test_xsa_anim.py`,
  `validate_addresses.py`, `verify_hook_init.py`, `scenario_tests.py --check`,
  `verify_reported_fixes.py` all PASS. No C++/asset changed -> no
  `build-all.ps1`/CTest rerun.
- Blocker: none. **Next:** decompile `CTextureContainer_VirtualXTC2` /
  `XTXCompile` in `MSystem.dll` (`rea search MSystem.dll "VirtualXTC2" --kind
  procedures`) to decode the per-mip `0x2C` header + block codec, then extract
  `Riddick_ArmScar_D` to RGBA and bind it in `VRHandRenderer::DrawNativeHand`
  (#61).
- Branch: committed on `main`, pushed to `origin/main`. (`Offline01.xtc` uses a
  different nested variant `SOURCE`/`IMAGELIST`/`IMAGEDIRECTORY4`/`IMAGE`/
  `NAME`/`PROPERTIES`/`MIPMAP` — out of scope, reported as unsupported.)

## Wake 23 — 2026-10-16 10:15 (scheduled loop)
- **Unit:** GH #59→#46 — finish ADR-0429's "Next probe" by re-checking the **field
  chain** of the live per-bone skin-matrix array against the MXR.dll
  **disassembly** (`FUN_1002dcc0`), then correct the record.
- Finding 1 (real evidence, read-only; Ghidra 12.1.4 via `rea`, full pseudo +
  2543-instruction listing saved at `tools/autoloop/logs/wake23_func_1002dcc0.json`):
  **ADR-0429's field chain is wrong at its base.** Ghidra's `local_a0` is one
  local assigned twice; the value that feeds the matrix array is the *second*
  assignment. Disassembly: `0x1002dd2a MOV EAX,[EAX+0xb0]` (early
  `local_a0 = param_1[0x2c]`), then `0x1002e47b MOV EBX,[EBX+0x94]` +
  `0x1002e481 MOV [EBP+0xffffff64],EBX` (`local_a0 = param_1[0x25]`), then
  `0x1002e476 MOV ECX,[ECX+0x3c]` / `0x1002e48b MOV [EBP+0xffffff64],ECX`
  (override `local_a0 = *(int*)(param_6 + 0x3c)`). So the owner is
  **`param_1[0x25]` (= `*(int*)(param_1 + 0x94)`), overridden by
  `*(int*)(param_6 + 0x3c)`** — **not** `param_1[0x2c]` (`+0xb0`).
- Finding 2 (confirms the offset shape, from the corrected base):
  `0x1002e593/0x1002e5a8/0x1002e5af/0x1002e5be` =
  `iVar15 = *(int*)(local_a0+0x14); matrices = *(int*)(iVar15+0x18);
  count = *(int*)(iVar15+0x04)`, then handed to `FUN_100238a0` at the already
  pinned call site (`0x1002f447..0x1002f44d`). `xrefs 0x1002dcc0` → only the
  vtable slot `0x10144db8` (no static caller → argument types unresolved).
- Finding 3 (container): the object at `iVar15` is shaped like the engine's
  **`TList_Vector<TMatrix43<float,float>>`** (`+0x04` count, `+0x18` data, 0x30-B
  elements) — matching the shipped RTTI at `0x10169fc8`/`0x1016a1c0`/`0x1016a214`
  and `FUN_10022f90`'s `* 0x30` stride; the same `{+0x04;+0x18}` accessor is used
  for other `TList_Vector` fields in the same function.
- Change (docs only, no code/asset):
  - `docs/adr/0430-xmd-live-skin-matrix-vector-field-chain.md` (new) — the
    correction, the container shape, and a "not claimed" list.
  - `docs/adr/0429-…md` — a Correction pointer to ADR-0430.
  - `docs/rea/reference/MXR_SkinMatrixProvider.txt` — field chain corrected +
    the disassembly excerpt.
  - `docs/rea/mos-xmd-format.md` — provider field chain + Next probe updated to
    `param_1[0x25]` / `*(param_6+0x3c)`.
- Evidence: `rea function MXR.dll 0x1002dcc0`, `rea xrefs MXR.dll 0x1002dcc0`;
  artefacts under `tools/autoloop/logs/wake23_*`. No C++/asset changed → no
  `build-all.ps1`/CTest rerun; `xmd_bake --verify` OK (4 meshes/1548 verts);
  `test_mos_xmd.py`, `test_xmd_export.py`, `test_xmd_bake.py`,
  `test_xmd_pose.py`, `test_xsa_anim.py`, `validate_addresses.py`,
  `verify_hook_init.py`, `verify_reported_fixes.py`, `scenario_tests.py --check`
  all PASS.
- Blocker: none for landing. **Next:** identify the owner class of
  `*(param_1+0x94)` / `*(param_6+0x3c)` and its matrix-list builder (opened
  GH #60) and decide `world` vs `world·bind_inverse`; the native skinned hand
  (#46) still needs that runtime array (GH #34) or an offline
  `bind_inverse × .xsa` pose. Rigid draw (ADR-0427) stays the honest default.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 22 — 2026-10-16 09:58 (scheduled loop)
- **Unit:** GH #59→#46 — close ADR-0428's "Next probe": decompile the **builder**
  of the draw-side skin matrix *provider* and decide whether a skinned native hand
  can be reconstructed offline or needs the live matrices (#34).
- Finding 1 (real evidence, read-only; Ghidra 12.1.4 via `rea`): the provider is a
  transient **`0x10`-byte** object built **per skinned cluster** by
  **`FUN_100238a0(mesh, matrices, countCap, desc)`** (`0x100238a0`, called from the
  model draw `FUN_1002dcc0` at `LAB_1002f3e0`, the `mesh[0x2b]&0x4000` path) and
  stored at `desc+0x0C` (`desc` = global `0x1016a7d0`; `desc+0x0C` = `DAT_1016a7dc`):
  `+0x00 float* matrices = param_2` (caller-supplied, **live**), `+0x04 u8* palette
  = FUN_1007fa90(mesh) = (mesh+0x24)+4` (the mesh's `BONEMATRIXMAP` bytes; `NULL`
  when the mesh has none), `+0x08 u32 count = FUN_1007eda0(mesh) = *(u32*)(mesh+0x24)`,
  `+0x0C = 0`. So `mesh+0x24 = {u32 n; u8 palette[n]}`.
- Finding 2 (decisive, #59): **`FUN_100238a0` composes no transform and reads no
  bind/reference/inverse matrix** — it only selects `matrices[palette[bone]]`
  (`FUN_10022f90`, ADR-0428, unchanged; `NULL` palette → direct `bone`). The
  `matrices` array is a **live** field of the model instance, read in `FUN_1002dcc0`
  before the cluster loop: `local_a0 = param_1[0x2c]; iVar15 = *(int*)(local_a0+0x14);
  matrices = *(int*)(iVar15+0x18); count = *(int*)(iVar15+4)`. So #59's per-cluster
  reference/bind-inverse is resolved **upstream** of the `.xmd` draw path: if it
  exists it is baked where that live array is built (candidate owner:
  `CXR_SkeletonInstance`, RTTI `0x10142fc8`), which is exactly a runtime / engine
  concern (#34), not an `.xmd` field.
- Finding 3: `FUN_1007fa90`/`FUN_1007eda0`/`FUN_1007fa40` first walk `mesh+0x28` up
  the mesh-parent chain (`FUN_10079ad0`) before reading `+0x24`/`+0x20`; `+0x20` =
  influence-pool block (`FUN_1007fa40` = `*(mesh+0x20)+0x18`).
- Change (docs only):
  - `docs/rea/reference/MXR_SkinMatrixProvider.txt` (new): `FUN_100238a0` +
    `FUN_1007fa90`/`FUN_1007eda0` + `FUN_10022f90` pseudocode and the `FUN_1002dcc0`
    field chain.
  - `docs/adr/0429-xmd-skin-matrix-provider-builder.md` (new) — the finding, the
    decision, and an explicit "not claimed" list.
  - `docs/rea/mos-xmd-format.md`: `BONEDEFORM` runtime-shape note (`+0x24 =
    {u32 n; u8[n]}`), a new "skin matrix provider" subsection, Next-steps retargeted
    at the live-array builder.
- Evidence: `rea xrefs` on `0x1016a7dc`/`0x1016a7d0`, `rea function` on
  `0x100238a0`/`0x1007fa90`/`0x1007eda0`/`0x1007fa40`/`0x1002f4f9`/`0x1002d890`;
  all `wake22_*` under `tools/autoloop/logs/`. No C++/asset changed → no
  `build-all.ps1`/CTest rerun; `test_mos_xmd.py`, `test_xmd_export.py`,
  `test_xmd_bake.py`, `test_xmd_pose.py`, `test_xsa_anim.py`, `validate_addresses.py`,
  `verify_hook_init.py`, `verify_reported_fixes.py`, `scenario_tests.py --check` PASS.
- Blocker: none for landing. **Next:** find the builder of the live per-bone array
  (`*(model[0x2c])+0x14` field `+0x18`; candidate `CXR_SkeletonInstance`) and check
  whether it composes `world·bind_inverse`; if so a skinned hand needs either the
  runtime array (#34) or an offline `bind_inverse × .xsa` pose. Rigid draw
  (ADR-0427) stays the honest default meanwhile.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 21 — 2026-10-16 09:05 (scheduled loop)
- **Note on numbering:** the 08:27 run (which labelled itself "wake 20") pursued
  this same `#59` draw-side track but ended with only RE artefacts and no state
  record; this wake completed it. Artifacts from that run are in
  `tools/autoloop/logs/wake20_*`.
- **Unit:** GH #59 (→ #46) — **close the "per-cluster reference/bind-inverse
  transform" hunt**: decompile the draw-side skinning consumer
  `CXR_Model_TriangleMesh::Cluster_TransformBones_V_N_TgU_TgV`.
- Finding 1 (real evidence, read-only; Ghidra 12.1.4 via `rea`): the warning
  string `WARNING: (CXR_Model_TriangleMesh::Cluster_TransformBones_V_N_TgU_TgV)
  Missing vertex component.` is MXR.dll **VA `0x10144a1d`** (file offset
  `0x143837`; present in **no** other DLL — raw byte scan of `RndrGL/MSystem/
  MCCDyn` finds none; `rea search --kind procedures` finds **no** named
  `Cluster_TransformBones*`, the PDB is not loadable). Its only code ref is
  `0x10025cf5` → emitter **`FUN_10025ced`** (39 B); `rea xrefs` on it →
  `{0x10025595, 0x1002559d, 0x100255a5, 0x100255ad}`, all inside
  **`FUN_100254e0`** (`0x100254e0..0x10025cec`) — i.e. the method is
  `FUN_100254e0(void* mesh, void* provider)`; only caller `0x1002dcc0`.
- Finding 2 (decisive, #59): `FUN_100254e0` reads **only** the mesh's geometry
  (`FUN_1007ee20/1007f100/100825a0/10082750`), its per-vertex influence records
  (`FUN_1007f9f0`) and its influence pool (`FUN_1007fa40`, 8-byte
  `{u16 bone; u16 pad; f32 weight}`), and fetches **one 3×4 matrix per bone from
  the second argument** via `FUN_10022f90(provider, bone)`. Per vertex: `count==1`
  → `v' = M·v`; `count==2` → inline weight-blend; `count>2` → linear weighted sum
  of the ≤4 matrices. **No bind/reference/inverse matrix is read from the mesh,
  the cluster, `MISCELLANEOUS`, or `matrix_map`/`matrix_map_id`.** So #59's
  "missing per-cluster transform" is **resolved negatively**: it does not exist on
  the `.xmd` draw path; the reference must be inside the provider.
- Finding 3: `FUN_10022f90(this, bone)` = `matrices[palette ? palette[bone] :
  bone]` where `this+0x00` = a **`0x30`-byte (12-float 3×4) per-bone matrix array**
  and `this+0x04` = an **optional `u8` palette** (callers `0x10024ca0`,
  `0x100254e0`). So the "matrix map" that matters is the provider's, built at
  runtime — not the mesh's stored `matrix_bones` (ADR-0423/0426).
- Change:
  - `docs/rea/reference/MXR_ClusterTransformBones.txt` (new): the
    `FUN_100254e0` + `FUN_10022f90` pseudocode with the field/call table.
  - `docs/adr/0428-xmd-draw-side-skinning-matrix-blend.md` (new) — the finding,
    the negative resolution of #59, the two remaining probes, and an explicit
    "not claimed" list.
  - Corrected the stale "the baked asset is the artist bind pose" wording that
    ADR-0424 had already disproved for the arm clusters in
    `src/hook/native_hand_mesh.h`, `tools/xmd_bake.py`, `docs/adr/0420-…md`;
    `docs/rea/mos-xmd-format.md` "Next steps" now points at the provider.
- Evidence: `build-all.ps1 -Config Release` OK (x86 `opengl32.dll` + x64 host);
  `ctest` **5/5 PASS** on x86 **and** x64; `xmd_bake --verify` OK (4 meshes /
  1548 verts — the header edit was comment-only); `test_mos_xmd.py` (50),
  `test_xmd_export.py`, `test_xmd_bake.py`, `test_xmd_pose.py` (7),
  `test_xsa_anim.py` (19), `validate_addresses.py`, `verify_hook_init.py`,
  `verify_reported_fixes.py`, `scenario_tests.py --check` all PASS.
- Blocker: none for landing. **Next:** decompile the provider's *builder* (does it
  multiply in `bind_inverse`?) and the caller `0x1002dcc0`, then decide whether a
  skinned native hand needs the provider's matrices read at runtime (#34) or can
  stay rigid (ADR-0427).
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 19 — 2026-10-16 08:21 (scheduled loop)
- **Unit:** GH #46 (highest priority) — stop leaving the ten wakes of baked
  native arm/hand RE unused: wire the baked mesh into `VRHandRenderer` behind a
  config flag, with the model→hand-local placement isolated in a tested header.
- Finding (real evidence, read-only; asset bounds + ADR-0420/0424):
  - `native_hand_mesh_data.h` bounds: `Riddick_LeftArm` y `-25.45..-5.49`,
    `Riddick_RightArm` y `17.61..25.45` — the arm's long axis is **model Y**
    (extent ~20 vs ~8 on X/Z) with the hand at the **-Y** end on the left and the
    **+Y** end on the right (left fingertips `y ≈ -24..-25.4` per ADR-0420/0424).
    Both `Pa2_Hand` meshes are also Y-longest, so "long axis = largest AABB
    extent" generalizes.
  - ADR-0424/0425/0426 already established the stored geometry is **not** the
    `NODES` bind pose (arm clusters carry a baked raised pose) and no per-cluster
    bind-inverse transform is stored in the `.xmd`, so only a **rigid** draw is
    honest today; a skinned draw still waits on the runtime pose (#59/#58).
- Change:
  - `src/hook/native_hand_pose.h` (new, header-only, dependency-free):
    `NativeHandPlacement`, `ComputeNativeHandPlacement`, `MapNativeHandPoint`,
    `MapNativeHandNormal` — AABB → hand-local basis (X right = up×backward,
    Y up = model +Z, Z back = ±long axis), anchor at the **hand end** (not the
    centre), scale = caller `meters/unit`.
  - `src/hook/vr_hand_renderer.{h,cpp}`: `DrawNativeHand(side)` resolves
    `Riddick_LeftArm`/`Riddick_RightArm`, emits the baked triangles with the
    already-loaded immediate-mode GL entry points and the existing per-hand
    fade; when on it replaces the procedural gauntlet/palm/digits and the
    elbow→wrist cloth segment (keeps interaction glow + upper-arm cloth). When
    off (the default) the procedural overlay is unchanged.
  - `src/hook/vr_config.{h,cpp}`: `NativeHands` (0/1), `NativeHandScale`
    (0.005..0.20, default `0.033`), `NativeHandOffset{X,Y,Z}` (m, ±1).
  - `tests/native_hand_pose_tests.cpp` (new) + CTest `native_hand_pose_mapping`:
    orthonormal/right-handed basis, left/right mirror, hand-end→origin, scaled
    span, normal mapping, degenerate/inverted/non-positive-scale rejection, and a
    baked-asset cross-check. `CMakeLists.txt` (test target + headers),
    `tools/riddick_vr.ini.example`, `riddick_vr.ini`, `src/hook/README.md`.
  - `docs/adr/0427-native-hand-draw-path.md` (new) — includes an explicit
    "not claimed" list (not a validated fit, not skinned, not the engine
    viewmodel).
- Evidence: `build/x86` opengl32.dll built; `ctest build/x86 -C Release` →
  **5/5 PASS** (new `native_hand_pose_mapping`), `ctest build/x64` → **5/5 PASS**
  (+ host exe built). `test_mos_xmd.py` (50), `test_xmd_export.py`,
  `test_xmd_bake.py`, `test_xmd_pose.py`, `test_xsa_anim.py` (19),
  `validate_addresses.py`, `verify_hook_init.py`, `verify_reported_fixes.py`,
  `scenario_tests.py --check` all PASS.
- Blocker: none for landing; **calibration** needs a headset (`NativeHands=1` +
  `NativeHandScale`/offsets; defaults deliberately untuned and documented as
  such). **Next:** tune the placement on hardware and/or feed the ADR-0423
  palette→node skin + `.xsa` pose (#58) into a skinned variant once #59's runtime
  per-cluster matrix is confirmed.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 18 — 2026-10-16 06:17 (scheduled loop)
- **Unit:** GH #59 → #46 — identify the mesh's `matrix_map` (`+0x24`) /
  `matrix_map_id` (`+0x28`) and test the "`matrix_map_id` registry" candidate
  queued by Wake 17.
- Finding 1 (real evidence, read-only; Ghidra 12.1.4 via `rea`): the field *is*
  the engine's **`BONEMATRIXMAP`**. `CTriangleMeshCore::Read` (`FUN_10080f50`,
  `MXR.dll`) legacy path (model version ≤ `0x100`) reads three named sibling
  chunks `BONEVERTEXINFO`→`+0x18`, `BONEINFLUENCE`→`+0x20`,
  **`BONEMATRIXMAP`→`+0x24`** (via `FUN_1007b630` + raw read), then sets
  `flags |= 0x4000` ("has bone deform" when vertex records non-empty) and
  `flags |= 0x8000` ("has bone matrix map" when `+0x24` non-empty). The modern
  path (shipped `0x102`) folds the same three roles into the embedded
  `BONEDEFORM` (`FUN_1007d260`) which writes the id to `+0x28` and the palette to
  `+0x24` — the *same* fields.
- Finding 2: `FUN_1007c150` (influence pool, version `0x200`) expands
  `{u8 bone; u8 weight_q}` → `{u16 bone; pad; f32 weight}` **verbatim**, no
  palette lookup — so palette→node resolution is deferred to the runtime skinning
  consumer, not done at load. `FUN_10081b10` (`"CreateMatrixIW"`) packs ≤ **4**
  bone indices/vertex (one byte each) into a `u32` at mesh `+0x30` plus 3 weights
  at `+0x34`, throwing `"Too many bones/vertex %d/4 (%s)"` above 4.
- Finding 3 (decisive): `matrix_map_id` is a **per-cluster id**, not a shared
  palette key. Corpus 631 models / 4662 clusters / 3103 skinned / **2411
  paletted**: 488 within-model multi-cluster id groups, of which **462 have
  differing palettes** (only 26 agree); 1007 within-model palette groups with
  >1 cluster, **1006 have differing ids**; 8/11 ids vary in palette across
  models. Decisive: `Riddick_B05` duplicate pairs c0/c3, c1/c4, c2/c5 have
  **identical palettes but different ids** (`-2/0`, `-2/1`, `-2/2`) while id `-2`
  alone spans legs + left arm + right arm (three different palettes).
- Change:
  - `tools/mos_xmd.py`: `Xmd.matrix_map_groups()` (per-id cluster groupings +
    `palettes_agree`; empty-palette clusters excluded); `geometry_stats()`/
    `geometry_summary()` add `matrix_map_ids` (id → paletted-cluster count),
    `matrix_map_shared_id_groups`, `matrix_map_palette_conflict_groups`;
    `report_file()`/`--matrix-map` prints groups; docstring documents the
    `BONEMATRIXMAP` identity.
  - `tools/test_mos_xmd.py`: **+3** tests (conflicting-palette id group,
    agreeing-palette id group, direct clusters excluded) → **50** total, PASS.
  - `docs/adr/0426-xmd-bonematrixmap-per-cluster-id.md` (new);
    `docs/rea/reference/MXR_CreateMatrixIW.txt`,
    `docs/rea/reference/MXR_ReadBoneInfluence.txt` (pseudocode evidence);
    `docs/rea/reference/xmd_matrix_map_registry.json` (aggregate table +
    `Riddick_B05` ids/palettes); ADR-0423 + ADR-0425 "still open" cross-refs;
    `docs/rea/mos-xmd-format.md` `BONEDEFORM` section + Next steps.
- Evidence: `mos_xmd.py --inventory <game>\Content\Models --check` → **631/631
  `ok`, 0 FAIL**; `mos_xmd.py Riddick_B05.xmd --matrix-map` →
  `matrix_map_id=-2 clusters=[0,1,2] palettes=3 agree=False`; `test_mos_xmd.py`
  (50), `test_xmd_export.py`, `test_xmd_bake.py`, `test_xmd_pose.py`,
  `test_xsa_anim.py`, `validate_addresses.py`, `verify_hook_init.py`,
  `verify_reported_fixes.py`, `scenario_tests.py --check` all PASS. No C++
  changed → no `build-all.ps1`/CTest rerun.
- Blocker: none. **Next:** #59's only remaining candidate is a **runtime**
  per-cluster matrix built by the model/skin class from the live skeleton
  (bind-inverse × current bone matrix) — trace the draw-side consumer of the
  packed indices at mesh `+0x30` and the
  `CXR_Model_TriangleMesh::Cluster_TransformBones_V_N_TgU_TgV` transform
  (RndrGL/MXR) to confirm, or bake a posed arm once a pose source is chosen.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 17 — 2026-10-16 05:40 (scheduled loop)
- **Unit:** GH #59 → #46 — rule out the "per-`CLUSTER`/`MISCELLANEOUS` matrix"
  candidate for the missing per-cluster reference/bind-inverse transform by
  decoding the record's 28 previously "opaque" trailing bytes.
- Finding (real evidence, read-only; `tools/mos_xmd.py`):
  - `CLUSTER`/`MISCELLANEOUS` is 44 bytes:
    `{u32 version=0x102; u32 flags; u32 surface_index; u32 reserved=0;
    f32 box_min[3]; f32 box_max[3]; f32 trailer=0.0}`. The 28 trailing bytes are
    **six floats, not a matrix**: `version/reserved/trailer` are constant on
    **4662/4662** shipped records (631 models, one per cluster).
  - **[box] == the paired `VERTEXFRAMES` frame's quantization range**
    (`bounds_min`/`bounds_max`, ADR-0415) component-wise on **4662/4662**
    clusters — the header just mirrors the cluster's reference/dequantization
    box. `--check` now enforces that equality.
  - The box encloses the *decoded* geometry AABB on **4662/4662** (guaranteed by
    dequantization) but equals it exactly on only **1751/4662** (2911 clusters
    have a quantization range strictly larger than their geometry).
  - Decisive (`Riddick_B05.xmd`): cluster 0's frame bounds
    (`-4.3519,-6.8949,0.0016 .. 7.6841,6.8949,36.2587`) equal its `MISCELLANEOUS`
    box exactly, while its *decoded* geometry AABB is much smaller
    (`-4.02,-1.87,0 .. 7.68,1.87,25.04`); the left/right-arm reference boxes
    (`z 32.85..52.50`) are not the bind AABB of their 24/20 arm nodes.
  - Loader support: `CTriangleMeshCore::Read` (`MXR.dll` `FUN_10080f50`) reads
    only the leading `u32` of `MISCELLANEOUS` (the model version that gates the
    version branches); the rest is cluster metadata. (Exploratory: a `Kabsch`
    bind→centroid fit and a shoulder-rotation scan are inconclusive as
    transforms — the T-pose/A-pose reading stays an inference, not claimed.)
- Change:
  - `tools/mos_xmd.py`: `ClusterInfo.box_min/box_max/trailer` + `encloses()`;
    `decode_cluster_info` decodes the 6 floats + trailer; `cluster_meshes()`
    carries `reference_box_min/max` + `reference_box_matches_frame_bounds`;
    `validate()` enforces the box == frame-bounds equality;
    `geometry_stats()`/`geometry_summary()` report
    `reference_box_clusters`/`reference_box_mismatch_clusters`/
    `reference_box_missing_clusters`; `--mesh` prints the reference box;
    docstrings.
  - `tools/test_mos_xmd.py`: **+4** tests (box decode + `encloses`, box == frame
    bounds, mismatch rejection, box/trailer decode) → **47** total, PASS.
  - `tools/xmd_pose.py`: `analyze()` attaches the reference box +
    `reference_box_encloses_geometry` per cluster; `check()` fails when a box
    fails to enclose the decoded geometry; report prints `refbox=`.
  - `docs/adr/0425-xmd-cluster-miscellaneous-bounds.md` (new);
    `docs/rea/mos-xmd-format.md` `MISCELLANEOUS` section + Next-steps update.
  - `docs/rea/reference/riddick_b05_pose.json` regenerated (per-cluster boxes).
- Evidence: `mos_xmd.py --inventory <game>\Content\Models --check` → **631/631
  ok, 0 FAIL** (with the new equality invariant). `test_mos_xmd.py` (47),
  `test_xmd_export.py`, `test_xmd_bake.py`, `test_xmd_pose.py`,
  `test_xsa_anim.py`, `validate_addresses.py`, `verify_hook_init.py`,
  `verify_reported_fixes.py`, `scenario_tests.py --check` all PASS. No C++
  changed → no `build-all.ps1`/CTest rerun.
- Blocker: none. **Next:** #59 now narrows to the `matrix_map_id` registry
  (ADR-0423) or a runtime per-cluster matrix in the model/skin class — trace the
  consumer of the mesh's `matrix_map` (`+0x24`) / `matrix_map_id` (`+0x28`)
  (reached from `FUN_10081b10` "CreateMatrixIW" → `FUN_1002a1d0`); the reference
  box can constrain a candidate transform but does not give one.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 16 — 2026-10-16 05:00 (scheduled loop)
- **Unit:** GH #46 → #55/#58 — pin the **pose/base pairing** left open by
  ADR-0422/0423: is a `.xmd` cluster's stored `VERTEXFRAMES` geometry in the
  `NODES` bind pose? (answer: **no, not universally**).
- Finding (real evidence, read-only; `tools/xmd_pose.py`):
  - Weighted geometry centroid per resolved bone (palette applied) minus
    `NODES.translation` = the per-bone **bind offset**. Two clusters on the same
    bone cancel the joint-vs-geometry bias, so their offset difference tells
    whether they share a reference pose.
  - `Riddick_B05.xmd` (70 nodes, 7 clusters): **legs c0/c3 vs body c6 agree to
    rms 0.93** (max 2.69; 10 shared leg bones; AABBs overlap) → same reference
    pose. **Left arm c1/c4 vs body c6 rms 26.58** (max 29.02; 24 shared bones) and
    **right arm c2/c5 vs body rms 27.27** (max 28.36; 20 shared bones), AABBs
    **disjoint**: arm geometry Z `44.2..52.5` is entirely above the body Z
    `0..36.3`. Duplicates (c0↔c3, c1↔c4, c2↔c5) are rms **0.0**.
  - The `NODES` bind places the arm bones at Z `33.6..48.8` (overlapping the
    body), so neither body nor arms is *simply* the bind pose: the arm clusters
    carry a large baked rotation (raised / chest-height ready pose — inference).
  - Corpus `tools/xmd_pose.py --inventory <game>\Content\Models --check` →
    **268 models, 3454 clusters, 3103 skinned, 0 problems**.
- Change:
  - `tools/xmd_pose.py` (new): `bone_centroids`, `bind_table` (node → bind
    translation, parent, children, rotation/movement slot, local offset, derived
    bind axis), `cluster_pose`, `cluster_relation`, `analyze`, `check`; CLI
    `--model`/`--cluster`/`--relation`/`--bind-table`/`--offsets`/
    `--inventory --check`/`--json`. Read-only, dependency-free.
  - `tools/test_xmd_pose.py` (new): **7** synthetic tests → PASS.
  - `docs/adr/0424-xmd-cluster-reference-pose-vs-bind.md` (new);
    `docs/rea/mos-xmd-format.md` reference-pose note + Next-steps update.
  - `docs/rea/reference/riddick_b05_pose.json` (new evidence: bind table +
    cluster summaries + relations).
  - `.github/workflows/vr-worker.yml`: run the new test.
- Evidence: `xmd_pose.py --model Riddick_B05.xmd` (relations table above);
  `xmd_pose.py --inventory <game>\Content\Models --check` → 268 models /
  3454 clusters / 3103 skinned / 0 problems; `test_xmd_pose.py` (7) PASS;
  `test_mos_xmd.py`, `test_xmd_export.py`, `test_xmd_bake.py`,
  `test_xsa_anim.py`, `validate_addresses.py`, `verify_hook_init.py`,
  `verify_reported_fixes.py`, `scenario_tests.py --check` all PASS. No C++
  changed → no `build-all.ps1`/CTest rerun.
- Blocker: none. **Next:** the skinned native draw needs a **per-cluster
  reference/bind-inverse transform** (not one global `NODES` bind) — likely
  resolved through `matrix_map_id` / the engine's per-cluster matrix setup;
  a **rigid** native draw of the (already raised, `RIDDICK_ARMSCAR_*`) arm
  clusters behind a `VRHandRenderer` config flag is viable now.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 15 — 2026-10-16 04:00 (scheduled loop)
- **Unit:** GH #58 → #46 — pin the `.xsa` **clip → skeleton-slot → track → key
  pool** binding (the "TRACKS-block → NODES-slot mapping" + "7
  QUATERNIONINDICES re-base sets" left open by Wakes 12–14 / ADR-0422).
- Finding (real evidence, read-only; Ghidra 12.1.4 via `rea`):
  - The compressed class is not evaluated by `EvalRot`/`EvalMove`
    (`FUN_10076680`/`FUN_10076700`, both throw `"WTF is this used for?"`).
    Evaluation is **`FUN_100776b0`**, reached from **`FUN_10077f20`** (the
    sequence object's `+0x24` method, `FUN_10078340`). For rotation slot `j` it
    reads `track = TRACKINDICES[track_start + j]`,
    `(start, flags) = TRACKS[track]`, `count = (flags & 0x8000) ? 1 :
    sequence.time_count`, and dereferences `QUATERNIONINDICES[start + key]`
    into **`QUATERNIONS`** (4 x i16, `3.051851e-05 == 1/32767`), Hermite
    interpolation. For movement slot `j` it uses `track = TRACKINDICES[track_mid
    + j]` and dereferences into **`MOVES`** (3 x f32). So a clip's key window is
    a **slot → track** table (not a track set); `0x8000` = single key else
    `time_count` keys; the **pool is chosen by the window**.
  - **The re-base exception is the movement flag `0x0001`:** corpus-wide every
    rotation-window value is `< QUATERNIONS.count` (**0 / 313** sets violate);
    for movement windows **all 81** blocks that exceed `MOVES.count` carry
    `flags & 0x0001` and **all** fit `QUATERNIONS.count` (0 exceed). Decisive:
    `02-cs_entranceintro\Johns.xsa` cut30 track 895 (start 17370, tc 350) reads
    `9339..11229` with `MOVES.count 7170`, `QUATERNIONS.count 48468`; every
    non-`0x0001` movement block fits `MOVES`. So a movement-window track with
    `flags & 0x0001` dereferences `QUATERNIONS`, not `MOVES` — which is exactly
    what the "7 sets where `QUATERNIONINDICES` exceeds `QUATERNIONS.count`" are
    (`Grenade.xsa`: `nq 1`, `nm 24`, movement values `0..23`).
- Change:
  - `tools/xsa_anim.py`: `SlotBinding`; `AnimSet.window_tracks/
    window_slot_track/movement_pool/pool_for/slot_binding/slot_bindings/
    clip_bindings/slot_key_times/slot_rotation_keys/slot_move_keys/
    sample_rotation/sample_move/sequence_by_name`; `clip_slot_table`;
    `_bracket/_lerp/_nlerp` helpers; `--clip NAME` (+ `--json`); `validate()`
    now checks every resolved slot's key values against its pool (rotation →
    `QUATERNIONS`; movement → `MOVES` unless `flags & 0x0001`); docstring.
  - `tools/test_xsa_anim.py`: **+5** tests (slot binding, flag-routed movement
    pool, pool-aware validation both ways, nlerp sampling, clip table) → **19**.
  - `docs/adr/0422-…md` "Update (Wake 15)" (sampler + pool finding, evidence
    commands, resolved follow-ups); `docs/rea/mos-xmd-format.md` `.xsa` section.
  - `docs/rea/reference/xsa_clip_slot_bindings.json` (new evidence: MovBasSha_arm
    `mhstd_sha`, Grenade `Gren_Equip`, Johns `cut30`).
- Evidence: `xsa_anim.py --inventory <game>\Content --check` → **313/313 `ok`,
  0 FAIL** (now with the pool-range invariant), 23 non-animation files skipped;
  `test_xsa_anim.py` (19) PASS; `test_mos_xmd.py`, `test_xmd_export.py`,
  `test_xmd_bake.py`, `validate_addresses.py`, `verify_hook_init.py`,
  `verify_reported_fixes.py`, `scenario_tests.py --check` all PASS. No C++
  changed → no `build-all.ps1`/CTest rerun.
- Blocker: none. **Next:** bind a clip (e.g. an arm/FPS clip) to the
  `Riddick_B05`/FPS-arm `.xmd` skeleton via `rotation_slot`, evaluate the
  bind-pose pairing, bake a posed arm (ADR-0423), then wire `VRHandRenderer`
  behind a config flag (procedural overlay kept as fallback).
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 14 — 2026-10-16 03:37 (scheduled loop)
- **Unit:** GH #58 → #46 — pin the `.xmd` `BONEDEFORM` **bone-index space**
  (the "node index vs `NODEINDICES` position" question left open by Wakes 8–10 /
  ADR-0417 / ADR-0419).
- Finding (real evidence, read-only; corpus-wide):
  - An influence's `bone` byte is a **`matrix_bones` palette index** whenever
    the trailing `matrix_bones` block is **non-empty** (skeleton node =
    `matrix_bones[bone]`); only an **empty** palette means the byte is a node
    index directly. `matrix_map_id` is `-1` for all 692 map-less clusters **and
    also** for **33** clusters that carry a palette, so the palette *length
    alone* decides the space (raw tail bytes confirmed: `… ff ff ff ff 18 00 00
    00 <24 palette bytes>`, id `-1`, count 24).
  - Decisive (`Riddick_B05.xmd` cluster 1, left arm): raw `0..23` remap through
    `matrix_bones` to nodes `{38..52, 16, 14, 13, 12, 9, 8, 1, 17, 18}` = the
    left-arm chain (12 shoulder → 13 → 14 elbow → 16 wrist) **plus five finger
    chains** (38..52, bind `y ≈ -20.4..-24.9`). Grouped by dominant remapped
    bone the fingertip geometry (`y -24..-25.4`) falls on the finger bones;
    read as direct node indices it would bind to pelvis/leg bones (`z 0..32`)
    while the geometry is at `z 45..52` — impossible. The unskinned shadow body
    (cluster 6) has an empty palette and uses nodes `0..69` directly.
  - Corpus (631 models / 4662 clusters / **3103 skinned**): **2411 paletted**
    (`len==max_bone+1` in 2402; 9 have unused entries) / **692 direct**;
    `max_bone >= len(palette)` **0**, palette entry `>= NODES.count` **0**,
    duplicate palette entries **0**, direct `max_bone >= NODES.count` **0**;
    max palette length **24**. `matrix_map_id` (paletted): `-6:1 -5:2 -4:6
    -3:2 -2:1166 -1:33 0:721 1:292 2:137 3:50 4:1`; (direct) `-1:692`.
  - **Still open:** the mesh pose ≠ `NODES` bind pose (the arm mesh is
    `z ≈ 45..52` while the arm bones bind `z 48.8 → 36.8`), so a correct skinned
    draw still needs the `.xsa` animation pose (ADR-0422) and the pose/base
    pairing.
- Change:
  - `tools/mos_xmd.py`: `BoneDeform.has_palette()/bone_space()/resolve_bone()/
    resolved_influences_of()/max_resolved_bone()/used_palette_indices()/
    palette_fully_used()`; `as_dict` carries `bone_space`/`max_resolved_bone`/
    `matrix_bones`/`palette_fully_used`; `cluster_meshes()` records the same;
    `validate()` now enforces the **space-correct** invariants (palette index <
    palette size, resolved node < `NODES.count`, palette entries < `NODES.count`,
    no duplicates); `geometry_stats()`/`geometry_summary()` add
    `paletted_bone_clusters`/`direct_bone_clusters`/`palette_unused_clusters`;
    `--mesh`/`--cluster` print the space + resolved max node + palette; docstring.
  - `tools/xmd_export.py`: `bones_used()` → resolved **node** indices;
    `cluster_skin_dict()` adds `bone_space`/`max_bone`/`max_resolved_bone`/
    `resolved_influences`; manifest records `bone_space`.
  - `tools/xmd_bake.py`: manifest records `bone_space` + resolved bones.
  - `tools/test_mos_xmd.py`: matrix-map test made shipping-valid; **+6** tests
    (palette space, direct space, `-1`-with-palette, index/entry out-of-range,
    duplicate entries, unused entries). `tools/test_xmd_export.py`: +1 palette
    manifest/skin round-trip test.
  - Regenerated `docs/rea/reference/{native_hand_mesh,riddick_b05_export,
    pa2_hand_01_export}.json`; new `docs/adr/0423-…`; ADR-0417 + ADR-0419 +
    `docs/rea/mos-xmd-format.md` corrected.
- Evidence: `mos_xmd.py --inventory <game>\Content\Models --check` → **631/631
  `ok`, 0 FAIL** (space-correct skin checks). `test_mos_xmd.py` (40) PASS;
  `test_xmd_export.py`, `test_xmd_bake.py`, `test_xsa_anim.py`,
  `validate_addresses.py`, `verify_hook_init.py`, `verify_reported_fixes.py`,
  `scenario_tests.py --check` all PASS. `xmd_bake.py --verify
  src/hook/native_hand_mesh_data.h` → still matches a fresh bake (geometry
  bytes unchanged). No C++ changed → no `build-all.ps1`/CTest rerun.
- Blocker: none. **Next:** pair the resolved bone space with the `.xsa` clip
  pose (ADR-0422) — resolve the `TRACKS`→rotation-slot binding and the 7
  `QUATERNIONINDICES` re-base sets, bake a posed arm, then wire
  `VRHandRenderer` behind a config flag (procedural overlay kept as fallback).
- Branch: committed on `main` (`ed92d3d`), pushed to `origin/main`.

## Wake 13 — 2026-10-16 03:35 (scheduled loop)
- **Unit:** GH #58 → #46 — pin the `.xsa` **`SEQUENCES` table** (each clip's key
  and time windows) and the **skeleton animation-slot space**, i.e. the first
  half of Wake 12's "Next" (the track→node side of the mapping).
- Finding (real evidence, read-only; Ghidra 12.1.4 via `rea`):
  - `CXR_Anim_Sequence::Read` = **`FUN_100774f0`** (`.\XRAnimCompressed.cpp`)
    stores its eight tail words straight into the object: `+0x20` u16
    `track_start`, `+0x22` u16 `track_mid` (= `track_start + rotation_count`),
    `+0x24` u8 `rotation_count`, `+0x25` u8 `movement_count`, `+0x26` u16
    `time_start`, `+0x28` u16 `time_count`, `+0x2a` u16, `+0x2c` u16. Base read
    is `CXR_Anim::ReadCompressed` `FUN_1001a5e0` (inner version `0x103`).
  - So every clip owns a **key window** `[track_start, track_start +
    rotation_count + movement_count)` into `TRACKINDICES` and a **time window**
    `[time_start, time_start + time_count)` into `KEYFRAMETIMES`. Over all 313
    shipped sets the non-empty windows are **exact partitions** (313/313 each)
    and `track_mid == track_start + rotation_count` (3,232/3,232).
  - Skeleton side: `CXR_Skeleton::Read` = **`FUN_100219d0`** (`.\XRSkeleton.cpp`)
    reads `NODES` (0x20 stride) / `NODEINDICES` (u16) / `ATTACHPOINTS` then calls
    **`FUN_1001ff60`**, which enforces **rotation slots ≤ 0x80 (128)** and
    **movement slots ≤ 0x20 (32)** from each node's i16 `+0x1c`/`+0x1e` — the
    same `rotation_slot`/`movement_slot` pinned on `.xmd` `NODES` in ADR-0418.
  - `max(TRACKINDICES) < TRACKS.count` and `max(TRACKS[i].start) <
    QUATERNIONINDICES.count` in **312/312** sets with tracks → `TRACKINDICES`
    values index `TRACKS`; `TRACKS` is a per-key-block table (cutscene clips run
    to **1,333** `TRACKS` > the 128-slot cap), so the clip→bone binding goes via
    the skeleton slot space.
  - `EvalRot` in `XRAnimCompressed.cpp` (**`FUN_10076680`**) unconditionally
    throws `"WTF is this used for?"` → the compressed class does not evaluate
    rotations; `COMPRESSEDANIMATIONSET2` is storage, the runtime path is the
    uncompressed `CXR_Anim_SequenceTracks::ReadData` (`FUN_1001b730`).
- Change:
  - `tools/xsa_anim.py`: `Sequence.track_start/track_mid/rotation_count/
    movement_count/time_start/time_count` + `key_window`/`time_window`,
    new `window_tiling_problems`, `validate()` now checks `track_mid`,
    `TRACKINDICES`/`KEYFRAMETIMES` window tiling and `max(TRACKINDICES) <
    len(TRACKS)`, new `--sequences` CLI, expanded docstring.
  - `tools/test_xsa_anim.py`: fixture made shipping-consistent; **+5** tests
    (14 total): key-window gap/overlap, time-window gap, `track_mid` mismatch,
    `TRACKINDICES` out of range.
  - `docs/adr/0422-xsa-compressed-animation-set.md`: seq-table field table,
    skeleton slot finding, new evidence table rows, honest "still open".
  - `docs/rea/mos-xmd-format.md` `.xsa` section (windows + slot binding).
- Evidence: `xsa_anim.py --inventory <game>\Content --check` → **313/313 `ok`,
  0 FAIL**, 0 sequence-window tiling violations, 0 `TRACKINDICES` range
  violations. `test_xsa_anim.py` (14) PASS; `test_mos_xmd.py`,
  `test_xmd_export.py`, `test_xmd_bake.py`, `validate_addresses.py`,
  `verify_hook_init.py`, `verify_reported_fixes.py`, `scenario_tests.py --check`
  all PASS. No C++ changed → no `build-all.ps1`/CTest rerun.
- Blocker: none. **Next:** resolve the `TRACKS`-block → `NODES`-slot mapping
  end-to-end and the 7 `max(QUATERNIONINDICES) >= QUATERNIONS.count` sets
  (`Grenade.xsa` et al., likely a per-clip pool re-base), then bake a rest/idle
  pose and wire `VRHandRenderer` behind a config flag (procedural overlay kept as
  fallback).
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 12 — 2026-10-16 02:25 (scheduled loop)
- **Unit:** GH #58 → #46 — pin the `.xsa` **animation-set** payload
  (`COMPRESSEDANIMATIONSET2`) to the shipped `MXR.dll` loader so per-bone
  rotations have an **offline**, verifiable source (ADR-0421 left this open).
- Finding (real evidence, read-only; Ghidra 12.1.4 via `rea`):
  - `.xsa` is the same `MOS DATAFILE2.0` container; its first child node is
    `COMPRESSEDANIMATIONSET2`. Dispatcher `FUN_1001d7c0` (`rea xrefs MXR.dll
    0x10144580` → `{0x1001d8e2, 0x10076d4e}`) probes `ANIMATIONSET` /
    `ANIMATIONTRACKSET` / `COMPRESSEDANIMATIONSET` / `COMPRESSEDANIMATIONSET2`;
    for the last it allocates a `CXR_Anim_Compressed` (vtable `0x101442b8`) and
    calls slot `+0x20` = **`FUN_10076d20`** (`.\XRAnimCompressed.cpp`), which
    reads each chunk as `element_size * node.count` raw bytes.
  - Element layout: `QUATERNIONS` 8 B (`CXR_Anim_Quatint16`, 4 x i16 → /32767
    unit quaternion), `MOVES` 12 B (3 x f32 translation), `TRACKINDICES`/
    `QUATERNIONINDICES` 2 B (u16), `KEYFRAMETIMES` 4 B (f32), `TRACKS` 4 B
    (`CXR_Anim_CompressedTrack` = u16 `start` + u16 `flags`), `DATAKEYS`/
    `SEQUENCES` variable. Sequence records = `CXR_Anim::ReadCompressed`
    (`FUN_1001a5e0` + `FUN_100774f0`), version **0x103** for every shipped clip.
  - RTTI strings (`.?AVCXR_Anim_Quatint16@@`, `CXR_Anim_CompressedTrack`, ...)
    confirm the encodings.
- Change:
  - `tools/xsa_anim.py` (new): read-only decoder + `AnimSet.validate()`;
    `--inventory`/`--check`/`--tracks`/`--quaternions`/`--json`.
  - `tools/test_xsa_anim.py` (new): 9 synthetic-container tests.
  - `docs/adr/0422-xsa-compressed-animation-set.md` (new);
    `docs/rea/mos-xmd-format.md` `.xsa` section; ADR-0421 follow-up marked
    resolved to path (a); `.github/workflows/vr-worker.yml` (+test step).
- Evidence: `xsa_anim.py --inventory <game>\Content --check` → **313/313 `ok`**,
  **0** failures (**23** non-anim files skipped: 3 legacy `ANIMATIONSET` `.xsa`
  + 20 `.xag`). Corpus: **2,710,465** pooled quaternions (all unit, |norm−1| ≤
  **3.0e-5**), **251,790** moves, **110,046** tracks (**0** `start` out of range,
  flags ⊂ `{0,1,0x8000,0x8001}`), **3,232** sequences (names like `Gren_Active`,
  `Hangrail`, `Ladder`), **105,801** keyframe times; **0** fixed-chunk tiling and
  **0** `SEQUENCES` tiling violations. `mos_xmd.py --inventory <game>\Content\
  Models --check` still **631/631 ok**; `test_mos_xmd.py`, `test_xmd_export.py`,
  `test_xmd_bake.py`, `test_xsa_anim.py`, `validate_addresses.py`,
  `verify_hook_init.py`, `verify_reported_fixes.py`, `scenario_tests.py --check`
  all PASS. No C++ changed, so `build-all.ps1`/CTest were not rerun.
- Blocker: none. **Next:** pin the **track → skeleton-node** mapping (how
  `TRACKS[i]` binds to a `.xmd` `NODES` bone and its key-block length) and the
  `QUATERNIONINDICES` indirection (`Grenade.xsa` has a 1-entry `QUATERNIONS`
  pool yet `QUATERNIONINDICES` values up to 23 → not a plain pool index). Then
  bake a rest/idle pose and wire `VRHandRenderer` behind a config flag
  (procedural overlay retained as fallback).
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 11 — 2026-10-16 00:47 (scheduled loop)
- **Unit:** GH #46 — pin the `.xmd` `KEYS` chunk to the shipped loader and prove
  it is a **string metadata map**, not bone rotations (ADR-0419/0420 and the
  `xmd_export`/`xmd_bake` notes assumed rotations could be read from `KEYS`).
- Finding (real evidence, read-only; Ghidra 12.1.4 via `rea`):
  - `MXR.dll` model reader `FUN_10083060` @`0x10083ce9` calls
    `CDataFile::GetNext(param_1,"KEYS")` then
    `CKeyContainer::Read((CKeyContainer*)((int)this+0x54), (CCFile*)*GetFile())`.
    `rea xrefs MXR.dll 0x10149df0` → `{0x100823d7 (writer FUN_10081f30),
    0x10083cbe (reader FUN_10083060)}`.
  - `MCCDyn.dll` `CKeyContainer::Read` @`0x100195d0` (found via
    `rea search MCCDyn.dll "KeyContainer" --kind procedures`) reads: `u32 count`;
    per entry `u32 key_len; char key[key_len+1] (NUL); u32 value_len; char
    value[value_len+1] (NUL)`. A **zero-length value consumes no bytes** (the
    loader stores "" and reads nothing), so a naive parse misaligns there.
  - Across all 631 shipped models: **631** `KEYS` chunks (one each), **4634**
    entries, **54 distinct keys** (`XTXPATH`, `MODEL`, `OGR_RENDERFLAGS`,
    `NORMALMAP_*`, `GI_LIGHT_*`, `VERTEXSPLITANGLE`, ...), **319** empty values,
    **0** control chars in values, `MODEL` == file relpath in **606** (18
    aliases, 7 absent). No rotations/quaternions anywhere → metadata only.
  - Shipped anomaly: `Characters\Dwellers\dweller.xmd` ships one corrupt
    non-printable first key (13 bytes); so `validate()` enforces only the exact
    tiling invariant, not printability.
- Change:
  - `tools/mos_xmd.py`: `KeyValue`, `decode_keys` (mirrors
    `CKeyContainer::Read`), `Xmd.keys()/key_map()/has_keys()`,
    `geometry_summary` `has_keys`/`keys_count`/`key_names`, `validate()` exact
    tiling cross-check, `--keys` CLI, module docstring.
  - `tools/test_mos_xmd.py`: +6 `KEYS` tests (round-trip incl. empty value,
    absent, bad tiling, count overrun, missing NUL terminator, latin1 binary
    key).
  - Corrected the "rotations live in `KEYS`" claim in `tools/xmd_export.py`
    (`ROTATIONS_NOTE`), `tools/xmd_bake.py`, `docs/rea/mos-xmd-format.md`,
    ADR-0419/0420; cross-ref added in ADR-0413. New ADR-0421.
  - Regenerated `docs/rea/reference/native_hand_mesh.json` (`rotations_note`).
- Evidence: `mos_xmd.py --inventory <game>\Content\Models --check` → **631/631
  `ok`, 0 failures** (with the new `KEYS` tiling cross-check). `xmd_bake.py
  --verify` → `native_hand_mesh_data.h` still matches a fresh bake (4 meshes /
  1548 verts). `test_mos_xmd.py`, `test_xmd_export.py`, `test_xmd_bake.py`,
  `validate_addresses.py`, `verify_hook_init.py`, `verify_reported_fixes.py`,
  `scenario_tests.py --check` all PASS. No C++ changed, so `build-all.ps1`/CTest
  were not rerun (as in Wakes 3-9).
- Blocker: none. **Next:** the native hand draw path still needs per-bone
  rotations, which are **not** in `.xmd`; source them from the `.xsa`/`.xag`
  animation groups (offline, decodable) or the engine's live skeleton
  in-process (#34/#46) — new issue opened. Then wire `VRHandRenderer` behind a
  config flag (procedural overlay retained as fallback).
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 10 — 2026-10-16 00:31 (scheduled loop)
- **Unit:** GH #55 → #46 — bake the decoded arm/hand geometry into a native,
  compile-in C++ asset, and pin the `.xmd` **cluster→surface** mapping that
  ADR-0419 left open.
- Finding (real evidence, read-only): every `CLUSTER` node has a 44-byte
  `MISCELLANEOUS` child = `{u32 version=0x102; u32 flags; u32 surface_index;
  u32 reserved; 24 opaque bytes}`. Over **all 631** shipped models /
  **4662** clusters: `surface_index < SURFACES.count` (**0** violations), the
  number of **distinct** surface values equals `SURFACES.count` in every model
  (**0** mismatches), and `flags == 0` **iff** the cluster carries a non-empty
  `TVERTEXFRAMES` (2099 textured all `flags==0`; 2563 untextured `{1:839,
  3:1724}` — the 2099 matches ADR-0416 exactly). So `Riddick_B05`: cluster 1
  (left arm) = surface **1** `RIDDICK_B03_D`, cluster 2 (right arm) = surface
  **2** `RIDDICK_ARMSCAR_D`, clusters 0/3/6 = surface 0 `PA1_INMATE_P17`
  (the base-name grouping is an inference — only the numeric index is proven).
- Change:
  - `tools/xmd_bake.py` (new): bakes clusters to a C++ header
    (`--set`/`--model`/`--cluster`, `--cpp`, `--manifest`, `--verify`,
    `--json`); dependency-free, read-only, built on `mos_xmd`/`xmd_export`.
  - `src/hook/native_hand_mesh_data.h` (generated): 4 meshes / 1548 verts /
    2610 tris (`Riddick_LeftArm` 526v, `Riddick_RightArm` 344v, `Pa2_Hand` 138v,
    `Pa2_Hand_Detail` 540v) as `inline constexpr` arrays.
  - `src/hook/native_hand_mesh.h` (new, header-only): struct + count/at/find
    accessors.
  - `tests/native_hand_mesh_tests.cpp` + CMake target `native_hand_mesh_asset`:
    index range/coverage, unit normals, declared-vs-computed AABB, frozen
    vertex/index/surface counts (drift guard), LOD bounds, lookup.
  - `tools/mos_xmd.py`: `ClusterInfo`/`decode_cluster_info`/`cluster_infos()`;
    `cluster_meshes()` carries `surface_index`/`cluster_flags`; `validate()` adds
    the 3 surface/flag invariants; summary + `--mesh` print the surface.
  - `tools/xmd_export.py`: manifests now record `surface_index`.
  - `tools/test_xmd_bake.py` (new, 6 tests), `tools/test_mos_xmd.py` (+4
    cluster-info tests), `.github/workflows/vr-worker.yml` (+bake test step),
    `docs/adr/0420-native-hand-mesh-asset.md`,
    `docs/rea/reference/native_hand_mesh.json`.
- Evidence: `mos_xmd.py --inventory <game>\Content\Models --check` → **631/631
  `ok`, 0 failures** (with the new invariants). `xmd_bake --verify` → **OK**
  (`Riddick_B05.xmd` sha256 `e6841e19…`, `Pa2_Hand_01.xmd` `bdd0dbd1…` recorded
  in the header). `build-all.ps1 -Config Release` OK (x64 + x86); CTest **4/4 on
  x64 and x86** (`native_hand_mesh_asset` passes both). `test_mos_xmd.py`,
  `test_xmd_export.py`, `test_xmd_bake.py`, `validate_addresses.py`,
  `verify_hook_init.py`, `verify_reported_fixes.py`, `scenario_tests.py --check`
  all PASS.
- Blocker: none. Next: wire the native draw path in `VRHandRenderer` behind a
  config flag (rigid bind-pose first, then skinned once `KEYS` bone rotations are
  decoded), procedural overlay retained as fallback; the `SURFACES` base-name
  grouping and the `MISCELLANEOUS` +0x10 bounds-like block remain open.
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 9 — 2026-10-16 00:17 (scheduled loop)
- **Unit:** GH #55 → #46 — emit the decoded mesh/skin data: new
  `tools/xmd_export.py` (+ `tools/test_xmd_export.py`) exports any `.xmd`'s
  clusters to Wavefront OBJ, a `.skin.json` (skeleton hierarchy + per-vertex
  bone influences) and a compact `.export.json` manifest, so the native hand/arm
  path can consume real data without re-deriving the binary format.
- Change:
  - `tools/xmd_export.py` (new): `decode_cluster()`, `mesh_obj()`,
    `combined_obj()`, `skeleton_dict()`, `cluster_skin_dict()`, `manifest()`,
    `export_model()`; CLI `--out/--prefix/--cluster/--combined/--no-skin/--list/--json`.
    Dependency-free, read-only, built on `tools/mos_xmd.py`.
  - `tools/test_xmd_export.py` (new): 6 headless synthetic-container tests
    (OBJ round-trip with/without UVs, combined index rebasing, skin weights,
    cluster select + out-of-range, manifest bone/surface stats, skeleton child
    table, CLI errors).
  - `.github/workflows/vr-worker.yml`: runs the new test next to
    `test_mos_xmd.py`.
  - `docs/adr/0419-xmd-mesh-skin-export.md` (new); ADR-0418 gets a pointer update.
  - `docs/rea/reference/riddick_b05_export.json`,
    `docs/rea/reference/pa2_hand_01_export.json`: generated manifests (evidence).
  - `tools/mos_xmd.py`: corrected the `NODES` `translation[3]` docstring from
    "local" to **model-space** (see below).
- Evidence: all **631** shipped `.xmd` under `Content/Models` decode and export →
  **631 ok / 0 fail**: **4662** clusters, **1,242,285** verts, **1,860,184**
  tris, **423,559** UVs, **3103** skinned; every emitted OBJ re-parses with
  `#v`/`#f` equal to the decoded counts and all face indices in range.
  `Riddick_B05.xmd` → 7 clusters (0/3 legs bones 0-9 z0-25; 1/4 and 2/5 the two
  arms bones 10-23, `|y|`≤25.4 z≈44-52; 6 = untextured `SHADOWDATA` body);
  clusters 3/4/5 duplicate 0/1/2 with an empty `TVERTEXFRAMES`.
  `Pa2_Hand_01.xmd` → 2 clusters / 678 verts / 1256 tris, unskinned.
  `test_mos_xmd.py`, `test_xmd_export.py`, `validate_addresses.py`,
  `verify_hook_init.py`, `scenario_tests.py --check`, `verify_reported_fixes.py`
  all PASS. No C++ changed, so `build-all.ps1`/CTest were not rerun.
- **Finding (correction):** `NODES` `translation[3]` is the node's **bind-pose
  model-space position**, not a local offset. `Riddick_B05`: node 1 (pelvis) and
  node 2 (left hip, `parent 1`) both `z == 32.617` (a local off. would put the
  hip at z 65 on a ~52-unit figure); node 15 (`parent 11`) z 56.1. Aggregate over
  **248** skeletons: chaining translations explodes past the mesh bbox (>25%
  span) in **196/248** vs **96/248** for the direct reading. ADR-0419 has the
  detail; ADR-0418/mos_xmd docstrings corrected.
- Blocker: none. Open (next): the exact `SURFACES`→`CLUSTER` mapping (which
  cluster is `RIDDICK_ARMSCAR_*`) and the `BONEDEFORM` bone-index space (node
  index vs `NODEINDICES` position) both still need the engine consumer to pin
  down; then bake the arm/hand mesh + skeleton and add the native skinned draw
  path in `VRHandRenderer` behind a config flag (fallback: procedural hands).
- Branch: committed on `main`, pushed to `origin/main`.

## Wake 8 — 2026-10-16 00:15 (scheduled loop)
- **Unit:** GH #55 → #46 — correct/extend the just-committed `.xmd` `SKELETON`
  decode: pin the `+0x0E` node field and the real meaning of `NODEINDICES`.
- Coordination: while this wake was doing independent RE, a **second concurrent
  writer** (extra `opencode` sessions were live) implemented the skeleton step
  and committed it as `2a5430d` (Wake 7). Working tree was clean by the time I
  went to commit, so this wake **builds on** that commit instead of duplicating
  it (no clobbering; explicit `git add <paths>` only).
- Observation (real evidence, read-only; Ghidra 12.1.4 via `rea`): the
  **load-time hierarchy walker `FUN_1001e040`** (called by `FUN_1001f2e0`, in
  turn called at the end of the skeleton reader `FUN_100219d0`) recurses over the
  tree. For node `i` it reads `child_count = *(u8*)(node + 0x10)` and
  `child_start = *(i16*)(node + 0x0E)` and takes each child as
  `*(u16*)(NODEINDICES_base + (child_start + k) * 2)`, recursing on it. It never
  reads the `parent` field (`+0x12`). Therefore Wake 7's "`+0x0E` index
  (uninterpreted)" is really **`child_start`**, and `NODEINDICES` is the
  **flattened child table** (`node.children == NODEINDICES[child_start :
  child_start + child_count]`), not merely the set of used non-root nodes.
- Change:
  - `tools/mos_xmd.py`: `SkeletonNode.child_start` replaces `unknown_e`;
    `Skeleton.children_of()` now reads the `NODEINDICES` table exactly like
    `FUN_1001e040`; added `child_ranges_tile()` and `derived_parents()`;
    `child_count_matches()` now cross-checks the table against the `parent`
    field; `validate()` adds child-range bounds, exact tiling,
    `sum(child_count) == len(NODEINDICES)`, and table-vs-parent-field agreement;
    padding nodes must also have `child_count == 0`; `geometry_summary()` reports
    `skeleton_child_table_ok`; `--skeleton` prints `child_start`; docstring
    documents the child table.
  - `tools/test_mos_xmd.py`: +3 headless tests (child-table chain walk incl.
    `child_start` offsets + `derived_parents`, overrun detection, parent-field
    vs child-table mismatch); the skin helper now emits a matching
    `NODEINDICES` child table.
  - `docs/adr/0418-xmd-skeleton-nodes-nodeindices.md`: `+0x0E` corrected to
    `child_start`; new "Update — 2026-10-16 (wake 8)" section with the
    `FUN_1001e040` evidence.
- Evidence: `mos_xmd.py --inventory <game>\Content\Models --check` → **631/631
  `ok`, 0 failures**. Aggregate via the parser API → **249** skeletons /
  **12593** nodes / **12424 used** + **169 padding** / **12175** indices;
  **0** tiling failures, **0** `child_start` overruns, **0** child_count/indices
  sum mismatches, **0** table-vs-parent mismatches; `max_rotation_slot 73`,
  `max_movement_slot 20`; deepest chain 12. `test_mos_xmd.py` PASS;
  `verify_reported_fixes.py`, `scenario_tests.py --check`,
  `validate_addresses.py`, `verify_hook_init.py` all pass. No C++ changed, so
  `build-all.ps1`/CTest were not rerun (as in Wakes 3–7).
- Blocker: none. Next: extract the Riddick hand/forearm surfaces
  (`RIDDICK_ARMSCAR_D/S/N` in `Riddick_B05.xmd`, `Pa2_Hand_01`) with geometry +
  UVs + skeleton + skin and add a native mesh path behind a flag, keeping
  `VRHandRenderer` as fallback (GH #46). Node names remain unshipped — identify
  the hand bones from geometry/skinning.
- Branch: committed on `main` (`7b33e0e`), pushed to `origin/main`.

## Wake 7 — 2026-10-16 00:03 (scheduled loop)
- **Unit:** GH #55 → #46 — decode the `.xmd` `SKELETON` `NODES`/`NODEINDICES`
  payloads from the shipped `MXR.dll` loader (the skeleton step after
  geometry/UV/skinning of Wakes 3–6).
- Observation (real evidence, read-only; Ghidra 12.1.4 via `rea`): the
  `SKELETON` string is at MXR.dll `.rdata` `0x10149df8`; `rea xrefs` → the model
  writer `FUN_10081f30` and the model reader `FUN_10083060`
  (`CTriangleMeshCore::Read`). The reader calls the skeleton reader
  **`FUN_100219d0`**, which `GetNext("NODES")` then reads one record per node via
  **`FUN_1001fc30`**, `GetNext("NODEINDICES")` (each element a `u16`), optional
  `ATTACHPOINTS`, then finalizes with `FUN_1001ff60`. `FUN_1001fc30` (reader) and
  `FUN_1001eec0` (writer) agree byte for byte: a **34-byte** node record
  (`u16 version==0x100`; `f32 t[3]`; `u16 reserved`; `u16 index`; `u16
  child_count`; `u16 parent`; `f32 scale[2]`; `u16 rotation_slot`; `u16
  movement_slot`). `FUN_1001ff60` caps rotation slots at `0x80` and movement
  slots at `0x20`.
- Recovered: **used** nodes have `rotation_slot == index`; a record with
  `rotation_slot == 0xFFFF` (also `movement_slot == 0xFFFF`, zero translation,
  `parent == 0`) is a **padding** node excluded from `NODEINDICES`.
  `NODEINDICES` = `u16[used_nodes − 1]`, every used non-root node exactly once
  (the bone/traversal order). Node **names are not stored** in `.xmd`.
- Change:
  - `tools/mos_xmd.py`: added `SkeletonNode`, `Skeleton` (`used_nodes`,
    `children_of`, `is_tree`, `child_count_matches`, `max_rotation_slot`,
    `max_movement_slot`), `decode_skeleton`; `Xmd.skeleton()`;
    `geometry_summary()` reports skeleton node/used/padding/index counts + tree
    flag; `validate()` cross-checks the skeleton (version, 34-byte tiling,
    reserved/scale, used `rotation_slot == index`, zeroed padding, `parent` in
    range, acyclic tree, `child_count == used children`, `NODEINDICES == used
    non-root set`); `--skeleton` CLI; docstring documents the layout.
  - `tools/test_mos_xmd.py`: +7 headless synthetic-container skeleton tests
    (round-trip, bad child_count, bad NODEINDICES, size mismatch, non-zeroed
    padding, bad version, absent skeleton); fixed the skin helper to emit a
    valid flat skeleton.
  - `docs/adr/0418-xmd-skeleton-nodes-nodeindices.md` (new).
- Evidence: `mos_xmd.py --inventory <game>\Content\Models --check` → **631/631
  `ok`, 0 failures** (with skeleton cross-checks). Across shipped models: **268**
  `NODES` chunks, **249** non-empty skeletons, **12593** records (**12424 used** /
  **169 padding**), **12175** `NODEINDICES`; **0** violations for every invariant
  above, **249/249** acyclic trees, max rotation slot **73** (< `0x80`), max
  movement slot **20** (< `0x20`). Samples: `Riddick_B05.xmd` 70 nodes / 69
  indices / coherent spine+arm chain (0→1→8→9→10→11→{15,20,24,…});
  `dweller.xmd` 69 records (50 used / 19 padding) / 49 indices.
  `test_mos_xmd.py` PASS; `validate_addresses.py`, `verify_reported_fixes.py`,
  `scenario_tests.py --check`, `verify_hook_init.py` all pass. No C++ changed, so
  `build-all.ps1`/CTest were not rerun (as in Wakes 3–6).
- Blocker: none. Next: extract the Riddick hand/forearm surfaces
  (`RIDDICK_ARMSCAR_D/S/N` in `Riddick_B05.xmd`) with geometry + UVs + skeleton +
  skin and add a native mesh path behind a flag, keeping `VRHandRenderer` as
  fallback (GH #46).
- Branch: committed directly on `main` (sole-weight/single-writer mode); pushed
  to `origin/main` (normal push). No game file modified.

## Wake 6 — 2026-10-15 23:08 (scheduled loop)
- **Unit:** GH #55 → #46 — decode the `.xmd` `BONEDEFORM` skinning payloads from
  the shipped `MXR.dll` loader.
- Observation (real evidence, read-only; Ghidra 12.1.4 via `rea`):
  `CTriangleMeshCore::Read` (`FUN_10080f50`) selects the **embedded**
  `BONEDEFORM` form when the model version is > `0x100` (shipped models report
  `0x102`, read from the first `MISCELLANEOUS` u32) and calls **`FUN_1007d260`**.
  That reader: `u32 n` + `n * 4`-byte vertex records (`{u8 flags; u8
  influence_count; u16 influence_start}`), then `u32 m` + the influence pool read
  by `FUN_1007c150(stream, 0x200, dst, m)` (version-0x200 branch: **2 bytes per
  influence**, `u8 bone` + `u8 weight` scaled by `0.003921569 == 1/255`), then a
  trailing matrix-map block. `influence_start` is the running prefix sum of
  `influence_count`; the loader repairs the last weight to `1 - sum(previous)`.
- Recovered (embedded `BONEDEFORM`, model version `0x102`): payload =
  `u32 n; n*{u8 flags; u8 count; u16 start}; u32 m; m*{u8 bone; u8 wq};
  u32 matrix_map_id (s32, 0xFFFFFFFF = none); u32 b; b*u8`, size
  `4 + 4n + 4 + 2m + 8 + b`. No shipped model uses the separate
  `BONEVERTEXINFO`/`BONEINFLUENCE`/`BONEMATRIXMAP` chunks.
- Change:
  - `tools/mos_xmd.py`: added `BoneVertex`, `BoneDeform` (`influences_of`,
    `max_bone`, `max_influences`), `decode_bonedeform`; `Xmd.bone_deform()`,
    `Xmd._skin_owner_map()` (pairs each `VERTEXFRAMES` with its sibling
    `BONEDEFORM`); `cluster_meshes()` records `skin_chunk`/`skin`/
    `skin_vertices`/`skin_matches`/`max_bone`; `validate()` cross-checks skin/
    vertex counts, prefix-sum starts (in `decode_bonedeform`), exact tiling, and
    bone index < `NODES.count`; `geometry_stats()`/`geometry_summary()` report
    `skinned_clusters`/`skinned_vertices`/`decoded_influences`/
    `max_influences_per_vertex`; `--mesh`/`--cluster N` print skin.
  - `tools/test_mos_xmd.py`: +6 headless synthetic-container skin tests
    (round-trip + weight scale + `influences_of`, matrix-map decode, prefix-sum
    mismatch, skin/vertex count mismatch, out-of-range bone, bad matrix-map
    tiling, no-`BONEDEFORM` case).
  - `docs/adr/0417-xmd-bonedeform-skinning.md` (new); ADR-0415/0416 follow-ups
    marked done.
- Evidence: `mos_xmd.py --inventory <game>\Content\Models --check` → **631/631
  `ok`, 0 failures** (with skinning cross-checks). Aggregate decode → **4662
  clusters**: **3103 skinned** (1559 rigid) / **1,315,626 influences** / **max 3
  influences per vertex**; every skinned cluster tiles exactly (`4+4n+4+2m+8+b`),
  **0** bad clusters, **0** out-of-range bone indices, **0** missing matrix-map
  tails. Per-vertex weights sum to exactly 1.0 on `Riddick_H01`/`B03`/`LGuard`/
  `H_Civ` (**0** violations). Sample `Riddick_H01` cluster 0: 985 verts / 1564
  influences / `max_bone=17` (< 70 skeleton nodes) / `v0 = {3:.008, 1:.102,
  0:.890}`. `test_mos_xmd.py` PASS; `validate_addresses.py`,
  `verify_reported_fixes.py`, `scenario_tests.py --check`, `verify_hook_init.py`
  all pass. No C++ changed, so `build-all.ps1`/CTest were not rerun (as in
  Wakes 3–5).
- Blocker: none. Next: decode the `NODES` skeleton (names + hierarchy) +
  `NODEINDICES` so bone indices resolve to named joints; then extract the
  Riddick hand/forearm surfaces (`RIDDICK_ARMSCAR_*`, `Pa2_Hand_01`) with
  geometry + UVs + skin and add a native mesh path behind a flag, keeping
  `VRHandRenderer` as fallback.
- Branch: committed on `docs/rea-mos-loader-layout` (single-writer mode — no new
  branch; not pushed; `main` untouched).

## Wake 5 — 2026-10-15 22:48 (scheduled loop)
- **Unit:** GH #55 → #46 — decode the `.xmd` `TVERTEXFRAMES` (UV) payloads from
  the shipped `MXR.dll` loader.
- Observation (real evidence, read-only; Ghidra 12.1.4 via `rea`): the
  `TVERTEXFRAMES` per-frame reader is **`FUN_100807d0`** in `MXR.dll`
  (`CTriangleMeshCore::Read` calls it once per cluster right after reading
  `VERTEXFRAMES`; `FUN_1007fce0` sizes the UV list, `FUN_1007fba0` the position
  list). It switches on a version argument: `0x201` = deferred/seeking form
  (`RelSeek(count*4 + 0x10)`), `0` = raw `u32[n][2]`, else
  `throw "Unsupported version %.4x"`. The branch every shipped asset selects is
  **`0x200`**.
- Recovered (VF version 0x200 UV frame): `u32 n; f32 min_u; f32 min_v; f32
  max_u; f32 max_v; u16 uv[n][2]`; dequant `min + q*(max-min)/65535` (same
  `1.5259022e-05` constant as positions); frame size exactly **`20 + 4n`**. A
  cluster with no UVs stores an **empty** (`data_size == 0`) `TVERTEXFRAMES`
  node.
- Change:
  - `tools/mos_xmd.py`: added `TVertexFrame`, `decode_tvertex_frame`,
    `Xmd.tvertex_frames()`; `cluster_meshes()` now pairs each cluster's
    `VERTEXFRAMES`/`TVERTEXFRAMES`/`TRIANGLES` and records `uv_count`/
    `uv_matches`; `validate()` cross-checks exact UV tiling + UV/vertex count
    agreement; `geometry_stats()`/`geometry_summary()` report `uv_clusters`/
    `decoded_uvs`; docstring documents the layout; `--mesh`/`--cluster N` print
    UVs.
  - `tools/test_mos_xmd.py`: +4 headless synthetic-container UV tests
    (round-trip/dequant, count mismatch, bad tiling, empty chunk).
  - `docs/adr/0416-xmd-uv-tvertexframes.md` (new); ADR-0415 follow-up marked
    done.
- Evidence: `mos_xmd.py --inventory <game>\Content\Models --check` → **631/631
  `ok`, 0 failures**. Aggregate decode → **4662 clusters**: **2099 carry UVs**
  (each tiling exactly `20+4n`, `n` == paired vertex count) and **2563 store an
  empty node** (untextured); **423,559** decoded UV pairs; **0** bad clusters,
  **0** UV/vertex mismatches. Sample `Riddick_H01.xmd` cluster 0:
  985 verts / 985 UVs / `uv_match=True`. `test_mos_xmd.py` PASS;
  `validate_addresses.py`, `verify_reported_fixes.py`, `scenario_tests.py
  --check`, `verify_hook_init.py` all pass. No C++ changed, so
  `build-all.ps1`/CTest were not rerun (as in Wakes 3–4).
- Blocker: none. Next: decode `BONEDEFORM`/`BONEINFLUENCE`/`BONEMATRIXMAP`
  (skinning; `FUN_1007d260` = BONEDEFORM reader — per-vertex
  `(u16 bone_index, u8 influence_count, ...)` + per-influence `u16 bone,
  f32 weight`), then extract the Riddick hand/forearm surfaces
  (`RIDDICK_ARMSCAR_*`, `Pa2_Hand_01`) with geometry + UVs + skin and add a
  native mesh path behind a flag, keeping `VRHandRenderer` as fallback.
- Branch: committed on `docs/rea-mos-loader-layout` (single-writer mode — no new
  branch; not pushed; `main` untouched).

## Wake 4 — 2026-10-15 22:40 (scheduled loop)
- **Unit:** GH #55 → #46 — decode the `.xmd` `VERTEXFRAMES`/`TRIANGLES`
  geometry payloads from the shipped `MXR.dll` loader.
- Observation (real evidence, read-only; Ghidra 12.1.4 via `rea`): the geometry
  chunk-name strings + readers live in **`MXR.dll`** (sha256 `06424f55…`, PDB
  `Dll_XR.pdb`), not `MCCDyn.dll`. `FUN_10080f50` = `CTriangleMeshCore::Read`;
  `FUN_1007fed0` = `CVertexFrame::Read` (switches on version `0x200`..`0x204`,
  else throws `"Unsupported vertex-frame version %.4x"`); positions are decoded
  by `FUN_1007c6c0`, normals by `FUN_1007ca20`; `FUN_10006620` = read `u32`,
  `FUN_10019390` = read 3 floats.
- Recovered (all shipped models use VF **version `0x204`**): frame =
  `u32 version; u32; f32 min[3]; f32 max[3]; f32 aux[3]; u32; u32 n;
  u16 pos[n][3]; s8 nrm[n][3]`; positions dequantized
  `min + q*(max-min)/65535` (`1.5259022e-05 == 1/65535`), normals `c/127`
  (`0.007874016 == 1/127`) re-normalized; frame size **`52 + 9n`**; when the
  model version is > `0x100` the chunk starts with one `u32` frame-count word
  (read + discarded). `TRIANGLES` = `u32 count` + `count` *u16* triplets.
- Change:
  - `tools/mos_xmd.py`: added `VertexFrame`, `decode_vertex_frame`,
    `decode_triangles`; `Xmd.vertex_frames()/triangle_indices()/
    cluster_meshes()/geometry_stats()`; `validate()` now cross-checks every
    `VERTEXFRAMES`/`TRIANGLES` cluster pair (index range + payload tiling);
    `--mesh`/`--cluster N` CLI; docstring documents the layout.
  - `tools/test_mos_xmd.py`: +3 headless synthetic-container geometry tests
    (round-trip, out-of-range index, triangle-size mismatch).
  - `docs/adr/0415-xmd-vertex-frame-geometry.md` (new); ADR-0414 follow-up
    marked done.
- Evidence: `mos_xmd.py --inventory <game>\Content\Models --check` → **631/631
  `ok`** (geometry cross-check on); aggregate decode → **4662/4662 cluster
  pairs** with every triangle index in range *and* `max(index) == n-1` exactly
  (0 unsupported, 0 corrupt); totals **1,242,285 vertices / 1,860,184
  triangles**; the `52+9n` frame size tiles every chunk with no remainder;
  `test_mos_xmd.py` PASS; `validate_addresses.py`, `verify_reported_fixes.py`,
  `scenario_tests.py --check`, `verify_hook_init.py` all pass. No C++ changed, so
  `build-all.ps1`/CTest were not rerun (as in Wake 3).
- Blocker: none. Next: decode `TVERTEXFRAMES` (UV; node flags `0x201`) and
  `BONEDEFORM`/`BONEINFLUENCE`/`BONEMATRIXMAP` (skinning), then extract the
  Riddick hand/forearm surfaces (`RIDDICK_ARMSCAR_*`, `Pa2_Hand_01`) and add a
  native mesh path behind a flag, keeping `VRHandRenderer` as fallback.
- Branch: committed on `docs/rea-mos-loader-layout` (single-writer mode — no new
  branch; not pushed; `main` untouched).
  - Note: `rea --snapshot <name>` writes `<name>` into the CWD — do **not**
    commit it (the `mxr` snapshot was removed from the tree).

## Wake 3 — 2026-10-15 21:47 (scheduled loop)
- **Unit:** GH #55 (→ #46) — recover the `MOS DATAFILE2.0` loader layout from
  the shipped engine and reconcile the parser with it.
- Observation (real evidence, read-only; Ghidra 12.1.4 via `rea`): the magic
  `"MOS DATAFILE2.0\0"` is at `MCCDyn.dll` RVA `0x1005C298` / file offset
  `0x5B698` (`.rdata`); its only xrefs are `CDataFile::Open` @`0x10032FB0` and
  `CDataFile::Create` @`0x10033F10`. `CDataFile::ReadTree_r` @`0x10032840`
  reads each node as a **0x18-byte name field + six `u32`s at `+0x18`**
  (sibling `+0x18`, child `+0x1C`, data offset `+0x20`, size `+0x24`, count
  `+0x28`, flags `+0x2C`), and the **file header is the root node record**
  (its name field *is* the magic). ADR-0413 §A had a 16-byte name + 8 fields;
  the sibling/child indices happened to be right, the split was not.
- Change:
  - `tools/mos_xmd.py`: parse `name[0x18]` + 6 fields; derive `node_start`/
    `data_start` from the root record; add `walk_linked()` (mirrors
    `ReadTree_r`) and `validate()` (root/header agreement, pointer integrity,
    linked-walk coverage, exact payload tiling) + a `--check` mode.
  - `tools/test_mos_xmd.py` (new): self-contained synthetic-container tests
    (round-trip, corrupt pointer, bad magic) — no game install needed.
  - `docs/adr/0414-mos-datafile2-loader-container.md` (new) + ADR-0413 §A
    correction note; `.github/workflows/vr-worker.yml` runs the new test.
- Evidence: `mos_xmd.py --inventory <game>\Content\Models --check` → **631/631
  `ok`, 0 failures**; `--inventory` output byte-identical old↔new for all 18
  `Characters/Riddick` models (regression-safe); `test_mos_xmd.py` PASS;
  `validate_addresses.py`, `verify_reported_fixes.py`, `scenario_tests.py
  --check` all pass; CTest 3/3 on **both** x64 and x86 (no C++ changed, so
  `build-all.ps1` was not rerun).
- Blocker: none. Next: decode the per-chunk payloads (`VERTEXFRAMES`/
  `BONEDEFORM`/`NODES`/`TRIANGLES`) using the now-pinned offsets — decompile the
  `CDF_Node` accessors + vertex/skinning decoders in `MCCDyn.dll`.
- Branch: `docs/rea-mos-loader-layout` (not pushed; `main` untouched).

## Wake 2 — 2026-10-15 21:20 (scheduled loop)
- **Unit:** GH #53 requirement 3.3 — dialogue-navigation flicker on diagonal.
- Observation: the selector latched `s_dialogueDirection` (so a held direction
  did not repeat) but `ChooseDialogueDirection` tie-breaks by `|x| > |y|`, so a
  stick resting on a diagonal (e.g. `0.62/0.60`) flips the dominant axis
  frame-to-frame and re-taps two different selections. That violates #53's
  "must not flicker at diagonal boundaries" criterion.
- Change:
  - `src/hook/dialogue_navigation.h`: added `DialogueNavigationState`,
    `UpdateDialogueNavigation` (edge-triggered + axis hysteresis,
    `kDialogueAxisSwitchMargin = 1.20f` ≈ 6° guard band), `ResetDialogueNavigation`.
  - `src/hook/camera_hook.cpp`: dialogue block now gates on menu/dialogue
    context, recentres below `deadzone * 0.55f`, and fires only the
    edge-triggered output (preserves the `verify_reported_fixes.py` invariant).
  - `tests/dialogue_navigation_tests.cpp`: added held-repeat suppression,
    diagonal no-flicker, dominance-margin boundary, recentre re-arm, and
    sub-deadzone cases.
  - `docs/adr/0410-…md`: documented the revision under "Update — GH #53".
- Evidence: `build\x64` and `build\x86` Release builds succeed (x86
  `opengl32.dll` + x64 `riddickvr-host.exe`); CTest 3/3 passed on **both** x64
  and x86; `tools/verify_reported_fixes.py` passed;
  `tools/scenario_tests.py --check` passed.
- **Coordination incident:** while this wake's edits were in progress, the
  interactive session's commit `8d36d85` (`git add -A`) swept my
  `dialogue_navigation.h` / `camera_hook.cpp` / test edits into `main` alongside
  its `.xmd` parser work. I did **not** rewrite `main` (forbidden), so the code
  half of this unit already lives on `main`; this branch carries the ADR + state
  record for it. Future wakes: stage explicitly (`git add <paths>`) to avoid the
  same sweep.
- Next: #53 3.1 (resolve the left-Y Eyeshine binding conflict explicitly) or
  3.2 (collider/target-based contextual interaction + documented proximity
  fallback); otherwise, per the updated coordination note, #55 (decode MOS
  geometry/skinning → real hand mesh).

> **COORDINATION (updated):** the `.xmd` parser (`tools/mos_xmd.py`) and ADR-0413
> are committed. Ghidra 12.1.4 is installed at
> `C:\Users\GigelCastel\Tools\ghidra_12.1.4_PUBLIC` and `GHIDRA_INSTALL_DIR` is
> set, so `rea` can now decompile. Issues #55 (decode MOS geometry/skinning →
> real hand mesh), #56 (pin target build), #57 (VR portability epic) are open.
> Loop should now prefer #55 and the #57 gaps; see below.

## Wake 2 — 2026-10-15 (interactive, deep RE)
- Ghidra analysis of the model pipeline via REA; committed
  `docs/rea/mos-xmd-format.md` + `docs/rea/reference/*.txt` (pseudocode).
- Container = generic **`CDataFile`** in `MCCDyn.dll`
  (`Create` xref `0x10082a74`, `ReadTree` xref `0x10082f15`); confirmed the
  `tools/mos_xmd.py` layout (0x30 header + 0x30 node records).
- Geometry reader = **`CTriangleMeshCore::Read`** in `MXR.dll`
  (`VERTEXFRAMES`/`TVERTEXFRAMES`/`BONEDEFORM` via `CDataFile::GetNext`).
- Next: finish `VERTEXFRAMES`/`BONEDEFORM` byte layouts, recover bone names,
  export OBJ, add skinned draw path. See GH #55.

## Wake 1 — 2026-10-15 (interactive, setup + RE)
- Provisioned **Ghidra 12.1.4** (`C:\Users\GigelCastel\Tools\ghidra_12.1.4_PUBLIC`,
  `GHIDRA_INSTALL_DIR` set). `rea doctor` now reports ghidra healthy → real
  decompilation is available to `rea` (CLI) and future wakes.
- Committed `tools/mos_xmd.py` — read-only `MOS DATAFILE2.0` `.xmd` parser,
  validated on Riddick/weapon models (node tree + chunk offsets/sizes +
  triangle-count cross-check).
- Wrote `docs/adr/0413-native-first-person-hand-model-reuse.md`.
- Opened issues: **#55** (decode MOS geometry/skinning → real hand mesh),
  **#56** (pin target build: EFBB 2004 vs AoDA 2009), **#57** (VR portability
  epic). Commented the plan on **#46**.
- Next (loop): start **#55** — use `rea`/Ghidra to xref the `MOS DATAFILE2.0`
  loader in `MXR.dll`/`MSystem.dll`, recover `VERTEXFRAMES`/`BONEDEFORM`/`NODES`
  layouts, then extract the hand/forearm mesh.

## Wake 0 — 2026-10-15 (setup)
- Created the autoloop harness (`tools/autoloop/`): standing prompt, runner
  script, scheduled task `RiddickVR-Autoloop` every 10 min for 24h.
- Investigation so far (from the interactive session):
  - Repo `main` clean at `25f90b3`. Open issues: #34, #40, #43-#54.
  - Renderer is a **monoscopic capture+blit** (`stereo_renderer.cpp`) that draws
    the procedural `VRHandRenderer` overlay into each eye FBO. Hands are NOT
    native (ADR-0406/0407 acknowledge this).
  - `src/hook` intercepts only `wgl*`, `glViewport`, `glMatrixMode`,
    `glLoadMatrixf`, `glFrustum`, `glOrtho`, `glClear`, `glBindFramebuffer`,
    and `GDI32!SwapBuffers` (IAT). `glUniformMatrix4fv` is NOT hooked despite
    README claim. Matrix concatenation paths (`glPush/Pop/Translate/Rotate`,
    `glMultMatrixf`) are pure forwarders => invisible.
  - `CameraHook::StabilizeViewMatrix` (camera_hook.cpp:228) and
    `OpenGLProxy::GetLastViewMatrix` are dead/unwired.
  - Game install found: `C:\Program Files (x86)\Starbreeze Studios\Riddick EFBB`
    = **2004 original** (`Riddick.exe`, `SbzEngine.exe`), NOT the 2009 AoDA
    (`DarkAthena.exe`) the `config/addresses.json` recon targeted. GOG AoDA
    installer present but unextracted in Downloads.
  - `.xmd` = `MOS DATAFILE2.0` container with `SKELETON`/`NODES`/`BONEDEFORM`/
    `TRIANGLES`/`PRIMITIVES`/`VERTEXFRAMES` chunks and texture refs
    (e.g. `RIDDICK_ARMSCAR_D`). Real geometry + skinning appear extractable.
  - REA 6.5.0 CLI present; no Ghidra/IDA/Hopper configured (static PE/format
    analysis only for now).
- Next steps: build a read-only `.xmd` (MOS DATAFILE2.0) parser to extract the
  Riddick arm/hand mesh + skeleton; identify the correct first-person arm model;
  produce an ADR + tool + test; then design the native-hand render path behind a
  config flag with the procedural renderer retained as fallback.
