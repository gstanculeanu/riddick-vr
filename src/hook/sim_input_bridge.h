#pragma once

// GH #80 (requirement 2/3): the simulator-only input bridge -- the *pure* core.
//
// WHY a bridge at all (ADR-0466 + its wake-64 addendum): in an XRSIM run
// (``RIDDICKVR_SIM=1``) the hook must never inject desktop input, and both
// input-free routes to a live scene are measured dead -- posted window messages
// (`WM_KEY*` to the game window: the engine reads DirectInput / Raw Input, not
// the message queue) and the engine's own command line (`+loadgame Checkpoint`,
// `+quit`, and a `COMMANDLINE=` environment-file override all leave the run on
// the ``PRESS ENTER`` splash). The only remaining route is to fabricate the
// confirm **in-process**, on the engine's own device read path.
//
// WHERE it must be installed (wake-65 finding, ADR-0467): MSystem.dll resolves
// its input entry points **once, in a constructor** --
// ``CInputContext_DInput::CInputContext_DInput`` at MSystem.dll ``0x1010b440``
// does ``LoadLibraryA("xinput1_3.dll")`` + resolve ``XInputGetState`` and
// ``LoadLibraryA("dinput.dll")`` + ``GetProcAddress("DirectInputCreateA")``.
// An IAT/``GetProcAddress`` intercept must therefore be installed *before that
// constructor runs*; the hook's deferred init (GH #27, first ``wglCreateContext``)
// is not a safe seam for it. That constraint lives in the (non-pure) interceptor;
// this header is the part that can be decided and tested headlessly.
//
// WHAT is pure here:
//   * the enable gate -- two independent tokens, so a mis-set simulator env can
//     never fabricate player input on its own (requirement 3);
//   * the confirm cadence -- a tap with a hold and a gap, edge-triggered and
//     bounded, off unless the gate is on (requirement 2);
//   * the two device-state encodings the wrappers OR into the engine's own
//     buffers: DirectInput's one-byte-per-scan-code keyboard buffer and
//     XInput's ``XINPUT_GAMEPAD`` button word;
//   * the two ownership predicates the interceptor needs (which resolved proc
//     name is ours, and whether a ``CreateDevice`` GUID is the keyboard);
//   * the vtable slots and env-token parsers the interceptor wires up.
//
// No Win32/GL dependency: unit-tested in ``tests/sim_input_bridge_tests.cpp``.
// The (non-pure) interceptor that consumes this core -- the early
// ``GetProcAddress`` IAT swap on MSystem.dll and the DirectInput/XInput wrappers
// -- lives in ``src/hook/sim_input_bridge_win32.cpp`` (ADR-0468).

#include <cstring>

