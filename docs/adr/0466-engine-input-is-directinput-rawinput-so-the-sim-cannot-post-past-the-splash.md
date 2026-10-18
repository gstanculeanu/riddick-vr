# ADR-0466: The engine reads input via DirectInput / Raw Input, so the simulator cannot post past the startup splash

> **CORRECTED (autoloop wake 84).** The *measurements* below stand (MSystem.dll
> resolves DirectInput/XInput and never polls them; RndrGL.dll registers Raw Input;
> `GetAsyncKeyState`/`GetKeyboardState` are imported by nothing). The **conclusion
> drawn from them -- "a posted window message cannot drive this engine" -- is
> false**, because the taps this ADR posted carried `lParam = 1`, i.e. scan code
> **0**: the engine's own window procedure resolves a key from `lParam >> 16` and
> forwarded key code 0, an unmapped key. With the real scan code
> (`MapVirtualKeyA(VK_RETURN, MAPVK_VK_TO_VSC) = 0x1C`) the same targeted
> `PostMessage` **does** advance the front-end into a live level: a replicated
> A/A/B/B (2 runs with `--advance-menu`, 2 without) is recorded in
> **ADR-0485** and `docs/rea/reference/RndrGL_Input_Path.txt`. The `WM_INPUT` /
> Raw Input surface this ADR points at is **mouse-only** (one `RAWINPUTDEVICE`,
> `usUsagePage = 1` / `usUsage = 2`, `RIDEV_INPUTSINK`), so it could never have
> carried a keyboard confirm either way.

- Status: Accepted for its measurements; conclusion superseded by ADR-0485
- Date: 2026-10-17
- Related: GH #71, GH #68 (simulator harness), GH #72 (splash is mislabelled), ADR-0447 (gameplay-only gate), `src/hook/gesture_tap.cpp`, **corrected by ADR-0485**

## Context

