#include <windows.h>
#include "logging.h"
#include "opengl_proxy.h"
#include "camera_hook.h"
#include "stereo_renderer.h"
#include "iat_hook.h"
#include "sim_input_bridge_win32.h"

// WGL exports hooked directly
extern "C" {

HGLRC WINAPI wglCreateContext(HDC hdc) {
    return RiddickVR::OpenGLProxy::Hook_wglCreateContext(hdc);
}

BOOL WINAPI wglDeleteContext(HGLRC hglrc) {
    return RiddickVR::OpenGLProxy::Hook_wglDeleteContext(hglrc);
}

BOOL WINAPI wglMakeCurrent(HDC hdc, HGLRC hglrc) {
    return RiddickVR::OpenGLProxy::Hook_wglMakeCurrent(hdc, hglrc);
}

BOOL WINAPI wglSwapBuffers(HDC hdc) {
    return RiddickVR::OpenGLProxy::Hook_wglSwapBuffers(hdc);
}

PROC WINAPI wglGetProcAddress(LPCSTR lpszProc) {
    return RiddickVR::OpenGLProxy::Hook_wglGetProcAddress(lpszProc);
}

void WINAPI glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    RiddickVR::OpenGLProxy::Hook_glViewport(x, y, width, height);
}

void WINAPI glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    RiddickVR::OpenGLProxy::Hook_glScissor(x, y, width, height);
}

void WINAPI glMatrixMode(GLenum mode) {
    RiddickVR::OpenGLProxy::Hook_glMatrixMode(mode);
}

void WINAPI glLoadMatrixf(const GLfloat* m) {
    RiddickVR::OpenGLProxy::Hook_glLoadMatrixf(m);
}

void WINAPI glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar) {
    RiddickVR::OpenGLProxy::Hook_glFrustum(left, right, bottom, top, zNear, zFar);
}

void WINAPI glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar) {
    RiddickVR::OpenGLProxy::Hook_glOrtho(left, right, bottom, top, zNear, zFar);
}

void WINAPI glClear(GLbitfield mask) {
    RiddickVR::OpenGLProxy::Hook_glClear(mask);
}

} // extern "C"

typedef BOOL (WINAPI *PFN_GdiSwapBuffers)(HDC);
static PFN_GdiSwapBuffers real_GdiSwapBuffers = nullptr;

static BOOL WINAPI Hook_GdiSwapBuffers(HDC hdc) {
    RiddickVR::StereoRenderer::Instance().OnSwapBuffers(hdc);
    return real_GdiSwapBuffers ? real_GdiSwapBuffers(hdc) : SwapBuffers(hdc);
}

// GH #63/#66 (approach 2): per-draw suppression of the engine's own first-person
// viewmodel. RndrGL.dll is the only shipped module that imports OpenGL and its
// geometry path is `glDrawElements` (vertex arrays) and `glCallList` (display
// lists) -- see docs/rea/reference/RndrGL_GL_Path.txt. We hook those two IAT
// entries and skip the call when the suppressor's window is open. These run only
// after InstallRndrGLHooks (below), which runs inside the deferred init, so no
// re-entrant EnsureHookInitialized is needed on this hot path. The saved
// original is the proxy's own forwarder, so forwarding is unchanged when the
// window is closed and the feature is off (the default).
typedef void (WINAPI *PFN_glDrawElements)(GLenum mode, GLsizei count, GLenum type, const void* indices);
typedef void (WINAPI *PFN_glCallList)(GLuint list);
// GH #67 step 1: RndrGL composes object transforms from glLoadMatrixf +
// glPushMatrix/glTranslatef/glPopMatrix (it does not import glMultMatrixf /
// glRotatef -- docs/rea/reference/RndrGL_GL_Path.txt section 3). Intercept those
// fixed-function calls so OpenGLProxy can mirror the effective GL_MODELVIEW for
// the draw-time capture (the GH #67 step-2 probe / GetLastViewMatrix).
typedef void (WINAPI *PFN_glLoadIdentity)(void);
typedef void (WINAPI *PFN_glPushMatrix)(void);
typedef void (WINAPI *PFN_glPopMatrix)(void);
typedef void (WINAPI *PFN_glTranslatef)(GLfloat, GLfloat, GLfloat);

static PFN_glDrawElements real_glDrawElements = nullptr;
static PFN_glCallList real_glCallList = nullptr;
static PFN_glLoadIdentity real_glLoadIdentity = nullptr;
static PFN_glPushMatrix real_glPushMatrix = nullptr;
static PFN_glPopMatrix real_glPopMatrix = nullptr;
static PFN_glTranslatef real_glTranslatef = nullptr;

