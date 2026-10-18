#include "stereo_renderer.h"
#include "camera_hook.h"
#include "vr_hand_renderer.h"
#include "vr_config.h" // GH #9: ComfortEnabled / ComfortMaxDim gate
#include "display_branch.h" // GH #72 req 1: cold-start full-bleed latch
#include "depth_parallax.h" // GH #76 interim: depth-based per-eye reprojection
#include "logging.h"
#include <tlhelp32.h>
#include <algorithm>
#include <string>
#include <vector>

namespace RiddickVR {

static bool IsHostRunning() {
    bool running = false;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe{};
        pe.dwSize = sizeof(pe);
        if (Process32First(snapshot, &pe)) {
            do {
                if (_stricmp(pe.szExeFile, "riddickvr-host.exe") == 0) {
                    running = true;
                    break;
                }
            } while (Process32Next(snapshot, &pe));
        }
        CloseHandle(snapshot);
    }
    return running;
}

static bool SpawnHostProcess() {
    static ULONGLONG s_lastSpawnAttempt = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastSpawnAttempt < 5000) {
        return false;
    }
    s_lastSpawnAttempt = now;

    if (IsHostRunning()) {
        VR_LOG_INFO("riddickvr-host.exe is already running in background.");
        return true;
    }

    // GH #26: never hardcode a developer machine path. Resolution order:
    // 1. RIDDICKVR_HOST env override, 2. hook DLL directory, 3. game EXE dir.
    std::string chosenHost = "";
    std::string hookDir = "";
    std::string gameDir = "";
    char envHost[MAX_PATH] = { 0 };
    if (GetEnvironmentVariableA("RIDDICKVR_HOST", envHost, MAX_PATH) > 0 &&
        GetFileAttributesA(envHost) != INVALID_FILE_ATTRIBUTES) {
        chosenHost = envHost;
    }

    auto DirOf = [](const char* path) {
        std::string dir = path;
        size_t lastSlash = dir.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            dir = dir.substr(0, lastSlash);
        }
        return dir;
    };

    // Resolve the hook directory even with an RIDDICKVR_HOST override so the
    // host receives the same INI that LoadVRConfig uses in this DLL.
    HMODULE hSelf = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&SpawnHostProcess), &hSelf) && hSelf) {
        char dllPath[MAX_PATH] = { 0 };
        if (GetModuleFileNameA(hSelf, dllPath, MAX_PATH) > 0) {
            hookDir = DirOf(dllPath);
            if (chosenHost.empty()) {
                std::string candidate = hookDir + "\\riddickvr-host.exe";
                if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
                    chosenHost = candidate;
            }
        }
    }

    char modulePath[MAX_PATH] = { 0 };
    if (GetModuleFileNameA(NULL, modulePath, MAX_PATH) > 0)
        gameDir = DirOf(modulePath);
    if (chosenHost.empty()) {
        if (!gameDir.empty()) {
            std::string candidate = gameDir + "\\riddickvr-host.exe";
            if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                chosenHost = candidate;
            }
        }
    }

    if (chosenHost.empty()) {
        VR_LOG_WARN("Could not locate riddickvr-host.exe next to the hook DLL or game EXE (RIDDICKVR_HOST override also unset).");
        return false;
    }

    VR_LOG_INFO("Auto-launching OpenXR Host Companion: %s", chosenHost.c_str());

    std::string hostWorkingDir = chosenHost.substr(0, chosenHost.find_last_of("\\/"));
    std::string configPath;
    const std::string configDirs[] = { hookDir, gameDir };
    for (const std::string& dir : configDirs) {
        if (dir.empty()) continue;
        std::string candidate = dir + "\\riddick_vr.ini";
        if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            configPath = candidate;
            break;
        }
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::string cmdLine = "\"" + chosenHost + "\" --auto-exit-with-game";
    if (!configPath.empty())
        cmdLine += " --config \"" + configPath + "\"";
    std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back('\0');

    // GH #26: CREATE_NO_WINDOW — never pop a console over the game.
    BOOL success = CreateProcessA(
        nullptr,
        cmdLineBuf.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        hostWorkingDir.c_str(),
        &si,
        &pi
    );

    if (success) {
        VR_LOG_INFO("Successfully launched riddickvr-host.exe (PID: %u)", pi.dwProcessId);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    } else {
        VR_LOG_WARN("Failed to launch riddickvr-host.exe (Error: %u)", GetLastError());
        return false;
    }
}

StereoRenderer& StereoRenderer::Instance() {
    static StereoRenderer s_instance;
    return s_instance;
}

// GH #9: true dynamic locomotion comfort vignette. Fixed-function only
// (no shaders): nested dark border rings over each eye FBO, alpha scaled by
// comfortVignette * ComfortMaxDim. File-local dispatch loaded from the real
// opengl32 (bypasses our own glViewport hook) mirrors VRHandRenderer state
// save/restore so game render state is bit-identical afterwards.
namespace {

typedef void (APIENTRY *VigPushAttrib_t)(GLbitfield);
typedef void (APIENTRY *VigPopAttrib_t)(void);
typedef void (APIENTRY *VigMatrixMode_t)(GLenum);
typedef void (APIENTRY *VigPushMatrix_t)(void);
typedef void (APIENTRY *VigPopMatrix_t)(void);
typedef void (APIENTRY *VigLoadIdentity_t)(void);
typedef void (APIENTRY *VigOrtho_t)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
typedef void (APIENTRY *VigViewport_t)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *VigEnable_t)(GLenum);
typedef void (APIENTRY *VigDisable_t)(GLenum);
typedef void (APIENTRY *VigBlendFunc_t)(GLenum, GLenum);
typedef void (APIENTRY *VigColor4f_t)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *VigBegin_t)(GLenum);
typedef void (APIENTRY *VigEnd_t)(void);
typedef void (APIENTRY *VigVertex2f_t)(GLfloat, GLfloat);
typedef void (APIENTRY *VigGetIntegerv_t)(GLenum, GLint*);
typedef void (APIENTRY *VigDepthMask_t)(GLboolean);
typedef void (APIENTRY *VigUseProgram_t)(GLuint);
typedef void (APIENTRY *VigBindBuffer_t)(GLenum, GLuint);

struct VignetteGL {
    VigPushAttrib_t vigPushAttrib = nullptr;
    VigPopAttrib_t vigPopAttrib = nullptr;
    VigMatrixMode_t vigMatrixMode = nullptr;
    VigPushMatrix_t vigPushMatrix = nullptr;
    VigPopMatrix_t vigPopMatrix = nullptr;
    VigLoadIdentity_t vigLoadIdentity = nullptr;
    VigOrtho_t vigOrtho = nullptr;
    VigViewport_t vigViewport = nullptr;
    VigEnable_t vigEnable = nullptr;
    VigDisable_t vigDisable = nullptr;
    VigBlendFunc_t vigBlendFunc = nullptr;
    VigColor4f_t vigColor4f = nullptr;
    VigBegin_t vigBegin = nullptr;
    VigEnd_t vigEnd = nullptr;
    VigVertex2f_t vigVertex2f = nullptr;
    VigGetIntegerv_t vigGetIntegerv = nullptr;
    VigDepthMask_t vigDepthMask = nullptr;
    VigUseProgram_t vigUseProgram = nullptr;
    VigBindBuffer_t vigBindBuffer = nullptr;
    bool initialized = false;

    void Init() {
        if (initialized) return;
        HMODULE hReal = OpenGLProxy::GetRealOpenGLModule();
        if (!hReal) hReal = GetModuleHandleA("opengl32.dll");
        if (!hReal) return;
        #define VIG_LOAD(member, type, glname) member = (type)GetProcAddress(hReal, #glname)
        VIG_LOAD(vigPushAttrib, VigPushAttrib_t, glPushAttrib);
        VIG_LOAD(vigPopAttrib, VigPopAttrib_t, glPopAttrib);
        VIG_LOAD(vigMatrixMode, VigMatrixMode_t, glMatrixMode);
        VIG_LOAD(vigPushMatrix, VigPushMatrix_t, glPushMatrix);
        VIG_LOAD(vigPopMatrix, VigPopMatrix_t, glPopMatrix);
        VIG_LOAD(vigLoadIdentity, VigLoadIdentity_t, glLoadIdentity);
        VIG_LOAD(vigOrtho, VigOrtho_t, glOrtho);
        VIG_LOAD(vigViewport, VigViewport_t, glViewport);
        VIG_LOAD(vigEnable, VigEnable_t, glEnable);
        VIG_LOAD(vigDisable, VigDisable_t, glDisable);
        VIG_LOAD(vigBlendFunc, VigBlendFunc_t, glBlendFunc);
        VIG_LOAD(vigColor4f, VigColor4f_t, glColor4f);
        VIG_LOAD(vigBegin, VigBegin_t, glBegin);
        VIG_LOAD(vigEnd, VigEnd_t, glEnd);
        VIG_LOAD(vigVertex2f, VigVertex2f_t, glVertex2f);
        VIG_LOAD(vigGetIntegerv, VigGetIntegerv_t, glGetIntegerv);
        VIG_LOAD(vigDepthMask, VigDepthMask_t, glDepthMask);
        #undef VIG_LOAD
        typedef PROC (WINAPI *WglGetProc_t)(LPCSTR);
        WglGetProc_t wglGetProc = (WglGetProc_t)GetProcAddress(hReal, "wglGetProcAddress");
        if (wglGetProc) {
            vigUseProgram = (VigUseProgram_t)wglGetProc("glUseProgram");
            if (!vigUseProgram) vigUseProgram = (VigUseProgram_t)wglGetProc("glUseProgramObjectARB");
            vigBindBuffer = (VigBindBuffer_t)wglGetProc("glBindBuffer");
            if (!vigBindBuffer) vigBindBuffer = (VigBindBuffer_t)wglGetProc("glBindBufferARB");
        }
        initialized = (vigPushAttrib && vigPopAttrib && vigBegin && vigVertex2f && vigColor4f &&
                       vigMatrixMode && vigOrtho && vigViewport && vigEnable && vigBlendFunc);
    }
};

