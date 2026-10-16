#include "opengl_proxy.h"
#include "opengl_forwarders.h"
#include "stereo_renderer.h"
#include "camera_hook.h"
#include "vr_config.h"
#include "viewmodel_suppressor.h"
#include "viewmodel_drive.h"
#include "logging.h"
#include <string>

namespace RiddickVR {

HMODULE OpenGLProxy::s_hRealOpenGL = nullptr;
bool OpenGLProxy::s_initialized = false;

// Extension pointers
PFNGLBINDFRAMEBUFFERPROC OpenGLProxy::glBindFramebuffer = nullptr;
PFNGLGENFRAMEBUFFERSPROC OpenGLProxy::glGenFramebuffers = nullptr;
PFNGLDELETEFRAMEBUFFERSPROC OpenGLProxy::glDeleteFramebuffers = nullptr;
PFNGLFRAMEBUFFERTEXTURE2DPROC OpenGLProxy::glFramebufferTexture2D = nullptr;
PFNGLCHECKFRAMEBUFFERSTATUSPROC OpenGLProxy::glCheckFramebufferStatus = nullptr;
PFNGLBLITFRAMEBUFFERPROC OpenGLProxy::glBlitFramebuffer = nullptr;
PFNGLUNIFORMMATRIX4FVPROC OpenGLProxy::glUniformMatrix4fv = nullptr;

PFNGLGENRENDERBUFFERSPROC OpenGLProxy::glGenRenderbuffers = nullptr;
PFNGLBINDRENDERBUFFERPROC OpenGLProxy::glBindRenderbuffer = nullptr;
PFNGLRENDERBUFFERSTORAGEPROC OpenGLProxy::glRenderbufferStorage = nullptr;
PFNGLFRAMEBUFFERRENDERBUFFERPROC OpenGLProxy::glFramebufferRenderbuffer = nullptr;
PFNGLDELETERENDERBUFFERSPROC OpenGLProxy::glDeleteRenderbuffers = nullptr;

PFNGLPROGRAMENVPARAMETER4FVARBPROC OpenGLProxy::glProgramEnvParameter4fvARB = nullptr;
PFNGLPROGRAMLOCALPARAMETER4FVARBPROC OpenGLProxy::glProgramLocalParameter4fvARB = nullptr;

PFNWGLDXOPENDEVICENVPROC OpenGLProxy::wglDXOpenDeviceNV = nullptr;
PFNWGLDXCLOSEDEVICENVPROC OpenGLProxy::wglDXCloseDeviceNV = nullptr;
PFNWGLDXREGISTEROBJECTNVPROC OpenGLProxy::wglDXRegisterObjectNV = nullptr;
PFNWGLDXUNREGISTEROBJECTNVPROC OpenGLProxy::wglDXUnregisterObjectNV = nullptr;
PFNWGLDXOBJECTACCESSNVPROC OpenGLProxy::wglDXObjectAccessNV = nullptr;
PFNWGLDXLOCKOBJECTSNVPROC OpenGLProxy::wglDXLockObjectsNV = nullptr;
PFNWGLDXUNLOCKOBJECTSNVPROC OpenGLProxy::wglDXUnlockObjectsNV = nullptr;

PFN_glBindTexture OpenGLProxy::glBindTexture = nullptr;
PFN_glGenTextures OpenGLProxy::glGenTextures = nullptr;
PFN_glDeleteTextures OpenGLProxy::glDeleteTextures = nullptr;
PFN_glTexImage2D OpenGLProxy::glTexImage2D = nullptr;
PFN_glTexParameteri OpenGLProxy::glTexParameteri = nullptr;
OpenGLProxy::PFN_glReadBuffer OpenGLProxy::glReadBuffer = nullptr;
OpenGLProxy::PFN_glColorMask OpenGLProxy::glColorMask = nullptr;
OpenGLProxy::PFN_glClearColor OpenGLProxy::glClearColor = nullptr;
OpenGLProxy::PFN_glFlush OpenGLProxy::glFlush = nullptr;
OpenGLProxy::PFN_glFinish OpenGLProxy::glFinish = nullptr;
OpenGLProxy::PFN_real_glClear OpenGLProxy::real_glClearFunc = nullptr;
PFNGLBINDFRAMEBUFFERPROC OpenGLProxy::real_glBindFramebuffer = nullptr;

// Real WGL & GL Typedefs
typedef HGLRC (WINAPI *PFN_wglCreateContext)(HDC);
typedef BOOL  (WINAPI *PFN_wglDeleteContext)(HGLRC);
typedef BOOL  (WINAPI *PFN_wglMakeCurrent)(HDC, HGLRC);
typedef BOOL  (WINAPI *PFN_wglSwapBuffers)(HDC);
typedef PROC  (WINAPI *PFN_wglGetProcAddress)(LPCSTR);
typedef void  (WINAPI *PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void  (WINAPI *PFN_glScissor)(GLint, GLint, GLsizei, GLsizei);
typedef void  (WINAPI *PFN_glMatrixMode)(GLenum);
typedef void  (WINAPI *PFN_glLoadMatrixf)(const GLfloat*);
typedef void  (WINAPI *PFN_glFrustum)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
typedef void  (WINAPI *PFN_glOrtho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
typedef void  (WINAPI *PFN_glClear)(GLbitfield);

static PFN_wglCreateContext real_wglCreateContext = nullptr;
static PFN_wglDeleteContext real_wglDeleteContext = nullptr;
static PFN_wglMakeCurrent   real_wglMakeCurrent = nullptr;
static PFN_wglSwapBuffers   real_wglSwapBuffers = nullptr;
static PFN_wglGetProcAddress real_wglGetProcAddress = nullptr;
static PFN_glViewport       real_glViewport = nullptr;
static PFN_glScissor        real_glScissor = nullptr;
static PFN_glMatrixMode     real_glMatrixMode = nullptr;
static PFN_glLoadMatrixf    real_glLoadMatrixf = nullptr;
static PFN_glFrustum        real_glFrustum = nullptr;
static PFN_glOrtho          real_glOrtho = nullptr;
static PFN_glClear          real_glClear = nullptr;

static GLenum s_currentMatrixMode = 0x1700; // GL_MODELVIEW
static float s_lastProjection[16] = {0};
static float s_lastView[16] = {0};
static bool s_hasProjection = false;
static bool s_has3DGameplayProjection = false;
// GH #43/#44: per-frame 3D freshness. Menus/cinematics use ortho/2D
// projections; gameplay uses perspective. The latch above never resets, so
// stamp the swap-frame of each perspective sighting instead.
static uint64_t s_swapFrameCount = 0;
static uint64_t s_last3DFrame = 0;
// GH #43: menus show the OS cursor while 3D backgrounds may still render
// perspective behind them - track cursor visibility per swap as well.
static uint64_t s_lastCursorFrame = 0;

void OpenGLProxy::NoteMenuCursor(bool visible) {
    if (visible) s_lastCursorFrame = s_swapFrameCount;
}

bool OpenGLProxy::HasMenuCursor() {
    return (s_swapFrameCount - s_lastCursorFrame <= 3);
}
static uint32_t s_projMatrixCount = 0;
static uint32_t s_modelviewMatrixCount = 0;

// GH #63/#66 (approach 2): the per-draw viewmodel suppression window (see
// viewmodel_suppressor.h). Fed by the intercepted glClear / projection / swap
// calls below; queried once per draw by the RndrGL draw hooks. Configured from
// the INI on every clear so a config edit takes effect without a rebuild.
static ViewmodelSuppressor s_viewmodelSuppressor;

// GH #67: drive the engine viewmodel with the controller only when it is not
// being suppressed (suppression wins; warned once in BuildViewmodelSuppressionConfig).
static bool ViewmodelDriveActive() {
    return g_vrConfig.driveEngineViewmodel != 0 && g_vrConfig.suppressEngineViewmodel == 0;
}

static ViewmodelSuppressionConfig BuildViewmodelSuppressionConfig() {
    static bool s_warnedDriveAndSuppress = false;
    if (g_vrConfig.driveEngineViewmodel != 0 && g_vrConfig.suppressEngineViewmodel != 0 &&
        !s_warnedDriveAndSuppress) {
        s_warnedDriveAndSuppress = true;
        VR_LOG_WARN("DriveEngineViewmodel=1 ignored: SuppressEngineViewmodel=1 hides the engine arms (suppression wins)");
    }
    ViewmodelSuppressionConfig cfg;
    cfg.enabled = (g_vrConfig.suppressEngineViewmodel != 0);
    cfg.track = ViewmodelDriveActive() || g_vrConfig.viewmodelTrace != 0;
    switch (g_vrConfig.viewmodelWindowOpener) {
        case 0: cfg.opener = ViewmodelWindowOpener::None; break;
        case 1: cfg.opener = ViewmodelWindowOpener::DepthOnlyClear; break;
        case 2: cfg.opener = ViewmodelWindowOpener::NearPlaneDrop; break;
        case 3: cfg.opener = ViewmodelWindowOpener::Either; break;
        default:
            cfg.opener = (g_vrConfig.viewmodelDepthClearWindow != 0)
                ? ViewmodelWindowOpener::DepthOnlyClear
                : ViewmodelWindowOpener::None;
            break;
    }
    cfg.nearRatio = g_vrConfig.viewmodelNearRatio;
    return cfg;
}

// GH #67: correction matrix for the current viewmodel window, computed once on
// the first modelview load inside the window and reused for later loads.
static bool s_driveCorrectionCached = false;
static bool s_driveCorrectionValid = false;
static float s_driveCorrection[16] = {0};

static bool ComputeDriveCorrectionForWindow() {
    const RiddickVRSharedState& st = StereoRenderer::Instance().GetLatestState();
    const RiddickVRControllerInput& hand = g_vrConfig.southpaw ? st.leftController : st.rightController;
    if (!st.hmdPose.isValid || !hand.isConnected || !hand.gripPose.isValid) return false;

    ViewmodelDriveConfig dc;
    dc.enabled = true;
    dc.unitsPerMeter = g_vrConfig.viewmodelUnitsPerMeter;
    dc.anchor[0] = g_vrConfig.viewmodelAnchorX;
    dc.anchor[1] = g_vrConfig.viewmodelAnchorY;
    dc.anchor[2] = g_vrConfig.viewmodelAnchorZ;
    dc.yawDeg = g_vrConfig.viewmodelDriveYaw;
    dc.pitchDeg = g_vrConfig.viewmodelDrivePitch;
    dc.rollDeg = g_vrConfig.viewmodelDriveRoll;

    DrivePose hmd;
    hmd.pos[0] = st.hmdPose.position.x; hmd.pos[1] = st.hmdPose.position.y; hmd.pos[2] = st.hmdPose.position.z;
    hmd.quat[0] = st.hmdPose.orientation.x; hmd.quat[1] = st.hmdPose.orientation.y;
    hmd.quat[2] = st.hmdPose.orientation.z; hmd.quat[3] = st.hmdPose.orientation.w;
    DrivePose ctrl;
    ctrl.pos[0] = hand.gripPose.position.x; ctrl.pos[1] = hand.gripPose.position.y; ctrl.pos[2] = hand.gripPose.position.z;
    ctrl.quat[0] = hand.gripPose.orientation.x; ctrl.quat[1] = hand.gripPose.orientation.y;
    ctrl.quat[2] = hand.gripPose.orientation.z; ctrl.quat[3] = hand.gripPose.orientation.w;
    return ComputeViewmodelCorrection(dc, hmd, ctrl, s_driveCorrection);
}

static void RawProxyLog(const char* msg) {
    // GH #26: %TEMP% instead of a hardcoded developer path.
    char path[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, path);
    if (len == 0 || len >= MAX_PATH) {
        strcpy_s(path, "hook_debug.txt");
    } else {
        strcat_s(path, "riddick_vr_hook_debug.txt");
    }
    HANDLE hFile = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, msg, (DWORD)strlen(msg), &written, NULL);
        WriteFile(hFile, "\r\n", 2, &written, NULL);
        CloseHandle(hFile);
    }
}

bool OpenGLProxy::Initialize() {
    if (s_initialized) return true;

    char sysPath[MAX_PATH];
    GetSystemDirectoryA(sysPath, MAX_PATH);
    std::string realDllPath = std::string(sysPath) + "\\opengl32.dll";

    char logBuf[512];
    sprintf_s(logBuf, "OpenGLProxy: sysPath='%s', realDllPath='%s'", sysPath, realDllPath.c_str());
    RawProxyLog(logBuf);

    s_hRealOpenGL = LoadLibraryA(realDllPath.c_str());
    if (!s_hRealOpenGL) {
        DWORD err = GetLastError();
        sprintf_s(logBuf, "OpenGLProxy: LoadLibraryA failed with error %lu!", err);
        RawProxyLog(logBuf);
        VR_LOG_ERROR("Failed to load real system opengl32.dll! Error: %lu", err);
        return false;
    }
    char buf[128];
    sprintf_s(buf, "OpenGLProxy: real opengl32 loaded at %p", (void*)s_hRealOpenGL);
    RawProxyLog(buf);

    real_wglCreateContext = (PFN_wglCreateContext)GetProcAddress(s_hRealOpenGL, "wglCreateContext");
    real_wglDeleteContext = (PFN_wglDeleteContext)GetProcAddress(s_hRealOpenGL, "wglDeleteContext");
    real_wglMakeCurrent   = (PFN_wglMakeCurrent)GetProcAddress(s_hRealOpenGL, "wglMakeCurrent");
    real_wglSwapBuffers   = (PFN_wglSwapBuffers)GetProcAddress(s_hRealOpenGL, "wglSwapBuffers");
    real_wglGetProcAddress = (PFN_wglGetProcAddress)GetProcAddress(s_hRealOpenGL, "wglGetProcAddress");
    real_glViewport       = (PFN_glViewport)GetProcAddress(s_hRealOpenGL, "glViewport");
    real_glScissor        = (PFN_glScissor)GetProcAddress(s_hRealOpenGL, "glScissor");
    real_glMatrixMode     = (PFN_glMatrixMode)GetProcAddress(s_hRealOpenGL, "glMatrixMode");
    real_glLoadMatrixf    = (PFN_glLoadMatrixf)GetProcAddress(s_hRealOpenGL, "glLoadMatrixf");
    real_glFrustum        = (PFN_glFrustum)GetProcAddress(s_hRealOpenGL, "glFrustum");
    real_glOrtho          = (PFN_glOrtho)GetProcAddress(s_hRealOpenGL, "glOrtho");
    real_glClear          = (PFN_glClear)GetProcAddress(s_hRealOpenGL, "glClear");

    glBindTexture         = (PFN_glBindTexture)GetProcAddress(s_hRealOpenGL, "glBindTexture");
    glGenTextures         = (PFN_glGenTextures)GetProcAddress(s_hRealOpenGL, "glGenTextures");
    glDeleteTextures      = (PFN_glDeleteTextures)GetProcAddress(s_hRealOpenGL, "glDeleteTextures");
    glTexImage2D          = (PFN_glTexImage2D)GetProcAddress(s_hRealOpenGL, "glTexImage2D");
    glTexParameteri       = (PFN_glTexParameteri)GetProcAddress(s_hRealOpenGL, "glTexParameteri");
    glReadBuffer          = (PFN_glReadBuffer)GetProcAddress(s_hRealOpenGL, "glReadBuffer");
    glColorMask           = (PFN_glColorMask)GetProcAddress(s_hRealOpenGL, "glColorMask");
    glClearColor          = (PFN_glClearColor)GetProcAddress(s_hRealOpenGL, "glClearColor");
    glFlush               = (PFN_glFlush)GetProcAddress(s_hRealOpenGL, "glFlush");
    glFinish              = (PFN_glFinish)GetProcAddress(s_hRealOpenGL, "glFinish");
    real_glClearFunc      = (PFN_real_glClear)GetProcAddress(s_hRealOpenGL, "glClear");

    InitializeForwarders(s_hRealOpenGL);

    s_initialized = true;
    VR_LOG_INFO("OpenGLProxy initialized successfully.");
    return true;
}

void OpenGLProxy::Shutdown() {
    if (s_hRealOpenGL) {
        FreeLibrary(s_hRealOpenGL);
        s_hRealOpenGL = nullptr;
    }
    s_initialized = false;
}

void OpenGLProxy::QueryContextExtensions() {
    if (!real_wglGetProcAddress) return;

    auto LoadExt = [](const char* name1, const char* name2 = nullptr) -> PROC {
        PROC p = real_wglGetProcAddress(name1);
        if (!p && name2) p = real_wglGetProcAddress(name2);
        return p;
    };

    if (!real_glBindFramebuffer) {
        real_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)LoadExt("glBindFramebuffer", "glBindFramebufferEXT");
        glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)Hook_glBindFramebuffer;
    }
    if (!glGenFramebuffers) glGenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)LoadExt("glGenFramebuffers", "glGenFramebuffersEXT");
    if (!glDeleteFramebuffers) glDeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)LoadExt("glDeleteFramebuffers", "glDeleteFramebuffersEXT");
    if (!glFramebufferTexture2D) glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)LoadExt("glFramebufferTexture2D", "glFramebufferTexture2DEXT");
    if (!glCheckFramebufferStatus) glCheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)LoadExt("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");
    if (!glBlitFramebuffer) glBlitFramebuffer = (PFNGLBLITFRAMEBUFFERPROC)LoadExt("glBlitFramebuffer", "glBlitFramebufferEXT");
    if (!glUniformMatrix4fv) glUniformMatrix4fv = (PFNGLUNIFORMMATRIX4FVPROC)LoadExt("glUniformMatrix4fv", "glUniformMatrix4fvARB");

    if (!glGenRenderbuffers) glGenRenderbuffers = (PFNGLGENRENDERBUFFERSPROC)LoadExt("glGenRenderbuffers", "glGenRenderbuffersEXT");
    if (!glBindRenderbuffer) glBindRenderbuffer = (PFNGLBINDRENDERBUFFERPROC)LoadExt("glBindRenderbuffer", "glBindRenderbufferEXT");
    if (!glRenderbufferStorage) glRenderbufferStorage = (PFNGLRENDERBUFFERSTORAGEPROC)LoadExt("glRenderbufferStorage", "glRenderbufferStorageEXT");
    if (!glFramebufferRenderbuffer) glFramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)LoadExt("glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT");
    if (!glDeleteRenderbuffers) glDeleteRenderbuffers = (PFNGLDELETERENDERBUFFERSPROC)LoadExt("glDeleteRenderbuffers", "glDeleteRenderbuffersEXT");

    if (!glProgramEnvParameter4fvARB) glProgramEnvParameter4fvARB = (PFNGLPROGRAMENVPARAMETER4FVARBPROC)LoadExt("glProgramEnvParameter4fvARB");
    if (!glProgramLocalParameter4fvARB) glProgramLocalParameter4fvARB = (PFNGLPROGRAMLOCALPARAMETER4FVARBPROC)LoadExt("glProgramLocalParameter4fvARB");

    // Query WGL_NV_DX_interop
    if (!wglDXOpenDeviceNV) wglDXOpenDeviceNV = (PFNWGLDXOPENDEVICENVPROC)LoadExt("wglDXOpenDeviceNV");
    if (!wglDXCloseDeviceNV) wglDXCloseDeviceNV = (PFNWGLDXCLOSEDEVICENVPROC)LoadExt("wglDXCloseDeviceNV");
    if (!wglDXRegisterObjectNV) wglDXRegisterObjectNV = (PFNWGLDXREGISTEROBJECTNVPROC)LoadExt("wglDXRegisterObjectNV");
    if (!wglDXUnregisterObjectNV) wglDXUnregisterObjectNV = (PFNWGLDXUNREGISTEROBJECTNVPROC)LoadExt("wglDXUnregisterObjectNV");
    if (!wglDXObjectAccessNV) wglDXObjectAccessNV = (PFNWGLDXOBJECTACCESSNVPROC)LoadExt("wglDXObjectAccessNV");
    if (!wglDXLockObjectsNV) wglDXLockObjectsNV = (PFNWGLDXLOCKOBJECTSNVPROC)LoadExt("wglDXLockObjectsNV");
    if (!wglDXUnlockObjectsNV) wglDXUnlockObjectsNV = (PFNWGLDXUNLOCKOBJECTSNVPROC)LoadExt("wglDXUnlockObjectsNV");

    if (wglDXOpenDeviceNV && wglDXRegisterObjectNV) {
        VR_LOG_INFO("Hardware Interop: WGL_NV_DX_interop supported by active OpenGL driver!");
    } else {
        VR_LOG_INFO("Hardware Interop: WGL_NV_DX_interop not advertised; using FBO blit pipeline.");
    }
}

