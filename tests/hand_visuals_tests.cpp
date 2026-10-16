// Headless tests for the hand visual helpers (GH #64 cue tint, GH #46 fade).

#include "hand_visuals.h"

#include <cstdio>

using namespace RiddickVR;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "hand visuals: %s\n", what);
        ++g_failures;
    }
}

bool Near(float a, float b) {
    float d = a - b;
    return d > -1e-5f && d < 1e-5f;
}

} // namespace

int main() {
    // No cue is the identity tint.
    {
        HandCueTint t = ComputeNativeHandCueTint(0.0f, false, false, 0.0f);
        Check(Near(t.r, 1.0f) && Near(t.g, 1.0f) && Near(t.b, 1.0f), "zero cues give white");
    }
    // Glow is monotonic: more glow, less blue/green, red stays 1.
    {
        HandCueTint a = ComputeNativeHandCueTint(0.25f, false, false, 0.0f);
        HandCueTint b = ComputeNativeHandCueTint(0.75f, false, false, 0.0f);
        HandCueTint c = ComputeNativeHandCueTint(1.0f, false, false, 0.0f);
        Check(a.b > b.b && b.b > c.b, "interaction glow darkens blue monotonically");
        Check(a.g > b.g && b.g > c.g, "interaction glow darkens green monotonically");
        Check(Near(c.r, 1.0f), "amber keeps full red");
        Check(Near(c.b, 1.0f + (0.45f - 1.0f) * 0.35f), "full glow reaches the 35% amber blend");
        HandCueTint g1 = ComputeNativeHandCueTint(0.0f, false, false, 0.2f);
        HandCueTint g2 = ComputeNativeHandCueTint(0.0f, false, false, 0.9f);
        Check(g1.r > g2.r, "grab glow lowers red monotonically");
    }
    // Clamped: out-of-range glow behaves like the range ends.
    {
        HandCueTint hi = ComputeNativeHandCueTint(5.0f, true, true, 5.0f);
        HandCueTint max = ComputeNativeHandCueTint(1.0f, true, true, 1.0f);
        HandCueTint lo = ComputeNativeHandCueTint(-3.0f, false, false, -3.0f);
        Check(Near(hi.r, max.r) && Near(hi.g, max.g) && Near(hi.b, max.b), "glow above 1 clamps");
        Check(Near(lo.r, 1.0f) && Near(lo.g, 1.0f) && Near(lo.b, 1.0f), "negative glow clamps to no cue");
        Check(hi.r >= 0.0f && hi.r <= 1.0f && hi.g >= 0.0f && hi.g <= 1.0f && hi.b >= 0.0f && hi.b <= 1.0f,
              "tint stays inside [0,1]");
    }
    // Combinable: cues stack and each active cue changes the result.
    {
        HandCueTint none = ComputeNativeHandCueTint(0.0f, false, false, 0.0f);
        HandCueTint cart = ComputeNativeHandCueTint(0.0f, true, false, 0.0f);
        HandCueTint heal = ComputeNativeHandCueTint(0.0f, false, true, 0.0f);
        HandCueTint both = ComputeNativeHandCueTint(0.0f, true, true, 0.0f);
        Check(cart.b < none.b, "cartridge tints toward orange");
        Check(heal.r < none.r, "heal tints toward green");
        Check(!(Near(both.r, cart.r) && Near(both.g, cart.g) && Near(both.b, cart.b)) &&
              !(Near(both.r, heal.r) && Near(both.g, heal.g) && Near(both.b, heal.b)),
              "cartridge plus heal differs from either alone");
    }

    // GH #46 fade.
    Check(Near(ComputeHandFade(0.0f, true, true, 1), 1.0f), "boxing guard stays opaque");
    Check(Near(ComputeHandFade(0.9f, true, true, 1), 1.0f), "boxing guard stays opaque even with trigger");
    Check(Near(ComputeHandFade(0.7f, false, false, 1), 0.25f), "trigger 0.7 without guard fades");
    Check(Near(ComputeHandFade(0.0f, true, false, 1), 0.25f), "two-handed hold fades");
    Check(Near(ComputeHandFade(0.6f, false, false, 1), 1.0f), "trigger exactly 0.60 does not fade");
    Check(Near(ComputeHandFade(0.9f, true, false, 0), 1.0f), "fade disabled gives 1.0");
    Check(Near(ComputeHandFade(0.0f, false, false, 1), 1.0f), "open hand is opaque");

    if (g_failures != 0) {
        std::fprintf(stderr, "hand visuals: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("hand visuals tests passed\n");
    return 0;
}