static VignetteGL s_vgl;

void Quad2(float x0, float y0, float x1, float y1) {
    s_vgl.vigVertex2f(x0, y0);
    s_vgl.vigVertex2f(x1, y0);
    s_vgl.vigVertex2f(x1, y1);
    s_vgl.vigVertex2f(x0, y1);
}

// Rectangular radial-ish gradient: K nested border rings, outermost darkest.
void ApplyComfortVignetteToEye(GLuint fbo, float maxAlpha, uint32_t eyeW, uint32_t eyeH) {
    if (maxAlpha < 0.01f || !fbo || !eyeW || !eyeH) return;
    s_vgl.Init();
    if (!s_vgl.initialized) return;
    if (!OpenGLProxy::real_glBindFramebuffer) return;

    OpenGLProxy::real_glBindFramebuffer(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, fbo);

    s_vgl.vigPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT |
                        GL_VIEWPORT_BIT | GL_DEPTH_BUFFER_BIT | GL_LIGHTING_BIT |
                        GL_POLYGON_BIT | GL_TEXTURE_BIT);
    GLint prevMatrixMode = GL_MODELVIEW;
    if (s_vgl.vigGetIntegerv) s_vgl.vigGetIntegerv(0x0BA0 /* GL_MATRIX_MODE */, &prevMatrixMode);
    GLint prevProgram = 0;
    GLint prevArrayBuffer = 0;
    GLint prevElementArrayBuffer = 0;
    if (s_vgl.vigGetIntegerv) {
        s_vgl.vigGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &prevProgram);
        s_vgl.vigGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &prevArrayBuffer);
        s_vgl.vigGetIntegerv(0x8895 /* GL_ELEMENT_ARRAY_BUFFER_BINDING */, &prevElementArrayBuffer);
    }
    if (s_vgl.vigUseProgram) s_vgl.vigUseProgram(0);
    if (s_vgl.vigBindBuffer) {
        s_vgl.vigBindBuffer(0x8892 /* GL_ARRAY_BUFFER */, 0);
        s_vgl.vigBindBuffer(0x8893 /* GL_ELEMENT_ARRAY_BUFFER */, 0);
    }
    if (OpenGLProxy::glColorMask) OpenGLProxy::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    s_vgl.vigViewport(0, 0, (GLsizei)eyeW, (GLsizei)eyeH);
    s_vgl.vigMatrixMode(GL_PROJECTION);
    s_vgl.vigPushMatrix();
    s_vgl.vigLoadIdentity();
    s_vgl.vigOrtho(0.0, 1.0, 0.0, 1.0, -1.0, 1.0);
    s_vgl.vigMatrixMode(GL_MODELVIEW);
    s_vgl.vigPushMatrix();
    s_vgl.vigLoadIdentity();

    s_vgl.vigDisable(GL_DEPTH_TEST);
    if (s_vgl.vigDepthMask) s_vgl.vigDepthMask(GL_FALSE);
    s_vgl.vigDisable(GL_LIGHTING);
    s_vgl.vigDisable(GL_CULL_FACE);
    s_vgl.vigDisable(GL_TEXTURE_2D);
    s_vgl.vigEnable(GL_BLEND);
    s_vgl.vigBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    static const int kRings = 6;
    static const float kStep = 0.07f; // 6 rings cover 0.42 from each edge; center stays clear
    s_vgl.vigBegin(GL_QUADS);
    for (int i = 0; i < kRings; ++i) {
        float t = (float)(kRings - i) / (float)kRings; // 1.0 outermost -> ~0.17 innermost
        float a = maxAlpha * t * t;                    // quadratic falloff toward center
        if (a < 0.004f) continue;
        s_vgl.vigColor4f(0.0f, 0.0f, 0.0f, (a > 1.0f) ? 1.0f : a);
        float o = (float)i * kStep;
        float n = o + kStep;
        Quad2(o, 1.0f - n, 1.0f - o, 1.0f - o); // top strip
        Quad2(o, o, 1.0f - o, n);               // bottom strip
        Quad2(o, n, n, 1.0f - n);               // left strip
        Quad2(1.0f - n, n, 1.0f - o, 1.0f - n); // right strip
    }
    s_vgl.vigEnd();

    s_vgl.vigMatrixMode(GL_MODELVIEW);
    s_vgl.vigPopMatrix();
    s_vgl.vigMatrixMode(GL_PROJECTION);
    s_vgl.vigPopMatrix();
    if (s_vgl.vigUseProgram) s_vgl.vigUseProgram((GLuint)prevProgram);
    if (s_vgl.vigBindBuffer) {
        s_vgl.vigBindBuffer(0x8892, (GLuint)prevArrayBuffer);
        s_vgl.vigBindBuffer(0x8893, (GLuint)prevElementArrayBuffer);
    }
    s_vgl.vigMatrixMode((GLenum)prevMatrixMode);
    s_vgl.vigPopAttrib();
}

} // namespace (file-local)

// ---------------------------------------------------------------------------
// GH #76 (interim depth-based reprojection, ADR-0501).
//
// The renderer captures the game's backbuffer ONCE and blits it into both eye
// FBOs. The single global half-IPD shift has no depth dependence, so XRSIM
// classifies every frame `stereo_kind: shift-only` (ADR-0461). True per-eye
// rendering is architecturally blocked on this title (the engine's transform is
// applied engine-side with an identity modelview and the hook cannot ask for a
// second view); the interim the issue names is a depth-based reprojection.
//
// Here: read the engine's own depth buffer (throttled), reduce it to a coarse
// grid, and give each grid cell a horizontal source shift that depends on that
// cell's nearest depth. The image is then blitted cell-by-cell per eye with
// +/-disp/2, so disparity finally varies with depth. Purely observational:
// it never changes the game's own render, and it falls back to the legacy
// uniform shift whenever the depth signal is unusable.
namespace {

typedef void (WINAPI *DibrReadPixels_t)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);

static DibrReadPixels_t s_dibrReadPixels = nullptr;
static bool s_dibrReadTried = false;

static DibrReadPixels_t DibrReadPixels() {
    if (!s_dibrReadTried) {
        s_dibrReadTried = true;
        HMODULE hReal = OpenGLProxy::GetRealOpenGLModule();
        if (!hReal) hReal = GetModuleHandleA("opengl32.dll");
        if (hReal) s_dibrReadPixels = (DibrReadPixels_t)GetProcAddress(hReal, "glReadPixels");
    }
    return s_dibrReadPixels;
}

typedef void (WINAPI *DibrGetIntegerv_t)(GLenum, GLint*);

static DibrGetIntegerv_t DibrGetIntegerv() {
    static DibrGetIntegerv_t s_getIntegerv = nullptr;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        HMODULE hReal = OpenGLProxy::GetRealOpenGLModule();
        if (!hReal) hReal = GetModuleHandleA("opengl32.dll");
        if (hReal) s_getIntegerv = (DibrGetIntegerv_t)GetProcAddress(hReal, "glGetIntegerv");
    }
    return s_getIntegerv;
}

// Where the last usable grid's depth came from. Logged so a live run names the
// source instead of only its outcome.
enum class DibrSource { None = 0, Clear, Window, EngineFbo };

static const char* DibrSourceName(DibrSource s) {
    switch (s) {
        case DibrSource::Clear:     return "clear";
        case DibrSource::Window:    return "window";
        case DibrSource::EngineFbo: return "fbo";
        default:                    return "none";
    }
}

struct DibrGrid {
    int cols = 0;
    int rows = 0;
    std::vector<float> fullDispPx; // cols*rows full-IPD disparity, 0 = unknown
    float minZ = 0.0f, maxZ = 0.0f;
    float minDisp = 0.0f, maxDisp = 0.0f;
    int validCells = 0;
    int depthW = 0, depthH = 0;    // depth buffer the grid was built from
    GLuint fbo = 0;                // engine FBO used (0 = window backbuffer)
    DibrSource source = DibrSource::None;
    bool valid = false;            // a usable, depth-varying warp
};

static DibrGrid s_dibrGrid;
static std::vector<float> s_dibrDepth;
static int s_dibrCountdown = 0;     // swaps until the next depth readback

// GH #76 (ADR-0502): the world-pass depth snapshotted at a depth-only clear --
// the instant *before* the engine wipes the depth buffer that still holds the
// scene (the first-person-window opener, ADR-0439). At SwapBuffers the world
// depth is gone (ADR-0501 measured it uniformly far), so this is the only
// surface-proven source. Captured throttled, opt-in, and only ever *used* when it
// reduces to a depth-varying warp; a missed capture is a fallback, never a crash.
struct DibrCapture {
    std::vector<float> depth;   // raw window depth of the bound buffer's viewport
    int w = 0, h = 0;
    GLuint fbo = 0;             // buffer the clear targeted (0 = window)
    uint64_t frame = 0;         // GetSwapFrameCount() at capture time
    float rawMin = 1.0f;        // raw depth range (1.0..1.0 => already cleared)
    float rawMax = 1.0f;
};
static DibrCapture s_dibrCapture;
static bool s_dibrCaptureValid = false;
static int s_dibrCaptureCountdown = 0;      // depth clears until the next capture
static uint32_t s_dibrDepthClears = 0;      // depth-only clears seen while opt-in
static uint32_t s_dibrCaptures = 0;         // captures actually taken