HGLRC WINAPI OpenGLProxy::Hook_wglCreateContext(HDC hdc) {
    // GH #27: run deferred hook init here (app thread, outside loader lock).
    RiddickVR_EnsureHookInitialized();
    if (!s_initialized) Initialize();
    HGLRC result = real_wglCreateContext ? real_wglCreateContext(hdc) : nullptr;
    VR_LOG_INFO("wglCreateContext called (HDC: %p -> HGLRC: %p)", hdc, result);
    return result;
}

BOOL WINAPI OpenGLProxy::Hook_wglDeleteContext(HGLRC hglrc) {
    RiddickVR_EnsureHookInitialized();
    VR_LOG_INFO("wglDeleteContext called for HGLRC: %p", hglrc);
    StereoRenderer::Instance().Shutdown();
    return real_wglDeleteContext ? real_wglDeleteContext(hglrc) : FALSE;
}

BOOL WINAPI OpenGLProxy::Hook_wglMakeCurrent(HDC hdc, HGLRC hglrc) {
    RiddickVR_EnsureHookInitialized();
    BOOL result = real_wglMakeCurrent ? real_wglMakeCurrent(hdc, hglrc) : FALSE;
    if (result && hglrc != nullptr) {
        QueryContextExtensions();
        StereoRenderer::Instance().Initialize(hdc, hglrc);
    }
    return result;
}

