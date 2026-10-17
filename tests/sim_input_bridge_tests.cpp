// GH #80: unit tests for the simulator-only input bridge's pure core
// (sim_input_bridge.h) -- the gate, the tap cadence and the two device-state
// encodings the (non-pure) interceptor ORs into the engine's own buffers.
#include "sim_input_bridge.h"

#include <cstdio>

using RiddickVR::ApplySimConfirmToGamepad;
using RiddickVR::BumpSimPacketNumber;
using RiddickVR::kDikReturn;
using RiddickVR::kDirectInputCreateDeviceSlot;
using RiddickVR::kDirectInputDeviceGetDeviceStateSlot;
using RiddickVR::kDirectInputKeyboardBytes;
using RiddickVR::kXInputGamepadA;
using RiddickVR::kXInputGamepadStart;
using RiddickVR::SetDirectInputKey;
using RiddickVR::SimBridgeIsKeyboardGuid;
using RiddickVR::SimBridgeOwnsProcName;
using RiddickVR::SimConfirmPeriod;
using RiddickVR::SimConfirmRoute;
using RiddickVR::SimConfirmScheduleConfig;
using RiddickVR::SimConfirmScheduleFromTokens;
using RiddickVR::SimConfirmState;
using RiddickVR::SimConfirmTick;
using RiddickVR::SimInputBridgeEnabled;
using RiddickVR::SimInputGate;
using RiddickVR::SimRouteFromToken;
using RiddickVR::SimRouteHasGamepad;
using RiddickVR::SimRouteHasKeyboard;
using RiddickVR::XInputGamepadState;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "sim input bridge test failed: %s\n", message);
        ++g_failures;
    }
}

static void TestGate() {
    SimInputGate gate;
    Check(!SimInputBridgeEnabled(gate), "default gate is off");
    gate.simEnvironment = true;
    Check(!SimInputBridgeEnabled(gate), "a plain simulator run must stay inert");
    gate.optIn = true;
    Check(SimInputBridgeEnabled(gate), "sim + opt-in enables the bridge");
    gate.simEnvironment = false;
    Check(!SimInputBridgeEnabled(gate), "the opt-in alone must not enable input");
}

static void TestRoutes() {
    Check(!SimRouteHasKeyboard(SimConfirmRoute::None), "None has no keyboard");
    Check(!SimRouteHasGamepad(SimConfirmRoute::None), "None has no gamepad");
    Check(SimRouteHasKeyboard(SimConfirmRoute::Keyboard), "Keyboard route");
    Check(!SimRouteHasGamepad(SimConfirmRoute::Keyboard), "Keyboard route is not gamepad");
    Check(SimRouteHasGamepad(SimConfirmRoute::Gamepad), "Gamepad route");
    Check(!SimRouteHasKeyboard(SimConfirmRoute::Gamepad), "Gamepad route is not keyboard");
    Check(SimRouteHasKeyboard(SimConfirmRoute::Both) && SimRouteHasGamepad(SimConfirmRoute::Both),
          "Both covers both");
}

static void TestRouteToken() {
    Check(SimRouteFromToken(nullptr, SimConfirmRoute::Both) == SimConfirmRoute::Both,
          "an absent token keeps the fallback");
    Check(SimRouteFromToken("", SimConfirmRoute::Both) == SimConfirmRoute::Both,
          "an empty token (bare opt-in) keeps the fallback");
    Check(SimRouteFromToken("keyboard") == SimConfirmRoute::Keyboard, "keyboard token");
    Check(SimRouteFromToken("KEYBOARD") == SimConfirmRoute::Keyboard, "the token is case-insensitive");
    Check(SimRouteFromToken("Gamepad") == SimConfirmRoute::Gamepad, "gamepad token");
    Check(SimRouteFromToken(" both ", SimConfirmRoute::Keyboard) == SimConfirmRoute::Keyboard,
          "a padded token is not trimmed, so it falls back");
    Check(SimRouteFromToken("both") == SimConfirmRoute::Both, "both token");
    Check(SimRouteFromToken("none") == SimConfirmRoute::None, "none opts out");
    Check(SimRouteFromToken("off") == SimConfirmRoute::None, "off opts out");
    Check(SimRouteFromToken("typo", SimConfirmRoute::Keyboard) == SimConfirmRoute::Keyboard,
          "an unrecognised token keeps the fallback (never widens the route)");
}

