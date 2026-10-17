// GH #80 (requirement 1): the simulator-only input bridge's interceptor.
//
// The pure half -- gate, cadence, state encodings, ownership predicates, vtable
// slots, env-token parsers -- is ``sim_input_bridge.h`` (headless-tested). This
// file is the Win32 half: it installs an early ``GetProcAddress`` intercept on
// MSystem.dll and wraps the two device entry points the engine resolves there.
//
// Mechanism (ADR-0468):
//   * MSystem.dll imports ``KERNEL32.dll!GetProcAddress``; we swap that ONE IAT
//     entry (``IATHook``, the in-process-proven primitive) from DllMain.
//   * ``Hook_GetProcAddress`` returns our wrapper for exactly the two names the
//     input context ctor asks for -- and stashes the real proc it just resolved
//     so the wrapper can forward.
//   * the gamepad route wraps ``XInputGetState`` (OR START|A into the state the
//     real call reported, bump the packet number).
//   * the keyboard route wraps ``DirectInputCreateA``, then patches the returned
//     IDirectInputA's ``CreateDevice`` (slot 3) in place, and -- when the engine
//     creates GUID_SysKeyboard -- patches that device's ``GetDeviceState``
//     (slot 9) to OR in DIK_RETURN. In-place slot patches keep ``this`` correct,
//     which a copied-vtable wrapper would not.
//
// Everything is inert unless RIDDICKVR_SIM *and* RIDDICKVR_SIM_CONFIRM are set.

#include "sim_input_bridge_win32.h"

#include "sim_input_bridge.h"
#include "iat_hook.h"
#include "logging.h"

#include <cstring>