The XRSIM simulator harness (#68) is the loop's only headset-free way to
reproduce and validate player-visible fixes. `--reach-gameplay` (#71 requirement
1) is supposed to drive a scripted run to a live, advancing 3D scene. It does
not: every recent run stalls on the game's startup splash/menu and captures
nothing (`frames_captured` 0, `scene_advanced` ungradable).

Wake 62 diagnosed the stall as "in sim mode no input can reach the game" and
proposed the fix as a hook change: let `camera_hook.cpp`'s `SendKey` post its
window message before the `SimModeNoInput()` return, and wire
`SetGestureGameHwnd` so `gesture_tap.cpp`'s posting path has an HWND. That
diagnosis is directionally right but **the proposed mechanism cannot work**.

The intended sim-safe mechanism is a *targeted window message*. `gesture_tap.cpp`
already documents it -- "STILL post the targeted window message to the game so
the harness can advance menus without hijacking the user's desktop"
(`src/hook/gesture_tap.cpp:27-29`) -- and `camera_hook.cpp`'s `SendKey` posts
`WM_KEYDOWN`/`WM_CHAR`/`WM_KEYUP` to the latched game HWND. A targeted
`PostMessage` only touches that window's queue, so it is not desktop input
injection and stays inside the #68 simulator exception.

## Decision

Carry the sim-safe attempt in the **harness** (it knows the game PID, so it does
not depend on the hook learning the HWND), keep it **opt-in and off by default**,
and record the measured finding that it is ineffective against this engine.

`tools/xrsim.py` gains:

- `confirm_key_messages()` / `post_confirm_to_game_windows(pid)` -- the exact
  `WM_KEYDOWN`/`WM_CHAR`/`WM_KEYUP` (VK_RETURN) triple posted to the visible
  top-level window(s) owned by the game PID (ctypes, dependency-free, no-op off
  Windows);
- `GameMenuAdvancer` -- pure cadence/max-taps/stop schedule (injected poster and
  clock), driven from the orchestration loop (no thread, no catch-up burst);
- `display_branch_sequence()` / `gameplay_reached_from_sequence()` -- the stop
  signal: the startup splash is full-bleed-labelled (GH #72), so gameplay is a
  full-bleed branch that *follows* a cinema/menu branch, not a bare full-bleed;
- `--advance-menu` (default **off**), `--advance-interval`, `--advance-max-taps`;
  the outcome is recorded in the report as `menu_advance`.

## Evidence

Mock run `RIDDICKVR_SIM=1`, fresh `dist/riddickvr-host.exe`, `--advance-menu`:

- 45 s `--reach-gameplay` run: **21 taps posted, `windows_reached=1`,
  `reached=false`**; the hook log's only `DisplayMode` line stayed
  `CINEMA screen ... (menuCursor=1)`.
- 30 s run with a temporary all-windows (incl. children) probe: the game window
  was found and is `class='MOS Win32/OpenGL'`,
  `title='The Chronicles of Riddick: Assault on Dark Athena'`,
  **visible and already foreground** (`fg=True`). 14 taps to it did not advance
  the splash either.
- Captured frame (`--dump-branch any`) renders the literal **"PRESS ENTER"**
  splash (Atari/Starbreeze logo plate) at 2496x2688, unchanged across the run.
- Binary import scan (ASCII import names):
  `MSystem.dll` -> `DirectInput`, `XInput`; `RndrGL.dll` ->
  `RegisterRawInputDevices`, `GetRawInputData` (and `PeekMessage`);
  no `GetAsyncKeyState`/`GetKeyboardState` in any module.
- The deployed hook confirms it is in sim mode:
  `XRSIM: RIDDICKVR_SIM=1 - desktop input injection disabled`.

## Consequences

- **A posted window message cannot drive this engine.** Both DirectInput and Raw
  Input are device-sourced; they are fed only by OS-level `SendInput`, which is
  exactly what the simulator suppresses (input safety). Posting to the *right*
  window, while foreground, changes nothing.
- Therefore **GH #71 requirement 1 is not achievable offline under the hard
  rules.** The loop must not "fix" it by posting messages (wake 62's proposed
  hook change), and must not reach gameplay by generating OS input.
- Viable routes (all out of scope for this wake): (a) a hook-side, simulator-only
  bridge of the engine's input -- hook `GetRawInputData` /
  `IDirectInputDevice8::GetDeviceState` and inject the scripted confirm state
  (deploy-gated, REA work); (b) launch directly into a level/savegame (no CLI
  switch exists -- the `-map` matches are `mip-map`/`environment-mapping` -- and
  no save exists); (c) permit the harness OS input (rejected: violates input
  safety).
- `--advance-menu` stays landed as the harness half of route (a): when the hook
  bridges the engine's input, enabling the flag validates it immediately. Until
  then it is off, so a default run posts nothing.
- The reach failure is now honestly attributed (splash branch + a disabled,
  documented attempt) rather than silently producing zero frames.

## Addendum (autoloop wake 64) — the console/command-line route is also dead

Wake 63 recorded "no savegame on the box" and left route (b) as "no CLI switch
and no save". A read-only sweep of the *sim* install (F:) corrects both:

- **Savegames do exist**, at
  `%APPDATA%/Starbreeze/Riddick/Content/Save/<profile>/` (`01.Escape`,
  `Checkpoint`, `_filler000`, `_profile`; profile `Gigel`, `GAME_LASTVALIDPROFILE`
  in `Environment.cfg`). So route (b) is not void for lack of a save.
- **The engine's console surface is real.** `MSystem.dll` registers the id-style
  console (`exec`/`echo`/`history`/`commands`/`bind*`); the game logic lives in
  `Content/GameClasses_Win32_x86.dll` and `Content/GameWorld_Win32_x86.dll`,
  which name `loadgame`, `savegame`, `checkpoint`, `changemap`,
  `cg_newgame`/`cg_goto_newgame`, `cg_loadlastvalidprofile`, and the message
  `"Have to select a valid profile before loading a map"`.
- **But the command line does not execute them.** Three input-free attempts
  (harness `--game-arg`, all bounded by wall-clock) each left the run on the
  `PRESS ENTER` splash for the full budget:
  1. `+loadgame Checkpoint` (60 s) — the engine's own console command;
  2. `+quit` (25 s) — a `+command` that would exit at once *if* the engine ran
     console commands from the command line;
  3. `-env <copy of Environment.cfg + COMMANDLINE=+quit> -nowriteenv` (20 s) —
     `MSystem.dll` documents `"CommandLine is overrided by value in environment
     file."`, so the override path was tested too.

  A posted command line therefore cannot drive this engine. Route (b)
  survives only as an *in-process* call (the hook executing the console command),
  which is route (a) with a different injection point.

Consequence: the harness keeps `--game-arg` (repeatable, pure `game_command`
helper, recorded as `game_args`) as the exact handle a hook-side bridge would use,
and the input bridge (hook `DirectInputCreateA`/`GetDeviceState`/`XInputGetState`
— see GH #80) remains the only path to an input-free reach. Note the input DLLs
are loaded at runtime (`dinput.dll` + `DirectInputCreateA`, `xinput1_3.dll` +
`XInputGetState`, both via `LoadLibrary`/`GetProcAddress`), so the bridge must
intercept the resolved proc, not an IAT import.

## Correction (autoloop wake 84) — the posted message was malformed, and the message path works

Read-only REA of the shipped renderer (`docs/rea/reference/RndrGL_Input_Path.txt`,
`RndrGL.dll` sha256 `b07baac4…`, image base `0x10000000`) settles what this ADR
inferred:

- `RndrGL.dll` owns the game window and the pump: `RegisterClassA`/
  `CreateWindowExA`, `GetMessageA(NULL, 0, 0, &msg)` → `TranslateMessage` →
  `DispatchMessageA`, and a 7-entry switch for `WM_INPUT`, `WM_KEYDOWN`,
  `WM_KEYUP`, `WM_CHAR`, `WM_DEADCHAR`, `WM_SYSKEYDOWN`, `WM_SYSKEYUP`. Posted
  messages **are** dispatched.
- Its key handler (`0x10042817`/`0x100428aa`) takes the key from
  `(lParam >> 16) & 0x1ff` and falls back to `MapVirtualKeyA(vk, 0)` only when
  that field is the `0x100` sentinel; `+0x100` marks an extended key. So
  `lParam = 1` (this ADR's taps) forwarded key code **0** — an unmapped key.
- Raw Input is **mouse-only**: one `RAWINPUTDEVICE`
  (`usUsagePage = 1`, `usUsage = 2`, `dwFlags = RIDEV_INPUTSINK`,
  `hwndTarget` = the game window) and a `WM_INPUT` handler that returns unless
  `dwType == RIM_TYPEMOUSE`. There is no raw-input keyboard device.

Harness change: `confirm_key_messages()` packs the real scan code
(`vk_to_scancode`, OS `MapVirtualKeyW` with a pure fallback table), and
`menu_advance` records the packed `vk`/`scan`. It stays **opt-in**
(`--advance-menu`, default off).

**Replicated A/A/B/B** (`RIDDICKVR_SIM=1`, read-only shadow, `--require-fresh-hook`,
hook `sha256=e82a2dee526e` = the repo build, no deploy):

| run | taps | first non-card page | page for the rest of the run |
|-----|------|---------------------|------------------------------|
| A (60 s) | 6 | frame **3300** (loading bar, mean 47.9) | dark level, mean 8.3/9.4 |
| A2 (45 s) | 6 | frame **3300** | dark level, mean 8.3/9.4 |
| B (60 s) | 0 | — | the title card, mean **86 ↔ 100** (identical to ADR-0484's measurement), frames 4500–17700 |
| B2 (45 s) | 0 | — | the title card, mean 86 ↔ 100, frames 4500–13200 |

Both tap runs left the start card at the ~5th-6th tap (≈11–14 s) through the
`LOADING` page into a dark level page (brightened, it shows interior geometry, a
lit hatch and the HUD blocks); both control runs sat on the pulsing title card for
the entire run. Every run passed `scene_advanced` and `gameplay_full_bleed_reached`,
so the *branch* signal still cannot tell the card from a level (GH #72) — the page
*content* does, which is why the earlier negatives and ADR-0484's "the page is the
start screen" reading were both session-specific: a run that never taps stays on
the card.

**What this changes:** GH #71 requirement 1's acceptance (≥2 distinct live
gameplay frames, no OS input) is reachable — with `--advance-menu`, which now
carries a well-formed key message. The Raw Input / DirectInput device routes
remain as measured (unpolled), so this ADR's route (a) is no longer needed for
the keyboard.