// GH #76 (ADR-0503): the world-depth hunt. The engine's scene depth is not in the
// window backbuffer (ADR-0501) and not in the buffer the depth-only clear targets
// (ADR-0502, far-only), so instead of guessing a capture point again, read *every*
// framebuffer object the engine generated (ADR-0503's Hook_glGenFramebuffers
// records the ids) at the depth-only clear -- the one instant a readback is
// *proven* to return real depth (ADR-0502 measured this clear's own buffer at
// raw 0.905..1.000, so that buffer is the scan's built-in positive control) -- and
// LOG each one's raw depth. This is evidence-gathering only: it never rewrites the
// snapshot or the warp, so it cannot regress the stereo pair.
//
// `s_dibrFboFound` latches once a genuinely near buffer is seen so the scan stops
// spending readbacks; nothing consumes the id yet (that is the follow-up unit).
static bool s_dibrFboFound = false;          // a scan saw a near-world-depth buffer
static uint32_t s_dibrFboScanCountdown = 0;  // depth-only clears until the next scan
static uint32_t s_dibrFboScans = 0;          // full scans actually taken
// A full scan reads every recorded FBO, so it runs on a slow cadence (in
// depth-only clears) rather than per frame.
static const uint32_t kDibrFboScanEveryClears = 600;
static const int kDibrFboScanMax = 32;       // matches OpenGLProxy's recorded cap

// A raw window depth this close to 0 means geometry within a few metres at the
// measured world projection (near 4.0 m, ADR-0488): the visible world pass. The
// depth-only clear's own buffer measured a minimum of 0.905 (~41 m, ADR-0502), so
// anything below this clearly separates "near world" from that far-only pass.
static const float kDibrNearDepthMax = 0.50f;

// Cap on how many presented frames a captured world depth may age before it is
// treated as stale instead of being applied to a frame it no longer describes.
static const uint64_t kDibrCaptureMaxAgeSwaps = 8;

// Read the depth of the active region and reduce it to the per-cell warp. Does
// NOT throw: every failure leaves `s_dibrGrid.valid == false` (legacy fallback).
//
// Three depth sources are tried, in order:
//   0. the world-pass depth captured at the last depth-only clear (ADR-0502) --
//      the only source measured to still carry the scene;
//   1. the window backbuffer's depth;
//   2. the engine's own last-bound FBO depth (measured: the engine renders the
//      3D scene into its own FBO and presents a colour-only copy, so the window
//      depth reads uniformly far). The FBO resolves against its OWN viewport and
//      the cells are mapped to the source rect by normalised coordinates, so a
//      size difference between the FBO and the window is not a problem.
static void BuildDibrGrid(GLint ox, GLint oy, GLint srcW, GLint srcH,
                          const float* proj, float baselineM) {
    s_dibrGrid.valid = false;
    DibrReadPixels_t read = DibrReadPixels();
    if (!read || srcW < 8 || srcH < 8) return;
    const int cols = g_vrConfig.dibrCols;
    const int rows = g_vrConfig.dibrRows;
    // Bounded readback: refuse an absurd buffer rather than allocate it.
    if (static_cast<double>(srcW) * static_cast<double>(srcH) > 4.0e6) return;

    const float fPx = FocalPixelsFromProjection(proj, srcW);
    if (!(fPx > 0.0f)) return;

    // Reduce a filled depth buffer (`buf`, dw x dh, GL bottom-up, row-major) to
    // the per-cell warp over the sub-rect (rx, ry, rw, rh); returns true when the
    // depth varies enough to be a usable signal.
    auto reduce = [&](const float* buf, int dw, int dh,
                      int rx, int ry, int rw, int rh) -> bool {
        if (!buf || dw < 8 || dh < 8) return false;
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx + rw > dw) rw = dw - rx;
        if (ry + rh > dh) rh = dh - ry;
        if (rw < 8 || rh < 8) return false;
        s_dibrGrid.cols = cols;
        s_dibrGrid.rows = rows;
        s_dibrGrid.depthW = dw;
        s_dibrGrid.depthH = dh;
        s_dibrGrid.fullDispPx.assign(static_cast<size_t>(cols) * static_cast<size_t>(rows), 0.0f);
        float minZ = 0.0f, maxZ = 0.0f, minD = 0.0f, maxD = 0.0f;
        int valid = 0;
        for (int r = 0; r < rows; ++r) {
            const int y0 = ry + (r * rh) / rows;
            const int y1 = ry + ((r + 1) * rh) / rows;
            for (int c = 0; c < cols; ++c) {
                const int x0 = rx + (c * rw) / cols;
                const int x1 = rx + ((c + 1) * rw) / cols;
                const float z = CellMinViewZ(buf, dw, dh, x0, y0, x1, y1, proj);
                if (!(z > 0.0f)) continue;
                const float disp = ParallaxShiftPx(z, fPx, baselineM, g_vrConfig.dibrStrength);
                s_dibrGrid.fullDispPx[static_cast<size_t>(r) * cols + c] = disp;
                if (valid == 0) {
                    minZ = maxZ = z;
                    minD = maxD = disp;
                } else {
                    if (z < minZ) minZ = z;
                    if (z > maxZ) maxZ = z;
                    if (disp < minD) minD = disp;
                    if (disp > maxD) maxD = disp;
                }
                ++valid;
            }
        }
        s_dibrGrid.validCells = valid;
        s_dibrGrid.minZ = minZ;
        s_dibrGrid.maxZ = maxZ;
        s_dibrGrid.minDisp = minD;
        s_dibrGrid.maxDisp = maxD;
        // A usable warp needs depth that actually varies across the frame. A
        // uniform read (no depth buffer / a cleared depth) must fall back to the
        // legacy uniform shift rather than flatten the image to zero disparity.
        return (valid > 0) && ((maxD - minD) >= 0.5f);
    };

    // 0. World-pass depth captured at the last depth-only clear (ADR-0502), the
    //    instant before the engine cleared the depth that still held the scene.
    if (s_dibrCaptureValid && s_dibrCapture.w >= 8 && s_dibrCapture.h >= 8 &&
        s_dibrCapture.depth.size() >=
            static_cast<size_t>(s_dibrCapture.w) * static_cast<size_t>(s_dibrCapture.h) &&
        (OpenGLProxy::GetSwapFrameCount() - s_dibrCapture.frame) <= kDibrCaptureMaxAgeSwaps) {
        int rx = 0, ry = 0, rw = s_dibrCapture.w, rh = s_dibrCapture.h;
        // A capture on the window is in the same (window) coordinate frame, so the
        // active rect can be mapped straight in; an engine FBO is tiled whole,
        // exactly as the swap-time FBO read below does.
        if (s_dibrCapture.fbo == 0 && s_dibrCapture.w == srcW && s_dibrCapture.h == srcH) {
            rx = ox; ry = oy; rw = srcW; rh = srcH;
        }
        if (reduce(s_dibrCapture.depth.data(), s_dibrCapture.w, s_dibrCapture.h, rx, ry, rw, rh)) {
            s_dibrGrid.fbo = s_dibrCapture.fbo;
            s_dibrGrid.source = DibrSource::Clear;
            s_dibrGrid.valid = true;
            return;
        }
    }

    // 1. Window backbuffer depth (the read framebuffer is already 0/GL_BACK).
    s_dibrDepth.resize(static_cast<size_t>(srcW) * static_cast<size_t>(srcH));
    read(ox, oy, srcW, srcH, 0x1902 /*GL_DEPTH_COMPONENT*/, 0x1406 /*GL_FLOAT*/,
         s_dibrDepth.data());
    s_dibrGrid.fbo = 0;
    if (reduce(s_dibrDepth.data(), srcW, srcH, 0, 0, srcW, srcH)) {
        s_dibrGrid.source = DibrSource::Window;
        s_dibrGrid.valid = true;
        return;
    }

    // 2. The engine's own FBO depth, resolved against its own viewport.
    const GLuint fbo = OpenGLProxy::GetLastBoundFramebuffer();
    if (fbo == 0 || !OpenGLProxy::real_glBindFramebuffer) return;
    DibrGetIntegerv_t getIntegerv = DibrGetIntegerv();
    if (!getIntegerv) return;
    OpenGLProxy::real_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, fbo);
    GLint vp[4] = { 0, 0, 0, 0 };
    getIntegerv(0x0BA2 /* GL_VIEWPORT */, vp);
    bool used = false;
    if (vp[2] >= 8 && vp[3] >= 8 && vp[0] >= 0 && vp[1] >= 0 &&
        static_cast<double>(vp[2]) * static_cast<double>(vp[3]) <= 4.0e6) {
        s_dibrDepth.resize(static_cast<size_t>(vp[2]) * static_cast<size_t>(vp[3]));
        read(vp[0], vp[1], vp[2], vp[3], 0x1902, 0x1406, s_dibrDepth.data());
        used = reduce(s_dibrDepth.data(), vp[2], vp[3], 0, 0, vp[2], vp[3]);
    }
    if (used) {
        s_dibrGrid.fbo = fbo;
        s_dibrGrid.source = DibrSource::EngineFbo;
    }
    OpenGLProxy::real_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
    s_dibrGrid.valid = used;
}

} // namespace (DIBR helpers)