namespace RiddickVR {
namespace {

// --------------------------------------------------------------------------
// Bridge state, written once at attach and read by the intercepts
// --------------------------------------------------------------------------
SimInputGate g_gate;
SimConfirmRoute g_route = SimConfirmRoute::None;
SimConfirmScheduleConfig g_schedule;  // bridge defaults: 400/100/300
SimConfirmState g_confirm;

volatile LONG g_installed = 0;             // the IAT swap succeeded
volatile LONG g_getProcCalls = 0;          // requests through our intercept
volatile LONG g_inputProcRequests = 0;     // ...of which were one of ours
volatile LONG g_createDevicePatched = 0;   // IDirectInputA::CreateDevice taken over
volatile LONG g_keyboardDeviceWrapped = 0; // GUID_SysKeyboard::GetDeviceState taken over

// --------------------------------------------------------------------------
// Real functions (resolved lazily -- never in DllMain, never under loader lock)
// --------------------------------------------------------------------------
typedef FARPROC(WINAPI* PFN_GetProcAddress)(HMODULE, LPCSTR);
PFN_GetProcAddress g_realGetProcAddress = nullptr;

typedef unsigned long(WINAPI* PFN_XInputGetState)(unsigned long, void*);
PFN_XInputGetState g_realXInputGetState = nullptr;

typedef long(WINAPI* PFN_DirectInputCreateA)(void*, unsigned long, void**, void*);
PFN_DirectInputCreateA g_realDirectInputCreateA = nullptr;

typedef long(WINAPI* PFN_CreateDevice)(void*, const void*, void**, void*);
PFN_CreateDevice g_realCreateDevice = nullptr;

typedef long(WINAPI* PFN_GetDeviceState)(void*, unsigned long, void*);
PFN_GetDeviceState g_realGetDeviceState = nullptr;

// The one IDirectInputA the engine created, and the keyboard device under it
// (so a shared device vtable can forward other devices -- e.g. the mouse --
// untouched).
void* g_diObject = nullptr;
void* g_keyboardDevice = nullptr;
bool g_keyboardInjected = false;

// Patches one entry of a live COM vtable in place, in the object's own image.
// In-place (rather than a copied vtable) so the patched method still receives
// the correct ``this``. ``origFn`` captures the replaced pointer once.
bool PatchVTableSlot(void** vtbl, int slot, void* newFunc, void** origFn) {
    if (!vtbl || slot < 0 || !newFunc) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    if (origFn && !*origFn) *origFn = vtbl[slot];
    vtbl[slot] = newFunc;
    DWORD restored = 0;
    VirtualProtect(&vtbl[slot], sizeof(void*), oldProtect, &restored);
    return true;
}

const char* RouteName(SimConfirmRoute route) {
    switch (route) {
        case SimConfirmRoute::Keyboard: return "keyboard";
        case SimConfirmRoute::Gamepad:  return "gamepad";
        case SimConfirmRoute::Both:     return "both";
        default:                        return "none";
    }
}

// --------------------------------------------------------------------------
// XInput route: XInputGetState
// --------------------------------------------------------------------------
// XINPUT_STATE is { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } and the pure
// core's XInputGamepadState is field-for-field XINPUT_GAMEPAD, so the wrapper
// writes through the engine's own buffer without <xinput.h>.
struct RawXInputState {
    unsigned int packet;
    XInputGamepadState pad;
};

unsigned long WINAPI Hook_XInputGetState(unsigned long userIndex, void* pState) {
    unsigned long result = g_realXInputGetState
        ? g_realXInputGetState(userIndex, pState)
        : 1167ul;  // ERROR_DEVICE_NOT_CONNECTED: a phantom pad must stay absent
    if (userIndex != 0 || !pState) return result;

    const bool pressed = SimConfirmTick(static_cast<unsigned long>(GetTickCount()),
                                        g_schedule, g_confirm);
    RawXInputState* state = static_cast<RawXInputState*>(pState);
    const bool changed = ApplySimConfirmToGamepad(&state->pad, pressed);
    state->packet = BumpSimPacketNumber(state->packet, changed);
    return result;
}

// --------------------------------------------------------------------------
// Keyboard route: DirectInputCreateA -> CreateDevice -> GetDeviceState
// --------------------------------------------------------------------------
long WINAPI Hook_GetDeviceState(void* self, unsigned long cbData, void* data) {
    long hr = g_realGetDeviceState
        ? g_realGetDeviceState(self, cbData, data)
        : static_cast<long>(0x80004005L);  // E_FAIL
    if (hr < 0 || self != g_keyboardDevice || !data) return hr;

    const bool pressed = SimConfirmTick(static_cast<unsigned long>(GetTickCount()),
                                        g_schedule, g_confirm);
    unsigned char* keys = static_cast<unsigned char*>(data);
    if (pressed) {
        if (SetDirectInputKey(keys, static_cast<unsigned int>(cbData), kDikReturn, true)) {
            g_keyboardInjected = true;
        }
    } else if (g_keyboardInjected) {
        // Only clear the byte we set: never stomp a real (non-sim) press.
        SetDirectInputKey(keys, static_cast<unsigned int>(cbData), kDikReturn, false);
        g_keyboardInjected = false;
    }
    return hr;
}

long WINAPI Hook_DirectInputCreateDevice(void* self, const void* rguid, void** ppDevice,
                                         void* punkOuter) {
    long hr = g_realCreateDevice
        ? g_realCreateDevice(self, rguid, ppDevice, punkOuter)
        : static_cast<long>(0x80004005L);
    if (hr < 0 || !ppDevice || !*ppDevice) return hr;
    if (rguid && SimBridgeIsKeyboardGuid(rguid)) {
        void* device = *ppDevice;
        void** vtbl = *reinterpret_cast<void***>(device);
        if (PatchVTableSlot(vtbl, kDirectInputDeviceGetDeviceStateSlot,
                            reinterpret_cast<void*>(&Hook_GetDeviceState),
                            reinterpret_cast<void**>(&g_realGetDeviceState))) {
            g_keyboardDevice = device;
            InterlockedExchange(&g_keyboardDeviceWrapped, 1);
        }
    }
    return hr;
}

long WINAPI Hook_DirectInputCreateA(void* hinst, unsigned long version, void** ppDI,
                                    void* punkOuter) {
    long hr = g_realDirectInputCreateA
        ? g_realDirectInputCreateA(hinst, version, ppDI, punkOuter)
        : static_cast<long>(0x80004005L);
    if (hr < 0 || !ppDI || !*ppDI) return hr;

    void* di = *ppDI;
    void** vtbl = *reinterpret_cast<void***>(di);
    if (PatchVTableSlot(vtbl, kDirectInputCreateDeviceSlot,
                        reinterpret_cast<void*>(&Hook_DirectInputCreateDevice),
                        reinterpret_cast<void**>(&g_realCreateDevice))) {
        g_diObject = di;
        InterlockedExchange(&g_createDevicePatched, 1);
    }
    return hr;
}

// --------------------------------------------------------------------------
// The intercept itself
// --------------------------------------------------------------------------
FARPROC WINAPI Hook_GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    PFN_GetProcAddress real = g_realGetProcAddress;
    if (!real) return nullptr;
    InterlockedIncrement(&g_getProcCalls);

    // An ordinal (HIWORD == 0) is not a name and is never ours.
    if (lpProcName && HIWORD(lpProcName) != 0 &&
        SimBridgeOwnsProcName(lpProcName, g_route)) {
        FARPROC resolved = real(hModule, lpProcName);
        InterlockedIncrement(&g_inputProcRequests);
        if (resolved) {
            if (std::strcmp(lpProcName, "XInputGetState") == 0) {
                g_realXInputGetState = reinterpret_cast<PFN_XInputGetState>(resolved);
                VR_LOG_INFO("XRSIM input bridge: took over XInputGetState (real=%p)",
                            reinterpret_cast<void*>(resolved));
                return reinterpret_cast<FARPROC>(&Hook_XInputGetState);
            }
            g_realDirectInputCreateA = reinterpret_cast<PFN_DirectInputCreateA>(resolved);
            VR_LOG_INFO("XRSIM input bridge: took over DirectInputCreateA (real=%p)",
                        reinterpret_cast<void*>(resolved));
            return reinterpret_cast<FARPROC>(&Hook_DirectInputCreateA);
        }
        return resolved;  // the loader said no: never fabricate a proc
    }
    return real(hModule, lpProcName);
}

// --------------------------------------------------------------------------
// Env plumbing (attach-time only)
// --------------------------------------------------------------------------
bool ReadEnvFlag(const char* name) {
    char buf[8] = {};
    return GetEnvironmentVariableA(name, buf, sizeof(buf)) > 0;
}

void ReadEnvString(const char* name, char* out, DWORD outSize) {
    if (outSize == 0) return;
    out[0] = '\0';
    GetEnvironmentVariableA(name, out, outSize);
}

}  // namespace

