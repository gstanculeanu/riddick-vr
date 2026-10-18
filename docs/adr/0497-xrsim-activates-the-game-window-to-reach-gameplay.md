# ADR-0497: XRSIM reaches live gameplay only when the game window is activated — the posted-confirm route is focus-gated, and the sim input bridge is never polled (GH #71 / #80)

- Status: Accepted (scheduled autoloop wake 96, GH #71/#80)
- Date: 2026-10-18
- Related: GH #71 (reach live gameplay), GH #80 (simulator-only input bridge),
  GH #68 (iteration vehicle), GH #67 (the state question this unblocks),
  ADR-0466 (+ wake-64 addendum: "posted window messages are dead"),
  ADR-0467/0468 (the sim input bridge), ADR-0459 (display branch)

## Context

Two facts framed this wake:

1. **ADR-0466's addendum measured the posted-window-message route as dead.** In a
   `RIDDICKVR_SIM=1` run, `PostMessage(WM_KEYDOWN/WM_CHAR/WM_KEYUP)` to the game
   window left the run on `PRESS ENTER`; the conclusion was that the engine reads
   DirectInput/Raw Input and not the message queue.
2. **GH #80 implemented the in-process bridge for that reason** (`sim_input_bridge.h`
   / `sim_input_bridge_win32.cpp`): intercept `GetProcAddress` in `MSystem.dll`,
   wrap `XInputGetState` and `DirectInputCreateA` → `CreateDevice` →
   `GetDeviceState`, and fabricate the confirm **only** under
   `RIDDICKVR_SIM=1` + `RIDDICKVR_SIM_CONFIRM`.

Before this wake the harness had **never captured a live 3D gameplay frame**: every
"gameplay" capture in the committed artifacts is the startup splash mislabelled by
the display branch (GH #72). The live report carried
`live_gameplay.gameplay=33..83` for the splash. The input bridge's own log had
never been read next to a live run.

## Decision

**The harness must activate the game's own top-level window** (window management
only: `SetForegroundWindow` under an `AttachThreadInput` handshake — *no* keyboard,
mouse or OS input is synthesized), and it must record the observed result in the
report as `window_activation`. Activation is attempted for
`WINDOW_ACTIVATION_TIMEOUT_S = 15 s` (0.5 s cadence) starting at game launch.

This is the missing precondition for the route GH #71 already had. It is not an
input injection, so it does not cross the simulator's input-safety line.

### Measured (live `RIDDICKVR_SIM=1`, read-only per-run shadow, current build)

Both runs exercised the **current** build:
`opengl32.dll sha256 a8897b5d5d9b…`, 500224 B, `deployed_hook_matches_build` PASS,
`--require-fresh-hook`, `--shadow-install "F:\…\Assault on Dark Athena"`
(`F:` untouched; junctions + a copied program dir), `--dump-branch any`.

| run | route | `window_activation` | menu-advance taps | capture |
|---|---|---|---|---|
| **A (control)** `--sim-confirm both` | bridge only | found/foreground after **3** attempts | 0 (not enabled) | **stayed on the `PRESS ENTER` splash**; last frame = title page |
| **B** `--advance-menu --advance-min-taps 8 --sim-confirm both` | posted confirm **+** activation | found/foreground after **3** attempts | **8** taps, 1 window reached | **reached live 3D gameplay**: 62 gameplay / 8 cinema / 0 unknown, **62 distinct**, last frame = an in-level corridor |

- Run A's bridge summary: `polls=0 taps=0 keyboardPolls=0 keyboardTaps=0
  createDevicePatched=0 keyboardWrapped=0`, with
  `procs[XInputGetState:owned,DirectInputCreateA:owned]` — the engine resolved
  **both** entry points through the hook's swapped `GetProcAddress`
  (`inputProcRequests=2`, `getProcCalls=90`) **and then never called either one**,
  on the splash.
- Run B is the same, **including after gameplay was reached**: the engine still
  never polled the wrapped `XInputGetState`/`DirectInputCreateA` device path, yet
  the game advanced and rendered a level. So the confirm that worked was the
  **posted message**, not the bridge.
- `window_activation` (run B):
  `{attempted: true, hwnd: 5112260, title: "The Chronicles of Riddick: Assault on
  Dark Athena", 426x201, was_foreground: true, foreground: true, found: true,
  attempts: 3}`.

## Consequences

- **ADR-0466's addendum is corrected (conditionally).** Posted window messages are
  *not* dead: they are dead while the game window is not active. In run A the
  window was never activated and the splash stayed; in run B activation plus the
  8 posted taps left the splash and reached a level. The message route is
  focus-gated, which is why the earlier measurement (and every earlier wake's
  version of it) read it as dead.
- **The sim input bridge's premise is not supported by this target.** The engine
  resolves `DirectInputCreateA` and `XInputGetState` through the hook and then does
  not poll either, even when the window is foreground and a level is running.
  The bridge stays as it is (opt-in, inert without both tokens) but it is **not**
  the route to gameplay; future wakes must not treat `--sim-confirm` as the reach
  mechanism.
- **The iteration vehicle now reaches live gameplay.** `python tools/xrsim.py
  --shadow-install <F:> --reach-gameplay --duration 75 --advance-menu
  --advance-min-taps 8 --require-fresh-hook --dump-branch any` produces live,
  advancing 3D captures on the current build. GH #71 requirement 1 and GH #68's
  "≥2 distinct live gameplay frames" are now met with measured evidence.
- **GH #67's state question is unblocked in principle**: a run can now reach a
  level, so the existing `ViewmodelObserve=1` probe can be sampled *during
  gameplay* instead of on the startup page (next step), and the same posted-message
  route is the first candidate for driving further input (movement / menu) toward
  an **armed** state.

## Limits (honest)

- One paired run (A control, B treatment); the difference is the posted taps, and
  A/B correctness rests on ADR-0466's repeated prior negative for the posted route
  **without** activation.
- The reached scene is whichever level the engine loads after the splash; it is
  **not** verified to be an armed/fighting state, so the GH #67 state question is
  not answered here.
- The bridge poll count is read from the hook log of these two runs only; a pad
  connected at launch could change whether XInput is polled. No conclusion is
  claimed about the 2004 build.
- `426x201` is the *client* rect the harness read; the render resolution is the
  OpenXR eye size. Nothing about the window size is claimed beyond window identity.
- No headset; capture validity is limited to what the mock OpenXR host records.
