#pragma once

#include "riddick_vr_protocol.h"
#include "ipc_bridge.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <vector>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace RiddickVR {

struct OpenXRSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<XrSwapchainImageD3D11KHR> images;
};

class OpenXRManager {
public:
    OpenXRManager();
    ~OpenXRManager();

    bool Initialize();
    bool Validate(std::string& outReport);
    void Shutdown();

    void RunLoop(IpcBridgeHost& ipcHost, bool autoExitWithGame = false);
    void RequestExit() { m_running = false; }

    bool IsSessionActive() const { return m_sessionActive; }
    bool IsOpenXRLoaded() const { return m_openxrAvailable; }

    bool TryConnectOpenXR();
    void TeardownSession();

    void SetMockMode(bool enable, const std::string& dumpDir = "", uint32_t maxFrames = 0, uint32_t dumpInterval = 10) {
        m_mockMode = enable;
        m_dumpDir = dumpDir;
        m_maxFrames = maxFrames;
        m_dumpInterval = dumpInterval;
    }
    // GH #69 requirement 1: only dump frames the hook reports as 3D gameplay,
    // so the XRSIM capture window is live gameplay rather than menu/loading.
    void SetDumpGameplayOnly(bool enable) { m_dumpGameplayOnly = enable; }
    // GH #48: file-driven scenario trajectories (tools/scenarios/*.json).
    // New scenarios need no rebuild: host interpolates keyframes per frame.
    struct ScenarioKey {
        uint32_t frame = 0;
        float lx = 0, ly = 0, lz = 0, lgrip = 0, ltrig = 0;
        uint32_t lbtn = 0;
        float rx = 0, ry = 0, rz = 0, rgrip = 0, rtrig = 0;
        uint32_t rbtn = 0;
        float stickLX = 0, stickLY = 0, stickRX = 0;
        float hyaw = 0, hpitch = 0; // fixed head pose (deg); 999 = keep oscillation
    };
    void SetScenarioFile(const std::string& path) { m_scenarioPath = path; }
    // GH #68: by default the scenario timeline starts only once the game shows
    // real gameplay (not cinema/menu); false restores frame-0 start.
    void SetScenarioStartOnGameplay(bool enable) { m_scenarioStartOnGameplay = enable; }
    bool LoadScenario(const std::string& path);
    void ApplyScenarioFrame(RiddickVRSharedState& state, uint32_t frame);
    void SetAmmoQuadCrop(float x, float y, float width, float height) {
        m_ammoQuadCropW = (width < 0.01f) ? 0.01f : (width > 1.0f ? 1.0f : width);
        m_ammoQuadCropH = (height < 0.01f) ? 0.01f : (height > 1.0f ? 1.0f : height);
        m_ammoQuadCropX = (x < 0.0f) ? 0.0f : (x > 1.0f - m_ammoQuadCropW ? 1.0f - m_ammoQuadCropW : x);
        m_ammoQuadCropY = (y < 0.0f) ? 0.0f : (y > 1.0f - m_ammoQuadCropH ? 1.0f - m_ammoQuadCropH : y);
    }
    bool IsMockMode() const { return m_mockMode; }
    // GH #79: write a BMP from an already-mapped private eye copy (the header
    // layout + row swizzle are the pure helpers in src/host/bmp_writer.h). The
    // caller stages both eyes once (StageEyePair) and samples + saves from that
    // same copy, instead of re-reading the shared texture.
    static bool WriteBmpFromMapped(const uint8_t* base, uint32_t rowPitch,
                                   uint32_t width, uint32_t height,
                                   const std::string& filename);

private:
    bool LoadLoader();
    bool CreateInstance();
    bool InitializeD3D11Device();
    bool InitializeSession();
    bool CreateSpaces();
    bool CreateActions();
    bool CreateSwapchains();
    void DestroySwapchains();

    bool PollEvents();
    void UpdateTracking(RiddickVRSharedState& state, XrTime predictedTime);
    void RenderFrame(IpcBridgeHost& ipcHost);