static void WINAPI Hook_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (RiddickVR::OpenGLProxy::ShouldSuppressDrawCall()) return;
    if (real_glDrawElements) real_glDrawElements(mode, count, type, indices);
}

static void WINAPI Hook_glCallList(GLuint list) {
    if (RiddickVR::OpenGLProxy::ShouldSuppressDrawCall()) return;
    if (real_glCallList) real_glCallList(list);
}

// GH #67 step 1: maintain the software GL_MODELVIEW stack, then forward verbatim.
// Off-modelview calls are ignored by the proxy (the projection stack is not
// tracked). These never suppress a draw.
static void WINAPI Hook_glLoadIdentity(void) {
    RiddickVR::OpenGLProxy::NoteModelviewLoadIdentity();
    if (real_glLoadIdentity) real_glLoadIdentity();
}

static void WINAPI Hook_glPushMatrix(void) {
    RiddickVR::OpenGLProxy::NoteModelviewPush();
    if (real_glPushMatrix) real_glPushMatrix();
}

static void WINAPI Hook_glPopMatrix(void) {
    RiddickVR::OpenGLProxy::NoteModelviewPop();
    if (real_glPopMatrix) real_glPopMatrix();
}

static void WINAPI Hook_glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    RiddickVR::OpenGLProxy::NoteModelviewTranslatef(x, y, z);
    if (real_glTranslatef) real_glTranslatef(x, y, z);
}

static void InstallRndrGLHooks() {
    HMODULE hRndrGL = GetModuleHandleA("RndrGL.dll");
    if (!hRndrGL) return;
    RiddickVR::IATHook::Hook(
        hRndrGL,
        "GDI32.dll",
        "SwapBuffers",
        reinterpret_cast<void*>(Hook_GdiSwapBuffers),
        reinterpret_cast<void**>(&real_GdiSwapBuffers)
    );
    // GH #66: intercept the engine's geometry draw entry points (forwarded
    // verbatim when suppression is off).
    RiddickVR::IATHook::Hook(
        hRndrGL,
        "OPENGL32.dll",
        "glDrawElements",
        reinterpret_cast<void*>(Hook_glDrawElements),
        reinterpret_cast<void**>(&real_glDrawElements)
    );
    RiddickVR::IATHook::Hook(
        hRndrGL,
        "OPENGL32.dll",
        "glCallList",
        reinterpret_cast<void*>(Hook_glCallList),
        reinterpret_cast<void**>(&real_glCallList)
    );
    // GH #67 step 1: intercept the fixed-function matrix composition the engine
    // uses for object/viewmodel transforms, so the effective GL_MODELVIEW is
    // observable (the step-2 probe and any controller-driven re-expression need
    // it). Forwarded verbatim; never suppresses.
    RiddickVR::IATHook::Hook(
        hRndrGL,
        "OPENGL32.dll",
        "glLoadIdentity",
        reinterpret_cast<void*>(Hook_glLoadIdentity),
        reinterpret_cast<void**>(&real_glLoadIdentity)
    );
    RiddickVR::IATHook::Hook(
        hRndrGL,
        "OPENGL32.dll",
        "glPushMatrix",
        reinterpret_cast<void*>(Hook_glPushMatrix),
        reinterpret_cast<void**>(&real_glPushMatrix)
    );
    RiddickVR::IATHook::Hook(
        hRndrGL,
        "OPENGL32.dll",
        "glPopMatrix",
        reinterpret_cast<void*>(Hook_glPopMatrix),
        reinterpret_cast<void**>(&real_glPopMatrix)
    );
    RiddickVR::IATHook::Hook(
        hRndrGL,
        "OPENGL32.dll",
        "glTranslatef",
        reinterpret_cast<void*>(Hook_glTranslatef),
        reinterpret_cast<void**>(&real_glTranslatef)
    );
}

static HMODULE g_hHookModule = nullptr;
static LONG g_hookInitState = 0; // 0=uninitialized, 1=initializing, 2=ready/failed

static void HookDebugLogPath(char* outPath, DWORD outSize) {
    // GH #26: never write to a hardcoded developer path; %TEMP% first,
    // hook DLL directory as fallback.
    DWORD len = GetTempPathA(outSize, outPath);
    if (len > 0 && len < outSize) {
        strcat_s(outPath, outSize, "riddick_vr_hook_debug.txt");
        return;
    }
    if (g_hHookModule && GetModuleFileNameA(g_hHookModule, outPath, outSize) > 0) {
        char* lastSlash = strrchr(outPath, '\\');
        if (lastSlash) {
            *(lastSlash + 1) = '\0';
            strcat_s(outPath, outSize, "hook_debug.txt");
            return;
        }
    }
    strcpy_s(outPath, outSize, "hook_debug.txt");
}