// GH #76 (ADR-0503): the world-depth hunt, run at the depth-only clear. ADR-0502
// closed the clear's *own* buffer as the world-depth source (far-only, nearest
// ~41 m), but that same clear is the only instant a readback is proven to return
// real depth -- so it is the right place to look for the *other* buffer that holds
// the visible world pass. Read every framebuffer object the engine generated (the
// ids Hook_glGenFramebuffers recorded), log each one's raw depth, and latch a
// genuinely near one. Evidence only: nothing here rewrites the snapshot or the
// warp, so it cannot regress the stereo pair.
void StereoRenderer::ScanFramebufferDepths(GLint w, GLint h) {
    if (!g_vrConfig.stereoParallaxDibr) return;
    DibrReadPixels_t read = DibrReadPixels();
    if (!read || !OpenGLProxy::real_glBindFramebuffer) return;
    if (w < 8 || h < 8) return;
    if (static_cast<double>(w) * static_cast<double>(h) > 4.0e6) return;

    GLuint ids[kDibrFboScanMax];
    const int n = OpenGLProxy::GetKnownFramebufferIds(ids, kDibrFboScanMax);
    ++s_dibrFboScans;
    VR_LOG_INFO("DIBR fbo scan: begin #%u known=%d rect=%dx%d", s_dibrFboScans, n, (int)w, (int)h);

    float bestLo = 1.0f;
    GLuint bestFbo = 0;
    std::vector<float> probe;
    // i == -1 is the target of this clear (id 0 = the window); the rest are the
    // engine's generated FBOs.
    for (int i = -1; i < n; ++i) {
        const GLuint fbo = (i < 0) ? 0u : ids[i];
        if (fbo != 0 && (fbo == m_leftFbo || fbo == m_rightFbo)) continue; // our eye targets
        OpenGLProxy::real_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, fbo);
        // Report the binding the driver actually accepted, so a scan that reads
        // "far" everywhere can be distinguished from a scan whose reads failed.
        GLint bound = -1;
        if (DibrGetIntegerv()) {
            DibrGetIntegerv()(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &bound);
        }
        // No skip on status: an FBO without a depth attachment (or a name the
        // engine has since deleted) must still be logged so "no near depth" is a
        // statement about the whole set. The 1.0 pre-fill keeps a failed or
        // partial read from reading as near geometry.
        GLenum status = 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */;
        if (fbo != 0 && OpenGLProxy::glCheckFramebufferStatus) {
            status = OpenGLProxy::glCheckFramebufferStatus(0x8CD5 /* GL_FRAMEBUFFER */);
        }
        probe.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 1.0f);
        read(0, 0, w, h, 0x1902 /* GL_DEPTH_COMPONENT */, 0x1406 /* GL_FLOAT */, probe.data());
        float lo = 1.0f, hi = 0.0f;
        for (float d : probe) {
            if (d < lo) lo = d;
            if (d > hi) hi = d;
        }
        VR_LOG_INFO("DIBR fbo scan: fbo=%u bound=%d %dx%d status=0x%04X rawDepth=[%.3f..%.3f]",
                    (unsigned)fbo, (int)bound, (int)w, (int)h, (unsigned)status, lo, hi);
        if (lo < bestLo) {
            bestLo = lo;
            bestFbo = fbo;
        }
    }
    OpenGLProxy::real_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);

    if (bestFbo != 0 && bestLo > 0.001f && bestLo <= kDibrNearDepthMax) {
        s_dibrFboFound = true;
        VR_LOG_INFO("DIBR fbo scan: NEAR world depth is in fbo=%u (rawMin=%.3f <= %.2f); "
                    "there is a real depth source to wire in", (unsigned)bestFbo, bestLo,
                    kDibrNearDepthMax);
    } else {
        VR_LOG_INFO("DIBR fbo scan: no near world depth in any FBO (nearest fbo=%u rawMin=%.3f)",
                    (unsigned)bestFbo, bestLo);
    }
}

// GH #76 (ADR-0502): the world-pass depth snapshot. Called from Hook_glClear for
// a depth-only clear, *before* the engine's clear executes, so the depth buffer
// still holds the world pass. Reads the currently bound draw framebuffer's depth
// at its own viewport and caches it for the swap-time reprojection. A no-op
// unless the interim is opted in; throttled by DibrDepthEvery; failure is always
// a silent fallback to the legacy uniform shift.
void StereoRenderer::OnDepthOnlyClear() {
    if (!g_vrConfig.stereoParallaxDibr) return;
    ++s_dibrDepthClears;
    // GH #76 (ADR-0503): look for the near visible world pass in the engine's
    // *other* framebuffer objects, at this clear -- the one point where a depth
    // readback is proven to return real depth (this clear's own buffer measures
    // raw 0.905..1.000, ADR-0502). Runs before the capture throttle so the scan
    // cadence is independent of DibrDepthEvery; latched off once found.
    if (!s_dibrFboFound) {
        if (s_dibrFboScanCountdown > 0) {
            --s_dibrFboScanCountdown;
        } else {
            s_dibrFboScanCountdown = kDibrFboScanEveryClears;
            ScanFramebufferDepths(m_viewportWidth > 0 ? m_viewportWidth : m_windowWidth,
                                  m_viewportHeight > 0 ? m_viewportHeight : m_windowHeight);
        }
    }
    if (s_dibrCaptureCountdown > 0) {
        --s_dibrCaptureCountdown;
        return;
    }
    DibrReadPixels_t read = DibrReadPixels();
    if (!read) return;

    const GLuint fbo = OpenGLProxy::GetCurrentDrawFramebuffer();
    GLint x = 0, y = 0, w = 0, h = 0;
    if (fbo == 0) {
        // Default framebuffer: read the back buffer, in the window's own frame.
        if (OpenGLProxy::real_glBindFramebuffer) {
            OpenGLProxy::real_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
        }
        if (OpenGLProxy::glReadBuffer) {
            OpenGLProxy::glReadBuffer(0x0405 /* GL_BACK */);
        }
        x = m_viewportX;
        y = m_viewportY;
        w = m_viewportWidth > 0 ? m_viewportWidth : m_windowWidth;
        h = m_viewportHeight > 0 ? m_viewportHeight : m_windowHeight;
    } else {
        if (!OpenGLProxy::real_glBindFramebuffer) return;
        DibrGetIntegerv_t getIntegerv = DibrGetIntegerv();
        if (!getIntegerv) return;
        OpenGLProxy::real_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, fbo);
        GLint vp[4] = { 0, 0, 0, 0 };
        getIntegerv(0x0BA2 /* GL_VIEWPORT */, vp);
        x = vp[0];
        y = vp[1];
        w = vp[2];
        h = vp[3];
    }

    s_dibrCaptureCountdown = g_vrConfig.dibrDepthEvery > 0 ? g_vrConfig.dibrDepthEvery : 1;
    if (w >= 8 && h >= 8 && x >= 0 && y >= 0 &&
        static_cast<double>(w) * static_cast<double>(h) <= 4.0e6) {
        s_dibrCapture.depth.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
        read(x, y, w, h, 0x1902 /* GL_DEPTH_COMPONENT */, 0x1406 /* GL_FLOAT */,
             s_dibrCapture.depth.data());
        s_dibrCapture.w = w;
        s_dibrCapture.h = h;
        s_dibrCapture.fbo = fbo;
        s_dibrCapture.frame = OpenGLProxy::GetSwapFrameCount();
        float lo = 1.0f, hi = 0.0f;
        for (float d : s_dibrCapture.depth) {
            if (d < lo) lo = d;
            if (d > hi) hi = d;
        }
        s_dibrCapture.rawMin = lo;
        s_dibrCapture.rawMax = hi;
        s_dibrCaptureValid = true;
        ++s_dibrCaptures;
    }
    if (fbo != 0) {
        OpenGLProxy::real_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
    }

    // Periodic evidence: does the depth-only clear actually hold a world pass?
    // rawMin < 1.0 says yes (window depth 0 = the near plane, 1 = the far plane);
    // rawMin == rawMax == 1.0 means the buffer was already cleared (a miss).
    static uint32_t s_dibrCaptureLogThrottle = 0;
    if (++s_dibrCaptureLogThrottle >= 240) {
        s_dibrCaptureLogThrottle = 0;
        VR_LOG_INFO("DIBR world-depth capture: clears=%u captures=%u fbo=%u %dx%d "
                    "rawDepth=[%.3f..%.3f]",
                    s_dibrDepthClears, s_dibrCaptures, static_cast<unsigned>(s_dibrCapture.fbo),
                    s_dibrCapture.w, s_dibrCapture.h,
                    s_dibrCapture.rawMin, s_dibrCapture.rawMax);
    }
}