    bool EnsureSharedTextures(uint64_t leftHandle, uint64_t rightHandle);
    void ReleaseSharedTextures();

    std::string GetXrErrorString(XrResult res);

    // OpenXR Loader handle & function pointers
    HMODULE m_hLoader = nullptr;
    bool m_openxrAvailable = false;

    // Core handles
    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSessionState m_sessionState = XR_SESSION_STATE_UNKNOWN;
    XrSpace m_appSpace = XR_NULL_HANDLE;
    XrSpace m_viewSpace = XR_NULL_HANDLE;

    // Action set and action handles
    XrActionSet m_actionSet = XR_NULL_HANDLE;
    XrAction m_aimPoseAction = XR_NULL_HANDLE;
    XrAction m_gripPoseAction = XR_NULL_HANDLE;
    XrAction m_locomotionAction = XR_NULL_HANDLE;
    XrAction m_turnAction = XR_NULL_HANDLE;
    XrAction m_triggerAction = XR_NULL_HANDLE;
    XrAction m_gripAction = XR_NULL_HANDLE;
    XrAction m_primaryBtnAction = XR_NULL_HANDLE;
    XrAction m_secondaryBtnAction = XR_NULL_HANDLE;
    XrAction m_menuBtnAction = XR_NULL_HANDLE;
    XrAction m_recenterBtnAction = XR_NULL_HANDLE;
    XrAction m_hapticAction = XR_NULL_HANDLE;

    // Subaction paths
    XrPath m_leftHandPath = XR_NULL_PATH;
    XrPath m_rightHandPath = XR_NULL_PATH;

    // Action spaces
    XrSpace m_leftGripSpace = XR_NULL_HANDLE;
    XrSpace m_rightGripSpace = XR_NULL_HANDLE;
    XrSpace m_leftAimSpace = XR_NULL_HANDLE;
    XrSpace m_rightAimSpace = XR_NULL_HANDLE;

    // Swapchains for Left [0] and Right [1] eyes
    OpenXRSwapchain m_swapchains[2];

    // D3D11 Compositor Device
    ID3D11Device* m_d3d11Device = nullptr;
    ID3D11DeviceContext* m_d3d11Context = nullptr;

    // Opened shared textures from Hook process
    uint64_t m_currentLeftHandle = 0;
    uint64_t m_currentRightHandle = 0;
    ID3D11Texture2D* m_sharedLeftTex = nullptr;
    ID3D11Texture2D* m_sharedRightTex = nullptr;

    // Headset display configuration
    uint32_t m_recommendedWidth = 2048;
    uint32_t m_recommendedHeight = 2048;
    XrView m_locatedViews[2] = { {XR_TYPE_VIEW}, {XR_TYPE_VIEW} };

    bool m_initialized = false;
    bool m_running = false;
    bool m_sessionActive = false;
    uint32_t m_frameIndex = 0;
    // GH #77: provenance of the per-eye view/projection matrices recorded in
    // `eye_frames.log`. True only when `xrLocateViews` actually returned views
    // (real headset); false for the synthetic constant baseline the mock host
    // uses, so a capture never claims a scene-faithful eye view it lacks.
    bool m_eyeViewsFromRuntime = false;

    bool m_mockMode = false;
    std::string m_dumpDir;
    uint32_t m_maxFrames = 0;
    uint32_t m_dumpInterval = 10;
    // GH #69 requirement 1: capture only live-3D-gameplay frames.
    bool m_dumpGameplayOnly = false;
    // GH #72 requirement 2: content gate. The 16x16 luma signature of the last
    // *dumped* left-eye frame, so a byte-identical repeat (a static 2D startup
    // page) is skipped instead of graded. Blank (all-zero interop warm-up)
    // frames are skipped by mean luma before this is consulted.
    uint64_t m_lastDumpSignature = 0;
    bool m_hasLastDumpSignature = false;
    // GH #48 scenario playback.
    std::string m_scenarioPath;
    std::vector<ScenarioKey> m_scenarioKeys;
    bool m_scenarioLoaded = false;
    uint32_t m_scenarioStartFrame = UINT32_MAX;
    uint32_t m_gameplayStreak = 0;
    bool m_scenarioStartOnGameplay = true;
    float m_ammoQuadCropX = 0.75f;
    float m_ammoQuadCropY = 0.75f;
    float m_ammoQuadCropW = 0.23f;
    float m_ammoQuadCropH = 0.23f;

