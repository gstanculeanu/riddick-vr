#include "openxr_manager.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdint>

namespace RiddickVR {

// GH #36: single interpupillary-distance constant (65mm IPD => 32.5mm half).
// All synthetic eye baselines and fallback views derive from this.
constexpr float kHalfIpdMeters = 0.0325f;

static RiddickVRMatrix4x4 CreateProjectionFromFov(const XrFovf& fov, float nearZ = 0.05f, float farZ = 1000.0f) {
    RiddickVRMatrix4x4 result{};
    for (int i = 0; i < 16; ++i) result.m[i] = 0.0f;

    const float tanLeft = tanf(fov.angleLeft);
    const float tanRight = tanf(fov.angleRight);
    const float tanDown = tanf(fov.angleDown);
    const float tanUp = tanf(fov.angleUp);

    const float idx = 1.0f / (tanRight - tanLeft);
    const float idy = 1.0f / (tanUp - tanDown);
    const float idz = 1.0f / (farZ - nearZ);

    result.m[0] = 2.0f * idx;
    result.m[5] = 2.0f * idy;
    result.m[8] = (tanRight + tanLeft) * idx;
    result.m[9] = (tanUp + tanDown) * idy;
    result.m[10] = -(farZ + nearZ) * idz;
    result.m[11] = -1.0f;
    result.m[14] = -(2.0f * farZ * nearZ) * idz;
    result.m[15] = 0.0f;

    return result;
}

static RiddickVRMatrix4x4 CreateViewFromPose(const XrPosef& pose) {
    RiddickVRMatrix4x4 result{};
    const float qx = pose.orientation.x;
    const float qy = pose.orientation.y;
    const float qz = pose.orientation.z;
    const float qw = pose.orientation.w;

    // Rotation matrix from quaternion (R)
    const float r00 = 1.0f - 2.0f * (qy * qy + qz * qz);
    const float r01 = 2.0f * (qx * qy - qz * qw);
    const float r02 = 2.0f * (qx * qz + qy * qw);

    const float r10 = 2.0f * (qx * qy + qz * qw);
    const float r11 = 1.0f - 2.0f * (qx * qx + qz * qz);
    const float r12 = 2.0f * (qy * qz - qx * qw);

    const float r20 = 2.0f * (qx * qz - qy * qw);
    const float r21 = 2.0f * (qy * qz + qx * qw);
    const float r22 = 1.0f - 2.0f * (qx * qx + qy * qy);

    const float px = pose.position.x;
    const float py = pose.position.y;
    const float pz = pose.position.z;

    // Invert rigid transform: R^T and -R^T * p
    result.m[0] = r00;
    result.m[1] = r01;
    result.m[2] = r02;
    result.m[3] = 0.0f;

    result.m[4] = r10;
    result.m[5] = r11;
    result.m[6] = r12;
    result.m[7] = 0.0f;

    result.m[8] = r20;
    result.m[9] = r21;
    result.m[10] = r22;
    result.m[11] = 0.0f;

    result.m[12] = -(r00 * px + r10 * py + r20 * pz);
    result.m[13] = -(r01 * px + r11 * py + r21 * pz);
    result.m[14] = -(r02 * px + r12 * py + r22 * pz);
    result.m[15] = 1.0f;

    return result;
}

static XrQuaternionf ExtractRoll(const XrQuaternionf& q) {
    float qx = q.x, qy = q.y, qz = q.z, qw = q.w;
    float norm = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
    if (norm > 0.0001f) {
        qx /= norm; qy /= norm; qz /= norm; qw /= norm;
    }
    // Extract head roll (Z-axis tilt) only; yaw and pitch are driven by in-game camera
    float sinRoll = 2.0f * (qw * qz + qx * qy);
    float cosRoll = 1.0f - 2.0f * (qy * qy + qz * qz);
    float roll = atan2f(sinRoll, cosRoll);

    float halfRoll = roll * 0.5f;
    XrQuaternionf result;
    result.x = 0.0f;
    result.y = 0.0f;
    result.z = sinf(halfRoll);
    result.w = cosf(halfRoll);
    return result;
}

static inline RiddickVRVector3 RotateVectorByQuat(const RiddickVRVector3& v, const RiddickVRQuaternion& q) {
    float tx = 2.0f * (q.y * v.z - q.z * v.y);
    float ty = 2.0f * (q.z * v.x - q.x * v.z);
    float tz = 2.0f * (q.x * v.y - q.y * v.x);

    return {
        v.x + q.w * tx + (q.y * tz - q.z * ty),
        v.y + q.w * ty + (q.z * tx - q.x * tz),
        v.z + q.w * tz + (q.x * ty - q.y * tx)
    };
}

static inline XrQuaternionf MultiplyQuaternions(const RiddickVRQuaternion& q1, const RiddickVRQuaternion& q2) {
    XrQuaternionf result{};
    result.w = q1.w * q2.w - q1.x * q2.x - q1.y * q2.y - q1.z * q2.z;
    result.x = q1.w * q2.x + q1.x * q2.w + q1.y * q2.z - q1.z * q2.y;
    result.y = q1.w * q2.y - q1.x * q2.z + q1.y * q2.w + q1.z * q2.x;
    result.z = q1.w * q2.z + q1.x * q2.y - q1.y * q2.x + q1.z * q2.w;

    float norm = sqrtf(result.x * result.x + result.y * result.y + result.z * result.z + result.w * result.w);
    if (norm > 0.0001f) {
        result.x /= norm;
        result.y /= norm;
        result.z /= norm;
        result.w /= norm;
    }
    return result;
}

OpenXRManager::OpenXRManager() {
    m_locatedViews[0].pose.orientation.w = 1.0f;
    m_locatedViews[0].fov = { -0.785f, 0.785f, 0.785f, -0.785f };
    m_locatedViews[1].pose.orientation.w = 1.0f;
    m_locatedViews[1].fov = { -0.785f, 0.785f, 0.785f, -0.785f };
}

OpenXRManager::~OpenXRManager() {
    Shutdown();
}

std::string OpenXRManager::GetXrErrorString(XrResult res) {
    if (xrResultToString && m_instance != XR_NULL_HANDLE) {
        char buffer[XR_MAX_RESULT_STRING_SIZE] = {0};
        if (XR_SUCCEEDED(xrResultToString(m_instance, res, buffer))) {
            return std::string(buffer);
        }
    }
    return "XrResult " + std::to_string(res);
}

bool OpenXRManager::LoadLoader() {
    if (m_hLoader) return true;

    // Search order: adjacent directory, current working dir, system path
    m_hLoader = LoadLibraryA("openxr_loader.dll");
    if (!m_hLoader) {
        m_hLoader = LoadLibraryA("dist\\openxr_loader.dll");
    }

    if (!m_hLoader) {
        return false;
    }

    // Resolve xrGetInstanceProcAddr
    xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)GetProcAddress(m_hLoader, "xrGetInstanceProcAddr");
    if (!xrGetInstanceProcAddr) {
        FreeLibrary(m_hLoader);
        m_hLoader = nullptr;
        return false;
    }

    // Resolve exported core functions
#define LOAD_PROC(name) name = (PFN_##name)GetProcAddress(m_hLoader, #name)
    LOAD_PROC(xrCreateInstance);
    LOAD_PROC(xrDestroyInstance);
    LOAD_PROC(xrGetInstanceProperties);
    LOAD_PROC(xrGetSystem);
    LOAD_PROC(xrGetSystemProperties);
    LOAD_PROC(xrEnumerateViewConfigurations);
    LOAD_PROC(xrEnumerateViewConfigurationViews);
    LOAD_PROC(xrEnumerateSwapchainFormats);
    LOAD_PROC(xrCreateSession);
    LOAD_PROC(xrDestroySession);
    LOAD_PROC(xrBeginSession);
    LOAD_PROC(xrEndSession);
    LOAD_PROC(xrRequestExitSession);
    LOAD_PROC(xrWaitFrame);
    LOAD_PROC(xrBeginFrame);
    LOAD_PROC(xrEndFrame);
    LOAD_PROC(xrLocateViews);
    LOAD_PROC(xrCreateReferenceSpace);
    LOAD_PROC(xrDestroySpace);
    LOAD_PROC(xrLocateSpace);
    LOAD_PROC(xrCreateActionSet);
    LOAD_PROC(xrDestroyActionSet);
    LOAD_PROC(xrCreateAction);
    LOAD_PROC(xrDestroyAction);
    LOAD_PROC(xrStringToPath);
    LOAD_PROC(xrPathToString);
    LOAD_PROC(xrSuggestInteractionProfileBindings);
    LOAD_PROC(xrAttachSessionActionSets);
    LOAD_PROC(xrCreateActionSpace);
    LOAD_PROC(xrSyncActions);
    LOAD_PROC(xrGetActionStateBoolean);
    LOAD_PROC(xrGetActionStateFloat);
    LOAD_PROC(xrGetActionStateVector2f);
    LOAD_PROC(xrGetActionStatePose);
    LOAD_PROC(xrApplyHapticFeedback);
    LOAD_PROC(xrStopHapticFeedback);
    LOAD_PROC(xrCreateSwapchain);
    LOAD_PROC(xrDestroySwapchain);
    LOAD_PROC(xrEnumerateSwapchainImages);
    LOAD_PROC(xrAcquireSwapchainImage);
    LOAD_PROC(xrWaitSwapchainImage);
    LOAD_PROC(xrReleaseSwapchainImage);
    LOAD_PROC(xrPollEvent);
    LOAD_PROC(xrResultToString);
#undef LOAD_PROC

    m_openxrAvailable = (xrCreateInstance != nullptr);
    return m_openxrAvailable;
}

bool OpenXRManager::CreateInstance() {
    if (!m_openxrAvailable || !xrCreateInstance) return false;

    const char* enabledExtensions[] = {
        XR_KHR_D3D11_ENABLE_EXTENSION_NAME
    };

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(createInfo.applicationInfo.applicationName, "Riddick VR");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "Starbreeze Engine");
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = enabledExtensions;

    XrResult res = xrCreateInstance(&createInfo, &m_instance);
    if (XR_FAILED(res) || m_instance == XR_NULL_HANDLE) {
        static DWORD s_lastLogTime = 0;
        DWORD now = GetTickCount();
        if (now - s_lastLogTime >= 5000) {
            std::cout << "[Host] Searching for active OpenXR headset runtime (" << GetXrErrorString(res) << ")... Ensure Virtual Desktop is streaming." << std::endl;
            s_lastLogTime = now;
        }
        return false;
    }

    // Load D3D11 extension function
    xrGetInstanceProcAddr(m_instance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&xrGetD3D11GraphicsRequirementsKHR);
    return true;
}