BOOL WINAPI OpenGLProxy::Hook_wglSwapBuffers(HDC hdc) {
    RiddickVR_EnsureHookInitialized();
    s_swapFrameCount++;
    // GH #66: end of frame - close the viewmodel window and latch the tallies.
    s_viewmodelSuppressor.NoteSwap();
    if (s_viewmodelSuppressor.Enabled() && (s_swapFrameCount % 300) == 0) {
        VR_LOG_INFO("Viewmodel suppression [#%llu]: last frame draws=%u suppressed=%u total=%llu",
            static_cast<unsigned long long>(s_swapFrameCount),
            s_viewmodelSuppressor.LastFrameDraws(), s_viewmodelSuppressor.LastFrameSuppressed(),
            s_viewmodelSuppressor.TotalSuppressed());
    }
    if (g_vrConfig.viewmodelTrace != 0 && (s_swapFrameCount % 300) == 0) {
        VR_LOG_INFO("Viewmodel pass trace [#%llu]: %s | draws=%u suppressed=%u",
            static_cast<unsigned long long>(s_swapFrameCount),
            s_viewmodelSuppressor.LastFrameTrace(),
            s_viewmodelSuppressor.LastFrameDraws(), s_viewmodelSuppressor.LastFrameSuppressed());
    }
    s_driveCorrectionCached = false;
    StereoRenderer::Instance().OnSwapBuffers(hdc);
    return real_wglSwapBuffers ? real_wglSwapBuffers(hdc) : SwapBuffers(hdc);
}

