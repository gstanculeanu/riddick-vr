# ADR-0466: The engine reads input via DirectInput / Raw Input, so the simulator cannot post past the startup splash

- Status: Accepted (scheduled autoloop wake 63, GH #71 requirement 1)
- Date: 2026-10-17
- Related: GH #71, GH #68 (simulator harness), GH #72 (splash is mislabelled), ADR-0447 (gameplay-only gate), `src/hook/gesture_tap.cpp`

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