static void TestScheduleTokens() {
    const SimConfirmScheduleConfig base;  // 400 / 100 / 300

    {
        SimConfirmScheduleConfig c = SimConfirmScheduleFromTokens(nullptr, nullptr, nullptr, base);
        Check(c.periodMs == base.periodMs && c.holdMs == base.holdMs && c.maxTaps == base.maxTaps,
              "absent tokens keep every field");
    }
    {
        SimConfirmScheduleConfig c = SimConfirmScheduleFromTokens("250", "80", "7", base);
        Check(c.periodMs == 250 && c.holdMs == 80 && c.maxTaps == 7, "plain decimals parse");
    }
    {
        SimConfirmScheduleConfig c = SimConfirmScheduleFromTokens("0", "0", "", base);
        Check(c.periodMs == base.periodMs && c.holdMs == base.holdMs, "a zero period/hold is refused, not applied");
        Check(c.maxTaps == base.maxTaps, "an empty maxTaps keeps the fallback");
    }
    {
        SimConfirmScheduleConfig c = SimConfirmScheduleFromTokens("abc", "12x", "0", base);
        Check(c.periodMs == base.periodMs && c.holdMs == base.holdMs, "a non-numeric millisecond is refused");
        Check(c.maxTaps == 0, "an explicit maxTaps 0 is legal (never tap)");
    }
    {
        SimConfirmScheduleConfig c = SimConfirmScheduleFromTokens("4294967295", nullptr, nullptr, base);
        Check(c.periodMs == 4294967295ul, "the 32-bit maximum is accepted");

        SimConfirmScheduleConfig over = SimConfirmScheduleFromTokens("4294967296", nullptr, nullptr, base);
        Check(over.periodMs == base.periodMs, "one past the 32-bit maximum is refused");
    }
}

static void TestVtableSlots() {
    // The interceptor hard-codes these two COM vtable indices; the ABI is pinned
    // here so a wrong slot is a test failure, not a silent mis-patch (GH #80).
    Check(kDirectInputCreateDeviceSlot == 3, "IDirectInputA::CreateDevice is vtable slot 3");
    Check(kDirectInputDeviceGetDeviceStateSlot == 9,
          "IDirectInputDeviceA::GetDeviceState is vtable slot 9");
}

static void TestProcOwnership() {
    Check(SimBridgeOwnsProcName("XInputGetState", SimConfirmRoute::Gamepad), "gamepad owns XInputGetState");
    Check(!SimBridgeOwnsProcName("XInputGetState", SimConfirmRoute::Keyboard), "keyboard route leaves XInput alone");
    Check(SimBridgeOwnsProcName("DirectInputCreateA", SimConfirmRoute::Keyboard), "keyboard owns DirectInputCreateA");
    Check(!SimBridgeOwnsProcName("DirectInputCreateA", SimConfirmRoute::Gamepad), "gamepad route leaves DirectInput alone");
    Check(SimBridgeOwnsProcName("XInputGetState", SimConfirmRoute::Both) &&
              SimBridgeOwnsProcName("DirectInputCreateA", SimConfirmRoute::Both),
          "Both owns both procs");
    Check(!SimBridgeOwnsProcName("xinputgetstate", SimConfirmRoute::Both), "GetProcAddress is case-sensitive");
    Check(!SimBridgeOwnsProcName("XInputGetStateEx", SimConfirmRoute::Both), "a near-miss name is not ours");
    Check(!SimBridgeOwnsProcName(nullptr, SimConfirmRoute::Both), "null name is not ours");
    Check(!SimBridgeOwnsProcName("XInputGetState", SimConfirmRoute::None), "None owns nothing");
}

static void TestSchedule() {
    SimConfirmScheduleConfig cfg;  // 400 ms period, 100 ms hold, 300 taps

    {
        SimConfirmState s;
        Check(SimConfirmTick(1000, cfg, s), "the first tick taps immediately");
        Check(s.taps == 1, "the first tap is counted");
        Check(SimConfirmTick(1099, cfg, s), "still down 99 ms into the hold");
        Check(!SimConfirmTick(1100, cfg, s), "released at the end of the hold");
        Check(!SimConfirmTick(1399, cfg, s), "still released before the next period");
        Check(SimConfirmTick(1400, cfg, s), "the second tap fires at the period");
        Check(s.taps == 2, "the second tap is counted");
        Check(SimConfirmTick(1400, cfg, s), "a re-tick in the same ms is inside the hold");
        Check(s.taps == 2, "a re-tick in the same ms counts no extra tap");
    }

    {
        // The period is clamped up to the hold, so a tap always has a gap.
        SimConfirmScheduleConfig tight;
        tight.periodMs = 10;
        tight.holdMs = 50;
        Check(SimConfirmPeriod(tight) == 50, "period clamps up to the hold");
        SimConfirmState s;
        Check(SimConfirmTick(0, tight, s), "tight first tap");
        Check(SimConfirmTick(49, tight, s), "tight tap still down at 49 ms");
        Check(SimConfirmTick(50, tight, s), "tight tap re-taps exactly at the clamped period");
        Check(s.taps == 2, "tight second tap is counted");
    }

    {
        // The tap budget is bounded; 0 means never.
        SimConfirmScheduleConfig once;
        once.maxTaps = 1;
        SimConfirmState s;
        Check(SimConfirmTick(0, once, s), "one allowed tap fires");
        Check(!SimConfirmTick(100000, once, s), "the budget stops further taps");
        Check(s.taps == 1, "the budget is not exceeded");

        SimConfirmScheduleConfig none;
        none.maxTaps = 0;
        SimConfirmState s2;
        Check(!SimConfirmTick(0, none, s2) && !SimConfirmTick(5000, none, s2), "maxTaps 0 never taps");
        Check(s2.taps == 0, "maxTaps 0 counts no tap");
    }

    {
        // 32-bit tick wrap: the gap arithmetic must survive the wrap.
        SimConfirmScheduleConfig cfg2;
        SimConfirmState s;
        const unsigned long near = 0xFFFFFF90ul;
        Check(SimConfirmTick(near, cfg2, s), "tap near the wrap");
        Check(SimConfirmTick((near + 0x63) & 0xFFFFFFFFul, cfg2, s), "still down one ms before the hold ends");
        Check(!SimConfirmTick((near + 0x64) & 0xFFFFFFFFul, cfg2, s), "the hold ends across the wrap point");
        Check(SimConfirmTick((near + 0x190) & 0xFFFFFFFFul, cfg2, s),
              "the next tap after the wrap fires on schedule");
        Check(s.taps == 2, "the wrapped second tap is counted");
    }
}