bool StereoRenderer::Initialize(HDC hdc, HGLRC hglrc) {
    if (m_initialized) return true;

    VR_LOG_INFO("Initializing StereoRenderer for OpenGL context %p...", hglrc);

    // GH #29: bounded ~200ms sync budget on the render thread. One quick
    // connect attempt; on failure, fire-and-forget an async host spawn
    // (SpawnHostProcess is internally 5s-throttled) plus a single retry, then
    // fall back to Mono standby. The old 25x100ms loop blocked ~2.5s.
    // Late connection is retried from OnSwapBuffers without blocking.
    if (!m_ipcClient.Connect(100)) {
        VR_LOG_INFO("RiddickVRHost not detected. Attempting auto-spawn...");
        SpawnHostProcess();
        m_ipcClient.Connect(100); // single retry; Mono standby on failure
    }

    if (m_ipcClient.IsConnected()) {
        VR_LOG_INFO("Connected to RiddickVRHost shared memory IPC bridge!");
        m_vrActive = true;
        m_currentPass = StereoPass::LeftEye;
        if (m_ipcClient.ReadState(m_sharedState)) {
            if (m_sharedState.renderWidth > 0 && m_sharedState.renderHeight > 0) {
                m_eyeWidth = m_sharedState.renderWidth;
                m_eyeHeight = m_sharedState.renderHeight;
                VR_LOG_INFO("OpenXR requested eye resolution: %ux%u", m_eyeWidth, m_eyeHeight);
            }
        }
    } else {
        VR_LOG_WARN("Could not connect to RiddickVRHost (host process may not be running yet). VR standby mode active.");
        m_vrActive = false;
        m_currentPass = StereoPass::Mono;
    }

    // GH #30: propagate eye-FBO failure — stay Mono instead of presenting
    // half-built stereo buffers.
    if (!CreateEyeBuffers(m_eyeWidth, m_eyeHeight)) {
        VR_LOG_ERROR("Eye framebuffer setup failed; staying in Mono standby.");
        m_vrActive = false;
        m_currentPass = StereoPass::Mono;
    }

    m_initialized = true;
    return true;
}

void StereoRenderer::Shutdown() {
    DestroyEyeBuffers();
    m_ipcClient.Disconnect();
    m_initialized = false;
    m_vrActive = false;
    VR_LOG_INFO("StereoRenderer shutdown complete.");
}

bool StereoRenderer::CreateEyeBuffers(uint32_t width, uint32_t height) {
    if (!OpenGLProxy::glGenFramebuffers) {
        VR_LOG_WARN("glGenFramebuffers pointer unavailable; cannot allocate custom eye FBOs yet.");
        return false;
    }

    DestroyEyeBuffers();

    m_eyeWidth = width;
    m_eyeHeight = height;

    // --- Left Eye FBO ---
    OpenGLProxy::glGenFramebuffers(1, &m_leftFbo);
    OpenGLProxy::glGenTextures(1, &m_leftColorTex);
    OpenGLProxy::glBindTexture(GL_TEXTURE_2D, m_leftColorTex);
    OpenGLProxy::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    OpenGLProxy::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    OpenGLProxy::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    OpenGLProxy::glBindFramebuffer(0x8D40 /* GL_FRAMEBUFFER */, m_leftFbo);
    OpenGLProxy::glFramebufferTexture2D(0x8D40, 0x8CE0 /* GL_COLOR_ATTACHMENT0 */, GL_TEXTURE_2D, m_leftColorTex, 0);

    if (OpenGLProxy::glGenRenderbuffers) {
        OpenGLProxy::glGenRenderbuffers(1, &m_leftDepthRbo);
        OpenGLProxy::glBindRenderbuffer(0x8D41 /* GL_RENDERBUFFER */, m_leftDepthRbo);
        OpenGLProxy::glRenderbufferStorage(0x8D41, 0x88F0 /* GL_DEPTH24_STENCIL8 */, width, height);
        OpenGLProxy::glFramebufferRenderbuffer(0x8D40, 0x821A /* GL_DEPTH_STENCIL_ATTACHMENT */, 0x8D41, m_leftDepthRbo);
    }

    GLenum leftStatus = OpenGLProxy::glCheckFramebufferStatus ? OpenGLProxy::glCheckFramebufferStatus(0x8D40) : 0x8CD5;
    if (leftStatus != 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */) {
        VR_LOG_WARN("Left Eye FBO incomplete: status 0x%X", leftStatus);
    }

    // --- Right Eye FBO ---
    OpenGLProxy::glGenFramebuffers(1, &m_rightFbo);
    OpenGLProxy::glGenTextures(1, &m_rightColorTex);
    OpenGLProxy::glBindTexture(GL_TEXTURE_2D, m_rightColorTex);
    OpenGLProxy::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    OpenGLProxy::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    OpenGLProxy::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    OpenGLProxy::glBindFramebuffer(0x8D40, m_rightFbo);
    OpenGLProxy::glFramebufferTexture2D(0x8D40, 0x8CE0, GL_TEXTURE_2D, m_rightColorTex, 0);

    if (OpenGLProxy::glGenRenderbuffers) {
        OpenGLProxy::glGenRenderbuffers(1, &m_rightDepthRbo);
        OpenGLProxy::glBindRenderbuffer(0x8D41, m_rightDepthRbo);
        OpenGLProxy::glRenderbufferStorage(0x8D41, 0x88F0, width, height);
        OpenGLProxy::glFramebufferRenderbuffer(0x8D40, 0x821A, 0x8D41, m_rightDepthRbo);
    }

    GLenum rightStatus = OpenGLProxy::glCheckFramebufferStatus ? OpenGLProxy::glCheckFramebufferStatus(0x8D40) : 0x8CD5;
    if (rightStatus != 0x8CD5) {
        VR_LOG_WARN("Right Eye FBO incomplete: status 0x%X", rightStatus);
    }

    // Unbind back to default framebuffer
    OpenGLProxy::glBindFramebuffer(0x8D40, 0);
    OpenGLProxy::glBindTexture(GL_TEXTURE_2D, 0);

    // GH #30: require COMPLETE on both eyes; a half-built stereo pair must
    // fail closed (caller drops to Mono) rather than present garbage.
    if (leftStatus != 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */ ||
        rightStatus != 0x8CD5) {
        VR_LOG_ERROR("Eye FBO setup failed (left=0x%X right=0x%X); discarding buffers.",
            leftStatus, rightStatus);
        DestroyEyeBuffers();
        return false;
    }

    // Initialize hardware texture sharing if supported
    InitD3D11Interop(width, height);
    LockSharedTextures();

    VR_LOG_INFO("Stereo Eye Framebuffers successfully created (%ux%u, LeftFBO: %u, RightFBO: %u)",
        width, height, m_leftFbo, m_rightFbo);
    return true;
}

void StereoRenderer::DestroyEyeBuffers() {
    ReleaseD3D11Interop();

    if (OpenGLProxy::glDeleteFramebuffers) {
        if (m_leftFbo) {
            OpenGLProxy::glDeleteFramebuffers(1, &m_leftFbo);
            m_leftFbo = 0;
        }
        if (m_rightFbo) {
            OpenGLProxy::glDeleteFramebuffers(1, &m_rightFbo);
            m_rightFbo = 0;
        }
    }

    if (OpenGLProxy::glDeleteRenderbuffers) {
        if (m_leftDepthRbo) {
            OpenGLProxy::glDeleteRenderbuffers(1, &m_leftDepthRbo);
            m_leftDepthRbo = 0;
        }
        if (m_rightDepthRbo) {
            OpenGLProxy::glDeleteRenderbuffers(1, &m_rightDepthRbo);
            m_rightDepthRbo = 0;
        }
    }

    if (OpenGLProxy::glDeleteTextures) {
        if (m_leftColorTex) {
            OpenGLProxy::glDeleteTextures(1, &m_leftColorTex);
            m_leftColorTex = 0;
        }
        if (m_rightColorTex) {
            OpenGLProxy::glDeleteTextures(1, &m_rightColorTex);
            m_rightColorTex = 0;
        }
    }
}

bool StereoRenderer::InitD3D11Interop(uint32_t width, uint32_t height) {
    if (!OpenGLProxy::HasDXInteropSupport()) {
        VR_LOG_INFO("Hardware Interop: WGL_NV_DX_interop not available; using standard FBO pipeline.");
        return false;
    }

    ReleaseD3D11Interop();

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

    if (FAILED(hr) || !m_d3d11Device) {
        VR_LOG_WARN("Hardware Interop: D3D11CreateDevice failed with hr=0x%08X", hr);
        return false;
    }

    m_hDXDevice = OpenGLProxy::wglDXOpenDeviceNV(m_d3d11Device);
    if (!m_hDXDevice) {
        VR_LOG_WARN("Hardware Interop: wglDXOpenDeviceNV failed!");
        ReleaseD3D11Interop();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    // Create Left Eye Shared D3D11 Texture
    hr = m_d3d11Device->CreateTexture2D(&desc, nullptr, &m_d3d11LeftTex);
    if (SUCCEEDED(hr) && m_d3d11LeftTex) {
        IDXGIResource* pResource = nullptr;
        if (SUCCEEDED(m_d3d11LeftTex->QueryInterface(__uuidof(IDXGIResource), (void**)&pResource))) {
            pResource->GetSharedHandle(&m_sharedHandleLeft);
            pResource->Release();
        }
    }

    // Create Right Eye Shared D3D11 Texture
    hr = m_d3d11Device->CreateTexture2D(&desc, nullptr, &m_d3d11RightTex);
    if (SUCCEEDED(hr) && m_d3d11RightTex) {
        IDXGIResource* pResource = nullptr;
        if (SUCCEEDED(m_d3d11RightTex->QueryInterface(__uuidof(IDXGIResource), (void**)&pResource))) {
            pResource->GetSharedHandle(&m_sharedHandleRight);
            pResource->Release();
        }
    }

    if (!m_sharedHandleLeft || !m_sharedHandleRight) {
        VR_LOG_WARN("Hardware Interop: Failed to retrieve DXGI shared resource handles.");
        ReleaseD3D11Interop();
        return false;
    }

    // Register with OpenGL textures
    m_hDXLeftTex = OpenGLProxy::wglDXRegisterObjectNV(
        m_hDXDevice, m_d3d11LeftTex, m_leftColorTex, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV
    );
    m_hDXRightTex = OpenGLProxy::wglDXRegisterObjectNV(
        m_hDXDevice, m_d3d11RightTex, m_rightColorTex, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV
    );

    if (!m_hDXLeftTex || !m_hDXRightTex) {
        VR_LOG_WARN("Hardware Interop: wglDXRegisterObjectNV failed!");
        ReleaseD3D11Interop();
        return false;
    }

    m_dxInteropActive = true;
    m_sharedState.leftEyeSharedHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m_sharedHandleLeft));
    m_sharedState.rightEyeSharedHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m_sharedHandleRight));

    if (m_ipcClient.IsConnected()) {
        m_ipcClient.WriteState(m_sharedState);
    }

    VR_LOG_INFO("Hardware Interop: SUCCESS! Zero-copy GPU texture sharing ACTIVE (Left: 0x%p, Right: 0x%p)",
        m_sharedHandleLeft, m_sharedHandleRight);
    return true;
}

