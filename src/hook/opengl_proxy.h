#pragma once

#include <windows.h>
#include <GL/gl.h>
#include <cstdint>

namespace RiddickVR {

// Extension function types
typedef void (APIENTRY *PFNGLBINDFRAMEBUFFERPROC)(GLenum target, GLuint framebuffer);
typedef void (APIENTRY *PFNGLGENFRAMEBUFFERSPROC)(GLsizei n, GLuint *framebuffers);
typedef void (APIENTRY *PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei n, const GLuint *framebuffers);
typedef void (APIENTRY *PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef GLenum (APIENTRY *PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum target);
typedef void (APIENTRY *PFNGLBLITFRAMEBUFFERPROC)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
typedef void (APIENTRY *PFNGLUNIFORMMATRIX4FVPROC)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);

// Renderbuffer extension types
typedef void (APIENTRY *PFNGLGENRENDERBUFFERSPROC)(GLsizei n, GLuint *renderbuffers);
typedef void (APIENTRY *PFNGLBINDRENDERBUFFERPROC)(GLenum target, GLuint renderbuffer);
typedef void (APIENTRY *PFNGLRENDERBUFFERSTORAGEPROC)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
typedef void (APIENTRY *PFNGLFRAMEBUFFERRENDERBUFFERPROC)(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
typedef void (APIENTRY *PFNGLDELETERENDERBUFFERSPROC)(GLsizei n, const GLuint *renderbuffers);

// ARB Vertex Program / GLSL extension types
typedef void (APIENTRY *PFNGLPROGRAMENVPARAMETER4FVARBPROC)(GLenum target, GLuint index, const GLfloat *params);
typedef void (APIENTRY *PFNGLPROGRAMENVPARAMETER4FARBPROC)(GLenum target, GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
typedef void (APIENTRY *PFNGLPROGRAMLOCALPARAMETER4FVARBPROC)(GLenum target, GLuint index, const GLfloat *params);
typedef void (APIENTRY *PFNGLPROGRAMSTRINGARBPROC)(GLenum target, GLenum format, GLsizei len, const void *string);
typedef void (APIENTRY *PFNGLBINDPROGRAMARBPROC)(GLenum target, GLuint program);

// WGL_NV_DX_interop extension types
#define WGL_ACCESS_READ_ONLY_NV     0x00000000
#define WGL_ACCESS_READ_WRITE_NV    0x00000001
#define WGL_ACCESS_WRITE_DISCARD_NV 0x00000002

typedef HANDLE (APIENTRY *PFNWGLDXOPENDEVICENVPROC)(void *dxDevice);
typedef BOOL   (APIENTRY *PFNWGLDXCLOSEDEVICENVPROC)(HANDLE hDevice);
typedef HANDLE (APIENTRY *PFNWGLDXREGISTEROBJECTNVPROC)(HANDLE hDevice, void *dxObject, GLuint name, GLenum type, GLenum access);
typedef BOOL   (APIENTRY *PFNWGLDXUNREGISTEROBJECTNVPROC)(HANDLE hDevice, HANDLE hObject);
typedef BOOL   (APIENTRY *PFNWGLDXOBJECTACCESSNVPROC)(HANDLE hObject, GLenum access);
typedef BOOL   (APIENTRY *PFNWGLDXLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE *hObjects);
typedef BOOL   (APIENTRY *PFNWGLDXUNLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE *hObjects);

typedef void (APIENTRY *PFN_glBindTexture)(GLenum target, GLuint texture);
typedef void (APIENTRY *PFN_glGenTextures)(GLsizei n, GLuint *textures);
typedef void (APIENTRY *PFN_glDeleteTextures)(GLsizei n, const GLuint *textures);
typedef void (APIENTRY *PFN_glTexImage2D)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
typedef void (APIENTRY *PFN_glTexParameteri)(GLenum target, GLenum pname, GLint param);

class OpenGLProxy {
public:
    static bool Initialize();
    static void Shutdown();
    static void QueryContextExtensions();

    // WGL Interception Points
    static HGLRC WINAPI Hook_wglCreateContext(HDC hdc);
    static BOOL  WINAPI Hook_wglDeleteContext(HGLRC hglrc);
    static BOOL  WINAPI Hook_wglMakeCurrent(HDC hdc, HGLRC hglrc);
    static BOOL  WINAPI Hook_wglSwapBuffers(HDC hdc);
    static PROC  WINAPI Hook_wglGetProcAddress(LPCSTR lpszProc);

    // GL Interception Points
    static void WINAPI Hook_glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
    static void WINAPI Hook_glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
    static void WINAPI Hook_glMatrixMode(GLenum mode);
    static void WINAPI Hook_glLoadMatrixf(const GLfloat *m);
    static void WINAPI Hook_glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar);
    static void WINAPI Hook_glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar);
    static void WINAPI Hook_glClear(GLbitfield mask);
    static void WINAPI Hook_glBindFramebuffer(GLenum target, GLuint framebuffer);
    // GH #76 (ADR-0503): record the framebuffer-object names the engine
    // generates. The engine's scene FBO ids are otherwise invisible to the hook
    // (only the ones it binds pass through Hook_glBindFramebuffer), and the
    // depth-only clear's buffer was measured *far-only* (ADR-0502), so the
    // remaining question -- "which buffer holds the near world pass?" -- needs
    // the full set to read. Forwarding only; never suppresses.
    static void WINAPI Hook_glGenFramebuffers(GLsizei n, GLuint *framebuffers);

    // GH #67 step 1 (corrected, ADR-0481): one *candidate* transform source is
    // the ARB vertex-program path. RndrGL.dll's static OPENGL32 import table has
    // no ARB entry point (measured, `tools/gl_vocabulary_scan.py --imports`), so
    // every ARB name can only be reached through `wglGetProcAddress`. These
    // intercept those extension calls, forward them verbatim, and feed
    // `vertex_program_observe.h` so the path is observable in the same live run
    // as the GH #67 step-2 probe. Both spellings of the env-parameter setter are
    // covered, so a zero count is a statement about the engine, not one name.
    // MEASURED (ADR-0481): the path is silent on the live title -- the transform
    // is applied engine-side to the submitted vertices (ADR-0482).
    static void APIENTRY Hook_glProgramStringARB(GLenum target, GLenum format, GLsizei len, const void *string);
    static void APIENTRY Hook_glBindProgramARB(GLenum target, GLuint program);
    static void APIENTRY Hook_glProgramEnvParameter4fvARB(GLenum target, GLuint index, const GLfloat *params);
    // The engine references BOTH spellings of the env-parameter setter
    // (`…4fvARB` and `…4fARB`), so both must be intercepted for "no env params
    // seen" to be a statement about the engine rather than about one spelling.
    static void APIENTRY Hook_glProgramEnvParameter4fARB(GLenum target, GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);

    // Matrix state inspection
    static GLenum GetCurrentMatrixMode();
    static bool HasInterceptedProjection();
    static bool Has3DGameplayProjection();
    // GH #43/#44: true when a perspective projection was seen within the
    // last few presented frames (menus/cinematics go ortho/2D between frames).
    static bool Has3DThisFrame();
    // GH #43: menu cursor seen within the last few swaps (menus render 3D
    // backgrounds behind 2D UI - projection alone cannot tell them apart).
    static void NoteMenuCursor(bool visible);
    static bool HasMenuCursor();
    static const float* GetLastProjectionMatrix();
    static const float* GetLastViewMatrix();

    // GH #67 step 1: mirror the fixed-function GL_MODELVIEW stack so the matrix
    // returned by GetLastViewMatrix() is the *effective* modelview (composed
    // from glLoadMatrixf + glPushMatrix/glTranslatef/glPopMatrix -- the only
    // matrix-composition calls RndrGL imports; see
    // docs/rea/reference/RndrGL_GL_Path.txt section 3). Fed by the RndrGL IAT
    // hooks in dllmain.cpp; each is a no-op outside GL_MODELVIEW. NOTE
    // (ADR-0481): measured inert on the live title -- the engine's only modelview
    // load is an identity and the real transform is applied engine-side (see
    // vertex_program_observe.h).
    static void NoteModelviewLoadIdentity();
    static void NoteModelviewPush();
    static void NoteModelviewPop();
    static void NoteModelviewTranslatef(float x, float y, float z);

    // GH #63/#66 (approach 2): per-draw suppression of the engine's own
    // first-person viewmodel. The window state machine lives in
    // viewmodel_suppressor.h and is fed by the already-intercepted glClear /
    // projection / swap calls; the RndrGL draw hooks call this once per draw.
    // Pure `false` when the INI flag is off (the default).
    static bool ShouldSuppressDrawCall();

    // GH #67 step 1 (second correction, ADR-0482): the transform is applied
    // engine-side to the *submitted vertices* (an identity modelview + a per-view
    // projection + client-array / display-list draws -- see
    // vertex_submission_observe.h). These feed that observer from the RndrGL IAT
    // hooks in dllmain.cpp: `glVertexPointer` records the client array, and each
    // `glDrawElements` / `glCallList` is tagged by whether it fell inside the
    // first-person window. Inert unless ViewmodelObserve=1.
    static void NoteVertexPointer(GLint size, GLenum type, GLsizei stride, const void* pointer);
    static void NoteDrawElementsSubmission(GLenum mode, GLsizei count, GLenum type, const void* indices);
    static void NoteDrawListSubmission();

    // GH #67 step 1 (ninth correction, ADR-0491): the immediate-mode census. The
    // shipped RndrGL.dll *imports* glBegin/glEnd/glVertex3f/glVertex3fv (and the
    // 2-D glVertex2f/glVertex2i) -- the reference's own open question was "do the
    // first-person arms use immediate mode?" -- and the submitted-vertex observer
    // never counted that path, so a mesh submitted with glBegin..glEnd produced no
    // sample at all. These feed immediate_mode_observe.h from the RndrGL IAT hooks
    // in dllmain.cpp. Inert unless ViewmodelObserve=1.
    static void NoteImmediateBegin();
    static void NoteImmediateVertex3(float x, float y, float z);
    static void NoteImmediateVertex2(float x, float y);
    static void NoteImmediateEnd();

    // GL 1.1 functions retrieved dynamically
    static PFN_glBindTexture glBindTexture;
    static PFN_glGenTextures glGenTextures;
    static PFN_glDeleteTextures glDeleteTextures;
    static PFN_glTexImage2D glTexImage2D;
    static PFN_glTexParameteri glTexParameteri;
    typedef void (WINAPI *PFN_glReadBuffer)(GLenum mode);
    static PFN_glReadBuffer glReadBuffer;
    typedef void (WINAPI *PFN_glColorMask)(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
    static PFN_glColorMask glColorMask;
    typedef void (WINAPI *PFN_glClearColor)(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
    static PFN_glClearColor glClearColor;
    typedef void (WINAPI *PFN_glFlush)(void);
    static PFN_glFlush glFlush;
    typedef void (WINAPI *PFN_glFinish)(void);
    static PFN_glFinish glFinish;
    typedef void (WINAPI *PFN_real_glClear)(GLbitfield mask);
    static PFN_real_glClear real_glClearFunc;

    // Extension pointers retrieved via wglGetProcAddress
    static PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer;
    static PFNGLBINDFRAMEBUFFERPROC real_glBindFramebuffer;
    static void BindRealFramebuffer(GLenum target, GLuint fbo) {
        if (real_glBindFramebuffer) real_glBindFramebuffer(target, fbo);
    }
    // GH #76 (interim depth-based reprojection, ADR-0501): the last non-zero
    // framebuffer the engine bound for drawing. The engine renders the 3D scene
    // into its own FBO and presents a colour-only copy to the window, so the
    // window's depth buffer reads uniformly far (measured live); this is the id
    // whose depth attachment carries the scene depth. 0 = none seen yet.
    static GLuint GetLastBoundFramebuffer();
    // GH #76 (ADR-0502): the *currently* bound draw framebuffer (0 = window).
    // GetLastBoundFramebuffer() deliberately ignores a rebind to 0, so it cannot
    // say which buffer a depth-only clear is about to clear; this can. The host's
    // world-pass depth is read at that clear, before the engine wipes it.
    static GLuint GetCurrentDrawFramebuffer();
    // GH #76 (ADR-0502): the presented-frame counter, so a depth buffer captured
    // mid-frame can be stamped and dropped once it is stale instead of being
    // applied to a much later frame.
    static uint64_t GetSwapFrameCount();
    // GH #76 (ADR-0503): the framebuffer-object names recorded by
    // Hook_glGenFramebuffers (bounded). Copies at most `maxOut` ids and returns
    // how many were written. The swap-time depth scan uses this to name the
    // buffer that actually holds the near world pass.
    static int GetKnownFramebufferIds(GLuint* out, int maxOut);
    static PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
    static PFNGLGENFRAMEBUFFERSPROC real_glGenFramebuffers;
    static PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;
    static PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;
    static PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus;
    static PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer;
    static PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv;

    // Renderbuffers
    static PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers;
    static PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer;
    static PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage;
    static PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer;
    static PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers;

    // ARB Program parameters
    static PFNGLPROGRAMENVPARAMETER4FVARBPROC glProgramEnvParameter4fvARB;
    static PFNGLPROGRAMLOCALPARAMETER4FVARBPROC glProgramLocalParameter4fvARB;

    // WGL_NV_DX_interop extension pointers
    static PFNWGLDXOPENDEVICENVPROC wglDXOpenDeviceNV;
    static PFNWGLDXCLOSEDEVICENVPROC wglDXCloseDeviceNV;
    static PFNWGLDXREGISTEROBJECTNVPROC wglDXRegisterObjectNV;
    static PFNWGLDXUNREGISTEROBJECTNVPROC wglDXUnregisterObjectNV;
    static PFNWGLDXOBJECTACCESSNVPROC wglDXObjectAccessNV;
    static PFNWGLDXLOCKOBJECTSNVPROC wglDXLockObjectsNV;
    static PFNWGLDXUNLOCKOBJECTSNVPROC wglDXUnlockObjectsNV;
    static bool HasDXInteropSupport() { return wglDXOpenDeviceNV != nullptr && wglDXRegisterObjectNV != nullptr; }

    static HMODULE GetRealOpenGLModule() { return s_hRealOpenGL; }

private:
    static HMODULE s_hRealOpenGL;
    static bool s_initialized;
};

} // namespace RiddickVR

// GH #27: deferred hook init entry (defined in dllmain.cpp). Called on first
// wglCreateContext so no LoadLibrary/IO runs under the loader lock.
void RiddickVR_EnsureHookInitialized();
