// GH #62: unit tests for the front-of-HMD block/guard envelope (combat_guard.h).
//
// The envelope decides (a) whether the fists are held in front of the face to
// block and (b) whether a swing is masked by the guard gate. These tests pin
// the head-local window semantics and the fix that a below-the-view hold no
// longer counts as a guard (which used to swallow forward/hook swings).
#include "combat_guard.h"

#include <cstdio>

using RiddickVR::GuardEnvelope;
using RiddickVR::GuardHandInWindow;
using RiddickVR::InGuardEnvelope;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "combat guard test failed: %s\n", message);
        ++g_failures;
    }
}

int main() {
    const GuardEnvelope env;  // defaults (ADR-0400 front-of-HMD window)

    // Defaults are the documented window.
    Check(env.yMin == -0.16f && env.yMax == 0.32f, "default Y window drifted");
    Check(env.zMin == -0.60f && env.zMax == -0.12f, "default Z window drifted");
    Check(env.maxHandSpan == 0.50f, "default hand span drifted");

    // A raised, steady, front-of-face two-hand pose blocks.
    Check(InGuardEnvelope(env, 0.00f, -0.30f, 0.05f, -0.32f, 0.20f),
          "a front-of-face two-hand guard must be inside the envelope");

    // Boundary values are inclusive.
    Check(GuardHandInWindow(env, env.yMin, env.zMin), "yMin/zMin must be inclusive");
    Check(GuardHandInWindow(env, env.yMax, env.zMax), "yMax/zMax must be inclusive");

    // GH #62 regression: a LOW rest hold (below the view) must NOT count as a
    // guard. It was inside the old loose gate volume (y >= -0.42), which is why
    // swings were masked; the tight envelope rejects it.
    Check(!InGuardEnvelope(env, -0.30f, -0.30f, -0.30f, -0.30f, 0.20f),
          "a below-the-view low hold must not count as a guard");

    // Hands pressed against the face (inside the view) are not a guard either.
    Check(!InGuardEnvelope(env, 0.00f, -0.05f, 0.00f, -0.08f, 0.20f),
          "hands inside the view (z >= zMax) must not count as a guard");

    // Too far forward (arms extended) is a strike, not a guard.
    Check(!InGuardEnvelope(env, 0.00f, -0.75f, 0.00f, -0.70f, 0.20f),
          "hands beyond zMin must not count as a guard");

    // One hand outside the window fails the whole pose.
    Check(!InGuardEnvelope(env, 0.00f, -0.30f, -0.30f, -0.30f, 0.20f),
          "one low hand must fail the two-hand guard pose");

    // Hand span: inside the shield width passes, at/over it fails, sign-agnostic.
    Check(InGuardEnvelope(env, 0.0f, -0.30f, 0.0f, -0.30f, 0.49f),
          "a shield-width stance inside maxHandSpan must pass");
    Check(!InGuardEnvelope(env, 0.0f, -0.30f, 0.0f, -0.30f, 0.50f),
          "a span at maxHandSpan must fail (strict upper bound)");
    Check(InGuardEnvelope(env, 0.0f, -0.30f, 0.0f, -0.30f, -0.49f),
          "hand span must be sign-insensitive (left/right order)");
    Check(!InGuardEnvelope(env, 0.0f, -0.30f, 0.0f, -0.30f, -0.50f),
          "negative span at the bound must still fail");

    // A tuned-in-place envelope is honoured.
    GuardEnvelope tuned = env;
    tuned.yMin = -0.30f;  // deliberately lowered for a seated player
    Check(InGuardEnvelope(tuned, -0.25f, -0.30f, -0.25f, -0.30f, 0.20f),
          "a tuned Y floor must be honoured");

    // An inverted window (min > max) can never match.
    GuardEnvelope inverted = env;
    inverted.yMin = 0.20f;
    inverted.yMax = -0.10f;
    Check(!InGuardEnvelope(inverted, 0.0f, -0.30f, 0.0f, -0.30f, 0.20f),
          "an inverted Y window must never match");

    // GH #62: engage/release hysteresis.
    {
        using RiddickVR::GuardPoseState;
        using RiddickVR::GuardPoseTiming;
        using RiddickVR::UpdateGuardPose;
        const GuardPoseTiming timing;
        Check(timing.engageMs == 40 && timing.releaseMs == 80 && timing.steadySpeed == 1.4f,
              "default guard timing drifted");

        // Engage boundary: 39 ms is not enough, 40 ms is.
        GuardPoseState a;
        Check(!UpdateGuardPose(a, true, false, 1000, timing), "first detection starts the timer only");
        Check(!UpdateGuardPose(a, true, false, 1039, timing), "39 ms must not engage");
        Check(UpdateGuardPose(a, true, false, 1040, timing), "40 ms must engage");

        // Jitter shorter than the release buffer keeps the block engaged.
        Check(UpdateGuardPose(a, false, false, 1100, timing), "pose loss starts the release timer, still engaged");
        Check(UpdateGuardPose(a, false, false, 1179, timing), "79 ms of pose loss stays engaged");
        Check(UpdateGuardPose(a, true, false, 1180, timing), "pose regained before 80 ms stays engaged");
        Check(a.releaseStart == 0, "regaining the pose cancels the release timer");
        Check(UpdateGuardPose(a, false, false, 1200, timing), "a second dropout restarts the 80 ms window");
        Check(UpdateGuardPose(a, false, false, 1279, timing), "79 ms after the restart is still engaged");
        Check(!UpdateGuardPose(a, false, false, 1280, timing), "80 ms of pose loss releases");

        // A punch releases at 0 ms.
        GuardPoseState b;
        UpdateGuardPose(b, true, false, 2000, timing);
        UpdateGuardPose(b, true, false, 2050, timing);
        Check(b.engaged, "engaged before the punch");
        Check(!UpdateGuardPose(b, false, true, 2051, timing), "a throwing punch releases instantly");
        Check(b.releaseStart == 0 && b.holdStart == 0, "punch release clears both timers");

        // Moving hands never engage (pose detector reports false every frame).
        GuardPoseState c;
        bool ever = false;
        for (unsigned long t = 3000; t < 3500; t += 10) ever = ever || UpdateGuardPose(c, false, false, t, timing);
        Check(!ever, "a pose that is never detected must never engage");

        // A pose that flickers off before 40 ms restarts the engage timer.
        GuardPoseState d;
        UpdateGuardPose(d, true, false, 4000, timing);
        UpdateGuardPose(d, false, false, 4030, timing);
        Check(!UpdateGuardPose(d, true, false, 4060, timing), "engage timer restarts after a flicker");
        Check(!UpdateGuardPose(d, true, false, 4099, timing), "39 ms after the restart does not engage");
        Check(UpdateGuardPose(d, true, false, 4100, timing), "40 ms after the restart engages");

        // Custom timing is honoured.
        GuardPoseTiming slow;
        slow.engageMs = 100;
        slow.releaseMs = 200;
        GuardPoseState e;
        UpdateGuardPose(e, true, false, 0 + 5000, slow);
        Check(!UpdateGuardPose(e, true, false, 5099, slow), "custom engage delay");
        Check(UpdateGuardPose(e, true, false, 5100, slow), "custom engage delay reached");
        UpdateGuardPose(e, false, false, 5200, slow);
        Check(UpdateGuardPose(e, false, false, 5399, slow), "custom release delay");
        Check(!UpdateGuardPose(e, false, false, 5400, slow), "custom release delay reached");
    }

    if (g_failures == 0) {
        std::puts("combat guard tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