PROC WINAPI OpenGLProxy::Hook_wglGetProcAddress(LPCSTR lpszProc) {
    RiddickVR_EnsureHookInitialized();
    if (!lpszProc) return nullptr;

    PROC realProc = real_wglGetProcAddress ? real_wglGetProcAddress(lpszProc) : nullptr;

    // Cache essential extension pointers when requested by the game engine
    std::string procName(lpszProc);
    if (procName == "glBindFramebuffer" || procName == "glBindFramebufferEXT") {
        real_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)realProc;
        glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)Hook_glBindFramebuffer;
        return (PROC)Hook_glBindFramebuffer;
    } else if (procName == "glGenFramebuffers" || procName == "glGenFramebuffersEXT") {
        glGenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)realProc;
    } else if (procName == "glDeleteFramebuffers" || procName == "glDeleteFramebuffersEXT") {
        glDeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)realProc;
    } else if (procName == "glFramebufferTexture2D" || procName == "glFramebufferTexture2DEXT") {
        glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)realProc;
    } else if (procName == "glCheckFramebufferStatus" || procName == "glCheckFramebufferStatusEXT") {
        glCheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)realProc;
    } else if (procName == "glBlitFramebuffer" || procName == "glBlitFramebufferEXT") {
        glBlitFramebuffer = (PFNGLBLITFRAMEBUFFERPROC)realProc;
    } else if (procName == "glUniformMatrix4fv" || procName == "glUniformMatrix4fvARB") {
        glUniformMatrix4fv = (PFNGLUNIFORMMATRIX4FVPROC)realProc;
    }

    return realProc;
}

