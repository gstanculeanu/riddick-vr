#pragma once

#include "riddick_vr_protocol.h"
#include <cstdint>

namespace RiddickVR {

struct CombatStanceState {
    bool active = false;
    bool closingBoth = false;
    uint32_t closeStartTime = 0;
};

struct CombatStanceIntent {
    bool entered = false;
    bool exited = false;
    bool active = false;
};

constexpr float ENTER_THRESHOLD = 0.85f;
constexpr float EXIT_THRESHOLD = 0.60f;
constexpr uint32_t ENTER_HOLD_MS = 100;

// GH #52: INI-tunable stance thresholds. Defaults are the constants above.
// `exit` must stay below `enter` or the stance would flicker; an invalid pair
// is clamped to enter - 0.05.
struct CombatStanceThresholds {
    float enter = ENTER_THRESHOLD;
    float exit = EXIT_THRESHOLD;
    uint32_t holdMs = ENTER_HOLD_MS;
};

inline CombatStanceIntent UpdateCombatStance(CombatStanceState& state,
        const RiddickVRControllerInput& left, const RiddickVRControllerInput& right,
        uint32_t now, const CombatStanceThresholds& thresholds) {
    const float enterThreshold = thresholds.enter;
    const float exitThreshold = (thresholds.exit >= thresholds.enter)
        ? (thresholds.enter - 0.05f) : thresholds.exit;
    const uint32_t holdMs = thresholds.holdMs;
    CombatStanceIntent out;
    const bool tracking = left.isConnected && right.isConnected &&
                          left.gripPose.isValid && right.gripPose.isValid;
    if (state.active) {
        if (!tracking || left.trigger < exitThreshold || right.trigger < exitThreshold) {
            state.active = false;
            state.closingBoth = false;
            state.closeStartTime = 0;
            out.exited = true;
        }
    } else if (tracking && left.trigger >= enterThreshold && right.trigger >= enterThreshold) {
        if (!state.closingBoth) {
            state.closingBoth = true;
            state.closeStartTime = now;
        } else if (now - state.closeStartTime >= holdMs) {
            state.active = true;
            state.closingBoth = false;
            state.closeStartTime = 0;
            out.entered = true;
        }
    } else {
        state.closingBoth = false;
        state.closeStartTime = 0;
    }
    out.active = state.active;
    return out;
}

inline CombatStanceIntent UpdateCombatStance(CombatStanceState& state,
        const RiddickVRControllerInput& left, const RiddickVRControllerInput& right,
        uint32_t now) {
    return UpdateCombatStance(state, left, right, now, CombatStanceThresholds{});
}

} // namespace RiddickVR