static void TestDirectInputKeyboard() {
    unsigned char state[kDirectInputKeyboardBytes];
    for (int i = 0; i < kDirectInputKeyboardBytes; ++i) state[i] = 0;

    Check(SetDirectInputKey(state, kDirectInputKeyboardBytes, kDikReturn, true), "set DIK_RETURN ok");
    Check(state[kDikReturn] == 0x80, "a down key is a full byte (0x80), not a bit");
    Check(state[kDikReturn - 1] == 0 && state[kDikReturn + 1] == 0, "the neighbours are untouched");
    Check(SetDirectInputKey(state, kDirectInputKeyboardBytes, kDikReturn, false), "clear DIK_RETURN ok");
    Check(state[kDikReturn] == 0, "a released key reads 0");

    Check(!SetDirectInputKey(nullptr, 256, kDikReturn, true), "a null buffer is refused");
    Check(!SetDirectInputKey(state, kDikReturn, kDikReturn, true), "a too-short buffer is refused");
    Check(!SetDirectInputKey(state, 0, kDikReturn, true), "an empty buffer is refused");
    const unsigned char last = static_cast<unsigned char>(kDirectInputKeyboardBytes - 1);
    Check(SetDirectInputKey(state, kDirectInputKeyboardBytes, last, true), "the last byte is addressable");
}

static void TestKeyboardGuid() {
    Check(SimBridgeIsKeyboardGuid(RiddickVR::SimKeyboardGuidBytes()), "GUID_SysKeyboard matches");
    unsigned char other[16];
    for (int i = 0; i < 16; ++i) other[i] = RiddickVR::SimKeyboardGuidBytes()[i];
    other[0] ^= 0x01;  // a mouse-ish neighbour
    Check(!SimBridgeIsKeyboardGuid(other), "a different GUID is not the keyboard");
    Check(!SimBridgeIsKeyboardGuid(nullptr), "a null GUID is not the keyboard");
}

static void TestGamepad() {
    XInputGamepadState pad{};
    pad.thumbLX = -1234;
    Check(!ApplySimConfirmToGamepad(&pad, false), "an unpressed confirm changes nothing");
    Check(pad.buttons == 0, "an unpressed confirm sets no button");

    Check(ApplySimConfirmToGamepad(&pad, true), "a pressed confirm changes the state");
    Check((pad.buttons & kXInputGamepadStart) != 0 && (pad.buttons & kXInputGamepadA) != 0,
          "START and A are both raised");
    Check(pad.thumbLX == -1234, "the sticks are preserved");
    Check(!ApplySimConfirmToGamepad(&pad, true), "a repeated press is not a change");

    pad.buttons = static_cast<unsigned short>(pad.buttons | 0x0004);  // a real pad's Y
    Check(ApplySimConfirmToGamepad(&pad, true) == false, "an existing real button is kept, no duplicate change");
    Check((pad.buttons & 0x0004) != 0, "the real button survives the synthetic press");

    Check(!ApplySimConfirmToGamepad(nullptr, true), "a null state is refused");

    Check(BumpSimPacketNumber(7, true) == 8, "a changed state bumps the packet number");
    Check(BumpSimPacketNumber(7, false) == 7, "an unchanged state keeps the packet number");
}

int main() {
    TestGate();
    TestRoutes();
    TestRouteToken();
    TestProcOwnership();
    TestSchedule();
    TestScheduleTokens();
    TestVtableSlots();
    TestDirectInputKeyboard();
    TestKeyboardGuid();
    TestGamepad();

    if (g_failures == 0) {
        std::puts("sim input bridge tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