void WINAPI OpenGLProxy::Hook_glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    RiddickVR_EnsureHookInitialized();
    StereoRenderer::Instance().OnViewport(x, y, width, height);
    if (real_glViewport) {
        real_glViewport(x, y, width, height);
    }
}

void WINAPI OpenGLProxy::Hook_glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    RiddickVR_EnsureHookInitialized();
    if (real_glScissor) {
        real_glScissor(x, y, width, height);
    }
}

void WINAPI OpenGLProxy::Hook_glMatrixMode(GLenum mode) {
    RiddickVR_EnsureHookInitialized();
    s_currentMatrixMode = mode;
    if (real_glMatrixMode) {
        real_glMatrixMode(mode);
    }
}

void WINAPI OpenGLProxy::Hook_glLoadMatrixf(const GLfloat *m) {
    RiddickVR_EnsureHookInitialized();
    if (!m) return;

    if (s_currentMatrixMode == 0x1701 /* GL_PROJECTION */) {
        // GH #66: classify from the ORIGINAL matrix (before any aspect correction).
        // Perspective has m[15]==0; near = m[14] / (m[10] - 1).
        const bool loadIsPerspective = fabsf(m[15]) < 0.01f;
        float loadNear = 0.0f;
        if (loadIsPerspective) {
            const float denom = m[10] - 1.0f;
            if (fabsf(denom) > 1.0e-6f) {
                const float n = m[14] / denom;
                if (n == n && n > 0.0f && n < 1.0e9f) loadNear = n; // rejects NaN/inf/negative
            }
        }
        s_viewmodelSuppressor.NoteProjectionLoad(loadIsPerspective, loadNear);
        memcpy(s_lastProjection, m, sizeof(s_lastProjection));
        s_hasProjection = true;
        s_projMatrixCount++;

        bool isPerspective = (fabsf(m[15]) < 0.01f && m[0] > 0.10f && m[0] < 4.0f);
        if (isPerspective) {
            s_has3DGameplayProjection = true;
            s_last3DFrame = s_swapFrameCount;
        }

        if (isPerspective) {
            // ADR 0403: Match 3D gameplay perspective projection aspect ratio to the VR eye buffer
            // Prevents geometry distortion and horizontal squishing in VR headset lenses
            uint32_t eyeW = StereoRenderer::Instance().GetEyeWidth();
            uint32_t eyeH = StereoRenderer::Instance().GetEyeHeight();
            if (eyeW > 0 && eyeH > 0) {
                float targetAspect = static_cast<float>(eyeW) / static_cast<float>(eyeH);
                GLfloat correctedProj[16];
                memcpy(correctedProj, m, sizeof(correctedProj));
                // In Starbreeze engine, m[5] is vertical focal length, m[0] is horizontal focal length.
                // Maintain vertical FOV and scale horizontal focal length to match target aspect ratio.
                correctedProj[0] = fabsf(m[5]) / targetAspect;
                memcpy(s_lastProjection, correctedProj, sizeof(s_lastProjection));

                if (s_projMatrixCount == 1 || s_projMatrixCount % 300 == 0) {
                    VR_LOG_INFO("glLoadMatrixf [3D VR PROJECTION #%u]: InAspect=%.3f OutAspect=%.3f (m0: %.2f->%.2f, m5: %.2f)",
                        s_projMatrixCount, fabsf(m[5]) / m[0], targetAspect, m[0], correctedProj[0], m[5]);
                }

                if (real_glLoadMatrixf) {
                    real_glLoadMatrixf(correctedProj);
                }
                return;
            }
        }

        if (s_projMatrixCount == 1 || s_projMatrixCount % 300 == 0) {
            VR_LOG_INFO("glLoadMatrixf [PROJECTION #%u]: [%.2f, %.2f, %.2f, %.2f] [%.2f, %.2f, %.2f, %.2f]",
                s_projMatrixCount, m[0], m[5], m[10], m[14], m[1], m[2], m[3], m[15]);
        }
    } else if (s_currentMatrixMode == 0x1700 /* GL_MODELVIEW */) {
        // GH #67: inside the viewmodel window, move the engine's arms to the controller.
        if (ViewmodelDriveActive() && s_viewmodelSuppressor.WindowOpen()) {
            if (!s_driveCorrectionCached) {
                s_driveCorrectionValid = ComputeDriveCorrectionForWindow();
                s_driveCorrectionCached = true;
            }
            if (s_driveCorrectionValid && real_glLoadMatrixf) {
                GLfloat driven[16];
                MulColMajor4(s_driveCorrection, m, driven);
                real_glLoadMatrixf(driven);
                return;
            }
        } else {
            s_driveCorrectionCached = false;
        }
        memcpy(s_lastView, m, sizeof(s_lastView));
        s_modelviewMatrixCount++;
        if (s_modelviewMatrixCount == 1 || s_modelviewMatrixCount % 300 == 0) {
            VR_LOG_INFO("glLoadMatrixf [MODELVIEW #%u]: Pos(%.2f, %.2f, %.2f)",
                s_modelviewMatrixCount, m[12], m[13], m[14]);
        }
    }

    if (real_glLoadMatrixf) {
        real_glLoadMatrixf(m);
    }
}