bool OpenXRManager::InitializeD3D11Device() {
    if (m_d3d11Device) return true;

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        featureLevels,
        2,
        D3D11_SDK_VERSION,
        &m_d3d11Device,
        &featureLevel,
        &m_d3d11Context
    );

    return SUCCEEDED(hr) && (m_d3d11Device != nullptr);
}

bool OpenXRManager::InitializeSession() {
    if (!m_instance) return false;

    // Get System ID for HMD
    XrSystemGetInfo systemGetInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult res = xrGetSystem(m_instance, &systemGetInfo, &m_systemId);
    if (XR_FAILED(res)) {
        std::cout << "[Host] Notice: No VR headset detected (" << GetXrErrorString(res) << ")." << std::endl;
        return false;
    }

    // Query D3D11 Requirements
    if (xrGetD3D11GraphicsRequirementsKHR) {
        XrGraphicsRequirementsD3D11KHR graphicsRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        xrGetD3D11GraphicsRequirementsKHR(m_instance, m_systemId, &graphicsRequirements);
    }

    if (!InitializeD3D11Device()) {
        std::cerr << "[Host] ERROR: Failed to create Direct3D 11 device for OpenXR session!" << std::endl;
        return false;
    }

    // Query recommended view configuration
    uint32_t viewConfigCount = 0;
    xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewConfigCount, nullptr);
    if (viewConfigCount >= 2) {
        std::vector<XrViewConfigurationView> views(viewConfigCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewConfigCount, &viewConfigCount, views.data());
        m_recommendedWidth = views[0].recommendedImageRectWidth;
        m_recommendedHeight = views[0].recommendedImageRectHeight;
        std::cout << "[Host] Headset Recommended Eye Resolution: " << m_recommendedWidth << "x" << m_recommendedHeight << std::endl;
    }

    // Create Session
    XrGraphicsBindingD3D11KHR graphicsBinding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    graphicsBinding.device = m_d3d11Device;

    XrSessionCreateInfo sessionCreateInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionCreateInfo.next = &graphicsBinding;
    sessionCreateInfo.systemId = m_systemId;

    res = xrCreateSession(m_instance, &sessionCreateInfo, &m_session);
    if (XR_FAILED(res)) {
        std::cerr << "[Host] ERROR: xrCreateSession failed (" << GetXrErrorString(res) << ")" << std::endl;
        return false;
    }

    CreateSpaces();
    CreateActions();
    CreateSwapchains();

    // Begin Session (if runtime is already in READY state, or wait for event)
    XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    res = xrBeginSession(m_session, &beginInfo);
    if (XR_SUCCEEDED(res)) {
        m_sessionActive = true;
        std::cout << "[Host] OpenXR Session ACTIVE and running!" << std::endl;
    } else if (res == XR_ERROR_SESSION_NOT_READY) {
        std::cout << "[Host] OpenXR Session initialized; awaiting runtime READY state..." << std::endl;
    } else {
        std::cout << "[Host] Notice: xrBeginSession returned " << GetXrErrorString(res) << std::endl;
    }

    return (m_session != XR_NULL_HANDLE);
}

bool OpenXRManager::CreateSpaces() {
    if (!m_session) return false;

    // Try Stage space first for room-scale, fallback to Local
    XrReferenceSpaceCreateInfo spaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    spaceCreateInfo.poseInReferenceSpace.orientation.w = 1.0f;

    XrResult res = xrCreateReferenceSpace(m_session, &spaceCreateInfo, &m_appSpace);
    if (XR_FAILED(res)) {
        spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        xrCreateReferenceSpace(m_session, &spaceCreateInfo, &m_appSpace);
    }

    // View space (HMD gaze)
    spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    xrCreateReferenceSpace(m_session, &spaceCreateInfo, &m_viewSpace);

    return (m_appSpace != XR_NULL_HANDLE);
}

