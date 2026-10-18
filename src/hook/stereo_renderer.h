#pragma once

#include "opengl_proxy.h"
#include "ipc_bridge.h"
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
    void BlitEye(bool isLeftEye, GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                 GLint dx0, GLint dy0, GLint dx1, GLint dy1);

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
};

} // namespace RiddickVR
