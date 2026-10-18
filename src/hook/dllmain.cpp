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
// GH #67 step 1 (second correction, ADR-0482): the client vertex array, so the
// submitted geometry the engine transforms on the CPU can be sampled at draw time
// (RndrGL imports glVertexPointer/glNormalPointer/glColorPointer; it never uses
// glDrawArrays or a buffer object, so the pointer is always a client pointer).
typedef void (WINAPI *PFN_glVertexPointer)(GLint size, GLenum type, GLsizei stride, const void* pointer);
// GH #67 step 1 (ninth correction, ADR-0491): the immediate-mode path, which the
// submitted-vertex observer never counted. The shipped 2009 RndrGL.dll imports
// glBegin/glEnd/glVertex3f/glVertex3fv (and the 2-D glVertex2f/glVertex2i) -- see
// docs/rea/reference/RndrGL_GL_Path.txt section 2 and
// `python tools/gl_vocabulary_scan.py --imports` -- so a mesh it submits with
// glBegin(..) { glVertex3f(..) } glEnd() produces no glDrawElements call and no
// sample at all. Intercepted for observation only (never suppressed).
typedef void (WINAPI *PFN_glBegin)(GLenum mode);
typedef void (WINAPI *PFN_glEnd)(void);
typedef void (WINAPI *PFN_glVertex3f)(GLfloat x, GLfloat y, GLfloat z);
typedef void (WINAPI *PFN_glVertex3fv)(const GLfloat* v);
typedef void (WINAPI *PFN_glVertex2f)(GLfloat x, GLfloat y);
typedef void (WINAPI *PFN_glVertex2i)(GLint x, GLint y);
// GH #67 step 1: RndrGL composes object transforms from glLoadMatrixf +
// glPushMatrix/glTranslatef/glPopMatrix (it does not import glMultMatrixf /
// glRotatef -- docs/rea/reference/RndrGL_GL_Path.txt section 3). Intercept those
// fixed-function calls so OpenGLProxy can mirror the effective GL_MODELVIEW for
// the draw-time capture (the GH #67 step-2 probe / GetLastViewMatrix).
typedef void (WINAPI *PFN_glLoadIdentity)(void);
typedef void (WINAPI *PFN_glPushMatrix)(void);
typedef void (WINAPI *PFN_glPopMatrix)(void);
typedef void (WINAPI *PFN_glTranslatef)(GLfloat, GLfloat, GLfloat);
// GH #76/#90 (ADR-0507): the glInterleavedArrays path -- GL 1.1, but absent from
// every shipped module's import table (ADR-0482), so this IAT hook is expected to
// fail to install; it is added so a run *states* the path is never used.
typedef void (WINAPI *PFN_glInterleavedArrays)(GLenum format, GLsizei stride, const void* pointer);
// GH #76/#90 (ADR-0510): the colour-write mask. ADR-0509 read it once per span
// (measured `cmask=0x0` at the span's first eligible draw) but the per-eye pass must
// decide *at each draw* whether the draw can colour an eye frame, so RndrGL's
// `glColorMask` import -- present in the live 2009 `RndrGL.dll`'s static import
// table -- is IAT-hooked (observe + forward verbatim; never changes a draw).
typedef void (WINAPI *PFN_glColorMask)(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);

static PFN_glDrawElements real_glDrawElements = nullptr;
static PFN_glCallList real_glCallList = nullptr;
static PFN_glVertexPointer real_glVertexPointer = nullptr;
static PFN_glLoadIdentity real_glLoadIdentity = nullptr;
static PFN_glPushMatrix real_glPushMatrix = nullptr;
static PFN_glPopMatrix real_glPopMatrix = nullptr;
static PFN_glTranslatef real_glTranslatef = nullptr;
static PFN_glBegin real_glBegin = nullptr;
static PFN_glEnd real_glEnd = nullptr;
static PFN_glVertex3f real_glVertex3f = nullptr;
static PFN_glVertex3fv real_glVertex3fv = nullptr;
static PFN_glVertex2f real_glVertex2f = nullptr;
static PFN_glVertex2i real_glVertex2i = nullptr;
static PFN_glInterleavedArrays real_glInterleavedArrays = nullptr;
static PFN_glColorMask real_glColorMask = nullptr;

