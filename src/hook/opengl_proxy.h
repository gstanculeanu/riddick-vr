#pragma once

#include <windows.h>
#include <GL/gl.h>

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
typedef void (APIENTRY *PFNGLPROGRAMLOCALPARAMETER4FVARBPROC)(GLenum target, GLuint index, const GLfloat *params);

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
    // from glLoadMatrixf + glPushMatrix/glTranslatef/glPopMatrix, all RndrGL
    // uses -- see docs/rea/reference/RndrGL_GL_Path.txt section 3). Fed by the
    // RndrGL IAT hooks in dllmain.cpp; each is a no-op outside GL_MODELVIEW.
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
    static PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
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
