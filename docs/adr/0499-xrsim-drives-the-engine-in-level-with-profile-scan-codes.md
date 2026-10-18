# ADR-0499 — XRSIM posts the profile's own scan codes to the game: the front-end accepts them, the in-level engine did not (GH #67 state question)

- Status: accepted
- Date: 2026-10-18
- Refs: GH #67 (state question, this), GH #80 (sim-only input), GH #68 (iteration
  vehicle), GH #71 (reach live gameplay), ADR-0496 (every engine-side route of
  #67 step 1 exhausted), ADR-0497 (the wake-96 live reach), ADR-0485
  (confirm-tap scan code), `docs/rea/reference/RndrGL_Input_Path.txt`

## Context

ADR-0496 closed the last *engine-side* route of GH #67 step 1: the engine-side
identification of the first-person viewmodel is exhausted (geometric
ADR-0481..0491, name tables ADR-0492/0495, `VIEWMODEL` ADR-0493/0494, the
destination `vtable[0x68]` ADR-0496). What is left is the **state question**:

> does the engine draw its own first-person arms *at all* in the state a
> scripted run observes?

The engine draws its arms when Riddick is **armed / fighting**; the level the
run reaches (ADR-0497: activate the game window, then post the confirm taps) is
a passive, unarmed start, so the `ViewmodelObserve` probe sees no arms and the
question cannot be answered. Wake 97 recorded this as the top next step and
noted two routes it had measured dead: the engine's **console** (no console key
name in `MSystem.dll`'s key table) and `--game-arg +loadgame` (the engine does
not run `+`-console commands from its command line).

## What was missing

The harness could only post the **confirm** key. It could not make the engine
*act* in-level. Two measured facts supply the missing route:

1. **The engine keys on the message's scan code.** `RndrGL.dll`'s window proc
   (`0x10042817`) resolves a key from `lParam >> 16 & 0x1ff`, not from `wParam`
   (`docs/rea/reference/RndrGL_Input_Path.txt` §4). The confirm tap already
   packs a real scan code (ADR-0485), and posting to the game's own window is
   targeted messaging, never desktop input.
2. **The player profile maps scan codes to actions.** The profile
   (`%APPDATA%\Starbreeze\Riddick\Content\Save\<profile>\_profile`, read-only)
   contains a plain `*ACTION_<NAME> <scancode>` list — e.g. `*ACTION_FORWARD 17`
   (W), `*ACTION_USE 18` (E), `*ACTION_JUMP 57` (Space), `*ACTION_CYCLEWEAPON 47`
   (V), `*ACTION_WEAPON_SHIV 3` ('2'), `*ACTION_WEAPON_CLUB 4`, `*ACTION_WEAPON_PISTOL 5`,
   … and `*ACTION_PRIMARYFIRE 240` (mouse). So a weapon can be **selected** and
   fired by posting the profile's own scan codes.

Together these make an **in-level key drive** possible with no hook change and
no desktop input: the harness posts `WM_KEYDOWN`/`WM_KEYUP` carrying the
profile's scan codes to the game's window once gameplay is reached.

## Decision

Add an opt-in, simulator-safe in-level key drive to XRSIM.

- `post_key_to_game_windows(pid, messages)` — the general form of the confirm
  poster; `post_confirm_to_game_windows` now delegates to it.
- `scan_key_message(scan, is_down)` / `post_scan_to_game_windows(pid, scan,
  is_down)` — one scan-code key edge (the engine reads the scan code from
  `lParam`; `wParam` carries no virtual key here).
- `parse_action_profile(text)` → `{NAME: scan}` from the profile's
  `*ACTION_*` lines; `profile_action_path(explicit)` finds the default profile
  (first `_profile` under the AppData save dir).
- `parse_drive_keys(spec, actions)` — parse a comma-separated script of
  `SCAN`, `SCAN:HOLD_MS`, or an action **name** (resolved via the profile). A
  typo raises `ValueError` so a run fails loudly instead of posting the wrong
  key.
- `KeyScriptDriver` — a pure, thread-free schedule (the orchestration loop
  drives `poll`): each step posts its key-down, holds `hold_ms`, posts its
  key-up, with a `gap` between steps. At most one edge per `poll`.
- CLI: `--drive-keys SPEC`, `--drive-gap-ms`, `--drive-after`, `--action-profile`.
  The drive starts `--drive-after` seconds **past** the gameplay-reached stop
  (or from the run start when no `--advance-menu` is used), so action keys never
  fire into the splash/menu. The outcome is recorded in the report as
  `key_drive` (`enabled`, resolved `steps`, `posted`, `windows`, `done`,
  `profile`, and the resolved `actions` map for auditability).