namespace RiddickVR {

// ---------------------------------------------------------------------------
// Enable gate (GH #80 requirement 3)
// ---------------------------------------------------------------------------
// Two independent tokens. ``simEnvironment`` is the harness's own switch
// (RIDDICKVR_SIM, which already suppresses desktop input injection); ``optIn``
// is the bridge's own switch (RIDDICKVR_SIM_CONFIRM) and is only ever set by a
// scripted harness run. A plain simulator run therefore stays inert -- the
// bridge must never be a side effect of running under the harness.
struct SimInputGate {
    bool simEnvironment = false;  // RIDDICKVR_SIM present
    bool optIn = false;           // RIDDICKVR_SIM_CONFIRM present
};

inline bool SimInputBridgeEnabled(const SimInputGate& gate) {
    return gate.simEnvironment && gate.optIn;
}

// ---------------------------------------------------------------------------
// Which device kind the bridge fabricates on
// ---------------------------------------------------------------------------
enum class SimConfirmRoute : unsigned char {
    None = 0,
    Keyboard = 1,  // DirectInput keyboard: DIK_RETURN (the splash prompt)
    Gamepad = 2,   // XInput: START / A
    Both = 3,
};

inline bool SimRouteHasKeyboard(SimConfirmRoute route) {
    return route == SimConfirmRoute::Keyboard || route == SimConfirmRoute::Both;
}

inline bool SimRouteHasGamepad(SimConfirmRoute route) {
    return route == SimConfirmRoute::Gamepad || route == SimConfirmRoute::Both;
}

// Case-insensitive ASCII equality (no <windows.h> here; ``_stricmp`` is not
// available to the headless test build).
inline bool SimTokenEquals(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// Parse ``RIDDICKVR_SIM_CONFIRM``'s *value* (the token that opted in) into the
// route the interceptor fabricates on. An empty token is a bare opt-in and
// keeps ``fallback``; an unrecognised token also keeps ``fallback`` (a typo
// must neither silently disable the confirm nor widen the route beyond what was
// asked for). ``none``/``off`` is the explicit opt-out for the route while
// leaving the bridge installed-and-idle.
inline SimConfirmRoute SimRouteFromToken(const char* token,
                                         SimConfirmRoute fallback = SimConfirmRoute::Both) {
    if (!token || !*token) return fallback;
    if (SimTokenEquals(token, "keyboard")) return SimConfirmRoute::Keyboard;
    if (SimTokenEquals(token, "gamepad")) return SimConfirmRoute::Gamepad;
    if (SimTokenEquals(token, "both")) return SimConfirmRoute::Both;
    if (SimTokenEquals(token, "none") || SimTokenEquals(token, "off")) {
        return SimConfirmRoute::None;
    }
    return fallback;
}

// The resolved proc names the interceptor takes over. ``GetProcAddress`` is
// case-sensitive and the engine asks for these exact spellings (MSystem.dll
// strings at VA 0x102fc7bc / 0x102fc79c). Exact match only: a near-miss name
// must fall through to the real loader.
inline bool SimBridgeOwnsProcName(const char* procName, SimConfirmRoute route) {
    if (!procName) return false;
    if (SimRouteHasGamepad(route) && std::strcmp(procName, "XInputGetState") == 0) {
        return true;
    }
    if (SimRouteHasKeyboard(route) && std::strcmp(procName, "DirectInputCreateA") == 0) {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Input-proc observability (GH #71 req 1 / #80 follow-up)
// ---------------------------------------------------------------------------
// The device entry points the engine can resolve *through* the swapped
// ``GetProcAddress`` on MSystem.dll (ADR-0467). Kept as an exact-match closed
// set so the interceptor can report which of them the engine actually asked for
// -- and whether the bridge owned the answer -- which is the datum that
// separates "the swap ran after the input context's ctor" from "the engine took
// the other input path". ``GetProcAddress`` is case-sensitive, so a different
// spelling or a near-miss name is deliberately *not* in the set.
inline constexpr int kSimInputProcNameCount = 7;

// The names, in a stable order (the interceptor indexes its per-name flags by
// this order). A closed table, not a substring match: a name like
// ``XInputGetStateEx`` must never be reported as one of ours.
inline const char* SimInputProcNameAt(int index) {
    static const char* const kNames[kSimInputProcNameCount] = {
        "XInputGetState", "XInputSetState", "XInputGetCapabilities", "XInputEnable",
        "DirectInputCreateA", "DirectInputCreateW", "DirectInput8Create",
    };
    return (index >= 0 && index < kSimInputProcNameCount) ? kNames[index] : nullptr;
}

// The table index of ``procName``, or -1 when it is not one of the input procs.
inline int SimInputProcNameIndex(const char* procName) {
    if (!procName) return -1;
    for (int i = 0; i < kSimInputProcNameCount; ++i) {
        if (std::strcmp(procName, SimInputProcNameAt(i)) == 0) return i;
    }
    return -1;
}

inline bool SimBridgeIsInputProcName(const char* procName) {
    return SimInputProcNameIndex(procName) >= 0;
}

// ---------------------------------------------------------------------------
// Poll-log cadence (GH #71 req 1 / #80 follow-up)
// ---------------------------------------------------------------------------
// A wrapper cannot log every poll of an uncapped poller, but it must not hide a
// *small* poll count either: "the engine never read the device" and "the engine
// read it 12 times and stopped" are different answers to the reach question.
// So the first ``kSimPollLogPowerCap`` polls are logged on a doubling cadence
// (1, 2, 4, ... -- two consecutive logged counts are never more than a factor
// of two apart) and every ``kSimPollLogEvery`` after that. Bounded: at 250 Hz
// that is 8 lines in the first half second and 2 lines/s afterwards, and the
// blind spot can never hide more than a doubling of the real count. The
// *absolute* count is also written once from the deferred-init report, which
// runs after the logger exists, so a poll that happened before the logger (its
// first-poll line dropped) is still reported by value.
inline constexpr long kSimPollLogEvery = 120;
inline constexpr long kSimPollLogPowerCap = 128;

// Whether poll number ``polls`` (1-based, as returned by the counter's
// post-increment) deserves a log line.
inline bool SimShouldLogPoll(long polls) {
    if (polls <= 0) return false;
    if (polls <= kSimPollLogPowerCap && (polls & (polls - 1)) == 0) return true;
    return (polls % kSimPollLogEvery) == 0;
}

// ---------------------------------------------------------------------------
// Confirm cadence (GH #80 requirement 2)
// ---------------------------------------------------------------------------
// A menu confirm is an *edge*: the callers tap, never hold. One tap is a press
// of ``holdMs`` followed by a gap until ``periodMs`` elapses, then the next tap,
// for at most ``maxTaps`` taps -- a bounded budget so a scripted run that
// already advanced (or never advances) does not poke the game forever.
struct SimConfirmScheduleConfig {
    unsigned long periodMs = 400;  // tap-to-tap spacing
    unsigned long holdMs = 100;    // how long one tap reads as down
    unsigned int maxTaps = 300;    // 0 = never tap
};

struct SimConfirmState {
    bool started = false;
    unsigned int taps = 0;
    unsigned long lastTapAt = 0;  // 32-bit tick; wrap-safe subtraction below
};

// Effective period: a hold longer than the period would leave no gap, so the
// period is clamped up to the hold.
inline unsigned long SimConfirmPeriod(const SimConfirmScheduleConfig& cfg) {
    return cfg.periodMs > cfg.holdMs ? cfg.periodMs : cfg.holdMs;
}

// Advance the schedule to ``now`` (milliseconds, any 32-bit tick origin) and
// report whether the confirm should read as *down* at this instant. Call once
// per poll; the first call taps immediately.
inline bool SimConfirmTick(unsigned long now,
                           const SimConfirmScheduleConfig& cfg,
                           SimConfirmState& state) {
    if (state.taps >= cfg.maxTaps) return false;  // budget spent (taps>=0 always)
    if (!state.started) {
        state.started = true;
        state.taps = 1;
        state.lastTapAt = now;
        return true;
    }
    const unsigned long since = now - state.lastTapAt;  // wrap-safe
    if (since >= SimConfirmPeriod(cfg)) {
        state.taps += 1;
        state.lastTapAt = now;
        return true;
    }
    return since < cfg.holdMs;
}

// Parse a plain unsigned decimal token (the harness's optional env overrides).
// Rejects an absent/empty token, any non-digit, and anything over 32 bits.
inline bool SimParseUnsigned(const char* token, unsigned long& out) {
    if (!token || !*token) return false;
    unsigned long value = 0;
    for (const char* p = token; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        const unsigned long digit = static_cast<unsigned long>(*p - '0');
        // Reject anything that would wrap 32 bits rather than silently accepting
        // a truncated value (``unsigned long`` is 32-bit on Windows).
        if (value > (0xFFFFFFFFul - digit) / 10ul) return false;
        value = value * 10ul + digit;
    }
    out = value;
    return true;
}

// Build the tap cadence from the three optional env tokens, falling back per
// field: a millisecond field must be present, numeric and non-zero (a zero
// period/hold is nonsense and would leave no gap), while ``maxTaps`` may be an
// explicit 0 -- that is the legal "never tap" budget, not an error.
inline SimConfirmScheduleConfig SimConfirmScheduleFromTokens(
        const char* periodMsToken, const char* holdMsToken, const char* maxTapsToken,
        SimConfirmScheduleConfig fallback) {
    SimConfirmScheduleConfig cfg = fallback;
    unsigned long value = 0;
    if (SimParseUnsigned(periodMsToken, value) && value > 0) cfg.periodMs = value;
    if (SimParseUnsigned(holdMsToken, value) && value > 0) cfg.holdMs = value;
    if (SimParseUnsigned(maxTapsToken, value)) cfg.maxTaps = static_cast<unsigned int>(value);
    return cfg;
}

// ---------------------------------------------------------------------------
// The two vtable slots the interceptor patches in place (the DirectInput route)
// ---------------------------------------------------------------------------
// IDirectInputA / IDirectInputDeviceA (the DirectX-3 ``dinput.dll`` interfaces,
// which is what ``DirectInputCreateA`` hands back): IUnknown occupies 0..2, so
// IDirectInputA::CreateDevice is slot 3 and
// IDirectInputDeviceA::GetDeviceState is slot 9 (GH #80). Pinned here so the
// headless test locks the ABI the interceptor hard-codes.
enum : int {
    kDirectInputCreateDeviceSlot = 3,
    kDirectInputDeviceGetDeviceStateSlot = 9,
};

// ---------------------------------------------------------------------------
// DirectInput keyboard state (the keyboard route)
// ---------------------------------------------------------------------------
// DirectInput hands back one BYTE per scan code (a down key is 0x80), not a bit
// array -- writing a bit here would raise the neighbouring scan code instead.
inline constexpr int kDirectInputKeyboardBytes = 256;
inline constexpr unsigned char kDikReturn = 0x1C;  // DIK_RETURN

inline bool SetDirectInputKey(unsigned char* keyboardState, unsigned int bytes,
                              unsigned char dik, bool down) {
    if (!keyboardState || bytes <= static_cast<unsigned int>(dik)) return false;
    keyboardState[dik] = down ? 0x80 : 0x00;
    return true;
}

// ---------------------------------------------------------------------------
// GUID_SysKeyboard (the ``CreateDevice`` selector the keyboard route owns)
// ---------------------------------------------------------------------------
// {6F1D2B61-D5A0-11CF-BFC7-444553540000} in DirectInput's on-the-wire byte order
// (Data1/Data2/Data3 little-endian, Data4 verbatim). Kept as raw bytes so the
// match is testable without <windows.h>.
inline const unsigned char* SimKeyboardGuidBytes() {
    static const unsigned char kSysKeyboard[16] = {
        0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
        0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00,
    };
    return kSysKeyboard;
}

inline bool SimBridgeIsKeyboardGuid(const void* guid) {
    return guid != nullptr && std::memcmp(guid, SimKeyboardGuidBytes(), 16) == 0;
}

// ---------------------------------------------------------------------------
// XInput gamepad state (the gamepad route)
// ---------------------------------------------------------------------------
// Field-for-field the head of XINPUT_GAMEPAD, so the wrapper can write it
// through a pointer into the engine's own XINPUT_STATE without <windows.h>.
inline constexpr unsigned short kXInputGamepadStart = 0x0010;
inline constexpr unsigned short kXInputGamepadA = 0x1000;

struct XInputGamepadState {
    unsigned short buttons;
    unsigned char leftTrigger;
    unsigned char rightTrigger;
    short thumbLX;
    short thumbLY;
    short thumbRX;
    short thumbRY;
};

// OR the confirm into the state the real function (or a real pad) reported: a
// synthetic press only ever *adds* a button, so a live controller keeps working
// and a released confirm cannot leave a stuck button behind. Returns true when
// this call changed the state.
inline bool ApplySimConfirmToGamepad(XInputGamepadState* pad, bool pressed) {
    if (!pad) return false;
    const unsigned short before = pad->buttons;
    if (pressed) {
        pad->buttons = static_cast<unsigned short>(
            pad->buttons | (kXInputGamepadStart | kXInputGamepadA));
    }
    return pad->buttons != before;
}

// XInput's dwPacketNumber must change whenever the reported state changes: a
// poller that skips an unchanged packet would otherwise never see the press.
inline unsigned int BumpSimPacketNumber(unsigned int packet, bool changed) {
    return changed ? packet + 1u : packet;
}

}  // namespace RiddickVR