bool OpenXRManager::CreateActions() {
    if (!m_session || !m_instance) return false;

    // Action set: gameplay
    XrActionSetCreateInfo actionSetInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(actionSetInfo.actionSetName, "gameplay");
    strcpy_s(actionSetInfo.localizedActionSetName, "Gameplay");
    actionSetInfo.priority = 0;
    if (XR_FAILED(xrCreateActionSet(m_instance, &actionSetInfo, &m_actionSet))) return false;

    // Subaction paths
    xrStringToPath(m_instance, "/user/hand/left", &m_leftHandPath);
    xrStringToPath(m_instance, "/user/hand/right", &m_rightHandPath);
    XrPath handSubactionPaths[] = { m_leftHandPath, m_rightHandPath };

    auto CreateAction = [&](const char* name, const char* localized, XrActionType type, uint32_t count, const XrPath* subactions, XrAction* outAction) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(info.actionName, name);
        strcpy_s(info.localizedActionName, localized);
        info.actionType = type;
        info.countSubactionPaths = count;
        info.subactionPaths = subactions;
        return XR_SUCCEEDED(xrCreateAction(m_actionSet, &info, outAction));
    };

    CreateAction("aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT, 2, handSubactionPaths, &m_aimPoseAction);
    CreateAction("grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT, 2, handSubactionPaths, &m_gripPoseAction);
    CreateAction("locomotion", "Locomotion", XR_ACTION_TYPE_VECTOR2F_INPUT, 2, handSubactionPaths, &m_locomotionAction);
    CreateAction("turn", "Turn", XR_ACTION_TYPE_VECTOR2F_INPUT, 2, handSubactionPaths, &m_turnAction);
    CreateAction("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, 2, handSubactionPaths, &m_triggerAction);
    CreateAction("grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, 2, handSubactionPaths, &m_gripAction);
    CreateAction("primary_btn", "Primary Button", XR_ACTION_TYPE_BOOLEAN_INPUT, 2, handSubactionPaths, &m_primaryBtnAction);
    CreateAction("secondary_btn", "Secondary Button", XR_ACTION_TYPE_BOOLEAN_INPUT, 2, handSubactionPaths, &m_secondaryBtnAction);
    CreateAction("menu_btn", "Menu Button", XR_ACTION_TYPE_BOOLEAN_INPUT, 2, handSubactionPaths, &m_menuBtnAction);
    CreateAction("recenter_btn", "Recenter Button", XR_ACTION_TYPE_BOOLEAN_INPUT, 2, handSubactionPaths, &m_recenterBtnAction);
    CreateAction("haptic", "Haptic Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT, 2, handSubactionPaths, &m_hapticAction);

    // Suggest bindings for Touch controller
    XrPath touchProfile;
    xrStringToPath(m_instance, "/interaction_profiles/oculus/touch_controller", &touchProfile);

    auto Path = [&](const char* str) -> XrPath {
        XrPath p;
        xrStringToPath(m_instance, str, &p);
        return p;
    };

    std::vector<XrActionSuggestedBinding> touchBindings = {
        { m_aimPoseAction, Path("/user/hand/left/input/aim/pose") },
        { m_aimPoseAction, Path("/user/hand/right/input/aim/pose") },
        { m_gripPoseAction, Path("/user/hand/left/input/grip/pose") },
        { m_gripPoseAction, Path("/user/hand/right/input/grip/pose") },
        { m_locomotionAction, Path("/user/hand/left/input/thumbstick") },
        { m_turnAction, Path("/user/hand/right/input/thumbstick") },
        { m_triggerAction, Path("/user/hand/left/input/trigger/value") },
        { m_triggerAction, Path("/user/hand/right/input/trigger/value") },
        { m_gripAction, Path("/user/hand/left/input/squeeze/value") },
        { m_gripAction, Path("/user/hand/right/input/squeeze/value") },
        { m_primaryBtnAction, Path("/user/hand/left/input/x/click") },
        { m_primaryBtnAction, Path("/user/hand/right/input/a/click") },
        { m_secondaryBtnAction, Path("/user/hand/left/input/y/click") },
        { m_secondaryBtnAction, Path("/user/hand/right/input/b/click") },
        { m_menuBtnAction, Path("/user/hand/left/input/menu/click") },
        { m_recenterBtnAction, Path("/user/hand/right/input/thumbstick/click") },
        { m_hapticAction, Path("/user/hand/left/output/haptic") },
        { m_hapticAction, Path("/user/hand/right/output/haptic") }
    };

    XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestedBindings.interactionProfile = touchProfile;
    suggestedBindings.suggestedBindings = touchBindings.data();
    suggestedBindings.countSuggestedBindings = (uint32_t)touchBindings.size();
    xrSuggestInteractionProfileBindings(m_instance, &suggestedBindings);

    // Attach to session
    XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &m_actionSet;
    xrAttachSessionActionSets(m_session, &attachInfo);

    // Create action spaces for grip and aim
    auto CreateActionSpace = [&](XrAction action, XrPath subaction, XrSpace* outSpace) {
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action = action;
        spaceInfo.subactionPath = subaction;
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        xrCreateActionSpace(m_session, &spaceInfo, outSpace);
    };

    CreateActionSpace(m_gripPoseAction, m_leftHandPath, &m_leftGripSpace);
    CreateActionSpace(m_gripPoseAction, m_rightHandPath, &m_rightGripSpace);
    CreateActionSpace(m_aimPoseAction, m_leftHandPath, &m_leftAimSpace);
    CreateActionSpace(m_aimPoseAction, m_rightHandPath, &m_rightAimSpace);

    return true;
}

bool OpenXRManager::CreateSwapchains() {
    if (!m_session) return false;

    DestroySwapchains();

    int64_t chosenFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (xrEnumerateSwapchainFormats) {
        uint32_t formatCount = 0;
        xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr);
        if (formatCount > 0) {
            std::vector<int64_t> formats(formatCount);
            xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data());

            bool foundPreferred = false;
            for (auto f : formats) {
                if (f == DXGI_FORMAT_R8G8B8A8_UNORM) {
                    chosenFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
                    foundPreferred = true;
                    break;
                }
            }
            if (!foundPreferred) {
                for (auto f : formats) {
                    if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM) {
                        chosenFormat = f;
                        break;
                    }
                }
            }
        }
    }

    for (int eye = 0; eye < 2; ++eye) {
        XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | 
                                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | 
                                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        createInfo.format = chosenFormat;
        createInfo.sampleCount = 1;
        createInfo.width = m_recommendedWidth;
        createInfo.height = m_recommendedHeight;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;

        XrResult res = xrCreateSwapchain(m_session, &createInfo, &m_swapchains[eye].handle);
        if (XR_FAILED(res)) {
            std::cerr << "[Host] ERROR: xrCreateSwapchain for eye " << eye << " failed (" << GetXrErrorString(res) << ")" << std::endl;
            return false;
        }

        m_swapchains[eye].width = m_recommendedWidth;
        m_swapchains[eye].height = m_recommendedHeight;

        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(m_swapchains[eye].handle, 0, &imageCount, nullptr);
        m_swapchains[eye].images.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(m_swapchains[eye].handle, imageCount, &imageCount, (XrSwapchainImageBaseHeader*)m_swapchains[eye].images.data());
    }

    std::cout << "[Host] Created dual eye OpenXR D3D11 swapchains (" << m_recommendedWidth << "x" << m_recommendedHeight << ")" << std::endl;
    return true;
}

void OpenXRManager::DestroySwapchains() {
    for (int eye = 0; eye < 2; ++eye) {
        if (m_swapchains[eye].handle != XR_NULL_HANDLE && xrDestroySwapchain) {
            xrDestroySwapchain(m_swapchains[eye].handle);
            m_swapchains[eye].handle = XR_NULL_HANDLE;
        }
        m_swapchains[eye].images.clear();
    }
}

bool OpenXRManager::TryConnectOpenXR() {
    if (m_sessionActive) return true;

    if (!m_openxrAvailable) {
        if (!LoadLoader()) return false;
    }

    if (!m_instance) {
        if (!CreateInstance()) {
            return false;
        }
    }

    if (!m_session) {
        if (!InitializeSession()) {
            TeardownSession();
            return false;
        }
    }

    return true;
}

void OpenXRManager::TeardownSession() {
    ReleaseSharedTextures();
    DestroySwapchains();

    if (m_leftGripSpace && xrDestroySpace) { xrDestroySpace(m_leftGripSpace); m_leftGripSpace = XR_NULL_HANDLE; }
    if (m_rightGripSpace && xrDestroySpace) { xrDestroySpace(m_rightGripSpace); m_rightGripSpace = XR_NULL_HANDLE; }
    if (m_leftAimSpace && xrDestroySpace) { xrDestroySpace(m_leftAimSpace); m_leftAimSpace = XR_NULL_HANDLE; }
    if (m_rightAimSpace && xrDestroySpace) { xrDestroySpace(m_rightAimSpace); m_rightAimSpace = XR_NULL_HANDLE; }
    if (m_appSpace && xrDestroySpace) { xrDestroySpace(m_appSpace); m_appSpace = XR_NULL_HANDLE; }
    if (m_viewSpace && xrDestroySpace) { xrDestroySpace(m_viewSpace); m_viewSpace = XR_NULL_HANDLE; }

    if (m_actionSet && xrDestroyActionSet) {
        xrDestroyActionSet(m_actionSet);
        m_actionSet = XR_NULL_HANDLE;
    }
    m_gripPoseAction = XR_NULL_HANDLE;
    m_aimPoseAction = XR_NULL_HANDLE;
    m_locomotionAction = XR_NULL_HANDLE;
    m_turnAction = XR_NULL_HANDLE;
    m_triggerAction = XR_NULL_HANDLE;
    m_gripAction = XR_NULL_HANDLE;
    m_primaryBtnAction = XR_NULL_HANDLE;
    m_secondaryBtnAction = XR_NULL_HANDLE;
    m_menuBtnAction = XR_NULL_HANDLE;
    m_recenterBtnAction = XR_NULL_HANDLE;
    m_hapticAction = XR_NULL_HANDLE;

    if (m_session && xrDestroySession) {
        xrDestroySession(m_session);
        m_session = XR_NULL_HANDLE;
    }

    if (m_instance && xrDestroyInstance) {
        xrDestroyInstance(m_instance);
        m_instance = XR_NULL_HANDLE;
    }

    m_sessionActive = false;
    m_sessionState = XR_SESSION_STATE_UNKNOWN;
    std::cout << "[Host] OpenXR session cleared. Ready for reconnection." << std::endl;
}

bool OpenXRManager::Initialize() {
    if (m_initialized) return true;

    std::cout << "[Host] Initializing Direct3D 11 & OpenXR runtime bridge..." << std::endl;

    InitializeD3D11Device();

    if (m_mockMode) {
        std::cout << "[Host] Mock Mode active: Headless simulated VR runtime engaged." << std::endl;
        m_initialized = true;
        m_running = true;
        return true;
    }

    if (LoadLoader()) {
        std::cout << "[Host] Successfully linked openxr_loader.dll." << std::endl;
        TryConnectOpenXR();
    } else {
        std::cout << "[Host] Notice: openxr_loader.dll not active; running in low-latency IPC standalone bridge mode." << std::endl;
    }

    m_initialized = true;
    m_running = true;
    return true;
}

bool OpenXRManager::Validate(std::string& outReport) {
    std::ostringstream ss;
    ss << "========================================================\n";
    ss << "         Riddick VR - OpenXR Validation Suite           \n";
    ss << "========================================================\n";

    bool loaderOk = LoadLoader();
    ss << "[1/4] OpenXR Loader:         " << (loaderOk ? "PRESENT (openxr_loader.dll)" : "NOT FOUND") << "\n";

    if (!loaderOk) {
        ss << "      Status: OpenXR runtime loader library is missing from PATH.\n";
        outReport = ss.str();
        return false;
    }

    bool d3dOk = InitializeD3D11Device();
    ss << "[2/4] Direct3D 11 Device:    " << (d3dOk ? "READY (Hardware FL 11.0)" : "FAILED") << "\n";

    bool instOk = CreateInstance();
    ss << "[3/4] OpenXR Instance:       " << (instOk ? "CREATED (Riddick VR / XR_KHR_D3D11_ENABLE)" : "FAILED / RUNTIME INACTIVE") << "\n";

    if (instOk && m_instance) {
        XrInstanceProperties instProps{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &instProps))) {
            ss << "      Runtime Name:          " << instProps.runtimeName << "\n";
            ss << "      Runtime Version:       " 
               << XR_VERSION_MAJOR(instProps.runtimeVersion) << "." 
               << XR_VERSION_MINOR(instProps.runtimeVersion) << "." 
               << XR_VERSION_PATCH(instProps.runtimeVersion) << "\n";
        }

        XrSystemGetInfo sysInfo{XR_TYPE_SYSTEM_GET_INFO};
        sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrResult sysRes = xrGetSystem(m_instance, &sysInfo, &m_systemId);
        bool hmdFound = XR_SUCCEEDED(sysRes);
        ss << "[4/4] Headset Discovery:     " << (hmdFound ? "DETECTED (Form Factor: HMD)" : "NOT CONNECTED") << "\n";

        if (hmdFound) {
            XrSystemProperties sysProps{XR_TYPE_SYSTEM_PROPERTIES};
            xrGetSystemProperties(m_instance, m_systemId, &sysProps);
            ss << "      System Name:           " << sysProps.systemName << "\n";
            ss << "      Vendor ID:             0x" << std::hex << sysProps.vendorId << std::dec << "\n";

            uint32_t viewCount = 0;
            xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
            if (viewCount >= 2) {
                std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
                xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data());
                ss << "      Stereo Recommended:    " << views[0].recommendedImageRectWidth << "x" << views[0].recommendedImageRectHeight << "\n";
                ss << "      Stereo Max Resolution: " << views[0].maxImageRectWidth << "x" << views[0].maxImageRectHeight << "\n";
            }
        }
    } else {
        ss << "[4/4] Headset Discovery:     SKIPPED (No active OpenXR runtime)\n";
    }

    ss << "--------------------------------------------------------\n";
    ss << "Overall Diagnostic Result:   " << ((loaderOk && d3dOk) ? "PASS / READY FOR VR" : "CONFIG ERROR") << "\n";
    ss << "========================================================\n";

    outReport = ss.str();
    return (loaderOk && d3dOk);
}

void OpenXRManager::Shutdown() {
    TeardownSession();

    if (m_hLoader) {
        FreeLibrary(m_hLoader);
        m_hLoader = nullptr;
    }

    if (m_d3d11Context) {
        m_d3d11Context->Release();
        m_d3d11Context = nullptr;
    }
    if (m_d3d11Device) {
        m_d3d11Device->Release();
        m_d3d11Device = nullptr;
    }

    m_running = false;
    m_sessionActive = false;
    m_initialized = false;
    std::cout << "[Host] OpenXR Manager cleanly shut down." << std::endl;
}

bool OpenXRManager::EnsureSharedTextures(uint64_t leftHandle, uint64_t rightHandle) {
    if (!m_d3d11Device || !leftHandle || !rightHandle) return false;
    if (leftHandle == m_currentLeftHandle && rightHandle == m_currentRightHandle && m_sharedLeftTex && m_sharedRightTex) {
        return true;
    }

    ReleaseSharedTextures();

    HANDLE hLeft = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(leftHandle & 0xFFFFFFFFULL));
    HANDLE hRight = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(rightHandle & 0xFFFFFFFFULL));

    HRESULT hrLeft = m_d3d11Device->OpenSharedResource(
        hLeft,
        __uuidof(ID3D11Texture2D),
        (void**)&m_sharedLeftTex
    );

    HRESULT hrRight = m_d3d11Device->OpenSharedResource(
        hRight,
        __uuidof(ID3D11Texture2D),
        (void**)&m_sharedRightTex
    );

    if (SUCCEEDED(hrLeft) && SUCCEEDED(hrRight) && m_sharedLeftTex && m_sharedRightTex) {
        m_currentLeftHandle = leftHandle;
        m_currentRightHandle = rightHandle;

        D3D11_TEXTURE2D_DESC desc{};
        m_sharedLeftTex->GetDesc(&desc);
        std::cout << "[Host] Bound GPU eye textures from game: " << desc.Width << "x" << desc.Height 
                  << " (Swapchain: " << m_swapchains[0].width << "x" << m_swapchains[0].height << ")" << std::endl;
        return true;
    } else {
        std::cerr << "[Host] ERROR: OpenSharedResource failed! hrLeft=0x" << std::hex << hrLeft << ", hrRight=0x" << hrRight << std::dec << std::endl;
        ReleaseSharedTextures();
        return false;
    }
}

void OpenXRManager::ReleaseSharedTextures() {
    if (m_sharedLeftTex) {
        m_sharedLeftTex->Release();
        m_sharedLeftTex = nullptr;
    }
    if (m_sharedRightTex) {
        m_sharedRightTex->Release();
        m_sharedRightTex = nullptr;
    }
    m_currentLeftHandle = 0;
    m_currentRightHandle = 0;
}

