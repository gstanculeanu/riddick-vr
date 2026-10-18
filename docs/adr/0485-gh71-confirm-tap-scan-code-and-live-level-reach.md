# ADR-0485 — GH #71 req 1: the confirm tap was malformed (scan code 0), and the corrected tap reaches a live level

- Status: accepted
- Date: 2026-10-18
- Refs: GH #71 (this — requirement 1), GH #80 (the input bridge this unblocks /
  whose Raw Input candidate it closes), ADR-0466 (+ its wake-64 addendum — the
  ADR this corrects), ADR-0472 (the bridge's observability), ADR-0448 (the
  wall-clock reach), ADR-0484 (the "the page is the start screen" reading this
  qualifies), GH #72 (the splash is branch-mislabelled), GH #68 (the iteration
  vehicle), `src/hook/gesture_tap.cpp`

## Context

XRSIM (#68) is the loop's only headset-free validation vehicle. Its
`--reach-gameplay` runs kept stalling on the game's front-end page, and the
record attributed that to the engine *not consuming posted window messages*
(ADR-0466: "MSystem.dll imports DirectInput, RndrGL.dll imports Raw Input, so
posted messages are ignored"), which made reaching gameplay a hook-side bridge
problem (GH #80) and blocked every deploy-gated player-visible validation.

Wake 84 re-read that claim against the shipped renderer instead of against the
import list, and found the message the harness posted:

```
tools/xrsim.py (pre-wake-84):  key_lparam(scan=0, is_down) -> lParam = 1
```

`lParam = 1` packs **scan code 0**. The engine's own window procedure resolves a
key from that field.

## Evidence (read-only REA; full record in `docs/rea/reference/RndrGL_Input_Path.txt`)

`RndrGL.dll` (`System\Win32_x86\RndrGL.dll`, 939248 B, sha256
`b07baac43ce4bbf6e0a73acdd31ae34e8bac76a6a4c9ca15bdd5c0de8898ead0`, image base
`0x10000000`) is the module that registers the game's window class and runs its
message pump.

1. **The pump dispatches posted messages.** `0x1001c407`:
   `GetMessageA(NULL, 0, 0, &msg)` → (non-`WM_QUIT`/`WM_USER`) →
   `TranslateMessage` → `DispatchMessageA` (`0x1001c50d`-`0x1001c51d`).
2. **The window proc has a key path.** Its switch (`0x10042698`, table
   `0x10042f7c`) covers `WM_INPUT` (0x00FF), `WM_KEYDOWN`, `WM_KEYUP`,
   `WM_CHAR`, `WM_DEADCHAR`, `WM_SYSKEYDOWN`, `WM_SYSKEYUP`.
3. **The key is decoded from `lParam`, not `wParam`.** `WM_KEYDOWN` handler
   `0x10042817`: `esi = (lParam >> 16) & 0x1ff`; `MapVirtualKeyA(vk, 0)` is
   computed but used **only** when `esi == 0x100` (the "no scan code in lParam"
   sentinel); `+0x100` marks an extended key; the result is passed to the input
   context's vtable slot `0xc4`. `WM_CHAR` (`0x100427c9`) forwards `wParam` to
   the same slot. Both are gated on bits of the system-context flags at
   `+0x6d4` (bit0 = key events, bit1 = char events).
4. **Raw Input is mouse-only.** `RegisterRawInputDevices` is called once
   (`0x10040b5a`) with a single `RAWINPUTDEVICE` (`usUsagePage = 1`,
   `usUsage = 2` = mouse, `dwFlags = 0x100` = `RIDEV_INPUTSINK`,
   `hwndTarget` = the game window), and the `WM_INPUT` handler
   (`0x100426be` → `GetRawInputData`, `0x1004270c`) returns unless
   `dwType == RIM_TYPEMOUSE`. So GH #80's "fabricate the confirm on the Raw Input
   path" candidate cannot carry a *keyboard* event.
5. The same USER32 entries are also reachable through an internal
   `jmp [IAT]` stub table (`0x100a7a5c`-`0x100a7b16`), so an IAT hook on them
   covers direct calls and stored pointers alike.

## Decision

Fix the **message**, not the bridge: the harness packs the key's real scan code,
exactly as the hook's own gesture layer already does
(`gesture_tap.cpp: MapVirtualKeyA(vk, MAPVK_VK_TO_VSC) << 16`).

- `tools/xrsim.py` gains `vk_to_scancode(vk)` (OS `MapVirtualKeyW(vk, 0)` with a
  pure `KEY_SCANCODE_FALLBACK` table so the encoding is testable off Windows) and
  `KEY_LPARAM_NO_SCANCODE = 0x100` (the sentinel the engine itself treats as "ask
  `MapVirtualKeyA`" — never to be sent as a scan code).
- `confirm_key_messages(vk=VK_RETURN, scan=None)` now defaults to the real scan
  code and still accepts an explicit `scan` (so the old malformed tap remains
  expressible in a test).
- `menu_advance` records the packed `vk` and `scan`; `--advance-menu` stays
  **opt-in / off by default** (it is a front-end driver, not something a normal
  run should do).

## Reading the result (replicated A/A/B/B)

`RIDDICKVR_SIM=1`, read-only per-run shadow, `--require-fresh-hook`, hook
`sha256=e82a2dee526e` (the repo build), no deploy:

| run | `--advance-menu` | frames dumped | page sequence |
|-----|------------------|---------------|---------------|
| A (60 s) | yes (6 taps) | 46, frames 2400-15900 | title card (2400-3000) → `LOADING` (3300-3600) → **dark level** (4200+) |
| A2 (45 s) | yes (6 taps) | 33, frames 2400-12000 | title card → `LOADING` (3300) → **dark level** (3600+) |
| B (60 s) | no | 45, frames 4500-17700 | the pulsing title card, mean **86 ↔ 100**, for the whole run |
| B2 (45 s) | no | 30, frames 4500-13200 | the pulsing title card for the whole run |

- The tap runs leave the front-end card at ~the 5th-6th tap (≈11-14 s of a
  ~250-300 fps run) and hold a dark page; brightened, that page is interior
  geometry with a lit hatch and the HUD blocks, i.e. a live scene, not a plate.
- The control runs never leave the card (mean 86 ↔ 100, **std 46-49**, exactly
  ADR-0484's title-card measurement) for 45-60 s.
- Every run passes `scene_advanced` (all dumped frames distinct — the card's fade
  is enough) and `gameplay_full_bleed_reached` (fill 1.00), so the *branch* signal
  cannot tell the card from a level (GH #72). The page **content** does, which is
  why the earlier "the engine ignores posted messages" negatives and ADR-0484's
  "the gameplay page is the start screen" reading were both properties of runs
  that never tapped.
- The A runs also pass `horizontal_disparity_present` (`dx = -48`), `dy = 0`,
  `capture_gate_matches_saved` (46/46, 0 mismatches) and
  `deployed_hook_matches_build`, so the reach is a normal, attributable run.

## Consequences

- **GH #71 requirement 1 is reachable** under the hard rules: a scripted run
  reaches a live level with no OS input, by posting a well-formed key message to
  the game's own window. The deploy-gated ladder (#67 step 3 `ViewmodelObserve=1`,
  #76 step 1, #79's hook half, #68 req 5) can now be validated on a real scene.
- The `--advance-menu` docstring, the CLI help and ADR-0466's conclusion are
  corrected in place; the ADR-0466 **measurements** (DirectInput/XInput resolved
  but unpolled, no `GetAsyncKeyState`, Raw Input present) stand.
- GH #80's remaining keyboard route (a hook-side bridge of the engine's device
  reads) is **not needed for the confirm**, and its Raw Input sub-route is closed
  (mouse-only). The bridge's landed, tested core stays as a documented inert
  fallback.
- The harness's gameplay branch still mislabels the start card (GH #72), so a
  *non-tapping* run can still grade the card as gameplay: reach remains opt-in,
  and page-content discrimination is the open follow-up.

## Honest limits

- The scan-code rule is read from static disassembly; the engine's *own* use of
  the value (vtable slot `0xc4`) is not decompiled. The behavioural A/A/B/B is the
  evidence, not the disassembly alone.
- Four runs is a small sample; the tap/level sequence is consistent across both
  tap runs and the card-only outcome across both controls, but the reach could in
  principle be shared with an unmeasured auto-advance path (ADR-0448 once measured
  an auto-advance to gameplay at ~5400 frames without any tap). Nothing here
  claims a *cause* deeper than "the corrected tap is followed by the advance in
  2/2 runs, and absent in 2/2 controls".
- The reached page is a live scene but its *state* is not established (the run
  drives no movement — the mock controller state never reaches the game's own
  input); the 46 frames differ only by the page's light flicker. This is "reached
  a live level", not "played".
- Only `VK_RETURN` is exercised; other keys/scancodes (and extended keys) are
  encoded but untested live.

## Validation

- Offline: `tools/test_xrsim.py` `test_confirm_key_messages_are_wellformed`
  (rewritten: pins `scan == 0x1C`, the `0x100` sentinel exclusion, and that the
  explicit-scan and pre-wake-84 forms remain expressible) +
  `test_confirm_scancode_fallback_is_pure`; `xrsim --selftest` +
  `confirm_tap_packs_scancode`; the GH #71 invariant in
  `tools/verify_reported_fixes.py` requires the scan-code packing, the fallback
  table, this ADR and the REA reference.
- Live: the A/A/B/B table above (recorded in `tools/autoloop/STATE.md`; the
  harness's own report keys `menu_advance.{vk,scan}` carry the packed values).
