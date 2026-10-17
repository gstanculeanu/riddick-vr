# ADR-0468: How the sim input bridge intercepts the engine's device read path

- Status: Accepted (scheduled autoloop wake 66, GH #80 requirement 1)
- Date: 2026-10-17
- Related: GH #80 (sim-only input bridge), GH #71 req 1 (reach gameplay), GH #68
  (simulator harness), GH #27 (deferred hook init), ADR-0466 (+ its wake-64
  addendum), ADR-0467 (install point), `src/hook/sim_input_bridge.h`,
  `src/hook/sim_input_bridge_win32.cpp`, `src/hook/iat_hook.cpp`

## Context

ADR-0467 decided *where* the bridge must install — before
`CInputContext_DInput::CInputContext_DInput` (MSystem.dll VA `0x1010b440`)
resolves `XInputGetState` / `DirectInputCreateA` once — and landed the bridge's
pure core. The proposal it answered (`GH #80`) named the shape of the wrappers
(`GetProcAddress` intercept; `XInputGetState`; `DirectInputCreateA` →
`CreateDevice` → `GetDeviceState`) but not the mechanism. This ADR records the
mechanism the interceptor actually uses and the measurements behind each choice.

## Findings (read-only, shipped sim install `F:`, PE import table)

`System/Win32_x86/MSystem.dll` (5,113,072 bytes, 2012-03-05), parsed from its
import directory:

1. **MSystem.dll imports `KERNEL32.dll!GetProcAddress` and `!LoadLibraryA`**
   (145 KERNEL32 thunks). So the constructor's resolution goes through
   MSystem.dll's own IAT entry — a *single* pointer that can be swapped — rather
   than a dynamic lookup that only an inline prologue hook could reach.
2. **MSystem.dll does not import `dinput.dll` / `dinput8.dll` / `xinput1_3.dll`.**
   Its DLL set is `{WSOCK32, WINMM, KERNEL32, USER32, GDI32, SHELL32, MCCDyn,
   WS2_32, ole32, OLEAUT32}`. The input DLLs are loaded at runtime, which is why
   the intercept must be on `GetProcAddress` and not on an IAT import of the
   input DLLs (this confirms the wake-64 inference with the module list).
3. **`DarkAthena.exe` imports `MSystem.dll` but not `opengl32` / `RndrGL`**
   (13 DLLs, one thunk each). MSystem.dll is therefore mapped at process entry,
   before any hook code runs (already ADR-0467 finding 5).

## Decision

1. **Install the intercept from `DllMain(DLL_PROCESS_ATTACH)`, as one IAT swap**
   on `MSystem.dll`'s `KERNEL32.dll!GetProcAddress` entry, using the existing
   `IATHook::Hook` primitive. This is the earliest seam the hook legally has
   (ADR-0467), and it is loader-lock-safe by construction: an environment read, a
   `GetModuleHandle`, and one `VirtualProtect`+pointer write. No `LoadLibrary`,
   no file IO, no allocation, no logging (`VR_LOG_*` is a no-op before
   `Logger::Initialize`; the only attach-time breadcrumb is `OutputDebugStringA`).
2. **The intercept returns a wrapper for exactly the two names the input context
   asks for**, and resolves + stashes the real proc first, so the wrapper can
   forward. Everything else falls straight through to the real
   `GetProcAddress`. An ordinal (`HIWORD(name) == 0`) is never ours.
3. **The gamepad route wraps `XInputGetState` in place of the resolved pointer.**
   The wrapper calls the real function and ORs `START|A` into the state the real
   call reported, bumping `dwPacketNumber` when the state changed (a poller that
   skips an unchanged packet would never see the edge). It deliberately does
   **not** turn a disconnected pad into a connected one — `ERROR_DEVICE_NOT_CONNECTED`
   is preserved — so this route can only add a confirm to a machine that already
   has an XInput device. The pure core encodes the 12-byte `XINPUT_GAMEPAD` head
   so no `<xinput.h>` is needed.