bool OpenXRManager::PollEvents() {
    if (!m_instance || !xrPollEvent) return m_running;

    XrEventDataBuffer eventBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(m_instance, &eventBuffer) == XR_SUCCESS) {
        if (eventBuffer.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* sessionStateChanged = (XrEventDataSessionStateChanged*)&eventBuffer;
            m_sessionState = sessionStateChanged->state;
            if (m_sessionState == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult res = xrBeginSession(m_session, &beginInfo);
                if (XR_SUCCEEDED(res)) {
                    m_sessionActive = true;
                    std::cout << "[Host] >>> OpenXR Session READY -> Switched to active VR rendering! <<<" << std::endl;
                } else {
                    std::cout << "[Host] Notice: xrBeginSession failed (" << GetXrErrorString(res) << ")" << std::endl;
                }
            } else if (m_sessionState == XR_SESSION_STATE_STOPPING) {
                std::cout << "[Host] OpenXR Session stopping..." << std::endl;
                if (m_sessionActive && xrEndSession) {
                    xrEndSession(m_session);
                }
                m_sessionActive = false;
            } else if (m_sessionState == XR_SESSION_STATE_EXITING || m_sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                std::cout << "[Host] OpenXR Session lost/exiting. Resetting session for reconnection..." << std::endl;
                TeardownSession();
                break;
            }
        }
        eventBuffer.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
    return m_running;
}

void OpenXRManager::UpdateTracking(RiddickVRSharedState& state, XrTime predictedTime) {
    m_frameIndex++;
    state.frameIndex = m_frameIndex;
    state.vrActive = m_sessionActive;
    state.renderWidth = m_recommendedWidth;
    state.renderHeight = m_recommendedHeight;

    if (m_sessionActive && m_session && m_appSpace) {
        // Sync Actions
        XrActiveActionSet activeSet{m_actionSet, XR_NULL_PATH};
        XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &activeSet;
        xrSyncActions(m_session, &syncInfo);

        // Read Locomotion & Turning
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};

        getInfo.action = m_locomotionAction;
        getInfo.subactionPath = m_leftHandPath;
        XrActionStateVector2f locoState{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &getInfo, &locoState)) && locoState.isActive) {
            state.leftController.thumbstickX = locoState.currentState.x;
            state.leftController.thumbstickY = locoState.currentState.y;
        }

        getInfo.action = m_turnAction;
        getInfo.subactionPath = m_rightHandPath;
        XrActionStateVector2f turnState{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &getInfo, &turnState)) && turnState.isActive) {
            state.rightController.thumbstickX = turnState.currentState.x;
            state.rightController.thumbstickY = turnState.currentState.y;
        }

        // Read Triggers
        getInfo.action = m_triggerAction;
        getInfo.subactionPath = m_leftHandPath;
        XrActionStateFloat leftTrig{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &getInfo, &leftTrig))) {
            state.leftController.trigger = leftTrig.currentState;
        }

        getInfo.subactionPath = m_rightHandPath;
        XrActionStateFloat rightTrig{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &getInfo, &rightTrig))) {
            state.rightController.trigger = rightTrig.currentState;
        }

        // Read Grips
        getInfo.action = m_gripAction;
        getInfo.subactionPath = m_leftHandPath;
        XrActionStateFloat leftGrip{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &getInfo, &leftGrip))) {
            state.leftController.grip = leftGrip.currentState;
        }

        getInfo.subactionPath = m_rightHandPath;
        XrActionStateFloat rightGrip{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &getInfo, &rightGrip))) {
            state.rightController.grip = rightGrip.currentState;
        }

        // Read Buttons & Capacitive Touch States (ADR 0386)
        state.leftController.buttons = 0;
        state.rightController.buttons = 0;

        if (state.leftController.trigger > 0.05f) {
            state.leftController.buttons |= RIDDICK_VR_TOUCH_TRIGGER;
        }
        if (fabsf(state.leftController.thumbstickX) > 0.08f || fabsf(state.leftController.thumbstickY) > 0.08f) {
            state.leftController.buttons |= RIDDICK_VR_TOUCH_THUMBSTICK;
        }
        if (state.rightController.trigger > 0.05f) {
            state.rightController.buttons |= RIDDICK_VR_TOUCH_TRIGGER;
        }
        if (fabsf(state.rightController.thumbstickX) > 0.08f || fabsf(state.rightController.thumbstickY) > 0.08f) {
            state.rightController.buttons |= RIDDICK_VR_TOUCH_THUMBSTICK;
        }

        getInfo.action = m_primaryBtnAction;
        getInfo.subactionPath = m_rightHandPath;
        XrActionStateBoolean btnA{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &getInfo, &btnA)) && btnA.currentState) {
            state.rightController.buttons |= RIDDICK_VR_BTN_A;
        }

        getInfo.action = m_secondaryBtnAction;
        getInfo.subactionPath = m_rightHandPath;
        XrActionStateBoolean btnB{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &getInfo, &btnB)) && btnB.currentState) {
            state.rightController.buttons |= RIDDICK_VR_BTN_B;
        }

        getInfo.action = m_primaryBtnAction;
        getInfo.subactionPath = m_leftHandPath;
        XrActionStateBoolean btnX{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &getInfo, &btnX)) && btnX.currentState) {
            state.leftController.buttons |= RIDDICK_VR_BTN_X;
        }

        getInfo.action = m_secondaryBtnAction;
        getInfo.subactionPath = m_leftHandPath;
        XrActionStateBoolean btnY{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &getInfo, &btnY)) && btnY.currentState) {
            state.leftController.buttons |= RIDDICK_VR_BTN_Y;
        }

        // Menu Button (Left Controller - Escape & Skip Cutscene)
        if (m_menuBtnAction) {
            getInfo.action = m_menuBtnAction;
            getInfo.subactionPath = m_leftHandPath;
            XrActionStateBoolean btnMenu{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &getInfo, &btnMenu)) && btnMenu.currentState) {
                state.leftController.buttons |= RIDDICK_VR_BTN_MENU;
            }
        }

        // Recenter (GH #37): edge-triggered single-frame pulse. The host
        // publishes a fresh state{} every frame (RenderFrame), which clears a
        // level flag after one frame; the hook consumes the rising edge for
        // calibration. A held button therefore recenters exactly once.
        // (Full sequence-counter consume-after-ack is follow-up.)
        getInfo.action = m_recenterBtnAction;
        getInfo.subactionPath = m_rightHandPath;
        XrActionStateBoolean btnRecenter{XR_TYPE_ACTION_STATE_BOOLEAN};
        static bool s_prevRecenterHeld = false;
        bool recenterHeld = false;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &getInfo, &btnRecenter)) && btnRecenter.isActive) {
            recenterHeld = btnRecenter.currentState;
        }
        state.shouldRecenter = recenterHeld && !s_prevRecenterHeld;
        s_prevRecenterHeld = recenterHeld;

        // GH #21/#38: no haptic dispatch here (deleted dead path). This state
        // is freshly zeroed by the caller, so pulse-gated ApplyHapticFeedback
        // could never fire. Hook->host pulses travel via IpcBridgeClient::
        // WriteState and are applied + consumed (ClearHaptics) in RenderFrame.

        // Locate Controller Poses and compute linear velocities
        static RiddickVRVector3 s_prevLeftPos{0, 0, 0};
        static RiddickVRVector3 s_prevRightPos{0, 0, 0};
        static auto s_prevTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - s_prevTime).count();
        if (dt < 0.001f) dt = 0.016f;

        auto LocateTrackedPose = [&](XrSpace space, RiddickVRTrackedPose& outPose, RiddickVRVector3& prevPos) {
            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
            XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
            loc.next = &vel;
            if (XR_SUCCEEDED(xrLocateSpace(space, m_appSpace, predictedTime, &loc)) &&
                (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
                outPose.position = { loc.pose.position.x, loc.pose.position.y, loc.pose.position.z };
                outPose.orientation = { loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w };
                outPose.isValid = true;

                if (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) {
                    outPose.linearVelocity = { vel.linearVelocity.x, vel.linearVelocity.y, vel.linearVelocity.z };
                } else {
                    outPose.linearVelocity = {
                        (outPose.position.x - prevPos.x) / dt,
                        (outPose.position.y - prevPos.y) / dt,
                        (outPose.position.z - prevPos.z) / dt
                    };
                }
                prevPos = outPose.position;
            }
        };

        static RiddickVRVector3 s_prevLeftAimPos{0, 0, 0};
        static RiddickVRVector3 s_prevRightAimPos{0, 0, 0};

        if (m_leftGripSpace) LocateTrackedPose(m_leftGripSpace, state.leftController.gripPose, s_prevLeftPos);
        if (m_rightGripSpace) LocateTrackedPose(m_rightGripSpace, state.rightController.gripPose, s_prevRightPos);
        if (m_leftAimSpace) LocateTrackedPose(m_leftAimSpace, state.leftController.aimPose, s_prevLeftAimPos);
        if (m_rightAimSpace) LocateTrackedPose(m_rightAimSpace, state.rightController.aimPose, s_prevRightAimPos);
        s_prevTime = now;

        state.leftController.isConnected = state.leftController.gripPose.isValid;
        state.rightController.isConnected = state.rightController.gripPose.isValid;

        // Locate Stereo Views & Projection
        XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        viewLocateInfo.displayTime = predictedTime;
        viewLocateInfo.space = m_appSpace;

        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t viewCount = 0;
        XrView views[2] = { {XR_TYPE_VIEW}, {XR_TYPE_VIEW} };

        if (XR_SUCCEEDED(xrLocateViews(m_session, &viewLocateInfo, &viewState, 2, &viewCount, views)) && viewCount >= 2) {
            m_locatedViews[0] = views[0];
            m_locatedViews[1] = views[1];
            state.leftEyeProjection = CreateProjectionFromFov(views[0].fov);
            state.rightEyeProjection = CreateProjectionFromFov(views[1].fov);

            state.leftEyeView = CreateViewFromPose(views[0].pose);
            state.rightEyeView = CreateViewFromPose(views[1].pose);

            // GH #36: HMD pose is the centroid midpoint of both eyes, not the
            // left eye (avoids a constant half-IPD lateral bias in gesture
            // zones and hand mapping). Orientation is shared by both eyes on
            // all supported runtimes, so views[0] orientation is used as-is.
            state.hmdPose.position = {
                (views[0].pose.position.x + views[1].pose.position.x) * 0.5f,
                (views[0].pose.position.y + views[1].pose.position.y) * 0.5f,
                (views[0].pose.position.z + views[1].pose.position.z) * 0.5f };
            state.hmdPose.orientation = { views[0].pose.orientation.x, views[0].pose.orientation.y, views[0].pose.orientation.z, views[0].pose.orientation.w };
            state.hmdPose.isValid = true;
            return;
        }
    }

    // Fallback: Default synthetic 65mm IPD baseline matrices
    state.hmdPose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    state.hmdPose.position = {0.0f, 0.0f, 0.0f};
    state.hmdPose.isValid = true;

    constexpr float HALF_IPD = kHalfIpdMeters;
    for (int i = 0; i < 16; ++i) {
        state.leftEyeView.m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        state.rightEyeView.m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    state.leftEyeView.m[12] = -HALF_IPD;
    state.rightEyeView.m[12] = +HALF_IPD;

    constexpr float fovTan = 1.0f;
    constexpr float nearZ = 0.05f;
    constexpr float farZ = 1000.0f;
    for (int i = 0; i < 16; ++i) {
        state.leftEyeProjection.m[i] = 0.0f;
        state.rightEyeProjection.m[i] = 0.0f;
    }
    state.leftEyeProjection.m[0] = 1.0f / fovTan;
    state.leftEyeProjection.m[5] = 1.0f / fovTan;
    state.leftEyeProjection.m[10] = -(farZ + nearZ) / (farZ - nearZ);
    state.leftEyeProjection.m[11] = -1.0f;
    state.leftEyeProjection.m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
    state.rightEyeProjection = state.leftEyeProjection;

    state.leftController.isConnected = true;
    state.rightController.isConnected = true;
}