    // Function pointers loaded dynamically from openxr_loader.dll
    PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
    PFN_xrCreateInstance xrCreateInstance = nullptr;
    PFN_xrDestroyInstance xrDestroyInstance = nullptr;
    PFN_xrGetInstanceProperties xrGetInstanceProperties = nullptr;
    PFN_xrGetSystem xrGetSystem = nullptr;
    PFN_xrGetSystemProperties xrGetSystemProperties = nullptr;
    PFN_xrEnumerateViewConfigurations xrEnumerateViewConfigurations = nullptr;
    PFN_xrEnumerateViewConfigurationViews xrEnumerateViewConfigurationViews = nullptr;
    PFN_xrEnumerateSwapchainFormats xrEnumerateSwapchainFormats = nullptr;
    PFN_xrCreateSession xrCreateSession = nullptr;
    PFN_xrDestroySession xrDestroySession = nullptr;
    PFN_xrBeginSession xrBeginSession = nullptr;
    PFN_xrEndSession xrEndSession = nullptr;
    PFN_xrRequestExitSession xrRequestExitSession = nullptr;
    PFN_xrWaitFrame xrWaitFrame = nullptr;
    PFN_xrBeginFrame xrBeginFrame = nullptr;
    PFN_xrEndFrame xrEndFrame = nullptr;
    PFN_xrLocateViews xrLocateViews = nullptr;
    PFN_xrCreateReferenceSpace xrCreateReferenceSpace = nullptr;
    PFN_xrDestroySpace xrDestroySpace = nullptr;
    PFN_xrLocateSpace xrLocateSpace = nullptr;
    PFN_xrCreateActionSet xrCreateActionSet = nullptr;
    PFN_xrDestroyActionSet xrDestroyActionSet = nullptr;
    PFN_xrCreateAction xrCreateAction = nullptr;
    PFN_xrDestroyAction xrDestroyAction = nullptr;
    PFN_xrStringToPath xrStringToPath = nullptr;
    PFN_xrPathToString xrPathToString = nullptr;
    PFN_xrSuggestInteractionProfileBindings xrSuggestInteractionProfileBindings = nullptr;
    PFN_xrAttachSessionActionSets xrAttachSessionActionSets = nullptr;
    PFN_xrCreateActionSpace xrCreateActionSpace = nullptr;
    PFN_xrSyncActions xrSyncActions = nullptr;
    PFN_xrGetActionStateBoolean xrGetActionStateBoolean = nullptr;
    PFN_xrGetActionStateFloat xrGetActionStateFloat = nullptr;
    PFN_xrGetActionStateVector2f xrGetActionStateVector2f = nullptr;
    PFN_xrGetActionStatePose xrGetActionStatePose = nullptr;
    PFN_xrApplyHapticFeedback xrApplyHapticFeedback = nullptr;
    PFN_xrStopHapticFeedback xrStopHapticFeedback = nullptr;
    PFN_xrCreateSwapchain xrCreateSwapchain = nullptr;
    PFN_xrDestroySwapchain xrDestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages xrEnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage xrAcquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage xrWaitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage xrReleaseSwapchainImage = nullptr;
    PFN_xrPollEvent xrPollEvent = nullptr;
    PFN_xrResultToString xrResultToString = nullptr;

    // Extension functions
    PFN_xrGetD3D11GraphicsRequirementsKHR xrGetD3D11GraphicsRequirementsKHR = nullptr;
};

} // namespace RiddickVR