static void RawDebugLog(const char* msg) {
    char path[MAX_PATH];
    HookDebugLogPath(path, MAX_PATH);
    HANDLE hFile = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, msg, (DWORD)strlen(msg), &written, NULL);
        WriteFile(hFile, "\r\n", 2, &written, NULL);
        CloseHandle(hFile);
    }
}

// GH #27: DllMain must not do LoadLibrary / IO / subsystem init under the
// loader lock. It only stashes the module handle; real init is deferred to
// the first wglCreateContext (worker/app thread) via EnsureHookInitialized.
void RiddickVR_EnsureHookInitialized();

static void EnsureHookInitializedImpl() {
    RawDebugLog("DeferredInit: entered");

    // Enable Per-Monitor V2 DPI awareness so Windows never downscales the game viewport
    SetProcessDPIAware();
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
        auto setDpiContext = (PFN_SetProcessDpiAwarenessContext)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (setDpiContext) {
            setDpiContext((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
        }
    }

    RawDebugLog("DeferredInit: Initializing Logger");
    char logPath[MAX_PATH];
    if (g_hHookModule) {
        GetModuleFileNameA(g_hHookModule, logPath, MAX_PATH);
    } else {
        logPath[0] = '\0';
    }
    char* lastSlash = strrchr(logPath, '\\');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
        strcat_s(logPath, "riddick_vr.log");
    } else {
        strcpy_s(logPath, "riddick_vr.log");
    }
    RiddickVR::Logger::Instance().Initialize(logPath);
    VR_LOG_INFO("Riddick VR Hook DLL Attached (PID: %lu)", GetCurrentProcessId());

    // GH #80: report the sim input bridge's attach-time install outcome (it is
    // silent unless RIDDICKVR_SIM + RIDDICKVR_SIM_CONFIRM are both set).
    RiddickVR::SimInputBridgeLogStartupReport();

    RawDebugLog("DeferredInit: Initializing OpenGLProxy");
    if (!RiddickVR::OpenGLProxy::Initialize()) {
        RawDebugLog("DeferredInit: OpenGLProxy::Initialize failed!");
        VR_LOG_ERROR("Failed to initialize OpenGL Proxy! Aborting hook setup.");
        return;
    }
    RawDebugLog("DeferredInit: OpenGLProxy::Initialize succeeded");

    RawDebugLog("DeferredInit: Installing RndrGL Hooks");
    InstallRndrGLHooks();
    RawDebugLog("DeferredInit: Initializing CameraHook");
    RiddickVR::CameraHook::Instance().Initialize();
    RawDebugLog("DeferredInit: Done");
}

void RiddickVR_EnsureHookInitialized() {
    // One-time, thread-safe-ish init without blocking the loader: first
    // caller wins via interlocked transition 0->1; losers spin briefly for 2.
    if (g_hookInitState == 2) return;
    if (InterlockedCompareExchange(&g_hookInitState, 1, 0) == 0) {
        EnsureHookInitializedImpl();
        InterlockedExchange(&g_hookInitState, 2);
    } else {
        for (int i = 0; i < 200 && g_hookInitState != 2; ++i) {
            Sleep(1);
        }
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH: {
            g_hHookModule = hModule;
            DisableThreadLibraryCalls(hModule);
            // GH #80 (ADR-0467/0468): the simulator-only input bridge must be
            // installed *before* CInputContext_DInput's constructor resolves
            // XInputGetState/DirectInputCreateA, which the deferred init (first
            // wglCreateContext) is not guaranteed to precede. The install is a
            // single loader-lock-safe IAT pointer swap and is inert unless both
            // RIDDICKVR_SIM and RIDDICKVR_SIM_CONFIRM are set.
            RiddickVR::SimInputBridgeInstallOnAttach();
            // Loader-lock-safe attach breadcrumb (OutputDebugString never touches
            // the loader lock; visible in DebugView/dbgview without file IO).
            // Answers "did the proxy DLL load?" when the game stalls pre-GL.
            OutputDebugStringA("RiddickVR: opengl32 proxy attached (init deferred to first wglCreateContext)\r\n");
            break;
        }
        case DLL_PROCESS_DETACH: {
            VR_LOG_INFO("Riddick VR Hook DLL Detaching...");
            RiddickVR::CameraHook::Instance().Shutdown();
            RiddickVR::StereoRenderer::Instance().Shutdown();
            RiddickVR::OpenGLProxy::Shutdown();
            break;
        }
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            break;
    }
    return TRUE;
}