void OpenXRManager::RenderFrame(IpcBridgeHost& ipcHost) {
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrTime predictedDisplayTime = 0;

    if (m_sessionActive && m_session && xrWaitFrame) {
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        xrWaitFrame(m_session, &waitInfo, &frameState);
        predictedDisplayTime = frameState.predictedDisplayTime;

        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(m_session, &beginInfo);
    }

    RiddickVRSharedState state{};
    UpdateTracking(state, predictedDisplayTime);

    // Transmit updated tracking to the game hook
    ipcHost.WriteState(state);

    // Wait for the game hook to finish rendering the frame (up to 16ms)
    RiddickVRSharedState clientState{};
    bool frameReady = ipcHost.WaitForFrameReady(16);
    if (frameReady) {
        if (ipcHost.ReadState(clientState)) {
            if (clientState.leftEyeSharedHandle && clientState.rightEyeSharedHandle) {
                EnsureSharedTextures(clientState.leftEyeSharedHandle, clientState.rightEyeSharedHandle);
            }

            // Apply tactile haptic waveforms requested by game hook (blade slices, gun recoil, climbing, eyeshine)
            auto ApplyTactileHaptics = [&](XrPath handPath, float pulseVal) {
                if (pulseVal <= 0.05f || !xrApplyHapticFeedback) return;

                XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
                vib.amplitude = (std::min)(pulseVal, 1.0f);

                // Differentiate tactile texture based on gesture intensity & frequency:
                if (pulseVal >= 0.90f) {
                    // High-frequency, sharp cutting blade slice / lethal finisher (280 Hz, 60ms)
                    vib.frequency = 280.0f;
                    vib.duration = 60000000;
                } else if (pulseVal >= 0.70f) {
                    // Resonant optical buzz (eyeshine), mechanical switch (flashlight), gun recoil (180 Hz, 45ms)
                    vib.frequency = 180.0f;
                    vib.duration = 45000000;
                } else if (pulseVal >= 0.45f) {
                    // Solid tactile impact (mantle ledge hoist, magazine slap) (120 Hz, 75ms)
                    vib.frequency = 120.0f;
                    vib.duration = 75000000;
                } else {
                    // Subtle tactile sensation (rung grab, physical crouch, wall-peek, ADS stabilization) (85 Hz, 35ms)
                    vib.frequency = 85.0f;
                    vib.duration = 35000000;
                }

                XrHapticActionInfo hapticInfo{XR_TYPE_HAPTIC_ACTION_INFO};
                hapticInfo.action = m_hapticAction;
                hapticInfo.subactionPath = handPath;
                xrApplyHapticFeedback(m_session, &hapticInfo, (XrHapticBaseHeader*)&vib);
            };

            ApplyTactileHaptics(m_leftHandPath, clientState.leftHapticPulse);
            ApplyTactileHaptics(m_rightHandPath, clientState.rightHapticPulse);
            // Consume-on-read (GH #38): clear latched pulses so a finisher buzz
            // does not re-fire every frame. Hook republishes while active.
            if (clientState.leftHapticPulse > 0.05f || clientState.rightHapticPulse > 0.05f) {
                ipcHost.ClearHaptics();
            }
        }
    }

    // Submit OpenXR compositor frame if session is active
    if (m_sessionActive && m_session && xrEndFrame) {
        std::vector<XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerQuad wristQuadLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad weaponAmmoQuadLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerProjectionView projViews[2] = {
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}
        };

        if (frameState.shouldRender && m_swapchains[0].handle && m_swapchains[1].handle) {
            for (int eye = 0; eye < 2; ++eye) {
                uint32_t imageIndex = 0;
                XrSwapchainImageAcquireInfo acqInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                xrAcquireSwapchainImage(m_swapchains[eye].handle, &acqInfo, &imageIndex);

                XrSwapchainImageWaitInfo waitImageInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                waitImageInfo.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(m_swapchains[eye].handle, &waitImageInfo);

                // Copy shared D3D11 texture into swapchain image if available
                ID3D11Texture2D* srcTex = (eye == 0) ? m_sharedLeftTex : m_sharedRightTex;
                if (srcTex && m_d3d11Context && imageIndex < m_swapchains[eye].images.size()) {
                    D3D11_TEXTURE2D_DESC srcDesc{};
                    srcTex->GetDesc(&srcDesc);
                    if (srcDesc.Width == m_swapchains[eye].width && srcDesc.Height == m_swapchains[eye].height) {
                        m_d3d11Context->CopyResource(m_swapchains[eye].images[imageIndex].texture, srcTex);
                    } else {
                        D3D11_BOX box{};
                        box.left = 0;
                        box.top = 0;
                        box.front = 0;
                        box.right = (std::min)(srcDesc.Width, m_swapchains[eye].width);
                        box.bottom = (std::min)(srcDesc.Height, m_swapchains[eye].height);
                        box.back = 1;
                        m_d3d11Context->CopySubresourceRegion(
                            m_swapchains[eye].images[imageIndex].texture, 0, 0, 0, 0,
                            srcTex, 0, &box
                        );
                    }
                    if (m_d3d11Context) {
                        m_d3d11Context->Flush();
                    }
                }

                XrSwapchainImageReleaseInfo relInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(m_swapchains[eye].handle, &relInfo);

                projViews[eye].pose.position = m_locatedViews[eye].pose.position;
                projViews[eye].pose.orientation = ExtractRoll(m_locatedViews[eye].pose.orientation);

                // In 3D action mode, project swapchains using the headset's native physical lens FOV
                // Edge-to-edge full VR coverage with ZERO black bars on left, right, top, or bottom.
                projViews[eye].fov = m_locatedViews[eye].fov;

                projViews[eye].subImage.swapchain = m_swapchains[eye].handle;
                projViews[eye].subImage.imageRect.offset = {0, 0};
                projViews[eye].subImage.imageRect.extent = { (int32_t)m_swapchains[eye].width, (int32_t)m_swapchains[eye].height };
            }

            // Unified Single-POV Architecture (ADR 0405, ADR 0409):
            // Completely eliminated dual-POV switching between Quad and Projection layers.
            // OpenXR strictly renders XrCompositionLayerProjection at all times.
            // Invariant full VR projection fills physical lenses with zero POV switching, portal expansions, or menu flashing.
            layer.layerFlags = 0;
            layer.space = m_appSpace;
            layer.viewCount = 2;
            layer.views = projViews;
            layers.push_back((XrCompositionLayerBaseHeader*)&layer);

                // Diegetic Wrist Health HUD on Left Glove / Gauntlet (Issue #7 & ADR 0006)
                if (clientState.leftController.gripPose.isValid) {
                    const auto& grip = clientState.leftController.gripPose;

                    // Offset from left grip position to dorsal/top surface of forearm wrist
                    // In controller local space:
                    // +Z is along handle towards forearm (+0.075m), +Y is dorsal/top side (+0.045m), -X is radial side (-0.01m)
                    RiddickVRVector3 localOffset = { -0.01f, 0.045f, 0.075f };
                    RiddickVRVector3 worldOffset = RotateVectorByQuat(localOffset, grip.orientation);

                    XrPosef wristPose{};
                    wristPose.position.x = grip.position.x + worldOffset.x;
                    wristPose.position.y = grip.position.y + worldOffset.y;
                    wristPose.position.z = grip.position.z + worldOffset.z;

                    // Rotate quad so its front normal (+Z) points outward from top of wrist (+Y of controller)
                    // and its top edge (+Y) points forward along forearm towards hand (-Z of controller)
                    // This is a -90 deg rotation around controller's X axis: Q_rel = (-0.7071f, 0, 0, 0.7071f)
                    RiddickVRQuaternion qRel = { -0.7071068f, 0.0f, 0.0f, 0.7071068f };
                    wristPose.orientation = MultiplyQuaternions(grip.orientation, qRel);

                    // Glance activation check: verify gauntlet display is turned towards the player's HMD
                    RiddickVRQuaternion quadQuat = { wristPose.orientation.x, wristPose.orientation.y, wristPose.orientation.z, wristPose.orientation.w };
                    RiddickVRVector3 normalWorld = RotateVectorByQuat({ 0.0f, 0.0f, 1.0f }, quadQuat);

                    float toHmdX = m_locatedViews[0].pose.position.x - wristPose.position.x;
                    float toHmdY = m_locatedViews[0].pose.position.y - wristPose.position.y;
                    float toHmdZ = m_locatedViews[0].pose.position.z - wristPose.position.z;
                    float glanceDot = normalWorld.x * toHmdX + normalWorld.y * toHmdY + normalWorld.z * toHmdZ;

                    // Render when gauntlet is oriented towards player's view (glance angle > -0.1f)
                    if (glanceDot > -0.1f) {
                        wristQuadLayer.layerFlags = 0;
                        wristQuadLayer.space = m_appSpace ? m_appSpace : (m_viewSpace ? m_viewSpace : XR_NULL_HANDLE);
                        wristQuadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                        wristQuadLayer.subImage.swapchain = m_swapchains[0].handle;

                        // Crop upper-left health box and stealth eyeshine status region from swapchain
                        float cropW = clientState.eyeshineActive ? 0.28f : 0.26f;
                        float cropH = clientState.eyeshineActive ? 0.18f : 0.16f;
                        int32_t hudW = static_cast<int32_t>(m_swapchains[0].width * cropW);
                        int32_t hudH = static_cast<int32_t>(m_swapchains[0].height * cropH);
                        wristQuadLayer.subImage.imageRect.offset = { 0, 0 };
                        wristQuadLayer.subImage.imageRect.extent = { hudW, hudH };

                        wristQuadLayer.pose = wristPose;
                        wristQuadLayer.size.width = 0.14f;  // 14 cm gauntlet biomonitor width
                        wristQuadLayer.size.height = 0.07f; // 7 cm gauntlet biomonitor height

                        layers.push_back((XrCompositionLayerBaseHeader*)&wristQuadLayer);
                    }
                }

                // Diegetic Weapon Ammo Counter & Holo-Display on Right Hand Weapon Receiver (ADR 0010)
                if (clientState.rightController.gripPose.isValid) {
                    const auto& weaponPose = clientState.rightController.aimPose.isValid ?
                        clientState.rightController.aimPose : clientState.rightController.gripPose;

                    // Local offset on firearm receiver / slide backplate:
                    // Aim pose: forward is -Z, Up is +Y, Right is +X.
                    // Slide backplate is located along +Z (+0.06m) and slightly elevated along +Y (+0.025m)
                    RiddickVRVector3 localOffset = { 0.0f, 0.025f, 0.060f };
                    RiddickVRVector3 worldOffset = RotateVectorByQuat(localOffset, weaponPose.orientation);

                    XrPosef ammoPose{};
                    ammoPose.position.x = weaponPose.position.x + worldOffset.x;
                    ammoPose.position.y = weaponPose.position.y + worldOffset.y;
                    ammoPose.position.z = weaponPose.position.z + worldOffset.z;

                    // Face rearward towards shooter (+Z in aim pose):
                    // Rotate 180 degrees around Y axis so quad normal (+Z) points directly along aim pose +Z (towards shooter's eyes)
                    // Q_rel = (0, 1, 0, 0)
                    RiddickVRQuaternion qRel = { 0.0f, 1.0f, 0.0f, 0.0f };
                    ammoPose.orientation = MultiplyQuaternions(weaponPose.orientation, qRel);

                    // Aim / Sight alignment check: verify rear of receiver faces towards player's HMD
                    RiddickVRQuaternion quadQuat = { ammoPose.orientation.x, ammoPose.orientation.y, ammoPose.orientation.z, ammoPose.orientation.w };
                    RiddickVRVector3 normalWorld = RotateVectorByQuat({ 0.0f, 0.0f, 1.0f }, quadQuat);

                    float toHmdX = m_locatedViews[0].pose.position.x - ammoPose.position.x;
                    float toHmdY = m_locatedViews[0].pose.position.y - ammoPose.position.y;
                    float toHmdZ = m_locatedViews[0].pose.position.z - ammoPose.position.z;
                    float distSq = toHmdX * toHmdX + toHmdY * toHmdY + toHmdZ * toHmdZ;

                    if (distSq > 0.0001f) {
                        float invDist = 1.0f / sqrtf(distSq);
                        float ammoGlanceDot = (normalWorld.x * toHmdX + normalWorld.y * toHmdY + normalWorld.z * toHmdZ) * invDist;

                        // Display when rear receiver is facing player within ~70 degree viewing cone (dot > 0.35f)
                        if (ammoGlanceDot > 0.35f) {
                            weaponAmmoQuadLayer.layerFlags = 0;
                            weaponAmmoQuadLayer.space = m_appSpace ? m_appSpace : (m_viewSpace ? m_viewSpace : XR_NULL_HANDLE);
                            weaponAmmoQuadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                            weaponAmmoQuadLayer.subImage.swapchain = m_swapchains[0].handle;

                            // Crop bottom-right weapon ammo / clip indicator from swapchain
                            int32_t ammoX = static_cast<int32_t>(m_swapchains[0].width * m_ammoQuadCropX);
                            int32_t ammoY = static_cast<int32_t>(m_swapchains[0].height * m_ammoQuadCropY);
                            int32_t ammoW = static_cast<int32_t>(m_swapchains[0].width * m_ammoQuadCropW);
                            int32_t ammoH = static_cast<int32_t>(m_swapchains[0].height * m_ammoQuadCropH);
                            weaponAmmoQuadLayer.subImage.imageRect.offset = { ammoX, ammoY };
                            weaponAmmoQuadLayer.subImage.imageRect.extent = { ammoW, ammoH };

                            weaponAmmoQuadLayer.pose = ammoPose;
                            weaponAmmoQuadLayer.size.width = 0.055f;  // 5.5 cm weapon display width
                            weaponAmmoQuadLayer.size.height = 0.040f; // 4.0 cm weapon display height

                            layers.push_back((XrCompositionLayerBaseHeader*)&weaponAmmoQuadLayer);
                        }
                    }
                }
            }

        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = (uint32_t)layers.size();
        endInfo.layers = layers.data();
        xrEndFrame(m_session, &endInfo);
    }

    if (frameReady) {
        ipcHost.SignalFrameConsumed();
    }
}

