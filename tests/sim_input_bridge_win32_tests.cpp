// GH #80 (requirement 1): an offline, end-to-end test of the simulator-only
// input bridge's *interceptor* (src/hook/sim_input_bridge_win32.cpp).
//
// Until now only the bridge's pure core (src/hook/sim_input_bridge.h) was
// unit-tested; the interceptor -- the Win32 half that actually routes the
// engine's resolution calls to our wrappers and fabricates the confirm state --
// had never executed anywhere except inside a deployed hook run (which this loop
// must not perform). That left the *only* remaining route to GH #71 requirement
// 1 validated by nothing but inspection.
//
// This test closes that gap without a game, without a deploy and without any OS
// input. It is exactly the engine's path, on a module we are allowed to touch:
//
//   1. it installs the bridge on its OWN module
//      (``SimInputBridgeInstallOnModule`` -- production passes MSystem.dll; the
//      mechanism is module-agnostic, ADR-0467/0468);
//   2. it then resolves the two procs the input context's constructor resolves
//      (``XInputGetState`` / ``DirectInputCreateA``) through the patched IAT and
//      asserts the wrapper is returned -- while an unrelated name, a near-miss
//      (``DirectInputCreateW``) and a *different* proc from the same DLL are NOT
//      (the exact-match, two-name contract);
//   3. it calls the wrapped ``XInputGetState`` with the engine's own
//      ``XINPUT_STATE`` buffer and asserts the scripted ``START|A`` confirm was
//      ORed in and the packet number bumped -- the state the engine reads;
//   4. it calls the wrapped ``DirectInputCreateA`` and asserts the returned
//      ``IDirectInputA``'s ``CreateDevice`` is live and that creating
//      ``GUID_SysKeyboard`` patches the device's ``GetDeviceState`` (vtable slot
//      9) in place;
//   5. in ``gate-off`` mode it proves the whole thing is *inert* without the
//      two-token gate (requirement 3): every resolved pointer is the real one.
//
// The "before" baselines come from ``ProbeGetProcAddressA`` in the tiny
// ``riddick_vr_sim_bridge_probe`` DLL, never from this executable's own
// ``GetProcAddress``: the optimizer may common-subexpression-eliminate two
// identical ``GetProcAddress`` calls (it did), which would make the "after"
// resolution silently reuse the "before" result and hide a broken intercept.
// The probe's own IAT is not the one under test.
//
// Exit 42 == SKIP when the input DLLs the engine resolves are not installed.
//
// Only ``dinput.dll`` (the legacy DirectX-3 flat DLL named in GH #80) and
// ``XInput1_3.dll`` are needed. A missing keyboard *device* downgrades check 4
// to a note, never a failure: the routing, the XInput injection and the
// inertness control need no device at all.

#include "sim_input_bridge_win32.h"

#include "sim_input_bridge.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

using RiddickVR::SimInputBridgeInstallOnModule;
using RiddickVR::SimInputBridgeLogStartupReport;
using RiddickVR::SimInputBridgeLogSummary;
using RiddickVR::kDirectInputCreateDeviceSlot;
using RiddickVR::kDirectInputDeviceGetDeviceStateSlot;
using RiddickVR::kXInputGamepadA;
using RiddickVR::kXInputGamepadStart;
using RiddickVR::SimKeyboardGuidBytes;
using RiddickVR::XInputGamepadState;

// The independent, unpatched resolution path (tests/sim_bridge_probe_dll.cpp).
extern "C" __declspec(dllimport) FARPROC WINAPI ProbeGetProcAddressA(HMODULE hModule,
                                                                    LPCSTR procName);

namespace {

constexpr int kSkipExitCode = 42;

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "sim input bridge win32 test failed: %s\n", message);
        ++g_failures;
    }
}

void Note(const char* message) {
    std::printf("sim input bridge win32 test note: %s\n", message);
}

// ``XINPUT_STATE`` is { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } and the
// pure core's ``XInputGamepadState`` is field-for-field ``XINPUT_GAMEPAD``.
struct RawXInputState {
    unsigned int packet;
    XInputGamepadState pad;
};

typedef long(WINAPI* PfnDirectInputCreateA)(void*, DWORD, void**, void*);
typedef DWORD(WINAPI* PfnXInputGetState)(DWORD, void*);
typedef long(WINAPI* PfnCreateDevice)(void*, const void*, void**, void*);

