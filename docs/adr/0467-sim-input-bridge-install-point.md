# ADR-0467: The sim input bridge must be installed before `CInputContext_DInput`'s constructor, not at the hook's deferred init

- Status: Accepted (scheduled autoloop wake 65, GH #80 requirement 1)
- Date: 2026-10-17
- Related: GH #80 (sim-only input bridge), GH #71 (reach gameplay), GH #68
  (simulator harness), GH #27 (deferred hook init), ADR-0466 (+ its wake-64
  addendum), `src/hook/iat_hook.cpp`, `src/hook/sim_input_bridge.h`

## Context

GH #80 is the only remaining route to GH #71 requirement 1 (a scripted XRSIM run
reaches a live 3D scene): the hook fabricates the scripted confirm **in-process**
on the engine's own device read path. Both input-free alternatives are measured
dead — posted window messages (ADR-0466) and the engine's command line (its
wake-64 addendum). The issue proposed intercepting `GetProcAddress` inside
`MSystem.dll` and wrapping `XInputGetState` / `DirectInputCreateA` →
`CreateDevice` → `GetDeviceState`.

The proposal never said **where in the hook's lifetime** the intercept is
installed. That is decisive, because the hook today defers *all* init to the
first `wglCreateContext` (GH #27: `DllMain` does no loader-lock work; the real
init runs from `EnsureHookInitialized`, called on the first GL context). Wake 65
re-measured the shipped binaries to decide whether that seam can work.

## Findings (read-only, shipped sim install `F:`, Ghidra 12.1.4 via `rea`)

`MSystem.dll` (SHA-256 `8cde669fb7b4f742565a3009920e3c2014f7a889431e636336bef2af8a67f770`,
image base `0x10000000`):

1. **The input entry points are resolved once, in a constructor.**
   `CInputContext_DInput::CInputContext_DInput` at VA `0x1010b440`
   (body `0x1010b440`–`0x1010b52a`) executes, verbatim:
   - `LoadLibraryA("xinput1_3.dll")` (string VA `0x102fc7cc`) then resolves
     `"XInputGetState"` (string VA `0x102fc7bc`) through a helper;
   - `LoadLibraryA("dinput.dll")` (string VA `0x102fc7b0`) then
     `GetProcAddress(..., "DirectInputCreateA")` (string VA `0x102fc79c`).

   So there is exactly one moment at which these procs can be taken over: before
   that constructor returns. Nothing re-resolves them per frame.

2. **That constructor is reached only through the input context factory.**
   `FUN_1010b7f0` (body `0x1010b7f0`–`0x1010b850`) allocates `0xb08` bytes and
   calls the constructor; it has **no direct callers** (it is invoked indirectly
   from the engine's subsystem/factory registration), and the only other
   "reference" to the constructor is linker-generated exception data
   (`0x1038a0bc`). The construction site cannot be pinned by xrefs alone.

3. **Device creation is tied to the engine's new-main-window event.**
   `MSystem.dll` contains the message id string `CSS_MSG_WIN32_NEWMAINWINDOW`
   (VA `0x102fc952`) next to
   `CInputContext_DInput::DI_CreateDevices` / `XI_CreateDevices` and
   `Unable to create keyboard device.` / `IN_Keyboard_UseDI`, i.e. the input
   subsystem (and its device set) is part of window/system bring-up.

4. **The renderer — and therefore this hook — is loaded as a display context.**
   `CSystemWin32::DC_InitList` (VA `0x100f69c0`, body `0x100f69c0`–`0x100f6cf4`)
   registers `"OpenGL DisplayContext"` / `"OpenGL"` / `"RndrGL.DLL"` (string used
   at `0x100f6a4e`). `RndrGL.dll` imports `OPENGL32.dll` (96 thunks) and does
   **not** ship its own; the game dir's `opengl32.dll` is the mod hook — the
   deployed hook's log confirms it (`Successfully IAT-hooked OPENGL32.dll!… in
   module 07690000`, i.e. RndrGL's image). The mod's hook must therefore be
   loaded *by* the display-context bring-up.

5. **`DarkAthena.exe` does not statically import `opengl32`/`RndrGL`.**
   Its import table is 13 DLLs (AVIFIL32, KERNEL32, USER32, GDI32, ADVAPI32,
   SHELL32, ole32, OLEAUT32, **MSystem.dll**, MCCDyn.dll, WINMM, WS2_32, dvm.dll),
   one thunk each. `MSystem.dll` (a static import) is already mapped at the
   process entry point, **before** any `opengl32` hook code runs.

## Decision

- The bridge's `GetProcAddress` interception **must not be built on the deferred
  init seam** (first `wglCreateContext`). A GL context needs a window HDC, and
  the input context is part of the same window bring-up (finding 3), so the
  deferred init can run *after* the input procs were already resolved
  (finding 1). Any code that installs on that seam is untrustworthy for #80 by
  construction.
- Install the intercept as early as the hook can legally act:
  a loader-lock-safe pointer swap (no IO, no `LoadLibrary`, no logging) on
  `MSystem.dll`'s `GetProcAddress` IAT entry from `DllMain(DLL_PROCESS_ATTACH)`,
  or from a thread started there whose only job is that swap. `MSystem.dll` is
  mapped before the hook (finding 5), so its IAT exists; `IATHook::Hook` is the
  existing, in-process-proven primitive (finding 4).
- The *residual* runtime question — does `CInputContext_DInput`'s constructor run
  before the hook's `DllMain` on an actual run? — is a load/constructor ordering
  that static xrefs cannot settle (finding 2, indirect factory). It needs one
  captured ordering datum from a real run (the one interactive deploy), so the
  bridge must **report** whether it installed before the procs were resolved
  rather than assume it.
- Land the bridge's **pure core** now (GH #80 requirement 2/3) so the
  interceptor is a thin, reviewable wrapper: the two-token enable gate, the
  bounded tap cadence, and the DirectInput-keyboard / XInput-gamepad state
  encodings — `src/hook/sim_input_bridge.h`, unit-tested headlessly in
  `tests/sim_input_bridge_tests.cpp`.

## Consequences

- #80's remaining work is now specified against a measured constraint
  ("install before `0x1010b440`'s constructor"), not an assumed one. The naive
  implementation (install during `EnsureHookInitialized`) is ruled out on
  evidence.
- The hook gains a small amount of `DllMain`-time work for #80. It must be the
  minimal, IO-free IAT swap so GH #27's loader-lock rule is not violated; all
  diagnostics are deferred to the existing logger, and it is inert unless the
  two-token gate is on.
- If the captured ordering shows the constructor already ran, the fallback is
  the *other* injection point from wake 64: the hook calling the engine's console
  (`loadgame Checkpoint`) in-process after init — which needs no early install,
  but is a different (also deploy-gated) slice.
- Live validation of the bridge remains deploy-gated: the sim install runs the
  stale 12:47 hook build, and this loop must not deploy. The pure core, the
  gate, and the install-point contract are the parts that are verified offline.
