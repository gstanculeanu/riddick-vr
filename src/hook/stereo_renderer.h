#pragma once

#include "opengl_proxy.h"
#include "ipc_bridge.h"
#include "stereo_dual.h"
#include <windows.h>
#include <GL/gl.h>
#include <d3d11.h>
#include <dxgi.h>

namespace RiddickVR {

enum class StereoPass {
    Mono,
    LeftEye,
    RightEye
};

class StereoRenderer {
public:
    static StereoRenderer& Instance();

    bool Initialize(HDC hdc, HGLRC hglrc);
    void Shutdown();

    void OnViewport(GLint x, GLint y, GLsizei width, GLsizei height);
    void OnSwapBuffers(HDC hdc);
    void OnClear(GLbitfield mask);

    // GH #76 (ADR-0502): called from Hook_glClear *before* the engine clears a
    // depth-only mask. At that instant the depth buffer still holds the world
    // pass; the DIBR path snapshots it (throttled, opt-in) so the swap-time
    // reprojection has a real depth source. A no-op unless StereoParallaxDibr=1.
    void OnDepthOnlyClear();

    // GH #76/#90 (ADR-0505): native per-eye render. `OnSceneClear` mirrors an
    // engine clear into the two per-eye scene frames; `OnSceneDraw` re-issues one
    // engine glDrawElements per eye with the per-eye eye-space translation. Both
    // are no-ops unless StereoDualRender=1.
    void OnSceneClear(GLbitfield mask);
    void OnSceneDraw(GLenum mode, GLsizei count, GLenum type, const void* indices);

    StereoPass GetCurrentPass() const { return m_currentPass; }
    void SetCurrentPass(StereoPass pass) { m_currentPass = pass; }

    GLuint GetActiveEyeFbo() const;
    GLuint GetLeftFbo() const { return m_leftFbo; }
    GLuint GetRightFbo() const { return m_rightFbo; }

    bool IsVREnabled() const { return m_vrActive; }
    const RiddickVRSharedState& GetLatestState() const { return m_sharedState; }

    float GetHudConvergenceOffset() const { return m_hudConvergenceOffset; }
    void SetHudConvergenceOffset(float offset) { m_hudConvergenceOffset = offset; }

    bool HasHardwareInterop() const { return m_dxInteropActive; }

    uint32_t GetEyeWidth() const { return m_eyeWidth; }
    uint32_t GetEyeHeight() const { return m_eyeHeight; }

private:
    StereoRenderer() = default;
    ~StereoRenderer() = default;

    bool CreateEyeBuffers(uint32_t width, uint32_t height);
    void DestroyEyeBuffers();

    // GH #43/#44: letterbox/pillarbox scan of the backbuffer (throttled,
    // smoothed) so baked gameplay bars are cropped out; menus use fit instead.
    // Rect in absolute GL-origin (y-up) backbuffer coords.
    void ScanActiveRegion(GLint ox, GLint oy, GLint srcW, GLint srcH);

    // GH #76 (ADR-0503): depth scan over the engine's own framebuffer objects,
    // run at the depth-only clear (the one instant a readback is proven to return
    // real depth). Reads every FBO the engine generated (ADR-0503's
    // Hook_glGenFramebuffers records the ids), logs each one's raw depth, and
    // latches a genuinely near one. Evidence only -- it never changes the
    // reprojection, so it cannot regress the stereo pair. A no-op unless
    // StereoParallaxDibr=1.
    void ScanFramebufferDepths(GLint w, GLint h);
    void BlitEye(bool isLeftEye, GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                 GLint dx0, GLint dy0, GLint dx1, GLint dy1);

    // GH #76/#90 (ADR-0505): native per-eye frames (see stereo_renderer.cpp).
    bool EnsureDualBuffers(GLsizei w, GLsizei h);
    void DestroyDualBuffers();
    void RefreshDualEyeOffsets();
    void BlitDualEye(bool isLeftEye);
    float SampleDualCoverage(bool isLeftEye, float* meanLumaOut = nullptr);
    // GH #76/#90 (ADR-0509): read the engine's output GL state once per span, at the
    // first eligible draw, so the run can say whether the span can produce colour.
    void CaptureDualDrawState();
    // GH #76/#90 (ADR-0511): the engine's own presented frame, sampled in the *same
    // swap* with the same sampler as the eye frames, so a dark engine frame (a dark
    // scene -> the coverage floor is at fault) is separable from a dark eye frame (the
    // re-issue produced no image). Returns the non-black fraction; `lumaOut` gets the
    // mean luminance when non-null. Read-only.
    float SampleEngineFrame(float* lumaOut = nullptr);
    // GH #76/#90 (ADR-0511): read the state a re-issued draw runs under *while the eye
    // FBO is bound* (its draw buffer, viewport, depth test, colour mask), so
    // "engine lit / eye dark" can be attributed to the eye framebuffer. Read-only.
    void CaptureDualEyeState();

    bool InitD3D11Interop(uint32_t width, uint32_t height);
    void ReleaseD3D11Interop();
    void LockSharedTextures();
    void UnlockSharedTextures();

    StereoPass m_currentPass = StereoPass::Mono;
    bool m_vrActive = false;
    bool m_initialized = false;
    bool m_dxInteropActive = false;
    bool m_texturesLocked = false;

    // Eye Framebuffers & Textures
    GLuint m_leftFbo = 0;
    GLuint m_leftColorTex = 0;
    GLuint m_leftDepthRbo = 0;

