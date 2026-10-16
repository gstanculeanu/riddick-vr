#pragma once

// GH #64 / GH #46: pure, dependency-free helpers for how the VR hands look.
// Header-only (no windows.h, no GL) so the maths is unit-tested headlessly
// (tests/hand_visuals_tests.cpp).

namespace RiddickVR {

struct HandCueTint {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
};

namespace HandVisualsDetail {

inline float Clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline void LerpToward(HandCueTint& t, float r, float g, float b, float amount) {
    const float a = Clamp01(amount);
    t.r += (r - t.r) * a;
    t.g += (g - t.g) * a;
    t.b += (b - t.b) * a;
}

} // namespace HandVisualsDetail

// GH #64: the procedural hand shows interaction cues as hand-local boxes. Those
// boxes sit at procedural coordinates and float as stray blocks on the native
// mesh, so the native hand shows the same cues as a tint on its skin instead.
// Starts at white (no cue) and blends toward each active cue colour in order:
// amber receiver glow, orange NanoMED cartridge, green heal scanner, cyan grab.
inline HandCueTint ComputeNativeHandCueTint(float interactionGlow, bool holdingCartridge,
                                            bool palmHeal, float grabGlow) {
    using namespace HandVisualsDetail;
    HandCueTint t;
    LerpToward(t, 1.00f, 0.78f, 0.45f, 0.35f * Clamp01(interactionGlow));
    if (holdingCartridge) LerpToward(t, 1.00f, 0.70f, 0.35f, 0.30f);
    if (palmHeal) LerpToward(t, 0.75f, 1.00f, 0.80f, 0.30f);
    LerpToward(t, 0.75f, 0.95f, 1.00f, 0.35f * Clamp01(grabGlow));
    t.r = Clamp01(t.r);
    t.g = Clamp01(t.g);
    t.b = Clamp01(t.b);
    return t;
}

// GH #46: the overlay fades to 0.25 where the engine's own gun/hands dominate
// the view (trigger pulled, two-handed hold). A boxer guard also has both grips
// closed, which used to count as "two-handed" and faded the hands while
// boxing. 1.0 when fading is disabled or in a boxer guard.
inline float ComputeHandFade(float trigger, bool twoHanded, bool isBoxerGuard, int fadeEnabled) {
    if (!fadeEnabled || isBoxerGuard) return 1.0f;
    if (trigger > 0.60f || twoHanded) return 0.25f;
    return 1.0f;
}

} // namespace RiddickVR