It is **opt-in** (`--drive-keys`, default off) and posts only to the game's own
top-level windows — never OS-global input — so it is inside the #68 simulator
exception.

## Consequences

- XRSIM can now post an ordered, policy-free scan-code script to the game's own
  window once gameplay is reached. The capability itself is measured working
  (see the live measurement below): the messages are delivered.
- The **live measurement did not support** "posting the profile's scan codes
  drives in-level player action" on this target/state (below). So the state
  question is **not** answered by this route, and the plan to reach an armed
  state moves to an engine-side input injection (ADR-0412's exported
  `MSystem.dll` input API — `QueueScanKey`/`CInputContextCore::KeyPressed`) or to
  a run state where the player controller reads the queue.
- The resolved action map is read from the *user's* profile; a profile that
  rebinds an action changes the posted scan code, and the report records the map
  actually used.

## Live measurement (autoloop wake 98)

Two read-only per-run shadow runs, `RIDDICKVR_SIM=1`, current build
(`opengl32.dll` sha256 `a8897b5d5d9b` = repo build, `deployed_hook_matches_build`
PASS, `--require-fresh-hook`):

- **Run A** — `--reach-gameplay --duration 110 --advance-menu --advance-min-taps 8
  --drive-keys "WEAPON_PISTOL,PRIMARYFIRE,PRIMARYFIRE,WEAPON_SHOTGUN,PRIMARYFIRE,
  CYCLEWEAPON,PRIMARYFIRE,FORWARD:2500" --drive-after 10 --ini ViewmodelObserve=1`:
  reached live gameplay (`35 gameplay / 0 cinema`, `gameplay_full_bleed_reached`
  fill 1.00, `menu advance: stopped after 8 tap(s) -- gameplay branch reached`),
  the window was activated (`foreground=true`) and the **key drive completed**
  (`posted=16 windows=1 done=True`, profile resolved:
  `FORWARD=17 PRIMARYFIRE=240 WEAPON_PISTOL=5 WEAPON_SHOTGUN=6 CYCLEWEAPON=47`).
  Frames `2700`→`7500` show the front-end **main menu** (the cube-stack
  `Continue/New Game/Checkpoints/...`), then `6300`→`33300` the in-level dark
  corridor — i.e. the drive fired **in-level** (frames ≥ 9000 ≈ 30 s, after the
  menu stop at 8 taps). **No weapon, no HUD/ammo change, no camera locomotion**
  is visible across the post-drive frames; the same frame is unchanged for the
  remaining ~55 s.
- **Run B** (discriminator) — `--drive-keys "PAUSE"` (profile `PAUSE=1`, Escape)
  in-level: the drive completed (`posted=2 windows=1`) and **no pause menu
  appeared** in any of the four captured frames over the rest of the run.
- **The same route does work, but only in the front-end**: the confirm tap
  (`--advance-menu`, ADR-0485/ADR-0497) walks the splash → main menu in the same
  runs. So the negative is specifically **in-level**, not a delivery/focus
  failure.
- The `ViewmodelObserve` census is unchanged by the drive (planar candidate
  `count=12` / `count=2520`; no immediate-mode in-window geometry), so no
  first-person arms were observed.

**Verdict (honest):** "posting the profile's scan-code actions to the game
window drives in-level player actions" is **not supported** on this target in
this state. Two hypotheses remain, not distinguished here: (a) the in-level
player controller does not read the message queue (only the front-end does), or
(b) the reached checkpoint is a scripted/no-control state. The `--dump-branch`
labels here are full-bleed gameplay (no cinema/letterbox), which weakly favours
(a), but that is an inference, not a measurement.

## Honest limits

- The drive posts *key* messages. Action codes the profile binds to the **mouse**
  (`*ACTION_PRIMARYFIRE 240`) are delivered as a scancode `240` in `lParam`; the
  engine's action layer is expected to key on the code regardless of device, but
  the negative above makes that moot on this target.
- The live result does not tell us *why* the in-level engine ignored the posted
  codes, nor whether the reached level is one in which Riddick has any weapon in
  inventory (a property of the save).
- Harness-only. No C++, no hook/game change, no deploy, no install write. The
  game install stayed READ-ONLY (runs used a read-only per-run shadow); no
  game/host processes were left behind and the live lock was released.


## Validation (offline)

- `tools/xrsim.py --selftest` — `drive_keys_parse_profile` and
  `drive_keys_driver_orders_edges`.
- `tools/test_xrsim.py` — `test_drive_keys_parse_and_resolve_the_action_profile`
  and `test_key_drive_orders_down_hold_up_gap_without_a_thread`.
- `tools/verify_reported_fixes.py` — new GH #67/#80 source invariant.
- No C++ changed, so no `build-all.ps1`/CTest.