// The real ``IDirectInputDeviceA::GetDeviceState`` pointer, captured *before*
// the bridge is installed (so check 4 can show the slot changed in place even
// when dinput.dll shares one vtable across objects).
void* g_realDeviceGetDeviceState = nullptr;

// Creates the keyboard device through the ``CreateDevice`` currently in the
// given ``IDirectInputA`` vtable and returns its ``GetDeviceState`` slot, or
// null when DirectInput or the keyboard device is unavailable.
void* ProbeDeviceGetDeviceStateSlot(void* diObject) {
    if (!diObject) return nullptr;
    void** vtbl = *reinterpret_cast<void***>(diObject);
    PfnCreateDevice createDevice = reinterpret_cast<PfnCreateDevice>(
        vtbl[kDirectInputCreateDeviceSlot]);
    if (!createDevice) return nullptr;

    void* device = nullptr;
    const long hr = createDevice(diObject, SimKeyboardGuidBytes(), &device, nullptr);
    if (hr < 0 || !device) return nullptr;

    void** deviceVtbl = *reinterpret_cast<void***>(device);
    return deviceVtbl[kDirectInputDeviceGetDeviceStateSlot];
}

// Creates an ``IDirectInputA`` through an already-resolved
// ``DirectInputCreateA`` pointer (which may be the real proc or our wrapper).
void* CreateDirectInputObject(FARPROC directInputCreateA) {
    if (!directInputCreateA) return nullptr;
    PfnDirectInputCreateA create = reinterpret_cast<PfnDirectInputCreateA>(directInputCreateA);
    void* di = nullptr;
    const long hr = create(GetModuleHandleA(nullptr), 0x0700, &di, nullptr);
    return hr >= 0 ? di : nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    const bool gateOn = argc > 1 && std::strcmp(argv[1], "gate-on") == 0;
    const bool gateOff = argc > 1 && std::strcmp(argv[1], "gate-off") == 0;
    if (!gateOn && !gateOff) {
        std::fprintf(stderr, "usage: %s gate-on|gate-off\n", argv[0]);
        return 2;
    }

    // Set the two-token gate ourselves so the test never depends on (nor
    // pollutes) the parent environment. A null value removes the variable.
    SetEnvironmentVariableA("RIDDICKVR_SIM", gateOn ? "1" : nullptr);
    SetEnvironmentVariableA("RIDDICKVR_SIM_CONFIRM", gateOn ? "both" : nullptr);

    HMODULE hDinput = LoadLibraryA("dinput.dll");
    HMODULE hXInput = LoadLibraryA("XInput1_3.dll");
    if (!hDinput || !hXInput) {
        std::printf("SKIP: dinput.dll (%p) / XInput1_3.dll (%p) unavailable; the "
                    "input DLLs the engine resolves are not installed\n",
                    static_cast<void*>(hDinput), static_cast<void*>(hXInput));
        return kSkipExitCode;
    }
    HMODULE hSelf = GetModuleHandleA(nullptr);

    // ---- the independent baselines (the real procs), via the probe DLL -----
    FARPROC realDirectInputCreateA = ProbeGetProcAddressA(hDinput, "DirectInputCreateA");
    FARPROC realXInputGetState = ProbeGetProcAddressA(hXInput, "XInputGetState");
    // A near-miss from the same DLL: only the exact A name is ours.
    FARPROC realDirectInputCreateW = ProbeGetProcAddressA(hDinput, "DirectInputCreateW");
    FARPROC realXInputSetState = ProbeGetProcAddressA(hXInput, "XInputSetState");
    FARPROC realUnrelated = ProbeGetProcAddressA(GetModuleHandleA("kernel32.dll"),
                                                 "GetCurrentProcessId");
    if (!realDirectInputCreateA || !realXInputGetState || !realDirectInputCreateW ||
        !realXInputSetState || !realUnrelated) {
        std::printf("SKIP: the input DLLs do not export the procs this test "
                    "drives (DIC=%p DCW=%p XIG=%p XIS=%p)\n",
                    reinterpret_cast<void*>(realDirectInputCreateA),
                    reinterpret_cast<void*>(realDirectInputCreateW),
                    reinterpret_cast<void*>(realXInputGetState),
                    reinterpret_cast<void*>(realXInputSetState));
        return kSkipExitCode;
    }

    // Baseline: a keyboard device created through the *real* DirectInput (the
    // vtable slot the bridge is supposed to replace).
    g_realDeviceGetDeviceState = ProbeDeviceGetDeviceStateSlot(
        CreateDirectInputObject(realDirectInputCreateA));

    // ---- install the bridge on our own module and re-resolve ---------------
    SimInputBridgeInstallOnModule(hSelf);
    SimInputBridgeLogStartupReport();  // must not crash in either mode
    // GH #71 req 1: the observed-activity summary is emitted from the wrappers on
    // a cadence and from DllMain's detach. Calling it here proves it is safe
    // before (and without) a live engine polling the wrappers.
    SimInputBridgeLogSummary();

    FARPROC gotDirectInputCreateA = GetProcAddress(hDinput, "DirectInputCreateA");
    FARPROC gotXInputGetState = GetProcAddress(hXInput, "XInputGetState");
    FARPROC gotDirectInputCreateW = GetProcAddress(hDinput, "DirectInputCreateW");
    FARPROC gotXInputSetState = GetProcAddress(hXInput, "XInputSetState");
    FARPROC gotUnrelated = GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                          "GetCurrentProcessId");

    // The narrow, exact-match contract holds in *both* modes: names we do not
    // own -- including a near-miss and a different proc in the same DLL -- are
    // never replaced.
    Check(gotUnrelated == realUnrelated, "an unrelated proc must pass through");
    Check(gotDirectInputCreateW == realDirectInputCreateW,
          "DirectInputCreateW (a near-miss) must pass through");
    Check(gotXInputSetState == realXInputSetState,
          "XInputSetState (same DLL, not ours) must pass through");

    if (gateOn) {
        Check(gotXInputGetState != realXInputGetState,
              "gate on: XInputGetState must be taken over");
        Check(gotDirectInputCreateA != realDirectInputCreateA,
              "gate on: DirectInputCreateA must be taken over");

        // 3. The engine's own XINPUT_STATE buffer receives the scripted confirm.
        RawXInputState state = {};
        state.packet = 0;
        PfnXInputGetState xinputGetState =
            reinterpret_cast<PfnXInputGetState>(gotXInputGetState);
        (void)xinputGetState(0, &state);
        const unsigned short expected = kXInputGamepadStart | kXInputGamepadA;
        Check((state.pad.buttons & expected) == expected,
              "gate on: the wrapper must OR START|A into the engine's XINPUT_STATE");
        Check(state.packet == 1,
              "gate on: the packet number must bump when the state changes");

        // Only the engine's index 0 is fabricated: a second pad must stay absent.
        RawXInputState second = {};
        (void)xinputGetState(1, &second);
        Check(second.pad.buttons == 0 && second.packet == 0,
              "gate on: user index 1 must not be fabricated");

        // 4. The wrapped DirectInputCreateA hands back an IDirectInputA whose
        //    CreateDevice patches GUID_SysKeyboard's GetDeviceState in place.
        void* di = CreateDirectInputObject(gotDirectInputCreateA);
        Check(di != nullptr,
              "gate on: the wrapped DirectInputCreateA must still create an IDirectInputA");
        if (di) {
            void** vtbl = *reinterpret_cast<void***>(di);
            Check(vtbl[kDirectInputCreateDeviceSlot] != nullptr,
                  "gate on: the returned IDirectInputA must have a live CreateDevice");

            void* patchedSlot = ProbeDeviceGetDeviceStateSlot(di);
            if (!patchedSlot) {
                Note("CreateDevice(GUID_SysKeyboard) unavailable here; the "
                     "device-vtable patch is not exercised this run");
            } else {
                Check(g_realDeviceGetDeviceState != nullptr &&
                      patchedSlot != g_realDeviceGetDeviceState,
                      "gate on: GUID_SysKeyboard's GetDeviceState (slot 9) must be "
                      "replaced by the bridge");
            }
        }
    } else {
        // 5. Requirement 3: with the gate off nothing is installed.
        Check(gotXInputGetState == realXInputGetState,
              "gate off: XInputGetState must NOT be taken over");
        Check(gotDirectInputCreateA == realDirectInputCreateA,
              "gate off: DirectInputCreateA must NOT be taken over");

        RawXInputState state = {};
        state.packet = 0;
        PfnXInputGetState xinputGetState =
            reinterpret_cast<PfnXInputGetState>(gotXInputGetState);
        (void)xinputGetState(0, &state);
        Check(state.pad.buttons == 0,
              "gate off: no confirm may be fabricated into the engine's state");
    }

    if (g_failures == 0) {
        std::printf("sim_input_bridge_win32 %s: all checks passed\n",
                    gateOn ? "gate-on" : "gate-off");
    }
    return g_failures == 0 ? 0 : 1;
}