bool OpenXRManager::SaveTextureToBMP(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, const std::string& filename) {
    if (!device || !context || !texture) return false;

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* stagingTex = nullptr;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
    if (FAILED(hr) || !stagingTex) return false;

    context->CopyResource(stagingTex, texture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        stagingTex->Release();
        return false;
    }

    FILE* fp = nullptr;
    fopen_s(&fp, filename.c_str(), "wb");
    if (fp) {
        #pragma pack(push, 1)
        struct BMPFileHeader {
            uint16_t bfType = 0x4D42;
            uint32_t bfSize = 0;
            uint16_t bfReserved1 = 0;
            uint16_t bfReserved2 = 0;
            uint32_t bfOffBits = 54;
            uint32_t biSize = 40;
            int32_t  biWidth = 0;
            int32_t  biHeight = 0;
            uint16_t biPlanes = 1;
            uint16_t biBitCount = 32;
            uint32_t biCompression = 0;
            uint32_t biSizeImage = 0;
            int32_t  biXPelsPerMeter = 2835;
            int32_t  biYPelsPerMeter = 2835;
            uint32_t biClrUsed = 0;
            uint32_t biClrImportant = 0;
        } header;
        #pragma pack(pop)

        header.biWidth = static_cast<int32_t>(desc.Width);
        header.biHeight = -static_cast<int32_t>(desc.Height); // Top-down
        header.biSizeImage = desc.Width * desc.Height * 4;
        header.bfSize = sizeof(header) + header.biSizeImage;

        fwrite(&header, sizeof(header), 1, fp);

        const uint8_t* pSrc = static_cast<const uint8_t*>(mapped.pData);
        std::vector<uint8_t> row(desc.Width * 4);

        for (uint32_t y = 0; y < desc.Height; ++y) {
            const uint8_t* rowSrc = pSrc + y * mapped.RowPitch;
            for (uint32_t x = 0; x < desc.Width; ++x) {
                // RGBA to BGRA
                row[x * 4 + 0] = rowSrc[x * 4 + 2];
                row[x * 4 + 1] = rowSrc[x * 4 + 1];
                row[x * 4 + 2] = rowSrc[x * 4 + 0];
                row[x * 4 + 3] = rowSrc[x * 4 + 3];
            }
            fwrite(row.data(), 1, desc.Width * 4, fp);
        }

        fclose(fp);
    }

    context->Unmap(stagingTex, 0);
    stagingTex->Release();
    return (fp != nullptr);
}