void StereoRenderer::ReleaseD3D11Interop() {
    UnlockSharedTextures();

    if (m_hDXDevice) {
        if (m_hDXLeftTex && OpenGLProxy::wglDXUnregisterObjectNV) {
            OpenGLProxy::wglDXUnregisterObjectNV(m_hDXDevice, m_hDXLeftTex);
            m_hDXLeftTex = nullptr;
        }
        if (m_hDXRightTex && OpenGLProxy::wglDXUnregisterObjectNV) {
            OpenGLProxy::wglDXUnregisterObjectNV(m_hDXDevice, m_hDXRightTex);
            m_hDXRightTex = nullptr;
        }
        if (OpenGLProxy::wglDXCloseDeviceNV) {
            OpenGLProxy::wglDXCloseDeviceNV(m_hDXDevice);
        }
        m_hDXDevice = nullptr;
    }

    if (m_d3d11LeftTex) {
        m_d3d11LeftTex->Release();
        m_d3d11LeftTex = nullptr;
    }
    if (m_d3d11RightTex) {
        m_d3d11RightTex->Release();
        m_d3d11RightTex = nullptr;
    }
    if (m_d3d11Context) {
        m_d3d11Context->Release();
        m_d3d11Context = nullptr;
    }
    if (m_d3d11Device) {
        m_d3d11Device->Release();
        m_d3d11Device = nullptr;
    }

    m_sharedHandleLeft = nullptr;
    m_sharedHandleRight = nullptr;
    m_dxInteropActive = false;
}

void StereoRenderer::LockSharedTextures() {
    if (!m_dxInteropActive || m_texturesLocked || !m_hDXDevice || !OpenGLProxy::wglDXLockObjectsNV) return;

    HANDLE objects[2] = { m_hDXLeftTex, m_hDXRightTex };
    if (OpenGLProxy::wglDXLockObjectsNV(m_hDXDevice, 2, objects)) {
        m_texturesLocked = true;
    }
}

void StereoRenderer::UnlockSharedTextures() {
    if (!m_dxInteropActive || !m_texturesLocked || !m_hDXDevice || !OpenGLProxy::wglDXUnlockObjectsNV) return;

    HANDLE objects[2] = { m_hDXLeftTex, m_hDXRightTex };
    if (OpenGLProxy::wglDXUnlockObjectsNV(m_hDXDevice, 2, objects)) {
        m_texturesLocked = false;
    }
}

void StereoRenderer::BlitEye(bool isLeftEye, GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                 GLint dx0, GLint dy0, GLint dx1, GLint dy1) {
    GLuint fbo = isLeftEye ? m_leftFbo : m_rightFbo;
    OpenGLProxy::real_glBindFramebuffer(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, fbo);
    // NOTE: call sites pass y-flipped src coords (top-down into GL bottom-up).
    OpenGLProxy::glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                                   GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

// GH #44: find the non-black content bounds of the backbuffer so baked
// letterbox/pillarbox bars can be cropped out of the gameplay blit.
// Throttled (every 90 swaps), smoothed, GL-origin (y-up) window coords.
void StereoRenderer::ScanActiveRegion(GLint ox, GLint oy, GLint srcW, GLint srcH) {
    typedef void (WINAPI *ReadPx_t)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
    typedef void (WINAPI *PixelStore_t)(GLenum, GLint);
    static ReadPx_t pRead = nullptr;
    static PixelStore_t pStore = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE hSelf = GetModuleHandleA("opengl32.dll"); // our proxy; forwarders self-resolve
        if (hSelf) {
            pRead = (ReadPx_t)GetProcAddress(hSelf, "glReadPixels");
            pStore = (PixelStore_t)GetProcAddress(hSelf, "glPixelStorei");
        }
    }
    if (!pRead || !pStore || srcW < 64 || srcH < 64) return;

    if (m_barScanCooldown > 0) { m_barScanCooldown--; return; }
    m_barScanCooldown = 90;

    pStore(0x0CF5 /* GL_PACK_ALIGNMENT */, 1);
    const int STRIP = 8;
    static unsigned char row[4096 * 8 * 3];
    static unsigned char col[8 * 4096 * 3];
    if (srcW > 4096 || srcH > 4096) return;
    auto rowLit = [&](GLint y, GLint h) {
        pRead(ox, oy + y, srcW, h, 0x1907 /* GL_RGB */, 0x1401 /* GL_UNSIGNED_BYTE */, row);
        for (int i = 0; i < srcW * h * 3; i += 3)
            if (row[i] > 14 || row[i + 1] > 14 || row[i + 2] > 14) return true;
        return false;
    };
    auto colLit = [&](GLint x, GLint w) {
        pRead(ox + x, oy, w, srcH, 0x1907 /* GL_RGB */, 0x1401 /* GL_UNSIGNED_BYTE */, col);
        for (int i = 0; i < w * srcH * 3; i += 3)
            if (col[i] > 14 || col[i + 1] > 14 || col[i + 2] > 14) return true;
        return false;
    };

    GLint y0 = 0, y1 = srcH, x0 = 0, x1 = srcW;
    for (int y = 0; y < srcH / 2; y += STRIP) { if (rowLit(y, STRIP)) { y0 = y; break; } y0 = y + STRIP; }
    for (int y = srcH - STRIP; y > srcH / 2; y -= STRIP) { if (rowLit(y, STRIP)) { y1 = y + STRIP; break; } y1 = y; }
    for (int x = 0; x < srcW / 2; x += STRIP) { if (colLit(x, STRIP)) { x0 = x; break; } x0 = x + STRIP; }
    for (int x = srcW - STRIP; x > srcW / 2; x -= STRIP) { if (colLit(x, STRIP)) { x1 = x + STRIP; break; } x1 = x; }

    // Reject degenerate detections (keep previous / full frame).
    if (x1 - x0 < srcW / 3 || y1 - y0 < srcH / 3) return;
    // Store absolute backbuffer coords.
    x0 += ox; x1 += ox; y0 += oy; y1 += oy;
    const GLint fx1 = ox + srcW, fy1 = oy + srcH;
    if (!m_hasActiveRect) {
        m_activeX0 = x0; m_activeY0 = y0; m_activeX1 = x1; m_activeY1 = y1;
        m_hasActiveRect = true;
        VR_LOG_INFO("BarScan: first active rect (%d,%d)-(%d,%d) of (%d,%d) win",
            x0, y0, x1, y1, fx1, fy1);
        return;
    }
    // Snap on large jumps (mode/scene change), else ease toward detection.
    auto snap = [](GLint a, GLint b, GLint span) { return abs(a - b) > span / 4; };
    if (snap(x0, m_activeX0, srcW) || snap(x1, m_activeX1, srcW) ||
        snap(y0, m_activeY0, srcH) || snap(y1, m_activeY1, srcH)) {
        m_activeX0 = x0; m_activeY0 = y0; m_activeX1 = x1; m_activeY1 = y1;
        VR_LOG_INFO("BarScan: snap to (%d,%d)-(%d,%d)", x0, y0, x1, y1);
    } else {
        m_activeX0 = (m_activeX0 + x0) / 2; m_activeY0 = (m_activeY0 + y0) / 2;
        m_activeX1 = (m_activeX1 + x1) / 2; m_activeY1 = (m_activeY1 + y1) / 2;
    }
}

void StereoRenderer::OnViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    // Capture the primary game viewport, including pillarbox/letterbox active regions
    if (width >= 640 && height >= 480) {
        m_viewportX = x;
        m_viewportY = y;
        m_viewportWidth = width;
        m_viewportHeight = height;
        if (x == 0 && y == 0) {
            m_windowWidth = width;
            m_windowHeight = height;
        }
    }
}