void WINAPI OpenGLProxy::Hook_glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar) {
    RiddickVR_EnsureHookInitialized();
    VR_LOG_INFO("glFrustum called: L=%.2f R=%.2f B=%.2f T=%.2f Near=%.2f Far=%.2f", left, right, bottom, top, zNear, zFar);
    if (s_currentMatrixMode == 0x1701 /* GL_PROJECTION */) {
        s_viewmodelSuppressor.NoteProjectionLoad(true, static_cast<float>(zNear));
        memset(s_lastProjection, 0, sizeof(s_lastProjection));
        s_lastProjection[0] = static_cast<float>((2.0 * zNear) / (right - left));
        s_lastProjection[5] = static_cast<float>((2.0 * zNear) / (top - bottom));
        s_lastProjection[8] = static_cast<float>((right + left) / (right - left));
        s_lastProjection[9] = static_cast<float>((top + bottom) / (top - bottom));
        s_lastProjection[10] = static_cast<float>(-(zFar + zNear) / (zFar - zNear));
        s_lastProjection[11] = -1.0f;
        s_lastProjection[14] = static_cast<float>(-(2.0 * zFar * zNear) / (zFar - zNear));
        s_lastProjection[15] = 0.0f;
        s_hasProjection = true;
        if (s_lastProjection[0] > 0.10f && s_lastProjection[0] < 4.0f) {
            s_has3DGameplayProjection = true;
            s_last3DFrame = s_swapFrameCount;
        }
    }
    if (real_glFrustum) {
        real_glFrustum(left, right, bottom, top, zNear, zFar);
    }
}