static void WINAPI Hook_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (RiddickVR::OpenGLProxy::ShouldSuppressDrawCall()) return;
    // GH #67 step 1 (second correction, ADR-0482): sample the submitted vertices
    // (inert unless ViewmodelObserve=1). Must run after ShouldSuppressDrawCall so
    // the window state used to classify the draw is current.
    RiddickVR::OpenGLProxy::NoteDrawElementsSubmission(mode, count, type, indices);
    // GH #76/#90 (ADR-0505): native per-eye render. The same call is re-issued once
    // per eye with the per-eye eye-space translation *before* the engine's own draw
    // runs, while the client arrays and the fixed-function state are exactly as the
    // engine set them. A no-op unless StereoDualRender=1.
    RiddickVR::StereoRenderer::Instance().OnSceneDraw(mode, count, type, indices);
    if (real_glDrawElements) real_glDrawElements(mode, count, type, indices);
}

static void WINAPI Hook_glCallList(GLuint list) {
    if (RiddickVR::OpenGLProxy::ShouldSuppressDrawCall()) return;
    // GH #67 step 1: count display-list draws by window side -- a display list's
    // vertices replay inside the driver and are not observable here.
    RiddickVR::OpenGLProxy::NoteDrawListSubmission();
    if (real_glCallList) real_glCallList(list);
}

