#include "combat_stance.h"
#include <cstdio>

static RiddickVRControllerInput Controller(float trigger) {
    RiddickVRControllerInput c{};
    c.isConnected = true;
    c.gripPose.isValid = true;
    c.trigger = trigger;
    return c;
}

static int Fail(const char* message) {
    std::fprintf(stderr, "combat stance test failed: %s\n", message);
    return 1;
}

int main() {
    using RiddickVR::CombatStanceState;
    using RiddickVR::UpdateCombatStance;
    CombatStanceState state{};
    auto left = Controller(0.90f);
    auto right = Controller(0.0f);

    if (UpdateCombatStance(state, left, right, 100).active)
        return Fail("one closed hand entered combat");

    right.trigger = 0.90f;
    auto first = UpdateCombatStance(state, left, right, 200);
    if (first.active || first.entered) return Fail("combat ignored its hold debounce");
    auto before = UpdateCombatStance(state, left, right, 299);
    if (before.active || before.entered) return Fail("combat entered before 100ms");
    auto entered = UpdateCombatStance(state, left, right, 300);
    if (!entered.active || !entered.entered) return Fail("both triggers did not enter combat at 100ms");

    left.trigger = 0.70f;
    auto held = UpdateCombatStance(state, left, right, 301);
    if (!held.active || held.exited) return Fail("hysteresis exited above the release threshold");
    left.trigger = 0.59f;
    auto exited = UpdateCombatStance(state, left, right, 302);
    if (exited.active || !exited.exited) return Fail("dropping either trigger did not exit");

    left.trigger = right.trigger = 0.90f;
    UpdateCombatStance(state, left, right, 0xFFFFFFF0u);
    auto wrapped = UpdateCombatStance(state, left, right, 0x00000054u);
    if (!wrapped.active || !wrapped.entered) return Fail("millisecond counter wrap broke debounce");

    right.isConnected = false;
    auto disconnected = UpdateCombatStance(state, left, right, 0x00000055u);
    if (disconnected.active || !disconnected.exited) return Fail("controller disconnect left combat latched");

    // GH #52: jitter around the enter threshold never latches the stance.
    {
        CombatStanceState s{};
        auto l = Controller(0.90f);
        auto r = Controller(0.90f);
        UpdateCombatStance(s, l, r, 1000);
        l.trigger = 0.84f;  // dips below enter before the hold completes
        UpdateCombatStance(s, l, r, 1050);
        l.trigger = 0.90f;
        auto again = UpdateCombatStance(s, l, r, 1100);
        if (again.active || again.entered) return Fail("a dip below enter must restart the hold timer");
        auto done = UpdateCombatStance(s, l, r, 1200);
        if (!done.active) return Fail("hold restarted at 1100 should enter at 1200");
    }
    // Invalid pose blocks entry and exits an active stance.
    {
        CombatStanceState s{};
        auto l = Controller(0.95f);
        auto r = Controller(0.95f);
        r.gripPose.isValid = false;
        UpdateCombatStance(s, l, r, 10);
        if (UpdateCombatStance(s, l, r, 500).active) return Fail("an invalid pose entered combat");
        r.gripPose.isValid = true;
        UpdateCombatStance(s, l, r, 600);
        if (!UpdateCombatStance(s, l, r, 700).active) return Fail("valid pose should enter");
        r.gripPose.isValid = false;
        auto lost = UpdateCombatStance(s, l, r, 701);
        if (lost.active || !lost.exited) return Fail("losing a pose must exit combat");
    }
    // Custom thresholds.
    {
        using RiddickVR::CombatStanceThresholds;
        CombatStanceThresholds th;
        th.enter = 0.50f;
        th.exit = 0.30f;
        th.holdMs = 20;
        CombatStanceState s{};
        auto l = Controller(0.55f);
        auto r = Controller(0.55f);
        UpdateCombatStance(s, l, r, 100, th);
        if (UpdateCombatStance(s, l, r, 119, th).active) return Fail("custom hold entered early");
        if (!UpdateCombatStance(s, l, r, 120, th).active) return Fail("custom hold did not enter at 20ms");
        l.trigger = 0.35f;
        if (!UpdateCombatStance(s, l, r, 121, th).active) return Fail("custom exit left too early");
        l.trigger = 0.29f;
        if (UpdateCombatStance(s, l, r, 122, th).active) return Fail("custom exit did not leave");
        // Default thresholds are unchanged.
        CombatStanceThresholds def;
        if (def.enter != 0.85f || def.exit != 0.60f || def.holdMs != 100)
            return Fail("default stance thresholds drifted");
    }
    // exit >= enter is clamped to enter - 0.05 so the stance cannot flicker.
    {
        using RiddickVR::CombatStanceThresholds;
        CombatStanceThresholds bad;
        bad.enter = 0.70f;
        bad.exit = 0.90f;  // invalid: above enter
        bad.holdMs = 0;
        CombatStanceState s{};
        auto l = Controller(0.75f);
        auto r = Controller(0.75f);
        UpdateCombatStance(s, l, r, 5, bad);
        if (!UpdateCombatStance(s, l, r, 6, bad).active) return Fail("zero hold should enter on the second frame");
        // Triggers 0.66 are above the clamped exit (0.65): stay. Unclamped 0.90 would exit.
        l.trigger = r.trigger = 0.66f;
        if (!UpdateCombatStance(s, l, r, 7, bad).active) return Fail("exit >= enter must be clamped below enter");
        l.trigger = 0.64f;
        if (UpdateCombatStance(s, l, r, 8, bad).active) return Fail("clamped exit should release below enter - 0.05");
    }

    std::puts("combat stance tests passed");
    return 0;
}