// GH #48: file-driven scenario trajectories. Strict flat-JSON subset, no
// external deps: {"name":..,"markers":[..],"keys":[{frame,lx,ly,lz,lgrip,
// ltrig,lbtn,rx,ry,rz,rgrip,rtrig,rbtn,stickLX,stickLY,stickRX,hyaw,hpitch}]}.
// Poses are WORLD coords (mock HMD at 1.70m, identity yaw unless hyaw set).
bool OpenXRManager::LoadScenario(const std::string& path) {
    m_scenarioKeys.clear();
    m_scenarioLoaded = false;
    std::ifstream f(path);
    if (!f.good()) {
        std::cout << "[Scenario] cannot open " << path << std::endl;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    size_t ka = text.find("\"keys\"");
    if (ka == std::string::npos) {
        std::cout << "[Scenario] no \"keys\" array in " << path << std::endl;
        return false;
    }
    auto num = [&](const std::string& obj, const char* key, float& out) {
        std::string pat = std::string("\"") + key + "\"";
        size_t p = obj.find(pat);
        if (p == std::string::npos) return false;
        p = obj.find(':', p);
        if (p == std::string::npos) return false;
        try { out = std::stof(obj.substr(p + 1)); } catch (...) { return false; }
        return true;
    };
    size_t pos = text.find('[', ka);
    int count = 0;
    while (pos != std::string::npos) {
        size_t o = text.find('{', pos);
        size_t c = (o == std::string::npos) ? std::string::npos : text.find('}', o);
        if (o == std::string::npos || c == std::string::npos) break;
        std::string obj = text.substr(o, c - o + 1);
        ScenarioKey k;
        float v = 0;
        bool ok = num(obj, "frame", v); k.frame = (uint32_t)v;
        ok = num(obj, "lx", k.lx) && ok; ok = num(obj, "ly", k.ly) && ok;
        ok = num(obj, "lz", k.lz) && ok; ok = num(obj, "lgrip", k.lgrip) && ok;
        ok = num(obj, "ltrig", k.ltrig) && ok; ok = num(obj, "lbtn", v) && ok;
        k.lbtn = (uint32_t)v;
        ok = num(obj, "rx", k.rx) && ok; ok = num(obj, "ry", k.ry) && ok;
        ok = num(obj, "rz", k.rz) && ok; ok = num(obj, "rgrip", k.rgrip) && ok;
        ok = num(obj, "rtrig", k.rtrig) && ok; ok = num(obj, "rbtn", v) && ok;
        k.rbtn = (uint32_t)v;
        if (!num(obj, "stickLX", k.stickLX)) k.stickLX = 0;
        if (!num(obj, "stickLY", k.stickLY)) k.stickLY = 0;
        if (!num(obj, "stickRX", k.stickRX)) k.stickRX = 0;
        if (!num(obj, "hyaw", k.hyaw)) k.hyaw = 999.0f;
        if (!num(obj, "hpitch", k.hpitch)) k.hpitch = 999.0f;
        if (!ok) {
            std::cout << "[Scenario] bad keyframe #" << count << " in " << path << std::endl;
            return false;
        }
        m_scenarioKeys.push_back(k);
        count++;
        pos = c + 1;
        if (text[pos] == ']') break;
    }
    if (m_scenarioKeys.empty()) {
        std::cout << "[Scenario] no keyframes in " << path << std::endl;
        return false;
    }
    m_scenarioLoaded = true;
    std::cout << "[Scenario] loaded " << path << " (" << count << " keys)" << std::endl;
    return true;
}

void OpenXRManager::ApplyScenarioFrame(RiddickVRSharedState& state, uint32_t frame) {
    if (!m_scenarioLoaded || m_scenarioKeys.empty()) return;
    // GH #68: hold the timeline at its first key until gameplay is detected.
    if (m_scenarioStartOnGameplay) {
        frame = (m_scenarioStartFrame != UINT32_MAX) ? (frame - m_scenarioStartFrame) : 0;
    }
    const ScenarioKey* a = &m_scenarioKeys.front();
    const ScenarioKey* b = &m_scenarioKeys.back();
    if (frame <= a->frame) { b = a; }
    else if (frame >= b->frame) { a = b; }
    else {
        for (size_t i = 0; i + 1 < m_scenarioKeys.size(); ++i) {
            if (m_scenarioKeys[i].frame <= frame && frame <= m_scenarioKeys[i + 1].frame) {
                a = &m_scenarioKeys[i];
                b = &m_scenarioKeys[i + 1];
                break;
            }
        }
    }
    float t = (b->frame > a->frame) ? (float)(frame - a->frame) / (float)(b->frame - a->frame) : 0.0f;
    auto lerp = [&](float x, float y) { return x + (y - x) * t; };
    const float frameSpan = (b->frame > a->frame) ? static_cast<float>(b->frame - a->frame) : 0.0f;
    auto velocity = [&](float x, float y) {
        // Keyframe time is expressed in 90 Hz mock-host frames. Supply actual
        // linear velocity so pull/strike/throw detectors see the scripted move.
        return (frameSpan > 0.0f && frame >= a->frame && frame <= b->frame)
            ? (y - x) * (90.0f / frameSpan) : 0.0f;
    };
    auto& L = state.leftController;
    auto& R = state.rightController;
    L.gripPose.position = { lerp(a->lx, b->lx), lerp(a->ly, b->ly), lerp(a->lz, b->lz) };
    R.gripPose.position = { lerp(a->rx, b->rx), lerp(a->ry, b->ry), lerp(a->rz, b->rz) };
    L.gripPose.linearVelocity = { velocity(a->lx, b->lx), velocity(a->ly, b->ly), velocity(a->lz, b->lz) };
    R.gripPose.linearVelocity = { velocity(a->rx, b->rx), velocity(a->ry, b->ry), velocity(a->rz, b->rz) };
    L.grip = lerp(a->lgrip, b->lgrip);
    L.trigger = lerp(a->ltrig, b->ltrig);
    R.grip = lerp(a->rgrip, b->rgrip);
    R.trigger = lerp(a->rtrig, b->rtrig);
    // Buttons/sticks snap (no interpolation for discrete inputs).
    L.buttons = (t < 0.5f) ? a->lbtn : b->lbtn;
    R.buttons = (t < 0.5f) ? a->rbtn : b->rbtn;
    L.thumbstickX = (t < 0.5f) ? a->stickLX : b->stickLX;
    L.thumbstickY = (t < 0.5f) ? a->stickLY : b->stickLY;
    R.thumbstickX = (t < 0.5f) ? a->stickRX : b->stickRX;
    L.aimPose = L.gripPose;
    R.aimPose = R.gripPose;
    // Fixed head unless keyframes steer it (999 = keep ambient oscillation).
    float hyaw = lerp(a->hyaw, b->hyaw);
    float hpitch = lerp(a->hpitch, b->hpitch);
    if (hyaw < 900.0f || hpitch < 900.0f) {
        if (hyaw > 900.0f) hyaw = 0.0f;
        if (hpitch > 900.0f) hpitch = 0.0f;
        float yaw = hyaw * 0.0174533f, pitch = hpitch * 0.0174533f;
        float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f);
        float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
        state.hmdPose.orientation.x = cy * sp;
        state.hmdPose.orientation.y = sy * cp;
        state.hmdPose.orientation.z = -sy * sp;
        state.hmdPose.orientation.w = cy * cp;
    }
}