// GH #67 step 1 (second correction, ADR-0482): remember the client vertex array
// so the draw hook can sample the geometry the engine transformed. Forwarded
// verbatim; never suppresses.
static void WINAPI Hook_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void* pointer) {
    RiddickVR::OpenGLProxy::NoteVertexPointer(size, type, stride, pointer);
    if (real_glVertexPointer) real_glVertexPointer(size, type, stride, pointer);
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

// GH #67 step 1 (ninth correction, ADR-0491): the immediate-mode census. These
// mirror the glDrawElements/glCallList hooks' contract -- observe, then forward
// verbatim, never suppress -- so enabling observation cannot change a frame. The
// counter is inert unless ViewmodelObserve=1, and the per-vertex hooks are a
// single observe-check + a few compares once a block is open.
static void WINAPI Hook_glBegin(GLenum mode) {
    RiddickVR::OpenGLProxy::NoteImmediateBegin();
    if (real_glBegin) real_glBegin(mode);
}

static void WINAPI Hook_glEnd(void) {
    RiddickVR::OpenGLProxy::NoteImmediateEnd();
    if (real_glEnd) real_glEnd();
}

static void WINAPI Hook_glVertex3f(GLfloat x, GLfloat y, GLfloat z) {
    RiddickVR::OpenGLProxy::NoteImmediateVertex3(x, y, z);
    if (real_glVertex3f) real_glVertex3f(x, y, z);
}

static void WINAPI Hook_glVertex3fv(const GLfloat* v) {
    if (v) RiddickVR::OpenGLProxy::NoteImmediateVertex3(v[0], v[1], v[2]);
    if (real_glVertex3fv) real_glVertex3fv(v);
}

static void WINAPI Hook_glVertex2f(GLfloat x, GLfloat y) {
    RiddickVR::OpenGLProxy::NoteImmediateVertex2(x, y);
    if (real_glVertex2f) real_glVertex2f(x, y);
}

static void WINAPI Hook_glVertex2i(GLint x, GLint y) {
    RiddickVR::OpenGLProxy::NoteImmediateVertex2(static_cast<float>(x), static_cast<float>(y));
    if (real_glVertex2i) real_glVertex2i(x, y);
}

// GH #76/#90 (ADR-0507): observe `glInterleavedArrays` (forward verbatim). It both
// sets the vertex array *and* rewrites the enable state, so a live call would
// explain an unset `glVertexPointer`; the census counts it either way.
static void WINAPI Hook_glInterleavedArrays(GLenum format, GLsizei stride, const void* pointer) {
    RiddickVR::OpenGLProxy::NoteInterleavedArrays();
    if (real_glInterleavedArrays) real_glInterleavedArrays(format, stride, pointer);
}

// GH #76/#90 (ADR-0510): record the engine's colour-write mask at each draw so the
// per-eye pass re-issues only draws that can colour an eye frame. Observe then
// forward verbatim -- the mask the engine set is never changed by the hook.
static void WINAPI Hook_glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
    RiddickVR::OpenGLProxy::NoteColourMask(red, green, blue, alpha);
    if (real_glColorMask) real_glColorMask(red, green, blue, alpha);
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
    // GH #67 step 1 (second correction, ADR-0482): the client vertex array is what
    // the submitted-geometry sample reads (the transform is engine-side on the
    // vertices, not in any GL matrix the hook can see).
    RiddickVR::IATHook::Hook(
        hRndrGL,
        "OPENGL32.dll",
        "glVertexPointer",
        reinterpret_cast<void*>(Hook_glVertexPointer),
        reinterpret_cast<void**>(&real_glVertexPointer)
    );
    // GH #67 step 1: intercept the fixed-function matrix composition RndrGL
    // imports, so the effective GL_MODELVIEW is observable (the step-2 probe and
    // any controller-driven re-expression need it). MEASURED (ADR-0481): the live
    // title never composes it -- one identity load per run -- so this is a
    // complete observation of a path the engine does not use, not the transform
    // source. Forwarded verbatim; never suppresses.
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
    // GH #67 step 1 (ninth correction, ADR-0491): the immediate-mode path. RndrGL
    // binds these (measured: `python tools/gl_vocabulary_scan.py --imports` lists
    // glBegin/glEnd/glVertex3f/glVertex3fv/glVertex2f/glVertex2i among its 96 static
    // OPENGL32 imports) and the probe counted none of them, so a mesh submitted as
    // glBegin..glEnd was invisible to every previous sample. Observation only: each
    // hook forwards to the saved original (the proxy's own forwarder) unconditionally.
    RiddickVR::IATHook::Hook(
        hRndrGL, "OPENGL32.dll", "glBegin",
        reinterpret_cast<void*>(Hook_glBegin), reinterpret_cast<void**>(&real_glBegin)
    );
    RiddickVR::IATHook::Hook(
        hRndrGL, "OPENGL32.dll", "glEnd",
        reinterpret_cast<void*>(Hook_glEnd), reinterpret_cast<void**>(&real_glEnd)
    );
    RiddickVR::IATHook::Hook(
        hRndrGL, "OPENGL32.dll", "glVertex3f",
        reinterpret_cast<void*>(Hook_glVertex3f), reinterpret_cast<void**>(&real_glVertex3f)
    );
    RiddickVR::IATHook::Hook(
        hRndrGL, "OPENGL32.dll", "glVertex3fv",
        reinterpret_cast<void*>(Hook_glVertex3fv), reinterpret_cast<void**>(&real_glVertex3fv)
    );
    RiddickVR::IATHook::Hook(
        hRndrGL, "OPENGL32.dll", "glVertex2f",
        reinterpret_cast<void*>(Hook_glVertex2f), reinterpret_cast<void**>(&real_glVertex2f)
    );
    RiddickVR::IATHook::Hook(
        hRndrGL, "OPENGL32.dll", "glVertex2i",
        reinterpret_cast<void*>(Hook_glVertex2i), reinterpret_cast<void**>(&real_glVertex2i)
    );
    // GH #76/#90 (ADR-0507): the glInterleavedArrays path. RndrGL does NOT import it
    // (ADR-0482: absent from every shipped module's import table), so this hook is
    // expected not to install; it is added so the census *states* the path is never
    // used rather than assuming it. A failed IATHook::Hook is a no-op.
    RiddickVR::IATHook::Hook(
        hRndrGL, "OPENGL32.dll", "glInterleavedArrays",
        reinterpret_cast<void*>(Hook_glInterleavedArrays), reinterpret_cast<void**>(&real_glInterleavedArrays)
    );
    // GH #76/#90 (ADR-0510): the colour-write mask, so the per-eye pass can gate a
    // re-issue on "this draw can colour the eye frame". RndrGL DOES import it
    // (`gl_vocabulary_scan.py --imports` on the 2009 target lists `glColorMask`), so
    // the hook installs; a failed Hook is a no-op.
    RiddickVR::IATHook::Hook(
        hRndrGL, "OPENGL32.dll", "glColorMask",
        reinterpret_cast<void*>(Hook_glColorMask), reinterpret_cast<void**>(&real_glColorMask)
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
            // GH #71 req 1: a graceful exit gets a final bridge line. A harness
            // run is killed (TerminateProcess), so the periodic lines emitted
            // from the wrappers are the run's real evidence.
            RiddickVR::SimInputBridgeLogSummary();
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