void SimInputBridgeInstallOnModule(HMODULE hModule) {
    // Requirement 3: two independent tokens. This -- and only this -- is the
    // work a non-sim (or non-opted-in) run does here; nothing is installed.
    g_gate.simEnvironment = ReadEnvFlag("RIDDICKVR_SIM");
    g_gate.optIn = ReadEnvFlag("RIDDICKVR_SIM_CONFIRM");
    if (!SimInputBridgeEnabled(g_gate)) return;

    char token[32] = {};
    ReadEnvString("RIDDICKVR_SIM_CONFIRM", token, sizeof(token));
    g_route = SimRouteFromToken(token, SimConfirmRoute::Both);
    if (g_route == SimConfirmRoute::None) return;

    char period[16] = {};
    char hold[16] = {};
    char taps[16] = {};
    ReadEnvString("RIDDICKVR_SIM_CONFIRM_PERIOD_MS", period, sizeof(period));
    ReadEnvString("RIDDICKVR_SIM_CONFIRM_HOLD_MS", hold, sizeof(hold));
    ReadEnvString("RIDDICKVR_SIM_CONFIRM_MAX_TAPS", taps, sizeof(taps));
    g_schedule = SimConfirmScheduleFromTokens(period, hold, taps, SimConfirmScheduleConfig{});

    // ADR-0467: MSystem.dll is a static import of DarkAthena.exe, so it is
    // always mapped by the time the hook DLL loads. Its GetProcAddress IAT entry
    // exists; swapping it is the loader-lock-safe early seam. The caller passes
    // the module (the DllMain entry passes MSystem.dll; the offline interceptor
    // test passes its own module -- the mechanism is module-agnostic).
    if (!hModule) {
        InterlockedExchange(&g_installed, 0);
        return;
    }

    const bool ok = IATHook::Hook(hModule, "KERNEL32.dll", "GetProcAddress",
                                  reinterpret_cast<void*>(&Hook_GetProcAddress),
                                  reinterpret_cast<void**>(&g_realGetProcAddress));
    InterlockedExchange(&g_installed, ok ? 1 : 0);

    // Loader-lock-safe breadcrumb (OutputDebugString touches no files): answers
    // "did the sim bridge install?" when the logger does not exist yet.
    OutputDebugStringA(ok
        ? "RiddickVR: XRSIM input bridge installed (GetProcAddress intercept)\r\n"
        : "RiddickVR: XRSIM input bridge FAILED to install (GetProcAddress not found)\r\n");
}

void SimInputBridgeInstallOnAttach() {
    // The production seam: MSystem.dll is the module whose GetProcAddress the
    // input context's constructor resolves through (ADR-0467).
    SimInputBridgeInstallOnModule(GetModuleHandleA("MSystem.dll"));
}

void SimInputBridgeLogStartupReport() {
    if (!SimInputBridgeEnabled(g_gate) || g_route == SimConfirmRoute::None) {
        return;  // inert: no log noise on a normal run
    }

    const long requests = g_inputProcRequests;
    VR_LOG_INFO("XRSIM input bridge: gate on route=%s installed=%s "
                "getProcCalls=%ld inputProcRequests=%ld createDevicePatched=%ld "
                "keyboardWrapped=%ld",
                RouteName(g_route), g_installed ? "yes" : "no",
                static_cast<long>(g_getProcCalls), requests,
                static_cast<long>(g_createDevicePatched),
                static_cast<long>(g_keyboardDeviceWrapped));

    if (!g_installed) {
        VR_LOG_WARN("XRSIM input bridge: the intercept did not install; the sim "
                    "cannot fabricate a confirm this run");
    } else if (requests == 0) {
        // Not (yet) proof of failure: the deferred init can legitimately run
        // before the input context's constructor. The proof is the per-request
        // line above, which appears the instant the ctor resolves the procs.
        VR_LOG_INFO("XRSIM input bridge: no input-proc request seen yet -- if this "
                    "run never reaches gameplay, the swap ran after "
                    "CInputContext_DInput's ctor (0x1010b440) and the confirm "
                    "cannot be fabricated (ADR-0467 residual)");
    }
}

}  // namespace RiddickVR