void WINAPI OpenGLProxy::Hook_glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar) {
    RiddickVR_EnsureHookInitialized();
    if (s_currentMatrixMode == 0x1701 /* GL_PROJECTION */) {
        s_viewmodelSuppressor.NoteProjectionLoad(false, 0.0f); // GH #66: ortho (HUD) ends the pass
        memset(s_lastProjection, 0, sizeof(s_lastProjection));
        s_lastProjection[0] = static_cast<float>(2.0 / (right - left));
        s_lastProjection[5] = static_cast<float>(2.0 / (top - bottom));
        s_lastProjection[10] = static_cast<float>(-2.0 / (zFar - zNear));
        s_lastProjection[12] = static_cast<float>(-(right + left) / (right - left));
        s_lastProjection[13] = static_cast<float>(-(top + bottom) / (top - bottom));
        s_lastProjection[14] = static_cast<float>(-(zFar + zNear) / (zFar - zNear));
        s_lastProjection[15] = 1.0f;
        s_hasProjection = true;
    }
    if (real_glOrtho) {
        real_glOrtho(left, right, bottom, top, zNear, zFar);
    }
}

void WINAPI OpenGLProxy::Hook_glClear(GLbitfield mask) {
    RiddickVR_EnsureHookInitialized();
    // GH #66: the frame boundary (full colour clear) re-bases the viewmodel
    // suppression window; a mid-frame depth-only clear opens it (see
    // viewmodel_suppressor.h). Configured here so an INI edit takes effect live.
    s_viewmodelSuppressor.Configure(BuildViewmodelSuppressionConfig());
    if (mask & 0x00004000u /* GL_COLOR_BUFFER_BIT */) {
        s_viewmodelSuppressor.NoteColorClear();
    } else if (mask & 0x00000100u /* GL_DEPTH_BUFFER_BIT */) {
        s_viewmodelSuppressor.NoteDepthOnlyClear();
    }
    if (real_glClear) {
        real_glClear(mask);
    }
}

void WINAPI OpenGLProxy::Hook_glBindFramebuffer(GLenum target, GLuint framebuffer) {
    RiddickVR_EnsureHookInitialized();
    if (real_glBindFramebuffer) {
        real_glBindFramebuffer(target, framebuffer);
    }
}

GLenum OpenGLProxy::GetCurrentMatrixMode() {
    return s_currentMatrixMode;
}

bool OpenGLProxy::HasInterceptedProjection() {
    return s_hasProjection;
}

bool OpenGLProxy::Has3DGameplayProjection() {
    return s_has3DGameplayProjection;
}

bool OpenGLProxy::Has3DThisFrame() {
    // Allow a 2-frame grace so a single ortho HUD pass inside gameplay
    // (crosshair, damage flash) does not flicker into cinema mode.
    return s_has3DGameplayProjection && (s_swapFrameCount - s_last3DFrame <= 2);
}

const float* OpenGLProxy::GetLastProjectionMatrix() {
    return s_lastProjection;
}

const float* OpenGLProxy::GetLastViewMatrix() {
    return s_lastView;
}

bool OpenGLProxy::ShouldSuppressDrawCall() {
    return s_viewmodelSuppressor.ShouldSuppressDraw();
}

} // namespace RiddickVR