    GLuint m_rightFbo = 0;
    GLuint m_rightColorTex = 0;
    GLuint m_rightDepthRbo = 0;

    // D3D11 Interop Resources
    ID3D11Device* m_d3d11Device = nullptr;
    ID3D11DeviceContext* m_d3d11Context = nullptr;
    HANDLE m_hDXDevice = nullptr;

    ID3D11Texture2D* m_d3d11LeftTex = nullptr;
    HANDLE m_hDXLeftTex = nullptr;
    HANDLE m_sharedHandleLeft = nullptr;

    ID3D11Texture2D* m_d3d11RightTex = nullptr;
    HANDLE m_hDXRightTex = nullptr;
    HANDLE m_sharedHandleRight = nullptr;

    uint32_t m_eyeWidth = 1920;
    uint32_t m_eyeHeight = 1080;

    // HUD virtual depth offset in normalized coordinates
    float m_hudConvergenceOffset = 0.015f; // ~1.5% screen offset = ~1.8m convergence

    // Window dimensions and viewport origin for spectator mirror and backbuffer blit
    GLsizei m_windowWidth = 1920;
    GLsizei m_windowHeight = 1080;
    GLint m_viewportX = 0;
    GLint m_viewportY = 0;
    GLsizei m_viewportWidth = 0;
    GLsizei m_viewportHeight = 0;

    // GH #44: smoothed active content rect (window coords, y-up) used as the
    // gameplay blit source so baked letterbox/pillarbox bars are cropped out.
    bool m_hasActiveRect = false;
    GLint m_activeX0 = 0, m_activeY0 = 0, m_activeX1 = 0, m_activeY1 = 0;
    uint32_t m_barScanCooldown = 0;

    IpcBridgeClient m_ipcClient;
    RiddickVRSharedState m_sharedState{};

    // GH #76/#90 (ADR-0505): native per-eye render state. `m_dualFbo[0/1]` hold the
    // world pass as seen from each eye; they are created at the engine's viewport
    // size so the engine's own viewport state applies unchanged to a re-issued
    // draw. `m_dualFrameActive` means the frame's scene clear was mirrored;
    // `m_dualEligible` counts re-issued draws (the cheap gate) and `m_dualCoverOk`
    // is the periodically measured pixel-coverage verdict (the fail-safe gate).
    bool m_dualEnabled = false;
    GLuint m_dualFbo[2] = {0, 0};
    GLuint m_dualColor[2] = {0, 0};
    GLuint m_dualDepth[2] = {0, 0};
    GLint m_dualW = 0;
    GLint m_dualH = 0;
    bool m_dualFrameActive = false;
    bool m_dualFrameUsed = false;
    // The 3-D world span: true from a frame/level colour clear (or the previous
    // swap) until the mid-frame depth-only clear that opens the first-person
    // window (ADR-0439/0483). Only draws inside it are candidate world geometry.
    bool m_dualWorldSpan = false;
    unsigned m_dualMirrorClears = 0;
    // GH #76/#90 (ADR-0506): the span's scale bands *and* the reason each refused
    // draw was refused, so a run names the rejected signature rather than only
    // counting it (ADR-0505's ~92% undecodable span draws). ADR-0510 adds the split of
    // its *eligible* draws by the engine's colour-write mask at the draw.
    SpanDrawCensus m_dualCensus;
    // GH #76/#90 (ADR-0509): the span's output GL state (glColorMask/glDepthMask/
    // viewport/scissor/enables), read once at the first eligible draw, plus the eye
    // frames' mean luminance at the periodic coverage sample -- together they say
    // whether the dark eye frames are "the span writes no colour" (a depth/colour-
    // masked stage) or "the re-issue is wrong".
    DualDrawState m_dualState;
    float m_dualMeanL = 0.0f;
    float m_dualMeanR = 0.0f;
    // GH #76/#90 (ADR-0511): the engine's own frame at the coverage sample, the state
    // the re-issued draw ran under inside the eye framebuffer, and the modelview probe
    // -- together they say whether the dark eye frames are "a dark scene" (the fixed
    // coverage floor) or "the re-issue produced no image" (the eye FBO's own state or a
    // non-identity modelview). ADR-0512 turns the first reading into the gate itself:
    // `m_dualCoverOk` is now `DualFrameUsableVsEngine` (each eye graded against
    // `m_dualEngineCover` plus a low absolute floor), not the absolute `0.5` floor.
    float m_dualEngineCover = 0.0f;
    float m_dualEngineLuma = 0.0f;
    DualEyeFrameState m_dualEyeState;
    bool m_dualMvCaptured = false;
    bool m_dualMvPostCaptured = false;
    bool m_dualMvIdentity = false;
    float m_dualMvPre[3] = {0.0f, 0.0f, 0.0f};
    float m_dualMvPost[3] = {0.0f, 0.0f, 0.0f};
    bool m_dualOffValid = false;
    float m_dualOff[2][3] = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    unsigned m_dualDraws = 0;
    unsigned m_dualEligible = 0;
    unsigned m_dualFrames = 0;
    unsigned m_dualFramesUsed = 0;
    int m_dualCoverCountdown = 0;
    // GH #76/#90 (ADR-0512): start *closed* -- the per-eye pair is presented only once
    // a span frame has been sampled and graded (a no-span frame holds the verdict, so
    // the initial value only applies before the first span sample).
    bool m_dualCoverOk = false;
    float m_dualCoverL = 0.0f;
    float m_dualCoverR = 0.0f;
};

} // namespace RiddickVR
