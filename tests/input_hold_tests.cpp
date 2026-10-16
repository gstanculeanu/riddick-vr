// GH #65: unit tests for the held-input scheduler (input_hold.h).
#include "input_hold.h"

#include <cstdio>

using RiddickVR::HeldInput;
using RiddickVR::HeldRelease;
using RiddickVR::InputHoldScheduler;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "input hold test failed: %s\n", message);
        ++g_failures;
    }
}

int main() {
    HeldRelease out[8];

    {
        InputHoldScheduler s;
        Check(s.Press(HeldInput::MouseLeft, 0, 1000, 60), "first press must emit DOWN");
        Check(s.CollectDue(1059, out, 8) == 0, "not due one ms early");
        Check(s.CollectDue(1060, out, 8) == 1, "due exactly at now + hold");
        Check(out[0].kind == HeldInput::MouseLeft && out[0].releaseAt == 1060, "release record is intact");
        Check(s.CollectDue(5000, out, 8) == 0, "a collected slot is cleared");
        Check(s.Press(HeldInput::MouseLeft, 0, 6000, 60), "slot is reusable after release");
    }
    {
        InputHoldScheduler s;
        Check(s.Press(HeldInput::MouseLeft, 0, 1000, 60), "press");
        Check(!s.Press(HeldInput::MouseLeft, 0, 1030, 60), "second press must not emit DOWN again");
        Check(s.CollectDue(1060, out, 8) == 0, "second press extends the release time");
        Check(s.CollectDue(1090, out, 8) == 1, "extended release fires at the new time");
        // A shorter later press never shortens the hold.
        Check(s.Press(HeldInput::MouseRight, 0, 2000, 100), "right press");
        Check(!s.Press(HeldInput::MouseRight, 0, 2010, 10), "short re-press");
        Check(s.CollectDue(2099, out, 8) == 0, "short re-press must not shorten the hold");
    }
    {
        InputHoldScheduler s;
        Check(s.Press(HeldInput::Key, 'A', 0, 50), "key A press");
        Check(s.Press(HeldInput::Key, 'B', 0, 50), "key B is a separate slot");
        Check(!s.Press(HeldInput::Key, 'A', 10, 50), "key A re-press is held");
        Check(s.Press(HeldInput::MouseLeft, 0, 0, 50), "mouse is separate from keys");
        Check(s.CollectDue(50, out, 8) == 2, "A is extended to 60, so only B and mouse are due at 50");
        Check(s.CollectDue(60, out, 8) == 1 && out[0].vk == 'A', "A fires at its extended time");
    }
    {
        InputHoldScheduler s;
        for (unsigned short k = 0; k < 8; ++k) {
            Check(s.Press(HeldInput::Key, static_cast<unsigned short>('A' + k), 0, 40), "fill slot");
        }
        Check(!s.Press(HeldInput::Key, 'Z', 0, 40), "overflow must refuse to press (nothing untracked)");
        Check(s.CollectAll(out, 8) == 8, "CollectAll returns every tracked slot");
        Check(s.CollectAll(out, 8) == 0, "CollectAll clears the table");
        Check(s.Press(HeldInput::Key, 'Z', 0, 40), "table usable after CollectAll");
    }
    {
        InputHoldScheduler s;
        Check(s.Press(HeldInput::MouseLeft, 0, 777, 0), "zero hold press");
        Check(s.CollectDue(777, out, 8) == 1, "zero hold is due immediately");
        Check(s.Press(HeldInput::Key, 'A', 1, 10) && s.Press(HeldInput::Key, 'B', 1, 10), "two presses");
        Check(s.CollectDue(11, out, 1) == 1, "max limits the collected count");
        Check(s.CollectDue(11, out, 1) == 1, "the remainder is collected next call");
    }
    {
        // 32-bit tick wrap: a hold that spans the wrap is not released early or late.
        InputHoldScheduler s;
        const unsigned long near = 0xFFFFFFF0ul;
        Check(s.Press(HeldInput::MouseLeft, 0, near, 0x20), "press near wrap");
        Check(s.CollectDue(near + 0x10, out, 8) == 0, "not due before the wrap point");
        Check(s.CollectDue((near + 0x20) & 0xFFFFFFFFul, out, 8) == 1, "due after the wrap");
    }

    if (g_failures == 0) {
        std::puts("input hold tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
