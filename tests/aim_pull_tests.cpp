// GH #50 / #51: unit tests for the aim-pull helper (aim_pull.h).
#include "aim_pull.h"

#include <cmath>
#include <cstdio>

using RiddickVR::AimPullInputs;
using RiddickVR::AimPullResult;
using RiddickVR::ComputeAimPull;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "aim pull test failed: %s\n", message);
        ++g_failures;
    }
}

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

int main() {
    // A steady (zero-delta) arm, even far off-centre, injects nothing.
    {
        AimPullInputs in;
        in.aimHeld = true;
        in.deltaYaw = 0.0f;
        in.deltaPitch = 0.0f;
        AimPullResult r = ComputeAimPull(in);
        Check(r.pullYaw == 0.0f && r.pullPitch == 0.0f, "a steady arm must pull nothing");
    }
    // Fist guard + left trigger (aimHeld) never steers.
    {
        AimPullInputs in;
        in.aimHeld = true;
        in.guardPoseLike = true;
        in.deltaYaw = 0.03f;
        in.deltaPitch = -0.02f;
        AimPullResult r = ComputeAimPull(in);
        Check(!r.active && r.pullYaw == 0.0f && r.pullPitch == 0.0f && r.weight == 0.0f,
              "guard pose with the left trigger held must not pull");
    }
    // Physical block never steers.
    {
        AimPullInputs in;
        in.isPhysicalADS = true;
        in.physicalBlock = true;
        in.deltaYaw = 0.03f;
        AimPullResult r = ComputeAimPull(in);
        Check(!r.active && r.pullYaw == 0.0f && r.weight == 0.0f, "physical block must not pull");
    }
    // Reloading never steers.
    {
        AimPullInputs in;
        in.twoHanded = true;
        in.reloading = true;
        in.deltaYaw = 0.03f;
        Check(!ComputeAimPull(in).active, "reloading must not pull");
    }
    // ADS weight 0.12, hip/two-handed/aim-held weight 0.08.
    {
        AimPullInputs in;
        in.isPhysicalADS = true;
        in.deltaYaw = 0.01f;
        AimPullResult r = ComputeAimPull(in);
        Check(r.active && Near(r.weight, 0.12f) && Near(r.pullYaw, 0.01f), "ADS weight is 0.12");
        in.isPhysicalADS = false;
        in.twoHanded = true;
        r = ComputeAimPull(in);
        Check(r.active && Near(r.weight, 0.08f), "two-handed weight is 0.08");
        in.twoHanded = false;
        in.aimHeld = true;
        r = ComputeAimPull(in);
        Check(r.active && Near(r.weight, 0.08f), "aim-held weight is 0.08");
    }
    // Not aiming: inactive.
    {
        AimPullInputs in;
        in.deltaYaw = 0.03f;
        AimPullResult r = ComputeAimPull(in);
        Check(!r.active && r.weight == 0.0f && r.pullYaw == 0.0f, "no aim intent means no pull");
    }
    // Clamp at 0.05 rad/frame in both directions and both axes.
    {
        AimPullInputs in;
        in.aimHeld = true;
        in.deltaYaw = 0.40f;
        in.deltaPitch = -0.40f;
        AimPullResult r = ComputeAimPull(in);
        Check(Near(r.pullYaw, 0.05f) && Near(r.pullPitch, -0.05f), "pull must clamp to 0.05 rad");
        in.deltaYaw = -0.40f;
        in.deltaPitch = 0.40f;
        r = ComputeAimPull(in);
        Check(Near(r.pullYaw, -0.05f) && Near(r.pullPitch, 0.05f), "negative yaw and positive pitch clamp too");
    }
    // Deadzone: below 0.0015 is zero, at/above passes through.
    {
        AimPullInputs in;
        in.aimHeld = true;
        in.deltaYaw = 0.0014f;
        in.deltaPitch = -0.0014f;
        AimPullResult r = ComputeAimPull(in);
        Check(r.pullYaw == 0.0f && r.pullPitch == 0.0f, "deltas inside the deadzone are dropped");
        in.deltaYaw = 0.0015f;
        r = ComputeAimPull(in);
        Check(Near(r.pullYaw, 0.0015f), "a delta at the deadzone edge passes");
    }
    // Melee equipped: no pull even while aiming.
    {
        AimPullInputs in;
        in.aimHeld = true;
        in.meleeEquipped = true;
        in.deltaYaw = 0.03f;
        in.deltaPitch = 0.03f;
        AimPullResult r = ComputeAimPull(in);
        Check(r.pullYaw == 0.0f && r.pullPitch == 0.0f && r.weight == 0.0f, "melee equipped must not pull");
    }
    // NaN delta is dropped.
    {
        AimPullInputs in;
        in.aimHeld = true;
        in.deltaYaw = std::nanf("");
        AimPullResult r = ComputeAimPull(in);
        Check(r.pullYaw == 0.0f, "NaN delta must not propagate");
    }

    if (g_failures == 0) {
        std::puts("aim pull tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