void StereoRenderer::OnSwapBuffers(HDC hdc) {
    if (!m_initialized) return;

    static uint32_t s_swapCount = 0;
    static ULONGLONG s_lastFpsTime = 0;
    static uint32_t s_fpsFrameCount = 0;

    s_swapCount++;
    s_fpsFrameCount++;

    ULONGLONG now = GetTickCount64();
    if (now - s_lastFpsTime >= 5000) {
        float fps = s_fpsFrameCount / ((now - s_lastFpsTime) / 1000.0f);
        VR_LOG_INFO("OnSwapBuffers: Frame %u (%.1f FPS, HDC: %p)", s_swapCount, fps, hdc);
        s_lastFpsTime = now;
        s_fpsFrameCount = 0;
    }

    // Determine current window size from HDC if not already set or resized
    HWND hwnd = WindowFromDC(hdc);
    if (hwnd) {
        CameraHook::Instance().SetGameWindow(hwnd);
        RECT rc;
        if (GetClientRect(hwnd, &rc)) {
            if (rc.right > rc.left && rc.bottom > rc.top) {
                m_windowWidth = rc.right - rc.left;
                m_windowHeight = rc.bottom - rc.top;
            }
        }
    }

    // Retry connecting to host if not yet connected
    if (!m_ipcClient.IsConnected()) {
        static ULONGLONG s_lastRetry = 0;
        if (now - s_lastRetry > 3000) {
            s_lastRetry = now;
            if (m_ipcClient.Connect(100)) {
                VR_LOG_INFO("Late connected to RiddickVRHost!");
                m_vrActive = true;
            } else {
                SpawnHostProcess();
            }
        }
    }

    if (m_ipcClient.IsConnected()) {
        // Read updated tracking state from host
        if (m_ipcClient.ReadState(m_sharedState)) {
            m_vrActive = m_sharedState.vrActive;
            CameraHook::Instance().Update(m_sharedState);

            // Dynamically resize eye framebuffers if OpenXR headset requested specific dimensions
            if (m_sharedState.renderWidth > 0 && m_sharedState.renderHeight > 0 &&
                (m_sharedState.renderWidth != m_eyeWidth || m_sharedState.renderHeight != m_eyeHeight)) {
                VR_LOG_INFO("Resizing Stereo Eye Framebuffers to match OpenXR headset (%ux%u -> %ux%u)",
                    m_eyeWidth, m_eyeHeight, m_sharedState.renderWidth, m_sharedState.renderHeight);
                CreateEyeBuffers(m_sharedState.renderWidth, m_sharedState.renderHeight);
            }
        }

        // 1. Lock textures for OpenGL drawing
        LockSharedTextures();

        // 2. Capture the game's rendered backbuffer and blit into Left & Right eye FBOs
        if (OpenGLProxy::real_glBindFramebuffer && OpenGLProxy::glBlitFramebuffer) {
            GLint srcX = 0;
            GLint srcY = 0;
            GLint srcW = (m_windowWidth > 0) ? m_windowWidth : 1920;
            GLint srcH = (m_windowHeight > 0) ? m_windowHeight : 1080;

            if (m_viewportWidth >= 640 && m_viewportHeight >= 480) {
                srcX = m_viewportX;
                srcY = m_viewportY;
                srcW = m_viewportWidth;
                srcH = m_viewportHeight;
            }

            GLint dstX = 0;
            GLint dstY = 0;
            GLint dstW = m_eyeWidth;
            GLint dstH = m_eyeHeight;

            // GH #43/#44: cinema screen for genuine menus/cinematics (2D frames),
            // full-bleed immersion for 3D gameplay. Menus can render 3D
            // backgrounds, so an OS cursor marks "menu" -- but only if it is
            // *sustained*. A cursor that merely flashes during gameplay must not
            // flip the whole view to a letterboxed cinema screen (that is the
            // "black bars while playing" symptom). Engage after ~0.75s of a
            // visible cursor, release after ~1s of it being hidden.
            static int s_cursorVisibleFrames = 0;
            static int s_cursorHiddenFrames = 0;
            static bool s_cursorMenu = false;
            if (OpenGLProxy::HasMenuCursor()) {
                if (s_cursorVisibleFrames < 100000) ++s_cursorVisibleFrames;
                s_cursorHiddenFrames = 0;
            } else {
                if (s_cursorHiddenFrames < 100000) ++s_cursorHiddenFrames;
                s_cursorVisibleFrames = 0;
            }
            if (!s_cursorMenu && s_cursorVisibleFrames >= 45) s_cursorMenu = true;
            if (s_cursorMenu && s_cursorHiddenFrames >= 60) s_cursorMenu = false;

            // GH #72 req 1: cold-start full-bleed latch. Opt-in
            // (DisplayBranchColdStartLatch): while it is on the BarScan rect is
            // measured *before* the branch decision and in every frame, so a
            // demoted startup page can still clear the latch once gameplay is
            // reached (the scan is otherwise gameplay-branch only, which would
            // leave a demoted page unable to ever re-measure).
            const bool branchLatch = g_vrConfig.displayBranchColdStartLatch != 0;
            if (branchLatch) {
                OpenGLProxy::real_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
                if (OpenGLProxy::glReadBuffer) {
                    OpenGLProxy::glReadBuffer(0x0405 /* GL_BACK */);
                }
                ScanActiveRegion(srcX, srcY, srcW, srcH);
            }

            DisplayBranchConfig branchCfg;
            branchCfg.coldStartLatch = branchLatch;
            bool haveFill = false;
            float activeFill = 0.0f;
            if (branchLatch && m_hasActiveRect && srcW > 0 && srcH > 0) {
                const float aw = static_cast<float>(m_activeX1 - m_activeX0);
                const float ah = static_cast<float>(m_activeY1 - m_activeY0);
                activeFill = (aw * ah) / (static_cast<float>(srcW) * static_cast<float>(srcH));
                haveFill = true;
            }
            static DisplayBranchState s_branchState;
            const bool isCinema = ComputeDisplayBranch(
                OpenGLProxy::Has3DThisFrame(), s_cursorMenu, activeFill, haveFill,
                branchCfg, s_branchState);
            m_sharedState.cinemaMode = isCinema;
            // GH #72 req 1 / ADR-0478: report the latch's *own* decision once, so
            // a live run proves the demotion instead of inferring it from an
            // absent `DisplayMode` transition (the branch simply never changes).
            if (branchLatch && isCinema && haveFill && !s_cursorMenu &&
                OpenGLProxy::Has3DThisFrame() &&
                activeFill <= branchCfg.letterboxFill && !s_branchState.fullBleedSeen) {
                static bool s_latchDemotionLogged = false;
                if (!s_latchDemotionLogged) {
                    s_latchDemotionLogged = true;
                    VR_LOG_INFO("DisplayMode: cold-start latch demoted a "
                                "letterboxed 3D frame to CINEMA (fill=%.2f "
                                "latch=1 fullBleed=0)", activeFill);
                }
            }
            {
                static bool s_lastCinema = false;
                static bool s_cinemaLogged = false;
                if (isCinema != s_lastCinema || !s_cinemaLogged) {
                    s_lastCinema = isCinema;
                    s_cinemaLogged = true;
                    VR_LOG_INFO("DisplayMode: %s (3Dfresh=%d menuCursor=%d latch=%d fill=%.2f fullBleed=%d)",
                        isCinema ? "CINEMA screen (menus/cinematics)" : "FULL-BLEED gameplay",
                        (int)OpenGLProxy::Has3DThisFrame(), (int)OpenGLProxy::HasMenuCursor(),
                        (int)branchLatch, haveFill ? activeFill : -1.0f,
                        (int)s_branchState.fullBleedSeen);
                }
            }

            // Read from default backbuffer 0
            OpenGLProxy::real_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
            if (OpenGLProxy::glReadBuffer) {
                OpenGLProxy::glReadBuffer(0x0405 /* GL_BACK */);
            }

            if (isCinema) {
                // GH #43: fit the 2D frame onto a virtual cinema screen -
                // contain aspect, centered, black surround (looking at a screen).
                if (OpenGLProxy::glClearColor && OpenGLProxy::real_glClearFunc) {
                    OpenGLProxy::real_glBindFramebuffer(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, m_leftFbo);
                    OpenGLProxy::glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    OpenGLProxy::real_glClearFunc(GL_COLOR_BUFFER_BIT);
                    OpenGLProxy::real_glBindFramebuffer(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, m_rightFbo);
                    OpenGLProxy::glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    OpenGLProxy::real_glClearFunc(GL_COLOR_BUFFER_BIT);
                }
                float fit = (std::min)(static_cast<float>(dstW) / srcW, static_cast<float>(dstH) / srcH);
                GLint cw = static_cast<GLint>(srcW * fit);
                GLint ch = static_cast<GLint>(srcH * fit);
                GLint cx0 = (dstW - cw) / 2;
                GLint cy0 = (dstH - ch) / 2;
                // y-flipped src (top-down into GL bottom-up), flat screen: no IPD shift.
                BlitEye(true, srcX, srcY + srcH, srcX + srcW, srcY, cx0, cy0, cx0 + cw, cy0 + ch);
                VRHandRenderer::Instance().RenderHands(m_sharedState, true, m_eyeWidth, m_eyeHeight);
                BlitEye(false, srcX, srcY + srcH, srcX + srcW, srcY, cx0, cy0, cx0 + cw, cy0 + ch);
                VRHandRenderer::Instance().RenderHands(m_sharedState, false, m_eyeWidth, m_eyeHeight);
            } else {
                // GH #44: full-bleed gameplay; crop baked letterbox/pillarbox out.
                // GH #72: with the cold-start latch on the rect was already
                // measured above in this same swap; this call is then a no-op
                // (the scan is throttled).
                ScanActiveRegion(srcX, srcY, srcW, srcH);
                GLint ax0 = m_hasActiveRect ? m_activeX0 : srcX;
                GLint ay0 = m_hasActiveRect ? m_activeY0 : srcY;
                GLint ax1 = m_hasActiveRect ? m_activeX1 : srcX + srcW;
                GLint ay1 = m_hasActiveRect ? m_activeY1 : srcY + srcH;

            // GH #12: monoscopic capture has no IPD parallax (both eyes blit
            // the same backbuffer). Apply a small half-IPD horizontal shift per
            // eye as a convergence cue. This does NOT synthesize true parallax
            // (occlusion/disparity need an engine-level dual-view render).
            // Track C #9: comfort vignette adds a motion-scaled extra shift.
            // Desktop mirror is untouched: only eye-FBO source rects shift.
            GLint spanW = ax1 - ax0;
            GLint spanH = ay1 - ay0;
            GLint halfIpdPx = (std::max)(1, spanW / 240);
            GLint comfortPx = 0;
            if (m_sharedState.comfortVignette > 0.01f) {
                int shiftPx = static_cast<int>(spanW * m_hudConvergenceOffset * m_sharedState.comfortVignette);
                comfortPx = (shiftPx > 1) ? shiftPx : 1;
            }
            GLint leftSrcX0 = ax0 - halfIpdPx - comfortPx;
            GLint leftSrcX1 = ax1 - halfIpdPx - comfortPx;
            GLint rightSrcX0 = ax0 + halfIpdPx + comfortPx;
            GLint rightSrcX1 = ax1 + halfIpdPx + comfortPx;

            // GH #76 (interim depth-based reprojection, ADR-0501/ADR-0502): when
            // enabled and the engine's depth is usable, replace the single global
            // shift with a per-cell shift derived from the scene depth, so disparity
            // varies with depth instead of being a flat translation. Falls back to
            // the legacy uniform shift whenever the depth read is unusable.
            const float* dibrProj = OpenGLProxy::GetLastProjectionMatrix();
            // GH #76 (ADR-0502): keep wake 99's fail-safe invariant -- the
            // depth-varying warp must never produce *less* eye separation than the
            // legacy half-IPD convergence cue it replaces (a depth read may sharpen
            // stereo, never flatten it). Measured: the depth captured at the
            // depth-only clear is a far-only pass (nearest ~41 m, ADR-0502), so its
            // sub-2 px disparity falls back to the legacy blit rather than turning
            // the pair into a mono duplicate.
            const float minSepPx = 2.0f * static_cast<float>(halfIpdPx);
            bool useDibr = false;
            if (g_vrConfig.stereoParallaxDibr && dibrProj != nullptr) {
                if (s_dibrCountdown <= 0) {
                    s_dibrCountdown = g_vrConfig.dibrDepthEvery;
                    const float baseline = EyeBaselineMeters(m_sharedState.leftEyeView.m,
                                                             m_sharedState.rightEyeView.m, 0.065f);
                    BuildDibrGrid(ax0, ay0, spanW, spanH, dibrProj, baseline);
                    useDibr = s_dibrGrid.valid && s_dibrGrid.maxDisp >= minSepPx;
                    static uint32_t s_dibrLogThrottle = 0;
                    if (++s_dibrLogThrottle >= 12) {
                        s_dibrLogThrottle = 0;
                        VR_LOG_INFO("DIBR: src=%s grid=%dx%d depth=%dx%d fbo=%u depth_z=[%.2f..%.2f]m "
                                    "fullDisp=[%.2f..%.2f]px baseline=%.3fm valid=%d/%d minSep=%.1f use=%d",
                                    DibrSourceName(s_dibrGrid.source),
                                    s_dibrGrid.cols, s_dibrGrid.rows, s_dibrGrid.depthW, s_dibrGrid.depthH,
                                    (unsigned)s_dibrGrid.fbo, s_dibrGrid.minZ, s_dibrGrid.maxZ,
                                    s_dibrGrid.minDisp, s_dibrGrid.maxDisp, baseline,
                                    s_dibrGrid.validCells, s_dibrGrid.cols * s_dibrGrid.rows,
                                    minSepPx, (int)useDibr);
                    }
                } else {
                    --s_dibrCountdown;
                    useDibr = s_dibrGrid.valid && s_dibrGrid.maxDisp >= minSepPx;
                }
            }

            if (useDibr) {
                const int cols = s_dibrGrid.cols;
                const int rows = s_dibrGrid.rows;
                for (int eye = 0; eye < 2; ++eye) {
                    const bool isLeft = (eye == 0);
                    for (int r = 0; r < rows; ++r) {
                        const GLint sy0 = ay0 + (r * spanH) / rows;
                        const GLint sy1 = ay0 + ((r + 1) * spanH) / rows;
                        // The legacy single blit flips vertically (src top ->
                        // dst bottom); reproduce that per cell.
                        const GLint dy0 = dstY + ((rows - 1 - r) * dstH) / rows;
                        const GLint dy1 = dstY + ((rows - r) * dstH) / rows;
                        for (int c = 0; c < cols; ++c) {
                            const GLint sx0 = ax0 + (c * spanW) / cols;
                            const GLint sx1 = ax0 + ((c + 1) * spanW) / cols;
                            const GLint dx0 = dstX + (c * dstW) / cols;
                            const GLint dx1 = dstX + ((c + 1) * dstW) / cols;
                            const float full = s_dibrGrid.fullDispPx[
                                static_cast<size_t>(r) * cols + c];
                            const float v = (isLeft ? -0.5f : 0.5f) * full;
                            const int shift = static_cast<int>(v >= 0.0f ? v + 0.5f : v - 0.5f);
                            BlitEye(isLeft, sx0 + shift, sy1, sx1 + shift, sy0,
                                    dx0, dy0, dx1, dy1);
                        }
                    }
                    // Render always-visible 3D VR tactical hands into this eye.
                    VRHandRenderer::Instance().RenderHands(m_sharedState, isLeft, m_eyeWidth, m_eyeHeight);
                }
            } else {
            // Blit active gameplay region full-bleed into both eyes
            // (y-flipped src: top-down into GL bottom-up).
            // In gameplay mode (dstX=0, dstY=0, dstW=m_eyeWidth, dstH=m_eyeHeight),
            // this fills the entire buffer with 100% immersion and ZERO black bars.
            BlitEye(true, leftSrcX0, ay1, leftSrcX1, ay0, dstX, dstY, dstX + dstW, dstY + dstH);

            // Render always-visible 3D VR tactical hands into Left Eye buffer
            VRHandRenderer::Instance().RenderHands(m_sharedState, true, m_eyeWidth, m_eyeHeight);

            // Blit active gameplay region full-bleed into Right Eye FBO.
            BlitEye(false, rightSrcX0, ay1, rightSrcX1, ay0, dstX, dstY, dstX + dstW, dstY + dstH);

            // Render always-visible 3D VR tactical hands into Right Eye buffer
            VRHandRenderer::Instance().RenderHands(m_sharedState, false, m_eyeWidth, m_eyeHeight);
            }

            // GH #9: true dynamic locomotion comfort vignette. Peripheral
            // dimming over each eye FBO (desktop mirror untouched). Zero cost
            // at rest: skipped when comfortVignette < 0.01 or INI-disabled.
            {
                float vig = m_sharedState.comfortVignette;
                if (vig < 0.0f) vig = 0.0f;
                if (vig > 1.0f) vig = 1.0f;
                if (g_vrConfig.comfortEnabled && vig >= 0.01f && g_vrConfig.comfortMaxDim > 0.001f) {
                    float maxAlpha = vig * g_vrConfig.comfortMaxDim;
                    if (maxAlpha > 1.0f) maxAlpha = 1.0f;
                    if (maxAlpha >= 0.01f) {
                        ApplyComfortVignetteToEye(m_leftFbo, maxAlpha, m_eyeWidth, m_eyeHeight);
                        ApplyComfortVignetteToEye(m_rightFbo, maxAlpha, m_eyeWidth, m_eyeHeight);
                    }
                }
            }

            // Force alpha channel to fully opaque 1.0f on both eye buffers to eliminate
            // Virtual Desktop / VDXR green screen chroma keying and transparency flashes
            if (OpenGLProxy::glColorMask && OpenGLProxy::glClearColor && OpenGLProxy::real_glClearFunc) {
                OpenGLProxy::glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
                OpenGLProxy::glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

                OpenGLProxy::real_glBindFramebuffer(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, m_leftFbo);
                OpenGLProxy::real_glClearFunc(GL_COLOR_BUFFER_BIT);

                OpenGLProxy::real_glBindFramebuffer(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, m_rightFbo);
                OpenGLProxy::real_glClearFunc(GL_COLOR_BUFFER_BIT);

                OpenGLProxy::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            }

            } // end gameplay full-bleed branch (GH #44)

            // Restore default framebuffer 0 for the game and spectator display
            OpenGLProxy::real_glBindFramebuffer(0x8D40 /* GL_FRAMEBUFFER */, 0);
        }

        // Flush OpenGL command queue so GPU work is committed before unlocking
        if (OpenGLProxy::glFlush) {
            OpenGLProxy::glFlush();
        }

        // 3. Unlock textures so D3D11 / OpenXR host can read them
        UnlockSharedTextures();

        // Flush D3D11 device context so GPU commands on the shared resource are committed
        // before the separate OpenXR host process accesses it
        if (m_d3d11Context) {
            m_d3d11Context->Flush();
        }

        // Re-assign persistent GPU shared texture handles
        if (m_dxInteropActive) {
            m_sharedState.leftEyeSharedHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m_sharedHandleLeft));
            m_sharedState.rightEyeSharedHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m_sharedHandleRight));
        }

        // Write client frame count and state
        m_ipcClient.WriteState(m_sharedState);

        // Signal host that frame is ready
        m_ipcClient.SignalFrameReady();

        // Alternate stereoscopic eye pass for the upcoming scene draw
        if (m_currentPass == StereoPass::LeftEye) {
            m_currentPass = StereoPass::RightEye;
        } else {
            m_currentPass = StereoPass::LeftEye;
        }
    }
}

GLuint StereoRenderer::GetActiveEyeFbo() const {
    if (!m_vrActive) return 0;
    return (m_currentPass == StereoPass::RightEye) ? m_rightFbo : m_leftFbo;
}

void StereoRenderer::OnClear(GLbitfield mask) {
    // Intentionally no-op: let the game engine clear its own framebuffers, shadow maps, and stencil buffers
}

} // namespace RiddickVR
