#pragma once

// GH #80 (requirement 1): the simulator-only input bridge's *interceptor* -- the
// Win32 half of the bridge whose pure core is ``sim_input_bridge.h``.
//
// WHY it exists: both input-free routes to a live XRSIM scene are measured dead
// (posted window messages -- ADR-0466; the engine's command line -- its wake-64
// addendum), so the only remaining way to reach gameplay under
// ``RIDDICKVR_SIM=1`` is to fabricate the scripted confirm *in-process*, on the
// engine's own device read path.
//
// WHERE it installs (ADR-0467): ``MSystem.dll`` resolves ``XInputGetState`` /
// ``DirectInputCreateA`` **once**, inside ``CInputContext_DInput``'s constructor
// (VA ``0x1010b440``), via ``LoadLibraryA`` + ``GetProcAddress``. So the
// intercept is a loader-lock-safe IAT swap on ``MSystem.dll``'s
// ``KERNEL32.dll!GetProcAddress`` entry, performed from ``DllMain``
// (``DLL_PROCESS_ATTACH``). The mechanism and its residual ordering risk are
// recorded in ADR-0468.
//
// The install is **inert** unless *both* ``RIDDICKVR_SIM`` and
// ``RIDDICKVR_SIM_CONFIRM`` are set (requirement 3), so a normal run -- and a
// plain simulator run -- never fabricates input.

#include <windows.h>

namespace RiddickVR {

// Called from ``DllMain(DLL_PROCESS_ATTACH)``. Does nothing (no allocation, no
// IO, no ``LoadLibrary``) unless the two-token gate is on; when it is, it
// performs the one IAT pointer swap described above. Returns nothing: the
// outcome is observable through ``SimInputBridgeLogStartupReport`` and the
// per-request log line, because the loader lock forbids logging here.
void SimInputBridgeInstallOnAttach();

// The module-parameterized install seam. ``SimInputBridgeInstallOnAttach()`` is
// exactly ``SimInputBridgeInstallOnModule(GetModuleHandleA("MSystem.dll"))``;
// the offline interceptor test installs the same bridge on *its own* module to
// drive the engine's resolution path (``GetProcAddress`` -> wrapper -> injected
// state) without a game and without a deploy. The mechanism -- swap one
// ``KERNEL32.dll!GetProcAddress`` IAT entry and wrap the two resolved procs --
// is module-agnostic, which is why this seam exists.
//
// Same contract as the DllMain entry: loader-lock-safe, inert (a single env
// read, no install) unless both ``RIDDICKVR_SIM`` and ``RIDDICKVR_SIM_CONFIRM``
// are set, and a no-op that records ``installed=no`` when ``hModule`` is null or
// imports no ``GetProcAddress``.
void SimInputBridgeInstallOnModule(HMODULE hModule);

// Called once from the deferred hook init, after the logger exists. Reports
// whether the intercept installed and whether the engine has since asked for
// its input procs (i.e. whether the swap provably beat the input context's
// constructor). Silent when the bridge is inert.
void SimInputBridgeLogStartupReport();

// GH #71 req 1 / #80 follow-up: emit the bridge's *observed* activity as one
// parseable line -- how often the engine polled each device entry point, how
// many confirms the bridge fabricated, and the real API's last return code.
// Called on a fixed cadence from the wrappers themselves, because a harness run
// kills the game process (``TerminateProcess``), so ``DLL_PROCESS_DETACH`` --
// which already logs -- must not be the only emission point. Silent when the
// bridge is inert.
void SimInputBridgeLogSummary();

}  // namespace RiddickVR