4. **The keyboard route patches vtables in place.** `DirectInputCreateA` is
   wrapped; after the real create it patches the returned IDirectInputA's
   `CreateDevice` (**vtable slot 3**), and when the engine creates
   `GUID_SysKeyboard` it patches that device's `GetDeviceState` (**vtable slot
   9**) to OR `DIK_RETURN` (0x1C) into the engine's own 256-byte, one-byte-per-
   scan-code buffer. Patching the *live* vtable (rather than handing back a
   copied-vtable wrapper object) keeps the `this` pointer correct for every other
   method the engine calls; the device hook checks `self == keyboardDevice` so a
   mouse sharing the same vtable forwards untouched. It only clears the byte it
   set, so a real (non-sim) press is never stomped.
5. **The bridge reports its own install ordering instead of assuming it**
   (ADR-0467 residual). The startup report logs gate/route/installed and whether
   the engine has asked for the input procs; the per-request line
   (`took over XInputGetState/DirectInputCreateA`) appears the instant the ctor
   resolves them, so a captured run shows whether the swap beat the constructor.

## Consequences

- #80's interceptor is now implemented and consumes the pure core unchanged; the
  only new headless-tested surface is the env-token parsing (`SimRouteFromToken`,
  `SimConfirmScheduleFromTokens`) and the two vtable-slot constants, which are the
  ABI the in-place patches depend on.
- **Honest limit (unchanged, and loud):** the interceptor cannot be live-validated
  by this loop — the sim install runs a stale hook build and deploying is a hard
  rule. What is verified offline is: it compiles into the proxy, changes no
  artifact while the gate is off, and its pure core (including the two slot
  numbers and the token parsers) passes. The first real datum is the
  `GetProcAddress`-request line from the one interactive deploy.
- If the swap turns out to run after the constructor (no request line, run still
  stuck on the splash), the fallback is the wake-64 in-process console route
  (`loadgame Checkpoint`), which needs no early install.
- The response-time cost is one `GetProcAddress` indirection for MSystem.dll's
  own resolutions and, while opted in, one `GetTickCount` + comparison per
  keyboard/XInput poll — no per-frame allocation, no draw-path change.

## Addendum (wake 67): the interceptor is now *executed* offline

The consequence above — "the interceptor cannot be live-validated by this loop"
— was true when written, but it left the single remaining route to GH #71 req 1
validated by inspection alone. That is now closed without a game and without a
deploy:

1. **A module-parameterized install seam.** `SimInputBridgeInstallOnModule(HMODULE)`
   is the mechanism; `SimInputBridgeInstallOnAttach()` is now exactly
   `SimInputBridgeInstallOnModule(GetModuleHandleA("MSystem.dll"))`. The swap
   (one `KERNEL32.dll!GetProcAddress` IAT entry) and the two wrappers are
   module-agnostic, so the bridge can be installed on a module a test is allowed
   to touch.
2. **`tests/sim_input_bridge_win32_tests.cpp` drives the engine's exact path**
   (*x86 only*: the hook ships x86 and the engine resolves the legacy 32-bit
   `dinput.dll`). It installs the bridge on its own module and CTest runs it in
   two modes:
   - `sim_input_bridge_win32_intercept` (`gate-on`): after the install,
     `GetProcAddress("XInputGetState")` / `GetProcAddress("DirectInputCreateA")`
     return the wrappers; an unrelated proc, a near-miss (`DirectInputCreateW`)
     and a different proc from the same DLL all pass through; calling the
     wrapped `XInputGetState` with the engine's own `XINPUT_STATE` buffer yields
     `START|A` with the packet number bumped (index 1 untouched); and the
     `IDirectInputA` returned by the wrapped `DirectInputCreateA` has a live
     `CreateDevice` whose `GUID_SysKeyboard` device has slot 9 replaced.
   - `sim_input_bridge_win32_inert` (`gate-off`, requirement 3): with the
     two-token gate off, every resolved pointer is the real one and no state is
     fabricated.
3. Both SKIP (exit 42) when `dinput.dll` / `XInput1_3.dll` are absent. The
   device-vtable sub-check downgrades to a note if `CreateDevice(GUID_SysKeyboard)`
   is unavailable; the routing and XInput checks need no device.

**What this does and does not prove.** It proves the interceptor's *mechanism*
end-to-end in-process: the IAT swap routes `GetProcAddress` calls to the
wrappers, the wrappers fabricate the confirm into the engine's buffer shapes, and
they stay inert without the gate. It does **not** prove the ADR-0467 residual
(the ordering against `CInputContext_DInput`'s constructor inside the real game)
— that still needs the one interactive deploy, whose first datum remains the
per-request `took over …` line.