void OpenXRManager::RunLoop(IpcBridgeHost& ipcHost, bool autoExitWithGame) {
    if (m_mockMode) {
        std::cout << "[Host] ========================================================\n";
        std::cout << "[Host] RUNNING IN HEADLESS MOCK / SELF-PLAY TEST MODE\n";
        std::cout << "[Host] Simulating Meta Quest 2 VR Headset (2496x2688 @ 90FPS)\n";
        if (!m_dumpDir.empty()) {
            std::cout << "[Host] Artifact frame dump directory: " << m_dumpDir << " (Interval: " << m_dumpInterval << ")\n";
            CreateDirectoryA(m_dumpDir.c_str(), NULL);
        }
        std::cout << "[Host] ========================================================" << std::endl;

        // GH #48: load file-driven scenario once (no rebuild needed for new ones).
        if (!m_scenarioPath.empty() && !m_scenarioLoaded) {
            LoadScenario(m_scenarioPath);
        }

        m_recommendedWidth = 2496;
        m_recommendedHeight = 2688;

        auto lastReport = std::chrono::steady_clock::now();
        uint32_t framesCounted = 0;
        uint32_t totalFrames = 0;
        bool hadClient = false;
        // GH #21/#38 coverage: observe hook->host haptic pulses in mock mode
        // to prove the publish path works end-to-end without a headset.
        bool sawHookHaptics = false;
        bool lastFrameCinema = true; // GH #68: until the game says otherwise
        uint32_t hapticFrames = 0;
        auto lastClientSeen = std::chrono::steady_clock::now();

        while (m_running) {
            auto now = std::chrono::steady_clock::now();
            bool clientConnected = ipcHost.IsClientConnected();
            if (clientConnected) {
                hadClient = true;
                lastClientSeen = now;
            } else if (autoExitWithGame && hadClient) {
                if (std::chrono::duration_cast<std::chrono::seconds>(now - lastClientSeen).count() >= 3) {
                    std::cout << "[Host] Game process disconnected. Exiting mock host..." << std::endl;
                    break;
                }
            }

            RiddickVRSharedState state{};
            state.protocolVersion = RIDDICK_VR_PROTOCOL_VERSION;
            state.vrActive = true;
            state.clientConnected = clientConnected;
            state.renderWidth = m_recommendedWidth;
            state.renderHeight = m_recommendedHeight;
            state.heartbeatTimestamp = static_cast<uint32_t>(GetTickCount64());
            state.frameIndex = ++m_frameIndex;

            state.hmdPose.isValid = true;
            state.hmdPose.position = { 0.0f, 1.70f, 0.0f };
            state.hmdPose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };

            constexpr float HALF_IPD = kHalfIpdMeters;
            for (int i = 0; i < 16; ++i) {
                state.leftEyeView.m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
                state.rightEyeView.m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
            }
            state.leftEyeView.m[12] = -HALF_IPD;
            state.rightEyeView.m[12] = +HALF_IPD;

            constexpr float fovTan = 1.0f;
            for (int i = 0; i < 16; ++i) {
                state.leftEyeProjection.m[i] = 0.0f;
                state.rightEyeProjection.m[i] = 0.0f;
            }
            state.leftEyeProjection.m[0] = 1.0f / fovTan;
            state.leftEyeProjection.m[5] = 1.0f / fovTan;
            state.leftEyeProjection.m[10] = -1.0001f;
            state.leftEyeProjection.m[11] = -1.0f;
            state.leftEyeProjection.m[14] = -0.1000f;
            state.rightEyeProjection = state.leftEyeProjection;

            state.leftController.isConnected = true;
            state.rightController.isConnected = true;
            state.leftController.buttons = RIDDICK_VR_TOUCH_TRIGGER | RIDDICK_VR_TOUCH_THUMBSTICK;
            state.rightController.buttons = RIDDICK_VR_TOUCH_TRIGGER | RIDDICK_VR_TOUCH_THUMBSTICK;
            state.leftController.gripPose.isValid = true;
            state.rightController.gripPose.isValid = true;
            state.leftController.aimPose.isValid = true;
            state.rightController.aimPose.isValid = true;
            state.leftController.gripPose.position = { -0.2f, 1.2f, -0.3f };
            state.rightController.gripPose.position = { 0.2f, 1.2f, -0.3f };
            state.leftController.gripPose.orientation = { 0, 0, 0, 1 };
            state.rightController.gripPose.orientation = { 0, 0, 0, 1 };
            state.leftController.aimPose = state.leftController.gripPose;
            state.rightController.aimPose = state.rightController.gripPose;
            state.leftController.grip = 0.5f + 0.45f * sinf(totalFrames * 0.08f);
            state.rightController.grip = 0.5f + 0.45f * cosf(totalFrames * 0.08f);
            state.leftController.trigger = 0.5f + 0.45f * sinf(totalFrames * 0.12f);
            state.rightController.trigger = 0.5f + 0.45f * cosf(totalFrames * 0.12f);

            // Automated self-play simulation:
            // 1. Natural head orientation oscillation (looking around in VR)
            float t = totalFrames * 0.05f;
            float simYaw = sinf(t * 0.8f) * 0.35f;   // +/- 20 degrees yaw
            float simPitch = cosf(t * 0.5f) * 0.18f; // +/- 10 degrees pitch
            float cy = cosf(simYaw * 0.5f), sy = sinf(simYaw * 0.5f);
            float cp = cosf(simPitch * 0.5f), sp = sinf(simPitch * 0.5f);
            state.hmdPose.orientation.x = cy * sp;
            state.hmdPose.orientation.y = sy * cp;
            state.hmdPose.orientation.z = -sy * sp;
            state.hmdPose.orientation.w = cy * cp;

            // 2. Intermittent button tapping:
            // Tap Button A (confirm / select / interact / talk)
            if ((totalFrames % 60) >= 30 && (totalFrames % 60) <= 36) {
                state.rightController.buttons |= RIDDICK_VR_BTN_A;
            }
            // Tap Button B (dismiss popup / smokes / back / jump)
            if ((totalFrames % 100) >= 80 && (totalFrames % 100) <= 86) {
                state.rightController.buttons |= RIDDICK_VR_BTN_B;
            }
            // Tap Button Y (Riddick Eyeshine night vision) so dark scenes stay
            // visible in captures: 6-frame press every 200 frames.
            if ((totalFrames % 200) >= 100 && (totalFrames % 200) <= 105) {
                state.leftController.buttons |= RIDDICK_VR_BTN_Y;
            }

            // 3. Locomotion: Move forward into level and turn
            if (totalFrames > 60) {
                state.leftController.thumbstickY = 0.85f; // Walk forward
            }
            if (totalFrames > 120 && (totalFrames % 120) > 60) {
                state.rightController.thumbstickX = 0.4f; // Turn slightly right
            }

            // 4. Semi-play gesture scripting (GH #48): file-driven scenario wins
            // when one is loaded (tools/scenarios/*.json, --scenario flag);
            // otherwise the legacy built-in 360-frame loop exercises hands.
            if (m_scenarioLoaded) {
                ApplyScenarioFrame(state, totalFrames);
            } else {
            uint32_t phase = totalFrames % 360;
            if (phase >= 120 && phase < 180) {
                // Reload: off-hand to weapon receiver (<0.18m) with grip + upward seating.
                state.leftController.gripPose.position = { 0.12f, 1.25f, -0.32f };
                state.rightController.gripPose.position = { 0.20f, 1.20f, -0.30f };
                state.leftController.grip = 0.80f;
                state.leftController.trigger = 0.10f;
                state.rightController.grip = 0.70f;
            } else if (phase >= 180 && phase < 240) {
                // Waist ammo pouch / NanoMED cartridge draw: left hand down to waist.
                state.leftController.gripPose.position = { -0.25f, 1.05f, -0.25f };
                state.leftController.grip = 0.85f;
                state.leftController.trigger = 0.10f;
            } else if (phase >= 240 && phase < 300) {
                // NanoMED palm heal: open flat palm forward into scanner volume.
                state.leftController.gripPose.position = { -0.10f, 1.60f, -0.45f };
                state.leftController.grip = 0.05f;
                state.leftController.trigger = 0.05f;
                state.rightController.gripPose.position = { 0.10f, 1.60f, -0.45f };
                state.rightController.grip = 0.05f;
                state.rightController.trigger = 0.05f;
            } else if (phase >= 300) {
                // Grab / interact reach: right hand forward to object/person volume.
                state.rightController.gripPose.position = { 0.15f, 1.45f, -0.55f };
                state.rightController.grip = 0.90f;
            }
            } // end legacy gesture loop

            ipcHost.WriteState(state);

            bool frameReady = ipcHost.WaitForFrameReady(20);
            if (frameReady) {
                RiddickVRSharedState clientState{};
                if (ipcHost.ReadState(clientState)) {
                    if (clientState.leftEyeSharedHandle && clientState.rightEyeSharedHandle) {
                        EnsureSharedTextures(clientState.leftEyeSharedHandle, clientState.rightEyeSharedHandle);
                    }
                    // GH #68: cinema/menu frames are mono by design and must not
                    // start the scenario or count toward the frame budget.
                    lastFrameCinema = clientState.cinemaMode;
                    if (clientState.cinemaMode) {
                        m_gameplayStreak = 0;
                    } else if (m_gameplayStreak < UINT32_MAX) {
                        m_gameplayStreak++;
                    }
                    if (m_scenarioLoaded && m_scenarioStartFrame == UINT32_MAX && m_gameplayStreak >= 30) {
                        m_scenarioStartFrame = totalFrames;
                        std::cout << "[Mock Host] Gameplay detected at frame " << totalFrames
                                  << "; scenario timeline starts" << std::endl;
                    }
                    // GH #21/#38: consume-on-read mirror of RenderFrame — log
                    // hook pulses once, then clear so latching is detectable.
                    if (clientState.leftHapticPulse > 0.05f || clientState.rightHapticPulse > 0.05f) {
                        hapticFrames++;
                        if (!sawHookHaptics) {
                            sawHookHaptics = true;
                            std::cout << "[Mock Host] Hook haptic pulse observed (L="
                                      << clientState.leftHapticPulse << " R="
                                      << clientState.rightHapticPulse << ")" << std::endl;
                        }
                        ipcHost.ClearHaptics();
                    }
                }

                totalFrames++;
                framesCounted++;

                // GH #68: frame-time budget sampling (host frame cadence). Emits a
                // parseable summary the simulator harness reads; budget = 90 Hz.
                {
                    static auto s_prevFrameTs = std::chrono::steady_clock::now();
                    static double s_sumFrameMs = 0.0, s_maxFrameMs = 0.0;
                    static uint64_t s_frameSamples = 0, s_overBudget = 0;
                    static const double s_budgetMs = 1000.0 / 90.0; // 11.11 ms @ 90 Hz
                    const auto nowTs = std::chrono::steady_clock::now();
                    const double dtMs = std::chrono::duration<double, std::milli>(nowTs - s_prevFrameTs).count();
                    s_prevFrameTs = nowTs;
                    // GH #68: only gameplay frames count; cinema/menu pacing is not the mod's budget.
                    if (!lastFrameCinema && dtMs > 0.0 && dtMs < 1000.0) { // ignore idle gaps
                        s_sumFrameMs += dtMs;
                        if (dtMs > s_maxFrameMs) s_maxFrameMs = dtMs;
                        if (dtMs > s_budgetMs) s_overBudget++;
                        s_frameSamples++;
                        if (s_frameSamples % 30 == 0) {
                            std::cout << "[Mock Host] FrameBudget samples=" << s_frameSamples
                                      << " mean_ms=" << std::fixed << std::setprecision(2)
                                      << (s_sumFrameMs / s_frameSamples)
                                      << " max_ms=" << s_maxFrameMs
                                      << " over_budget=" << s_overBudget
                                      << " budget_ms=" << s_budgetMs
                                      << " gameplay_only=1" << std::endl;
                        }
                    }
                }

                // GH #68: capture BOTH eyes for the XR-simulator stereo diff.
                // test_frame_####.bmp stays the LEFT eye (legacy tooling), and
                // right_####.bmp is the RIGHT eye.
                if (!m_dumpDir.empty() && m_sharedLeftTex && m_sharedRightTex &&
                    (totalFrames == 1 || totalFrames % m_dumpInterval == 0)) {
                    char leftPath[MAX_PATH];
                    char rightPath[MAX_PATH];
                    sprintf_s(leftPath, "%s\\test_frame_%04u.bmp", m_dumpDir.c_str(), totalFrames);
                    sprintf_s(rightPath, "%s\\right_%04u.bmp", m_dumpDir.c_str(), totalFrames);
                    const bool okL = SaveTextureToBMP(m_d3d11Device, m_d3d11Context, m_sharedLeftTex, leftPath);
                    const bool okR = SaveTextureToBMP(m_d3d11Device, m_d3d11Context, m_sharedRightTex, rightPath);
                    std::cout << "[Mock Host] Captured stereo frame " << totalFrames
                              << " (L=" << (okL ? "ok" : "err") << " R=" << (okR ? "ok" : "err")
                              << ") -> " << leftPath << " / " << rightPath
                              << " mode=" << (lastFrameCinema ? "cinema" : "gameplay") << std::endl;
                    // GH #68: per-frame pose/matrix sidecar so a capture can be checked offline.
                    char metaPath[MAX_PATH];
                    sprintf_s(metaPath, "%s\\frame_%04u.json", m_dumpDir.c_str(), totalFrames);
                    FILE* meta = nullptr;
                    if (fopen_s(&meta, metaPath, "w") == 0 && meta) {
                        auto writeMat = [&](const char* name, const float* m, bool last) {
                            fprintf(meta, "  \"%s\": [", name);
                            for (int i = 0; i < 16; ++i)
                                fprintf(meta, "%s%.6g", i ? ", " : "", static_cast<double>(m[i]));
                            fprintf(meta, "]%s\n", last ? "" : ",");
                        };
                        fprintf(meta, "{\n  \"frame\": %u,\n  \"mode\": \"%s\",\n", totalFrames,
                                lastFrameCinema ? "cinema" : "gameplay");
                        fprintf(meta, "  \"hmd\": {\"pos\": [%.6g, %.6g, %.6g], \"quat\": [%.6g, %.6g, %.6g, %.6g]},\n",
                                static_cast<double>(state.hmdPose.position.x), static_cast<double>(state.hmdPose.position.y),
                                static_cast<double>(state.hmdPose.position.z),
                                static_cast<double>(state.hmdPose.orientation.x), static_cast<double>(state.hmdPose.orientation.y),
                                static_cast<double>(state.hmdPose.orientation.z), static_cast<double>(state.hmdPose.orientation.w));
                        writeMat("leftEyeView", state.leftEyeView.m, false);
                        writeMat("rightEyeView", state.rightEyeView.m, false);
                        writeMat("leftEyeProjection", state.leftEyeProjection.m, true);
                        fprintf(meta, "}\n");
                        fclose(meta);
                    }
                }

                ipcHost.SignalFrameConsumed();

                if (m_maxFrames > 0 && totalFrames >= m_maxFrames) {
                    std::cout << "[Mock Host] Completed requested " << m_maxFrames << " test frames. Exiting successfully." << std::endl;
                    break;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastReport).count() >= 3) {
                float fps = framesCounted / 3.0f;
                std::cout << "[Mock Host] Test Heartbeat: " << std::fixed << std::setprecision(1) << fps 
                          << " FPS | Total Frames: " << totalFrames << " | Game Connected: " 
                          << (clientConnected ? "YES" : "WAITING")
                          << " | HookHaptics: " << (sawHookHaptics ? "SEEN" : "none")
                          << " (" << hapticFrames << " frames)" << std::endl;
                lastReport = now;
                framesCounted = 0;
            }
        }
        return;
    }

    std::cout << "[Host] Entering VR compositor main loop. Press Ctrl+C in console to terminate." << std::endl;

    auto lastReport = std::chrono::steady_clock::now();
    auto lastRetry = std::chrono::steady_clock::now();
    auto lastClientSeen = std::chrono::steady_clock::now();
    bool hadClient = false;
    uint32_t framesCounted = 0;

    while (m_running && PollEvents()) {
        auto now = std::chrono::steady_clock::now();

        if (ipcHost.IsClientConnected()) {
            hadClient = true;
            lastClientSeen = now;
        } else if (autoExitWithGame && hadClient) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastClientSeen).count() >= 4) {
                std::cout << "[Host] Game process disconnected. Auto-exiting host companion..." << std::endl;
                break;
            }
        }

        // Retry connecting to OpenXR runtime periodically if not yet active
        if (!m_sessionActive || m_session == XR_NULL_HANDLE) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRetry).count() >= 1500) {
                lastRetry = now;
                TryConnectOpenXR();
            }
        }

        RenderFrame(ipcHost);
        framesCounted++;

        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastReport).count() >= 5) {
            float fps = framesCounted / 5.0f;
            std::cout << "[Host] Heartbeat: " << std::fixed << std::setprecision(1) << fps << " FPS | Connected: " 
                      << (ipcHost.IsClientConnected() ? "YES" : "WAITING FOR GAME")
                      << " | OpenXR Session: " << (m_sessionActive ? "RUNNING (IMMERSIVE VR)" : "SEARCHING FOR HEADSET...") << std::endl;
            framesCounted = 0;
            lastReport = now;
        }

        if (!ipcHost.IsClientConnected() && !m_sessionActive) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

} // namespace RiddickVR
