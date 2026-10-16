#pragma once

#include "riddick_vr_protocol.h"
#include <windows.h>
#include <GL/gl.h>
#include "hand_visuals.h"

namespace RiddickVR {

enum class HandSide {
    Left,
    Right
};

class VRHandRenderer {
public:
    static VRHandRenderer& Instance();

    // Renders both 3D prisoner VR hands for the specified stereoscopic eye pass
    void RenderHands(const RiddickVRSharedState& state, bool isLeftEye, uint32_t eyeWidth, uint32_t eyeHeight);

    // GH #46: per-hand fade factor 0..1 (1 = fully visible, 0 = faded out).
    // Per-hand opacity factor 0..1 (1 = fully visible, 0 = transparent).
    void SetHandFade(float leftFade, float rightFade);
    // 0..1 fingertip cue: dim while holding a rung, bright for the hand expected
    // to complete a mantle during the grace window.
    void SetLadderCue(float leftCue, float rightCue);

private:
    VRHandRenderer() = default;
    ~VRHandRenderer() = default;

    // Renders an individual prisoner hand (left or right)
    // interactionGlow 0..1 = reload/receiver proximity highlight, holdingCartridge draws NanoMED cell, palmHeal draws scanner glow
    // grabGlow 0..1 = object/person interact-volume proximity (vents, terminals, throat-grip corridor)
    void DrawTacticalHand(HandSide side, float grip, float trigger, bool isBoxerGuard, uint32_t buttons,
                           float interactionGlow, bool holdingCartridge, bool palmHeal, float grabGlow = 0.0f);

    // Procedural prisoner hand geometry components (slim Butcher Bay inmate look)
    void DrawForearmGauntlet(HandSide side);
    void DrawPalmAndKnuckleGuard(HandSide side);
    void DrawArticulatedFinger(HandSide side, int fingerIndex, float curl, float splayAngle, float lengthScale);
    void DrawArticulatedThumb(HandSide side, float curl);

    // GH #46: draws the baked native Riddick arm/hand mesh for this hand.
    // Returns false (and draws nothing) when the enable flag is off, the mesh
    // is missing, or the placement degenerates -- the caller then falls back to
    // the procedural primitives. Only active when VRConfig::nativeHands is set.
    // GH #64: rendered two-sided (culling off + GL two-sided lighting) with the
    // baked normals driving per-vertex GL lighting, and the on-headset
    // orientation correction (NativeHandYaw/Pitch/Roll) applied hand-local.
    // GH #64: `cue` tints the skin to show interaction cues (the procedural
    // hand-local cue boxes are not drawn on the native mesh).
    bool DrawNativeHand(HandSide side, const HandCueTint& cue);

    // GH #61: create (once) the GL texture for the baked Riddick skin diffuse
    // (native_hand_texture.h). Returns false while the texture is unavailable
    // (disabled, no GL entry points, or no baked asset) so DrawNativeHand falls
    // back to its flat skin-tone tint under the GL lighting.
    bool EnsureNativeHandTexture();
    void VerifyNativeHandTexture();
    GLuint UploadNativeHandTexture();
    void SetNativeHandTextureParams();
    void LogNativeHandGlStateOnce();
    void SanitizeTextureStateForHands();

    // Geometry utility primitives (colors multiplied by active hand fade)
    void DrawBeveledBox(float x0, float y0, float z0, float x1, float y1, float z1, float r, float g, float b);
    void DrawTaperedCylinder(float z0, float r0, float z1, float r1, int slices, float r, float g, float b);
    void DrawArmSegment(const RiddickVRVector3& start, const RiddickVRVector3& end,
                        float startRadius, float endRadius, float r, float g, float b);

    float m_leftFade = 1.0f;
    float m_rightFade = 1.0f;
    float m_currentFade = 1.0f;
    float m_leftLadderCue = 0.0f;
    float m_rightLadderCue = 0.0f;
    bool m_useAlphaFade = false;
    uint32_t m_lastIkFrame = 0xFFFFFFFFu;
    RiddickVRVector3 m_leftElbow{};
    RiddickVRVector3 m_rightElbow{};
    RiddickVRVector3 m_leftArmWrist{};
    RiddickVRVector3 m_rightArmWrist{};
    bool m_hasLeftElbow = false;
    bool m_hasRightElbow = false;
    // GH #61: baked native-hand diffuse texture (lazily created on first use).
    GLuint m_nativeHandTexture = 0;
    bool m_nativeTextureTried = false;
    bool m_nativeTextureReady = false;
    uint32_t m_currentFrameIndex = 0;
    uint32_t m_lastTexCheckFrame = 0xFFFFFFFFu;
    uint32_t m_lastClobberLogFrame = 0xFFFFFFFFu;
    bool m_glStateLogged = false;
    int m_prevActiveUnit = 0x84C0;
};

} // namespace RiddickVR
