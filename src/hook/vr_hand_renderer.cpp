#include "vr_hand_renderer.h"
#include "arm_ik.h"
#include "vr_config.h" // Track C #13: world scale
#include "native_hand_mesh.h" // GH #46: baked native arm/hand geometry
#include "native_hand_pose.h" // GH #46: model-space -> hand-local placement
#include "native_hand_texture.h" // GH #61: baked Riddick skin diffuse (DXT1)
#include "bc1.h" // GH #61: BC1/DXT1 block decoder
#include "hand_visuals.h" // GH #64/#46: cue tint and fade helpers
#include "combat_guard.h" // GH #46: shared guard envelope (boxer-guard fade exemption)
#include "opengl_proxy.h"
#include "logging.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace RiddickVR {

using VRArmIK::ClampWristOrientation;

typedef void (APIENTRY *glPushAttrib_t)(GLbitfield mask);
typedef void (APIENTRY *glPopAttrib_t)(void);
typedef void (APIENTRY *glViewport_t)(GLint x, GLint y, GLsizei width, GLsizei height);
typedef void (APIENTRY *glClearDepth_t)(GLclampd depth);
typedef void (APIENTRY *glClear_t)(GLbitfield mask);
typedef void (APIENTRY *glMatrixMode_t)(GLenum mode);
typedef void (APIENTRY *glPushMatrix_t)(void);
typedef void (APIENTRY *glPopMatrix_t)(void);
typedef void (APIENTRY *glLoadMatrixf_t)(const GLfloat *m);
typedef void (APIENTRY *glMultMatrixf_t)(const GLfloat *m);
typedef void (APIENTRY *glFrustum_t)(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar);
typedef void (APIENTRY *glEnable_t)(GLenum cap);
typedef void (APIENTRY *glDisable_t)(GLenum cap);
typedef void (APIENTRY *glDepthFunc_t)(GLenum func);
typedef void (APIENTRY *glDepthMask_t)(GLboolean flag);
typedef void (APIENTRY *glLightfv_t)(GLenum light, GLenum pname, const GLfloat *params);
typedef void (APIENTRY *glColorMaterial_t)(GLenum face, GLenum mode);
typedef void (APIENTRY *glShadeModel_t)(GLenum mode);
typedef void (APIENTRY *glCullFace_t)(GLenum mode);
typedef void (APIENTRY *glFrontFace_t)(GLenum mode);
typedef void (APIENTRY *glBegin_t)(GLenum mode);
typedef void (APIENTRY *glEnd_t)(void);
typedef void (APIENTRY *glVertex3f_t)(GLfloat x, GLfloat y, GLfloat z);
typedef void (APIENTRY *glNormal3f_t)(GLfloat nx, GLfloat ny, GLfloat nz);
typedef void (APIENTRY *glColor3f_t)(GLfloat red, GLfloat green, GLfloat blue);
typedef void (APIENTRY *glColor4f_t)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
typedef void (APIENTRY *glBlendFunc_t)(GLenum sfactor, GLenum dfactor);
typedef void (APIENTRY *glRotatef_t)(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
typedef void (APIENTRY *glTranslatef_t)(GLfloat x, GLfloat y, GLfloat z);
typedef void (APIENTRY *glGetIntegerv_t)(GLenum pname, GLint *params);
typedef void (APIENTRY *glGenTextures_t)(GLsizei n, GLuint *textures);
typedef void (APIENTRY *glBindTexture_t)(GLenum target, GLuint texture);
typedef void (APIENTRY *glTexImage2D_t)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
typedef void (APIENTRY *glTexParameteri_t)(GLenum target, GLenum pname, GLint param);
typedef void (APIENTRY *glTexEnvi_t)(GLenum target, GLenum pname, GLint param);
typedef void (APIENTRY *glTexCoord2f_t)(GLfloat s, GLfloat t);
typedef void (APIENTRY *glLightModeli_t)(GLenum pname, GLint param);

typedef GLboolean (APIENTRY *glIsTexture_t)(GLuint texture);
typedef void (APIENTRY *glGetTexLevelParameteriv_t)(GLenum target, GLint level, GLenum pname, GLint *params);
typedef void (APIENTRY *glGetTexParameteriv_t)(GLenum target, GLenum pname, GLint *params);
typedef GLenum (APIENTRY *glGetError_t)(void);
typedef void (APIENTRY *glPixelStorei_t)(GLenum pname, GLint param);
typedef void (APIENTRY *glPushClientAttrib_t)(GLbitfield mask);
typedef void (APIENTRY *glPopClientAttrib_t)(void);
typedef void (APIENTRY *glGetFloatv_t)(GLenum pname, GLfloat *params);
typedef GLboolean (APIENTRY *glIsEnabled_t)(GLenum cap);
typedef void (APIENTRY *glLoadIdentity_t)(void);
typedef void (APIENTRY *glGetTexEnviv_t)(GLenum target, GLenum pname, GLint *params);

typedef void (APIENTRY *PFNGLACTIVETEXTUREPROC)(GLenum texture);
typedef void (APIENTRY *PFNGLUSEPROGRAMPROC)(GLuint program);
typedef void (APIENTRY *PFNGLBINDBUFFERPROC)(GLenum target, GLuint buffer);

struct GLDispatch {
    glPushAttrib_t glPushAttrib = nullptr;
    glPopAttrib_t glPopAttrib = nullptr;
    glViewport_t glViewport = nullptr;
    glClearDepth_t glClearDepth = nullptr;
    glClear_t glClear = nullptr;
    glMatrixMode_t glMatrixMode = nullptr;
    glPushMatrix_t glPushMatrix = nullptr;
    glPopMatrix_t glPopMatrix = nullptr;
    glLoadMatrixf_t glLoadMatrixf = nullptr;
    glMultMatrixf_t glMultMatrixf = nullptr;
    glFrustum_t glFrustum = nullptr;
    glEnable_t glEnable = nullptr;
    glDisable_t glDisable = nullptr;
    glDepthFunc_t glDepthFunc = nullptr;
    glDepthMask_t glDepthMask = nullptr;
    glLightfv_t glLightfv = nullptr;
    glColorMaterial_t glColorMaterial = nullptr;
    glShadeModel_t glShadeModel = nullptr;
    glCullFace_t glCullFace = nullptr;
    glFrontFace_t glFrontFace = nullptr;
    glBegin_t glBegin = nullptr;
    glEnd_t glEnd = nullptr;
    glVertex3f_t glVertex3f = nullptr;
    glNormal3f_t glNormal3f = nullptr;
    glColor3f_t glColor3f = nullptr;
    glColor4f_t glColor4f = nullptr;
    glBlendFunc_t glBlendFunc = nullptr;
    glRotatef_t glRotatef = nullptr;
    glTranslatef_t glTranslatef = nullptr;
    glGetIntegerv_t glGetIntegerv = nullptr;
    glGenTextures_t glGenTextures = nullptr;
    glBindTexture_t glBindTexture = nullptr;
    glTexImage2D_t glTexImage2D = nullptr;
    glTexParameteri_t glTexParameteri = nullptr;
    glTexEnvi_t glTexEnvi = nullptr;
    glTexCoord2f_t glTexCoord2f = nullptr;
    glLightModeli_t glLightModeli = nullptr;
    glIsTexture_t glIsTexture = nullptr;
    glGetTexLevelParameteriv_t glGetTexLevelParameteriv = nullptr;
    glGetTexParameteriv_t glGetTexParameteriv = nullptr;
    glGetError_t glGetError = nullptr;
    glPixelStorei_t glPixelStorei = nullptr;
    glPushClientAttrib_t glPushClientAttrib = nullptr;
    glPopClientAttrib_t glPopClientAttrib = nullptr;
    glGetFloatv_t glGetFloatv = nullptr;
    glIsEnabled_t glIsEnabled = nullptr;
    glLoadIdentity_t glLoadIdentity = nullptr;
    glGetTexEnviv_t glGetTexEnviv = nullptr;

    PFNGLACTIVETEXTUREPROC glActiveTexture = nullptr;
    bool hasArbFragmentProgram = false;
    PFNGLUSEPROGRAMPROC glUseProgram = nullptr;
    PFNGLBINDBUFFERPROC glBindBuffer = nullptr;
    bool initialized = false;

    void Init() {
        if (initialized) return;
        HMODULE hReal = OpenGLProxy::GetRealOpenGLModule();
        if (!hReal) {
            hReal = GetModuleHandleA("opengl32.dll");
        }
        if (!hReal) return;

        #define LOAD_GL(fn) fn = (fn##_t)GetProcAddress(hReal, #fn)
        LOAD_GL(glPushAttrib);
        LOAD_GL(glPopAttrib);
        LOAD_GL(glViewport);
        LOAD_GL(glClearDepth);
        LOAD_GL(glClear);
        LOAD_GL(glMatrixMode);
        LOAD_GL(glPushMatrix);
        LOAD_GL(glPopMatrix);
        LOAD_GL(glLoadMatrixf);
        LOAD_GL(glMultMatrixf);
        LOAD_GL(glFrustum);
        LOAD_GL(glEnable);
        LOAD_GL(glDisable);
        LOAD_GL(glDepthFunc);
        LOAD_GL(glDepthMask);
        LOAD_GL(glLightfv);
        LOAD_GL(glColorMaterial);
        LOAD_GL(glShadeModel);
        LOAD_GL(glCullFace);
        LOAD_GL(glFrontFace);
        LOAD_GL(glBegin);
        LOAD_GL(glEnd);
        LOAD_GL(glVertex3f);
        LOAD_GL(glNormal3f);
        LOAD_GL(glColor3f);
        LOAD_GL(glColor4f);
        LOAD_GL(glBlendFunc);
        LOAD_GL(glRotatef);
        LOAD_GL(glTranslatef);
        LOAD_GL(glGetIntegerv);
        LOAD_GL(glGenTextures);
        LOAD_GL(glBindTexture);
        LOAD_GL(glTexImage2D);
        LOAD_GL(glTexParameteri);
        LOAD_GL(glTexEnvi);
        LOAD_GL(glTexCoord2f);
        LOAD_GL(glLightModeli);
        LOAD_GL(glIsTexture);
        LOAD_GL(glGetTexLevelParameteriv);
        LOAD_GL(glGetTexParameteriv);
        LOAD_GL(glGetError);
        LOAD_GL(glPixelStorei);
        LOAD_GL(glPushClientAttrib);
        LOAD_GL(glPopClientAttrib);
        LOAD_GL(glGetFloatv);
        LOAD_GL(glIsEnabled);
        LOAD_GL(glLoadIdentity);
        LOAD_GL(glGetTexEnviv);
        #undef LOAD_GL

        typedef PROC (WINAPI *wglGetProcAddress_t)(LPCSTR);
        wglGetProcAddress_t wglGetProc = (wglGetProcAddress_t)GetProcAddress(hReal, "wglGetProcAddress");
        if (wglGetProc) {
            glUseProgram = (PFNGLUSEPROGRAMPROC)wglGetProc("glUseProgram");
            if (!glUseProgram) glUseProgram = (PFNGLUSEPROGRAMPROC)wglGetProc("glUseProgramObjectARB");
            glBindBuffer = (PFNGLBINDBUFFERPROC)wglGetProc("glBindBuffer");
            if (!glBindBuffer) glBindBuffer = (PFNGLBINDBUFFERPROC)wglGetProc("glBindBufferARB");
            // GH #61: multitexture + ARB program entry points, used to switch
            // off engine state that would otherwise change how our hand samples.
            glActiveTexture = (PFNGLACTIVETEXTUREPROC)wglGetProc("glActiveTexture");
            if (!glActiveTexture) glActiveTexture = (PFNGLACTIVETEXTUREPROC)wglGetProc("glActiveTextureARB");
            hasArbFragmentProgram = wglGetProc("glProgramStringARB") != nullptr;
        }

        initialized = (glBegin != nullptr && glVertex3f != nullptr);
    }
};

static RiddickVRVector3 HandWorldToHmdLocal(const RiddickVRVector3& worldPos, const RiddickVRTrackedPose& hmdPose) {
    float dx = worldPos.x;
    float dy = worldPos.y;
    float dz = worldPos.z;
    if (hmdPose.isValid && (fabsf(hmdPose.position.x) > 0.001f || fabsf(hmdPose.position.y) > 0.5f || fabsf(hmdPose.position.z) > 0.001f)) {
        dx -= hmdPose.position.x;
        dy -= hmdPose.position.y;
        dz -= hmdPose.position.z;
        float qx = -hmdPose.orientation.x;
        float qy = -hmdPose.orientation.y;
        float qz = -hmdPose.orientation.z;
        float qw = hmdPose.orientation.w;
        float norm = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
        if (norm > 0.0001f) {
            qx /= norm; qy /= norm; qz /= norm; qw /= norm;
            float tx = 2.0f * (qy * dz - qz * dy);
            float ty = 2.0f * (qz * dx - qx * dz);
            float tz = 2.0f * (qx * dy - qy * dx);
            float rx = dx + qw * tx + (qy * tz - qz * ty);
            float ry = dy + qw * ty + (qz * tx - qx * tz);
            float rz = dz + qw * tz + (qx * ty - qy * tx);
            return { rx, ry, rz };
        }
    }
    return { dx, dy, dz };
}

static float HandDist(const RiddickVRVector3& a, const RiddickVRVector3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static GLDispatch s_gl;

VRHandRenderer& VRHandRenderer::Instance() {
    static VRHandRenderer s_instance;
    return s_instance;
}

void VRHandRenderer::SetHandFade(float leftFade, float rightFade) {
    m_leftFade = (std::clamp)(leftFade, 0.0f, 1.0f);
    m_rightFade = (std::clamp)(rightFade, 0.0f, 1.0f);
}

static RiddickVRVector3 Add3(const RiddickVRVector3& a, const RiddickVRVector3& b) {
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}

static RiddickVRVector3 Sub3(const RiddickVRVector3& a, const RiddickVRVector3& b) {
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

static RiddickVRVector3 Scale3(const RiddickVRVector3& v, float s) {
    return { v.x * s, v.y * s, v.z * s };
}

static RiddickVRVector3 RotateByQuaternion(const RiddickVRVector3& v, const RiddickVRQuaternion& input) {
    float x = input.x, y = input.y, z = input.z, w = input.w;
    float norm = sqrtf(x * x + y * y + z * z + w * w);
    if (norm < 0.0001f) return v;
    x /= norm; y /= norm; z /= norm; w /= norm;
    RiddickVRVector3 t = { 2.0f * (y * v.z - z * v.y),
                           2.0f * (z * v.x - x * v.z),
                           2.0f * (x * v.y - y * v.x) };
    return { v.x + w * t.x + (y * t.z - z * t.y),
             v.y + w * t.y + (z * t.x - x * t.z),
             v.z + w * t.z + (x * t.y - y * t.x) };
}

void VRHandRenderer::DrawArmSegment(const RiddickVRVector3& start, const RiddickVRVector3& end,
        float startRadius, float endRadius, float r, float g, float b) {
    RiddickVRVector3 dir = Sub3(end, start);
    float length = HandDist(start, end);
    if (length < 0.01f || !s_gl.glTranslatef || !s_gl.glRotatef) return;
    dir = Scale3(dir, 1.0f / length);
    float angle = acosf((std::clamp)(dir.z, -1.0f, 1.0f)) * 57.2957795f;
    float axisX = -dir.y;
    float axisY = dir.x;
    float axisLength = sqrtf(axisX * axisX + axisY * axisY);
    if (axisLength < 0.0001f) {
        axisX = 1.0f; axisY = 0.0f;
    } else {
        axisX /= axisLength; axisY /= axisLength;
    }
    s_gl.glPushMatrix();
    s_gl.glTranslatef(start.x, start.y, start.z);
    if (angle > 0.01f) s_gl.glRotatef(angle, axisX, axisY, 0.0f);
    DrawTaperedCylinder(0.0f, startRadius, length, endRadius, 12, r, g, b);
    s_gl.glPopMatrix();
}

void VRHandRenderer::SetLadderCue(float leftCue, float rightCue) {
    m_leftLadderCue = (std::clamp)(leftCue, 0.0f, 1.0f);
    m_rightLadderCue = (std::clamp)(rightCue, 0.0f, 1.0f);
}

void VRHandRenderer::RenderHands(const RiddickVRSharedState& state, bool isLeftEye, uint32_t eyeWidth, uint32_t eyeHeight) {
    const auto& leftCtrl = state.leftController;
    const auto& rightCtrl = state.rightController;

    if (!leftCtrl.gripPose.isValid && !rightCtrl.gripPose.isValid) {
        return; // No valid motion controller tracking data available
    }

    s_gl.Init();
    if (!s_gl.initialized) return;

    m_currentFrameIndex = state.frameIndex;

    // GH #61: log the engine's GL state once, before we change any of it, so a
    // hand that still reads white can be diagnosed from the log alone.
    LogNativeHandGlStateOnce();

    // 1. Push complete OpenGL pipeline attributes to preserve game render state
    // GH #61: GL_TEXTURE_BIT also saves/restores the 2D texture binding and the
    // texture-env mode the native hand texture draw mutates.
    s_gl.glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT |
                      GL_LIGHTING_BIT | GL_POLYGON_BIT | GL_VIEWPORT_BIT | GL_CURRENT_BIT |
                      GL_TEXTURE_BIT);

    // GH #22: save every extra piece of state we mutate below (program,
    // buffer bindings, active matrix mode) so the game render state is
    // bit-identical after RenderHands returns.
    GLint prevMatrixMode = GL_MODELVIEW;
    if (s_gl.glGetIntegerv) {
        s_gl.glGetIntegerv(0x0BA0 /* GL_MATRIX_MODE */, &prevMatrixMode);
    }
    // Save previous GLSL program and unbind to allow clean fixed-function tactical glove rendering
    GLint prevProgram = 0;
    if (s_gl.glGetIntegerv) {
        s_gl.glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &prevProgram);
    }
    GLint prevArrayBuffer = 0;
    GLint prevElementArrayBuffer = 0;
    if (s_gl.glGetIntegerv) {
        s_gl.glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &prevArrayBuffer);
        s_gl.glGetIntegerv(0x8895 /* GL_ELEMENT_ARRAY_BUFFER_BINDING */, &prevElementArrayBuffer);
    }
    if (s_gl.glUseProgram) {
        s_gl.glUseProgram(0);
    }
    if (s_gl.glBindBuffer) {
        s_gl.glBindBuffer(0x8892 /* GL_ARRAY_BUFFER */, 0);
        s_gl.glBindBuffer(0x8893 /* GL_ELEMENT_ARRAY_BUFFER */, 0);
    }

    // 2. Set viewport to match the active eye framebuffer
    s_gl.glViewport(0, 0, eyeWidth, eyeHeight);

    // 3. Clear depth buffer for the hand overlay pass (ensures hands never clip through distant geometry)
    s_gl.glClearDepth(1.0f);
    s_gl.glClear(GL_DEPTH_BUFFER_BIT);

    // GH #61: the engine may leave a non-identity texture matrix; force identity
    // for our pass so the baked UVs sample where they are meant to.
    const bool texMatrixPushed = (s_gl.glLoadIdentity != nullptr);
    if (texMatrixPushed) {
        s_gl.glMatrixMode(GL_TEXTURE);
        s_gl.glPushMatrix();
        s_gl.glLoadIdentity();
    }

    // 4. Configure stereoscopic projection matrix
    s_gl.glMatrixMode(GL_PROJECTION);
    s_gl.glPushMatrix();

    RiddickVRMatrix4x4 projMat = isLeftEye ? state.leftEyeProjection : state.rightEyeProjection;
    if (projMat.m[0] > 0.05f) {
        // Invert Y focal length to match the vertically-flipped FBO orientation (ADR 0405)
        projMat.m[5] = -projMat.m[5];
        s_gl.glLoadMatrixf(projMat.m);
    } else {
        // Fallback perspective matrix (90 deg horizontal FOV, near 0.05m, far 50.0m)
        float fovRad = 1.5707963f; // 90 deg
        float aspect = static_cast<float>(eyeWidth) / static_cast<float>(eyeHeight);
        float nearZ = 0.05f;
        float farZ = 50.0f;
        float top = nearZ * tanf(fovRad * 0.5f);
        float bottom = -top;
        float right = top * aspect;
        float left = -right;
        // Invert top and bottom for FBO top-down alignment
        s_gl.glFrustum(left, right, top, bottom, nearZ, farZ);
    }

    // 5. Configure view & lighting state
    s_gl.glMatrixMode(GL_MODELVIEW);
    s_gl.glPushMatrix();

    const RiddickVRMatrix4x4& viewMat = isLeftEye ? state.leftEyeView : state.rightEyeView;
    s_gl.glLoadMatrixf(viewMat.m);

    // Enable high-fidelity depth testing
    s_gl.glEnable(GL_DEPTH_TEST);
    s_gl.glDepthFunc(GL_LEQUAL);
    s_gl.glDepthMask(GL_TRUE);

    // Tactical directional lighting
    s_gl.glEnable(GL_LIGHTING);
    s_gl.glEnable(GL_LIGHT0);
    GLfloat lightPos[] = { 0.25f, 1.20f, 0.40f, 0.0f }; // Overhead-forward key light
    GLfloat lightDiffuse[] = { 0.90f, 0.90f, 0.92f, 1.0f };
    GLfloat lightAmbient[] = { 0.32f, 0.32f, 0.35f, 1.0f };
    GLfloat lightSpecular[] = { 0.45f, 0.45f, 0.50f, 1.0f };
    s_gl.glLightfv(GL_LIGHT0, GL_POSITION, lightPos);
    s_gl.glLightfv(GL_LIGHT0, GL_DIFFUSE, lightDiffuse);
    s_gl.glLightfv(GL_LIGHT0, GL_AMBIENT, lightAmbient);
    s_gl.glLightfv(GL_LIGHT0, GL_SPECULAR, lightSpecular);

    s_gl.glEnable(GL_COLOR_MATERIAL);
    s_gl.glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    s_gl.glShadeModel(GL_SMOOTH);
    s_gl.glEnable(0x0BA7 /* GL_NORMALIZE */);

    // Enable backface culling (CW front face due to inverted Y projection)
    s_gl.glEnable(GL_CULL_FACE);
    s_gl.glFrontFace(GL_CW);
    s_gl.glCullFace(GL_BACK);
    m_useAlphaFade = (s_gl.glColor4f && s_gl.glBlendFunc);
    if (m_useAlphaFade) {
        s_gl.glEnable(GL_BLEND);
        s_gl.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        s_gl.glDisable(GL_BLEND);
    }
    s_gl.glDisable(GL_TEXTURE_2D);
    SanitizeTextureStateForHands();

    // Check if Boxer Guard is active (both fists clenched and raised)
    bool isBoxerGuard = (leftCtrl.grip > 0.45f && rightCtrl.grip > 0.45f &&
                         fabsf(leftCtrl.gripPose.position.x - rightCtrl.gripPose.position.x) < 0.32f);

    auto ApplyControllerPose = [](const RiddickVRTrackedPose& pose) {
        float qx = pose.orientation.x;
        float qy = pose.orientation.y;
        float qz = pose.orientation.z;
        float qw = pose.orientation.w;

        float r00 = 1.0f - 2.0f * (qy * qy + qz * qz);
        float r01 = 2.0f * (qx * qy - qz * qw);
        float r02 = 2.0f * (qx * qz + qy * qw);

        float r10 = 2.0f * (qx * qy + qz * qw);
        float r11 = 1.0f - 2.0f * (qx * qx + qz * qz);
        float r12 = 2.0f * (qy * qz - qx * qw);

        float r20 = 2.0f * (qx * qz - qy * qw);
        float r21 = 2.0f * (qy * qz + qx * qw);
        float r22 = 1.0f - 2.0f * (qx * qx + qy * qy);

        GLfloat m[16];
        m[0] = r00; m[1] = r10; m[2] = r20; m[3] = 0.0f;
        m[4] = r01; m[5] = r11; m[6] = r21; m[7] = 0.0f;
        m[8] = r02; m[9] = r12; m[10] = r22; m[11] = 0.0f;
        m[12] = pose.position.x;
        m[13] = pose.position.y;
        m[14] = pose.position.z;
        m[15] = 1.0f;

        s_gl.glMultMatrixf(m);
    };

    // 6. Render Left 3D Tactical Glove (with reload / NanoMED / heal interaction visuals)
    // HMD-local interaction volumes mirror camera_hook gesture zones so visuals match actions.
    RiddickVRVector3 leftLocal = HandWorldToHmdLocal(leftCtrl.gripPose.position, state.hmdPose);
    RiddickVRVector3 rightLocal = HandWorldToHmdLocal(rightCtrl.gripPose.position, state.hmdPose);

    // Solve elbow targets once per host frame and reuse them for both eye
    // passes. Shoulder anchors follow the HMD; controller grip poses are the
    // wrist targets. The pole vector is HMD-local and points outward/downward,
    // which stabilizes the elbow plane during cross-body and overhead reaches.
    if (state.frameIndex != m_lastIkFrame) {
        m_lastIkFrame = state.frameIndex;
        if (state.hmdPose.isValid) {
            auto updateElbow = [&](const RiddickVRControllerInput& controller, bool isLeft,
                                   RiddickVRVector3& cached, bool& haveCached) {
                if (!controller.gripPose.isValid) {
                    haveCached = false;
                    return;
                }
                float side = isLeft ? -1.0f : 1.0f;
                RiddickVRVector3 shoulderOffset = RotateByQuaternion({ side * 0.19f, -0.22f, 0.055f }, state.hmdPose.orientation);
                RiddickVRVector3 pole = RotateByQuaternion({ side * 0.80f, -0.35f, 0.12f }, state.hmdPose.orientation);
                RiddickVRVector3 shoulder = Add3(state.hmdPose.position, shoulderOffset);
                auto solved = VRArmIK::SolveArm(shoulder, controller.gripPose.position, pole, 0.36f, 0.34f);
                if (haveCached) {
                    constexpr float IK_SMOOTH_ALPHA = 0.62f;
                    cached = Add3(Scale3(cached, 1.0f - IK_SMOOTH_ALPHA), Scale3(solved.elbow, IK_SMOOTH_ALPHA));
                } else {
                    cached = solved.elbow;
                    haveCached = true;
                }
                if (isLeft) m_leftArmWrist = solved.wrist;
                else m_rightArmWrist = solved.wrist;
            };
            updateElbow(leftCtrl, true, m_leftElbow, m_hasLeftElbow);
            updateElbow(rightCtrl, false, m_rightElbow, m_hasRightElbow);
        } else {
            m_hasLeftElbow = false;
            m_hasRightElbow = false;
        }
    }

    const float armScale = GetWorldScale();
    auto drawArm = [&](const RiddickVRControllerInput& controller, bool isLeft,
                       const RiddickVRVector3& elbow, const RiddickVRVector3& wristTarget, bool haveElbow) {
        if (!state.hmdPose.isValid || !controller.gripPose.isValid || !haveElbow) return;
        float side = isLeft ? -1.0f : 1.0f;
        RiddickVRVector3 shoulder = Add3(state.hmdPose.position,
            RotateByQuaternion({ side * 0.19f, -0.22f, 0.055f }, state.hmdPose.orientation));
        m_currentFade = isLeft ? m_leftFade : m_rightFade;
        // Prisoner uniform cloth, tapering at elbow and wrist. Hand geometry
        // continues from the same tracked wrist target.
        // GH #64: the native mesh is placed from its own bounds, so the IK
        // upper-arm tube does not join it and reads as a stray block. Draw it
        // only for the procedural arm, or when NativeHandUpperArm opts in.
        if (!g_vrConfig.nativeHands || g_vrConfig.nativeHandUpperArm) {
            DrawArmSegment(shoulder, elbow, 0.050f * armScale, 0.040f * armScale, 0.45f, 0.43f, 0.37f);
        }
        // GH #46: when the native arm mesh is enabled it carries the forearm,
        // so only the upper-arm cloth is drawn procedurally (no doubling).
        if (!g_vrConfig.nativeHands) {
            DrawArmSegment(elbow, wristTarget, 0.040f * armScale, 0.028f * armScale, 0.46f, 0.44f, 0.38f);
        }
        m_currentFade = 1.0f;
    };
    drawArm(leftCtrl, true, m_leftElbow, m_leftArmWrist, m_hasLeftElbow);
    drawArm(rightCtrl, false, m_rightElbow, m_rightArmWrist, m_hasRightElbow);

    float reloadDist = HandDist(leftLocal, rightLocal);
    bool nearReceiver = (reloadDist < 0.20f);
    auto InScanner = [](const RiddickVRVector3& p) {
        return (p.z >= -0.65f && p.z <= -0.20f) && (p.y >= -0.25f && p.y <= 0.42f) && (fabsf(p.x) < 0.30f);
    };
    auto PalmOpen = [](const RiddickVRControllerInput& c) {
        return (c.trigger < 0.20f && c.grip < 0.25f);
    };
    bool leftWaist = (leftLocal.y < -0.38f);
    bool rightWaist = (rightLocal.y < -0.38f);
    // Visual latches (renderer-local, no protocol change): waist grip implies cartridge in hand.
    bool leftHolding = leftWaist && (leftCtrl.grip > 0.60f);
    bool rightHolding = rightWaist && (rightCtrl.grip > 0.60f);
    bool leftHeal = PalmOpen(leftCtrl) && InScanner(leftLocal);
    bool rightHeal = PalmOpen(rightCtrl) && InScanner(rightLocal);
    float pulse = 0.5f + 0.5f * sinf(static_cast<float>(GetTickCount()) * 0.008f);
    float leftGlow = nearReceiver ? (0.4f + 0.6f * pulse) : 0.0f;
    float rightGlow = nearReceiver ? (0.4f + 0.6f * pulse) : 0.0f;
    // GH #46: fade our overlay where the engine's own gun/hands dominate the
    // view (firing grip, ADS pose, two-handed hold) so we don't double-render
    // over the game's arms. Full opacity for open/interacting hands.
    bool twoHanded = (leftCtrl.grip > 0.70f && rightCtrl.grip > 0.70f);
    // GH #46: a boxer guard (both fists up in front of the face, no trigger
    // pulled) also has both grips closed; it must not fade the hands out.
    GuardEnvelope guardVolume;
    guardVolume.yMin = g_vrConfig.guardYMin;
    guardVolume.yMax = g_vrConfig.guardYMax;
    guardVolume.zMin = g_vrConfig.guardZMin;
    guardVolume.zMax = g_vrConfig.guardZMax;
    guardVolume.maxHandSpan = g_vrConfig.guardMaxHandSpan;
    const bool fadeBoxerGuard = twoHanded && leftCtrl.trigger <= 0.60f && rightCtrl.trigger <= 0.60f &&
        InGuardEnvelope(guardVolume, leftLocal.y, leftLocal.z, rightLocal.y, rightLocal.z,
                        leftLocal.x - rightLocal.x);
    const int fadeEnabled = g_vrConfig.handFadeWhenWeapon;
    float leftFade = ComputeHandFade(leftCtrl.trigger, twoHanded, fadeBoxerGuard, fadeEnabled);
    float rightFade = ComputeHandFade(rightCtrl.trigger, twoHanded, fadeBoxerGuard, fadeEnabled);
    VRHandRenderer::Instance().SetHandFade(leftFade, rightFade);
    // Grab proximity (vents, biometric terminals, guard throat-grip corridor):
    // hand forward of chest with grip closed (grab) or index extended (poke).
    auto GrabGlowFor = [](const RiddickVRVector3& local, const RiddickVRControllerInput& c) {
        bool inCorridor = (local.z >= -0.45f && local.z <= -0.15f) && (fabsf(local.x) < 0.30f);
        bool gripping = (c.grip > 0.50f);
        bool poking = (c.trigger < 0.20f && c.grip < 0.35f);
        if (!inCorridor || (!gripping && !poking)) return 0.0f;
        float depth = 1.0f - (local.z + 0.45f) / 0.30f; // 0 at entry -> 1 deep inside
        if (depth < 0.0f) depth = 0.0f;
        if (depth > 1.0f) depth = 1.0f;
        return 0.35f + 0.65f * depth;
    };
    float leftGrab = (std::max)(GrabGlowFor(leftLocal, leftCtrl), m_leftLadderCue);
    float rightGrab = (std::max)(GrabGlowFor(rightLocal, rightCtrl), m_rightLadderCue);
    if (leftCtrl.gripPose.isValid) {
        RiddickVRTrackedPose handPose = leftCtrl.gripPose;
        if (state.hmdPose.isValid && m_hasLeftElbow) {
            RiddickVRVector3 pole = RotateByQuaternion({ -0.80f, -0.35f, 0.12f }, state.hmdPose.orientation);
            handPose.orientation = ClampWristOrientation(handPose.orientation,
                handPose.position, m_leftElbow, pole);
        }
        s_gl.glPushMatrix();
        ApplyControllerPose(handPose);
        DrawTacticalHand(HandSide::Left, leftCtrl.grip, leftCtrl.trigger, isBoxerGuard, leftCtrl.buttons,
                         leftGlow, leftHolding, leftHeal, leftGrab);
        s_gl.glPopMatrix();
    }

    // 7. Render Right 3D Tactical Glove
    if (rightCtrl.gripPose.isValid) {
        RiddickVRTrackedPose handPose = rightCtrl.gripPose;
        if (state.hmdPose.isValid && m_hasRightElbow) {
            RiddickVRVector3 pole = RotateByQuaternion({ 0.80f, -0.35f, 0.12f }, state.hmdPose.orientation);
            handPose.orientation = ClampWristOrientation(handPose.orientation,
                handPose.position, m_rightElbow, pole);
        }
        s_gl.glPushMatrix();
        ApplyControllerPose(handPose);
        DrawTacticalHand(HandSide::Right, rightCtrl.grip, rightCtrl.trigger, isBoxerGuard, rightCtrl.buttons,
                         rightGlow, rightHolding, rightHeal, rightGrab);
        s_gl.glPopMatrix();
    }

    // 8. Restore matrices and OpenGL state (GH #22: explicit restores for
    // everything RenderHands mutated; glPopAttrib alone does not cover
    // program/binding/matrix-mode state).
    s_gl.glMatrixMode(GL_MODELVIEW);
    s_gl.glPopMatrix();

    s_gl.glMatrixMode(GL_PROJECTION);
    s_gl.glPopMatrix();

    if (texMatrixPushed) {
        s_gl.glMatrixMode(GL_TEXTURE);
        s_gl.glPopMatrix();
    }
    if (s_gl.glActiveTexture) {
        s_gl.glActiveTexture((GLenum)m_prevActiveUnit);
    }

    // Explicitly undo enable/disable changes (mirrors section 5 setup).
    s_gl.glDisable(GL_LIGHT0);
    s_gl.glDisable(GL_LIGHTING);
    s_gl.glDisable(GL_COLOR_MATERIAL);
    s_gl.glDisable(0x0BA7 /* GL_NORMALIZE */);
    s_gl.glDisable(GL_CULL_FACE);

    if (s_gl.glUseProgram) {
        s_gl.glUseProgram((GLuint)prevProgram);
    }
    if (s_gl.glBindBuffer) {
        s_gl.glBindBuffer(0x8892 /* GL_ARRAY_BUFFER */, (GLuint)prevArrayBuffer);
        s_gl.glBindBuffer(0x8893 /* GL_ELEMENT_ARRAY_BUFFER */, (GLuint)prevElementArrayBuffer);
    }
    s_gl.glMatrixMode(prevMatrixMode);

    s_gl.glPopAttrib();
    m_useAlphaFade = false;
}

void VRHandRenderer::DrawTacticalHand(HandSide side, float grip, float trigger, bool isBoxerGuard, uint32_t buttons,
                                      float interactionGlow, bool holdingCartridge, bool palmHeal, float grabGlow) {
    s_gl.glPushMatrix();

    // GH #46: select per-hand fade for this hand; all primitives multiply by it.
    m_currentFade = (side == HandSide::Left) ? m_leftFade : m_rightFade;

    // Squeeze factor: grip trigger curls middle, ring, pinky
    float effectiveGrip = (std::max)(grip, isBoxerGuard ? 0.90f : 0.0f);
    float effectiveTrigger = (std::max)(trigger, isBoxerGuard ? 0.90f : 0.0f);

    // Capacitive sensing and trigger discipline (ADR 0411):
    // If boxer guard is active, both hands clench into complete fists
    // Otherwise, index finger respects capacitive touch & trigger pull (trigger discipline)
    bool triggerTouch = (buttons & RIDDICK_VR_TOUCH_TRIGGER) != 0 || trigger > 0.05f;
    bool thumbTouch = (buttons & (RIDDICK_VR_TOUCH_THUMBSTICK | RIDDICK_VR_TOUCH_THUMBREST |
                                 RIDDICK_VR_BTN_A | RIDDICK_VR_BTN_B | RIDDICK_VR_BTN_X | RIDDICK_VR_BTN_Y)) != 0;

    float indexCurl = 0.0f;
    if (isBoxerGuard) {
        indexCurl = 0.92f;
    } else if (effectiveGrip > 0.70f && trigger > 0.40f) {
        // Tight dual clench creates a full fist
        indexCurl = (std::max)(effectiveTrigger, effectiveGrip);
    } else if (triggerTouch) {
        // Finger resting on trigger guard or pulling trigger
        indexCurl = 0.15f + 0.80f * effectiveTrigger;
    } else {
        // Extended pointing / trigger discipline along barrel
        indexCurl = 0.0f;
    }

    float thumbCurl = 0.0f;
    if (isBoxerGuard) {
        thumbCurl = 0.88f;
    } else if (thumbTouch) {
        thumbCurl = 0.35f + 0.50f * effectiveGrip;
    } else {
        thumbCurl = 0.08f;
    }

    // 1. Forearm Gauntlet & Wrist Vambrace
    // GH #46/#64: the baked native arm mesh (when enabled) replaces the
    // procedural gauntlet/palm/digits *and* the hand-local interaction boxes,
    // which are authored for the procedural geometry and otherwise float as
    // stray blocks (GH #64). `native` reflects an actual native draw (flag on,
    // mesh + placement valid); anything else keeps the full procedural hand.
    // GH #64: interaction cues show as a skin tint on the native mesh.
    const HandCueTint cue = ComputeNativeHandCueTint(interactionGlow, holdingCartridge, palmHeal, grabGlow);
    const bool native = DrawNativeHand(side, cue);
    if (!native) {
        DrawForearmGauntlet(side);

        // 2. Palm & Armored Knuckle Guard
        DrawPalmAndKnuckleGuard(side);

        // 3. Articulated Digits (Fingers)
        // Splay angles in degrees: Index outward, Middle straight, Ring inward, Pinky inward
        float splayIndex  = (side == HandSide::Left) ? -4.5f :  4.5f;
        float splayMiddle = 0.0f;
        float splayRing   = (side == HandSide::Left) ?  3.0f : -3.0f;
        float splayPinky  = (side == HandSide::Left) ?  6.5f : -6.5f;

        // Track C #13: world scale multiplier on hand render scale (INI WorldScale).
        const float handScale = GetWorldScale();
        DrawArticulatedFinger(side, 0, indexCurl,     splayIndex,  0.96f * handScale); // Index
        DrawArticulatedFinger(side, 1, effectiveGrip, splayMiddle, 1.00f * handScale); // Middle
        DrawArticulatedFinger(side, 2, effectiveGrip, splayRing,   0.93f * handScale); // Ring
        DrawArticulatedFinger(side, 3, effectiveGrip, splayPinky,  0.80f * handScale); // Pinky

        // 4. Articulated Thumb
        DrawArticulatedThumb(side, thumbCurl);
    }

    // 5. Interaction visuals: reload proximity glow, NanoMED cartridge, heal glow.
    // Fixed-function only, no new state leaks (restored via PopAttrib in RenderHands).
    // GH #46: warm diegetic palette - amber receiver, warm-orange NanoMED,
    // soft green scanner, warm-shifted cyan grab (all harmonized, fade-scaled).
    // GH #64: only with the procedural hand -- these boxes sit at procedural
    // hand-local coordinates and read as stray floating blocks on the native
    // mesh. Re-anchoring them to the native frame is future work.
    if (!native) {
        if (interactionGlow > 0.01f) {
            float g = interactionGlow;
            // Warm amber receiver-proximity highlight above knuckles.
            DrawBeveledBox(-0.023f, 0.024f, -0.052f, 0.023f, 0.030f, -0.028f,
                           0.30f + 0.60f * g, 0.24f + 0.38f * g, 0.16f + 0.06f * g);
        }
        if (holdingCartridge) {
            // Warm NanoMED cartridge block held in palm (waist draw visual).
            DrawBeveledBox(-0.014f, -0.009f, -0.030f, 0.014f, 0.009f, 0.010f,
                           0.82f, 0.48f, 0.14f);
            DrawBeveledBox(-0.014f, 0.009f, -0.030f, 0.014f, 0.014f, 0.010f,
                           0.95f, 0.68f, 0.22f);
        }
        if (palmHeal) {
            // Soft warm green scanner glow under palm.
            DrawBeveledBox(-0.021f, -0.017f, -0.036f, 0.021f, -0.011f, 0.028f,
                           0.18f, 0.72f, 0.34f);
        }
        if (grabGlow > 0.01f) {
            // Warm-shifted cyan grab highlight on fingertips region (object/person volumes).
            float g = grabGlow;
            DrawBeveledBox(-0.023f, -0.005f, -0.062f, 0.023f, 0.020f, -0.040f,
                           0.22f + 0.24f * g, 0.60f + 0.24f * g, 0.62f + 0.22f * g);
        }
    }

    m_currentFade = 1.0f;
    s_gl.glPopMatrix();
}

// GH #61: log the engine's GL state the first time hands are drawn, before
// RenderHands changes anything. One line is enough to tell whether another
// texture unit/target, a texture matrix or an ARB program was left active.
void VRHandRenderer::LogNativeHandGlStateOnce() {
    if (m_glStateLogged) return;
    m_glStateLogged = true;

    GLint activeUnit = 0;
    GLint maxUnits = 0;
    if (s_gl.glGetIntegerv) {
        s_gl.glGetIntegerv(0x84E0 /* GL_ACTIVE_TEXTURE */, &activeUnit);
        s_gl.glGetIntegerv(0x84E2 /* GL_MAX_TEXTURE_UNITS */, &maxUnits);
    }
    const int cube = s_gl.glIsEnabled ? (s_gl.glIsEnabled(0x8513 /* GL_TEXTURE_CUBE_MAP */) ? 1 : 0) : -1;
    const int tex3d = s_gl.glIsEnabled ? (s_gl.glIsEnabled(0x806F /* GL_TEXTURE_3D */) ? 1 : 0) : -1;
    const int fp = s_gl.glIsEnabled ? (s_gl.glIsEnabled(0x8804 /* GL_FRAGMENT_PROGRAM_ARB */) ? 1 : 0) : -1;
    int texMatIdentity = -1;
    if (s_gl.glGetFloatv) {
        GLfloat tm[16] = {};
        s_gl.glGetFloatv(0x0BA8 /* GL_TEXTURE_MATRIX */, tm);
        const GLfloat identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        texMatIdentity = 1;
        for (int i = 0; i < 16; ++i) {
            if (fabsf(tm[i] - identity[i]) > 1e-4f) { texMatIdentity = 0; break; }
        }
    }
    GLint envMode = 0;
    if (s_gl.glGetTexEnviv) {
        s_gl.glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &envMode);
    }
    VR_LOG_INFO("NativeHand GL state: activeUnit=%d maxUnits=%d cube=%d tex3d=%d fp=%d texMatIdentity=%d envMode=0x%X",
                (int)(activeUnit - 0x84C0), (int)maxUnits, cube, tex3d, fp, texMatIdentity, (unsigned)envMode);
}

// GH #61: switch off every texture state the engine might have left on that
// would change how the hand's GL_TEXTURE_2D sampling resolves. Called with
// GL_ENABLE_BIT | GL_TEXTURE_BIT pushed, so glPopAttrib restores all of it.
void VRHandRenderer::SanitizeTextureStateForHands() {
    m_prevActiveUnit = 0x84C0;
    if (s_gl.glActiveTexture) {
        GLint prevUnit = 0x84C0;
        GLint maxUnits = 0;
        if (s_gl.glGetIntegerv) {
            s_gl.glGetIntegerv(0x84E0 /* GL_ACTIVE_TEXTURE */, &prevUnit);
            s_gl.glGetIntegerv(0x84E2 /* GL_MAX_TEXTURE_UNITS */, &maxUnits);
        }
        if (prevUnit >= 0x84C0 && prevUnit < 0x84C0 + 32) m_prevActiveUnit = prevUnit;
        if (maxUnits > 8) maxUnits = 8;
        for (GLint u = 1; u < maxUnits; ++u) {
            s_gl.glActiveTexture((GLenum)(0x84C0 + u));
            s_gl.glDisable(GL_TEXTURE_1D);
            s_gl.glDisable(GL_TEXTURE_2D);
            s_gl.glDisable(0x806F /* GL_TEXTURE_3D */);
            s_gl.glDisable(0x8513 /* GL_TEXTURE_CUBE_MAP */);
            s_gl.glDisable(0x84F5 /* GL_TEXTURE_RECTANGLE */);
        }
        s_gl.glActiveTexture(0x84C0 /* GL_TEXTURE0 */);
    }
    // Unit 0: only our 2D texture may be on, and no coordinate generation.
    s_gl.glDisable(GL_TEXTURE_1D);
    s_gl.glDisable(0x806F /* GL_TEXTURE_3D */);
    s_gl.glDisable(0x8513 /* GL_TEXTURE_CUBE_MAP */);
    s_gl.glDisable(0x84F5 /* GL_TEXTURE_RECTANGLE */);
    s_gl.glDisable(GL_TEXTURE_GEN_S);
    s_gl.glDisable(GL_TEXTURE_GEN_T);
    s_gl.glDisable(GL_TEXTURE_GEN_R);
    s_gl.glDisable(GL_TEXTURE_GEN_Q);
    if (s_gl.hasArbFragmentProgram) {
        s_gl.glDisable(0x8804 /* GL_FRAGMENT_PROGRAM_ARB */);
        s_gl.glDisable(0x8620 /* GL_VERTEX_PROGRAM_ARB */);
    }
}

// GH #61: decode the baked DXT1 diffuse and upload it into a freshly generated
// texture id with the unpack state forced to known values (the engine may leave
// odd alignment/row-length/skip, a bound pixel-unpack buffer or a clipped mip
// range behind). Returns 0 on failure.
GLuint VRHandRenderer::UploadNativeHandTexture() {
    const NativeHandTexture* tex = FindNativeHandTexture("Riddick_ArmScar_D");
    if (!tex || !tex->dxt1 || tex->width == 0 || tex->height == 0) {
        return 0;
    }
    if (tex->dxt1_bytes < Bc1SurfaceBytes(tex->width, tex->height)) {
        return 0;
    }

    std::vector<unsigned char> rgba(
        static_cast<size_t>(tex->width) * tex->height * 4u);
    Bc1DecodeSurface(tex->dxt1, tex->width, tex->height, rgba.data());

    if (s_gl.glGetError) {
        for (int i = 0; i < 16 && s_gl.glGetError() != 0; ++i) {} // drain engine errors
    }

    GLuint id = 0;
    s_gl.glGenTextures(1, &id);
    if (id == 0) {
        return 0;
    }

    const bool clientAttrib = s_gl.glPushClientAttrib && s_gl.glPopClientAttrib && s_gl.glPixelStorei;
    if (clientAttrib) {
        s_gl.glPushClientAttrib(0x1 /* GL_CLIENT_PIXEL_STORE_BIT */);
        s_gl.glPixelStorei(0x0CF5 /* GL_UNPACK_ALIGNMENT */, 4);
        s_gl.glPixelStorei(0x0CF2 /* GL_UNPACK_ROW_LENGTH */, 0);
        s_gl.glPixelStorei(0x0CF3 /* GL_UNPACK_SKIP_ROWS */, 0);
        s_gl.glPixelStorei(0x0CF4 /* GL_UNPACK_SKIP_PIXELS */, 0);
        s_gl.glPixelStorei(0x0CF0 /* GL_UNPACK_SWAP_BYTES */, 0);
        s_gl.glPixelStorei(0x0CF1 /* GL_UNPACK_LSB_FIRST */, 0);
    }
    GLint prevUnpackBuffer = 0;
    const bool unbindPbo = (s_gl.glBindBuffer != nullptr && s_gl.glGetIntegerv != nullptr);
    if (unbindPbo) {
        s_gl.glGetIntegerv(0x88EF /* GL_PIXEL_UNPACK_BUFFER_BINDING */, &prevUnpackBuffer);
        s_gl.glBindBuffer(0x88EC /* GL_PIXEL_UNPACK_BUFFER */, 0);
    }

    s_gl.glBindTexture(GL_TEXTURE_2D, id);
    SetNativeHandTextureParams();
    s_gl.glTexParameteri(GL_TEXTURE_2D, 0x813C /* GL_TEXTURE_BASE_LEVEL */, 0);
    s_gl.glTexParameteri(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */, 0);
    s_gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)tex->width,
                      (GLsizei)tex->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                      rgba.data());

    if (unbindPbo) {
        s_gl.glBindBuffer(0x88EC /* GL_PIXEL_UNPACK_BUFFER */, (GLuint)prevUnpackBuffer);
    }
    if (clientAttrib) {
        s_gl.glPopClientAttrib();
    }

    if (s_gl.glGetError) {
        const GLenum err = s_gl.glGetError();
        if (err != 0) {
            VR_LOG_WARN("native hand texture: glTexImage2D raised GL error 0x%X", (unsigned)err);
        }
    }
    VR_LOG_INFO("native hand texture: %s mip %d %ux%u (%u DXT1 bytes) uploaded",
                tex->name, tex->mip, tex->width, tex->height, tex->dxt1_bytes);
    return id;
}

void VRHandRenderer::SetNativeHandTextureParams() {
    s_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    s_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    s_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    s_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

// GH #61: create the GL texture for the baked Riddick skin diffuse, and once
// per host frame verify it is still ours.
//
// The engine shares this GL context and can reuse or overwrite the id we
// generated, which leaves an incomplete texture bound (samples as white). The
// check asks the driver for the object's size and min filter and re-uploads
// into a fresh id (never deleting the old one: the engine may own it now) or
// re-sets the sampling parameters. Returns false (permanently, until the
// process restarts) when the flag is off, the asset is missing, or the GL entry
// points are unavailable, so DrawNativeHand keeps its flat skin-tone fallback.
bool VRHandRenderer::EnsureNativeHandTexture() {
    if (m_nativeTextureTried) {
        if (m_nativeTextureReady) {
            VerifyNativeHandTexture();
        }
        return m_nativeTextureReady;
    }
    m_nativeTextureTried = true;

    if (!g_vrConfig.nativeHandTexture) {
        return false;
    }
    if (!s_gl.glGenTextures || !s_gl.glBindTexture || !s_gl.glTexImage2D ||
        !s_gl.glTexParameteri) {
        return false;
    }

    const GLuint id = UploadNativeHandTexture();
    if (id == 0) {
        return false;
    }
    m_nativeHandTexture = id;
    m_nativeTextureReady = true;
    m_lastTexCheckFrame = m_currentFrameIndex;
    return true;
}

void VRHandRenderer::VerifyNativeHandTexture() {
    if (m_currentFrameIndex == m_lastTexCheckFrame) return;
    m_lastTexCheckFrame = m_currentFrameIndex;
    if (!s_gl.glIsTexture || !s_gl.glGetTexLevelParameteriv || !s_gl.glGetTexParameteriv) {
        return;
    }
    const NativeHandTexture* tex = FindNativeHandTexture("Riddick_ArmScar_D");
    if (!tex) return;

    const bool isTexture = s_gl.glIsTexture(m_nativeHandTexture) != 0;
    GLint w = 0, h = 0, minFilter = 0;
    if (isTexture) {
        s_gl.glBindTexture(GL_TEXTURE_2D, m_nativeHandTexture);
        s_gl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, 0x1000 /* GL_TEXTURE_WIDTH */, &w);
        s_gl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, 0x1001 /* GL_TEXTURE_HEIGHT */, &h);
        s_gl.glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
    }
    const NativeTextureCheck verdict = CheckNativeTextureIntegrity(
        isTexture, w, h, minFilter, tex->width, tex->height, GL_LINEAR);
    if (verdict == NativeTextureCheck::Ok) return;

    if (verdict == NativeTextureCheck::Reparam) {
        SetNativeHandTextureParams();
        return;
    }

    const GLuint fresh = UploadNativeHandTexture();
    if (fresh == 0) return;
    if (m_currentFrameIndex - m_lastClobberLogFrame >= 300u || m_lastClobberLogFrame == 0xFFFFFFFFu) {
        m_lastClobberLogFrame = m_currentFrameIndex;
        VR_LOG_WARN("native hand texture: id %u clobbered (w=%d h=%d), re-uploaded as %u",
                    (unsigned)m_nativeHandTexture, (int)w, (int)h, (unsigned)fresh);
    }
    m_nativeHandTexture = fresh;
}

// GH #46: draw the baked native Riddick arm/hand mesh for one hand.
//
// Geometry comes from native_hand_mesh_data.h (baked from the shipped .xmd by
// tools/xmd_bake.py); the model-space -> hand-local mapping is the provisional,
// INI-tunable placement in native_hand_pose.h (ADR-0427). GH #58 adds an
// optional *posed* variant of the same topology, baked offline from a shipped
// first-person .xsa clip (ADR-0443) and selected with NativeHandPose -- its
// placement is derived from the posed AABB. GH #61 adds the real skin/scar
// diffuse (native_hand_texture.h, baked from Models.xtc), modulated over the
// fixed-function lighting so the hand reads as Riddick's arm; when the texture
// is unavailable the flat skin tone is kept. Deliberately built on the
// already-loaded immediate-mode GL entry points, matching the rest of this
// renderer: it mutates no state that RenderHands does not already restore.
bool VRHandRenderer::DrawNativeHand(HandSide side, const HandCueTint& cue) {
    if (!g_vrConfig.nativeHands) {
        return false;
    }
    if (!s_gl.glBegin || !s_gl.glEnd || !s_gl.glVertex3f || !s_gl.glNormal3f || !s_gl.glTranslatef) {
        return false;
    }

    const char* name = (side == HandSide::Left) ? "Riddick_LeftArm" : "Riddick_RightArm";
    const NativeHandMesh* mesh = FindNativeHandMesh(name);
    if (!mesh || !mesh->vertices || !mesh->indices || mesh->index_count == 0 || mesh->vertex_count == 0) {
        return false;
    }

    // GH #58: prefer the offline-posed variant (ADR-0443) when enabled and baked;
    // the placement is then derived from the *posed* AABB so the mesh anchors on
    // its own geometry. Falls back to the bind mesh/pose when absent.
    const bool posed = g_vrConfig.nativeHandPose != 0 && mesh->has_pose() &&
                       mesh->posed_vertices != nullptr;
    const NativeHandVertex* verts = posed ? mesh->posed_vertices : mesh->vertices;
    const float* bmin = posed ? mesh->posed_bounds_min : mesh->bounds_min;
    const float* bmax = posed ? mesh->posed_bounds_max : mesh->bounds_max;

    const float metersPerUnit = GetWorldScale() * g_vrConfig.nativeHandScale;
    // GH #64 / ADR-0444: use the mesh's baked long axis (stable across the bind
    // and posed variants) instead of re-deriving it from the AABB -- the posed
    // right arm's AABB is nearly cubic, so the largest-extent rule flipped the
    // axis and produced a rotated, mis-anchored (non-mirrored) hand.
    NativeHandPlacement placement;
    if (mesh->arm_axis >= 0 && mesh->arm_axis <= 2) {
        placement = ComputeNativeHandPlacementAlongAxis(
            bmin, bmax, side == HandSide::Left, mesh->arm_axis, metersPerUnit);
    } else {
        placement = ComputeNativeHandPlacement(bmin, bmax, side == HandSide::Left,
                                                metersPerUnit);
    }
    if (!placement.valid) {
        return false;
    }
    // GH #64: on-headset orientation correction (degrees, applied hand-local).
    RotateNativeHandPlacement(placement, g_vrConfig.nativeHandYaw,
                              g_vrConfig.nativeHandPitch, g_vrConfig.nativeHandRoll);

    // GH #61: bind the baked diffuse (no-op fallback when unavailable).
    const bool textured = EnsureNativeHandTexture() && s_gl.glTexCoord2f != nullptr;
    if (textured) {
        s_gl.glEnable(GL_TEXTURE_2D);
        s_gl.glBindTexture(GL_TEXTURE_2D, m_nativeHandTexture);
        if (s_gl.glTexEnvi) {
            // MODULATE so the lit per-vertex colour (fade) multiplies the
            // texture instead of replacing it.
            s_gl.glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        }
    }

    // Provisional hand-local placement offset (metres), INI-tunable; defaults
    // to zero so the mesh's hand end sits at the tracked controller origin.
    s_gl.glTranslatef(g_vrConfig.nativeHandOffsetX,
                      g_vrConfig.nativeHandOffsetY,
                      g_vrConfig.nativeHandOffsetZ);

    // GH #64: the baked mesh is an open shell and its triangle winding is not
    // guaranteed relative to the Y-flipped stereo projection. Render it
    // two-sided (culling off + GL two-sided lighting) so no faces vanish and the
    // real per-vertex normals light both the inner and outer surface. The
    // redundant hand-rolled two-sided abs-lambert that flattened the silhouette
    // is gone; GL_LIGHTING (set up in RenderHands) now provides the 3D shading.
    s_gl.glDisable(GL_CULL_FACE);
    if (s_gl.glLightModeli) {
        s_gl.glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    }

    // Fallback skin tone approximating the RIDDICK_ARMSCAR_D diffuse. When the
    // baked texture is bound the tint goes to white so MODULATE shows the real
    // texture at full strength (the fade still scales alpha/colour).
    // GH #64: the cue tint multiplies the base so glow/heal/grab cues stay visible.
    const float r = (textured ? 1.0f : 0.55f) * cue.r;
    const float g = (textured ? 1.0f : 0.42f) * cue.g;
    const float b = (textured ? 1.0f : 0.33f) * cue.b;
    // GH #64: native hands must never inherit the overlay's cull-away fade.
    // The 0.25 fade existed to hide the *procedural* hands when the engine's own
    // arms dominate; with native hands there is nothing to hide from, and the
    // fade just made them translucent. Keep them fully opaque.
    const float f = 1.0f;
    const bool flipV = g_vrConfig.nativeHandTextureVFlip != 0;

    s_gl.glBegin(GL_TRIANGLES);
    for (unsigned int i = 0; i + 2 < mesh->index_count; i += 3) {
        for (int k = 0; k < 3; ++k) {
            const unsigned int vi = mesh->indices[i + k];
            if (vi >= mesh->vertex_count) {
                continue;
            }
            const NativeHandVertex& v = verts[vi];
            float localPos[3];
            float localNormal[3];
            MapNativeHandPoint(placement, v.position, localPos);
            MapNativeHandNormal(placement, v.normal, localNormal);

            if (m_useAlphaFade) s_gl.glColor4f(r, g, b, f);
            else s_gl.glColor3f(r * f, g * f, b * f);

            if (textured) {
                const float tu = v.uv[0];
                const float tv = flipV ? (1.0f - v.uv[1]) : v.uv[1];
                s_gl.glTexCoord2f(tu, tv);
            }
            s_gl.glNormal3f(localNormal[0], localNormal[1], localNormal[2]);
            s_gl.glVertex3f(localPos[0], localPos[1], localPos[2]);
        }
    }
    s_gl.glEnd();

    if (s_gl.glLightModeli) {
        s_gl.glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_FALSE);
    }
    s_gl.glEnable(GL_CULL_FACE);
    if (textured) {
        // Keep the interaction-glow boxes and the other hand's primitives
        // untextured (state is fully restored by glPopAttrib regardless).
        s_gl.glDisable(GL_TEXTURE_2D);
    }
    return true;
}

void VRHandRenderer::DrawForearmGauntlet(HandSide side) {
    // Prisoner sleeve cuff joins the tracked forearm IK segment; keep this
    // wrist-local portion short so the arm mesh is not doubled.
    DrawTaperedCylinder(0.055f, 0.0285f, 0.125f, 0.035f, 10, 0.46f, 0.44f, 0.38f);

    // Cuff fold: rolled sleeve hem near wrist (fabric ring, slightly darker).
    DrawTaperedCylinder(0.048f, 0.0295f, 0.060f, 0.0300f, 10, 0.40f, 0.38f, 0.32f);
    DrawTaperedCylinder(0.120f, 0.0315f, 0.130f, 0.0325f, 10, 0.42f, 0.40f, 0.34f);

    // Bare wrist skin between cuff and palm (weathered mid-brown-grey).
    DrawTaperedCylinder(0.020f, 0.0240f, 0.048f, 0.0260f, 10, 0.50f, 0.40f, 0.32f);

    // Left wrist: beveled mounting bracket directly beneath the Diegetic Wrist Health HUD (kept).
    if (side == HandSide::Left) {
        DrawBeveledBox(-0.017f, 0.030f, 0.065f, 0.004f, 0.035f, 0.085f, 0.45f, 0.46f, 0.50f);
    }
}

void VRHandRenderer::DrawPalmAndKnuckleGuard(HandSide side) {
    (void)side;
    // GH #46: slim bare prisoner hand (~25% narrower), weathered mid-brown-grey skin.
    // 1. Central palm body (carpal/metacarpal block) - skin base.
    DrawBeveledBox(-0.0255f, -0.012f, -0.042f, 0.0255f, 0.015f, 0.038f, 0.52f, 0.41f, 0.33f);

    // 2. Palm underside shading (-Y palmar surface, slightly deeper skin tone).
    DrawBeveledBox(-0.0225f, -0.015f, -0.038f, 0.0225f, -0.011f, 0.030f, 0.46f, 0.36f, 0.29f);

    // 3. Dorsal knuckle shading (subtle, skin-on-skin - no armor plate).
    DrawBeveledBox(-0.0248f, 0.0135f, -0.048f, 0.0248f, 0.0233f, -0.030f, 0.55f, 0.44f, 0.35f);

    // 4. Subtle knuckle wraps: cloth/leather wrap band across metacarpal heads
    // (replaces tactical studs; muted wrap tone).
    DrawBeveledBox(-0.0248f, 0.021f, -0.050f, 0.0248f, 0.0255f, -0.038f, 0.40f, 0.33f, 0.28f);
}

void VRHandRenderer::DrawArticulatedFinger(HandSide side, int fingerIndex, float curl, float splayAngle, float lengthScale) {
    s_gl.glPushMatrix();

    // Knuckle root positions across distal palm (z = -0.045m), slim ~25% spacing
    float baseOffsets[4] = { -0.018f, -0.006f, 0.006f, 0.018f };
    float xOffset = baseOffsets[fingerIndex];
    if (side == HandSide::Right) {
        xOffset = -xOffset;
    }

    // Move to knuckle joint
    s_gl.glTranslatef(xOffset, 0.005f, -0.045f);

    // Splay rotation (fan out across yaw axis)
    s_gl.glRotatef(splayAngle, 0.0f, 1.0f, 0.0f);

    // Dynamic curling angles:
    // When relaxed (curl ~ 0): slight natural curve (~6 deg)
    // When clenched (curl ~ 1): tight fist flexion
    float curlFactor = (std::clamp)(curl, 0.0f, 1.0f);
    float angleProx  = 6.0f + curlFactor * 62.0f;
    float angleInter = 8.0f + curlFactor * 78.0f;
    float angleDist  = 6.0f + curlFactor * 52.0f;

    float lProx  = 0.026f * lengthScale;
    float lInter = 0.020f * lengthScale;
    float lDist  = 0.015f * lengthScale;
    float w      = 0.0049f * lengthScale; // GH #46: slim fingers (~25% narrower)

    // --- Segment 1: Proximal Phalanx (bare skin) ---
    s_gl.glRotatef(-angleProx, 1.0f, 0.0f, 0.0f); // Rotate downward/inward towards palm
    DrawBeveledBox(-w, -w, -lProx, w, w, 0.0f, 0.52f, 0.41f, 0.33f);

    // Segment 1 dorsal shading (subtle skin highlight, no armor)
    DrawBeveledBox(-w * 0.85f, w * 0.9f, -lProx * 0.9f, w * 0.85f, w * 1.3f, -lProx * 0.1f, 0.55f, 0.44f, 0.35f);

    s_gl.glTranslatef(0.0f, 0.0f, -lProx);

    // --- Segment 2: Intermediate Phalanx ---
    s_gl.glRotatef(-angleInter, 1.0f, 0.0f, 0.0f);
    DrawBeveledBox(-w * 0.9f, -w * 0.9f, -lInter, w * 0.9f, w * 0.9f, 0.0f, 0.49f, 0.38f, 0.30f);

    // Segment 2 joint crease (skin crease shading, replaces knuckle ring)
    DrawTaperedCylinder(-lInter * 0.15f, w * 0.95f, 0.0f, w * 0.95f, 6, 0.38f, 0.29f, 0.23f);

    s_gl.glTranslatef(0.0f, 0.0f, -lInter);

    // --- Segment 3: Distal Phalanx (Fingertip) ---
    s_gl.glRotatef(-angleDist, 1.0f, 0.0f, 0.0f);
    DrawBeveledBox(-w * 0.8f, -w * 0.8f, -lDist, w * 0.8f, w * 0.8f, 0.0f, 0.46f, 0.36f, 0.29f);

    // Fingertip pad shading (deeper skin tone)
    DrawBeveledBox(-w * 0.7f, -w * 1.0f, -lDist * 0.9f, w * 0.7f, -w * 0.7f, -lDist * 0.1f, 0.40f, 0.31f, 0.25f);

    s_gl.glPopMatrix();
}

void VRHandRenderer::DrawArticulatedThumb(HandSide side, float curl) {
    s_gl.glPushMatrix();

    float xSign = (side == HandSide::Left) ? -1.0f : 1.0f;

    // Thumb originates from radial/palmar side (slim spacing)
    s_gl.glTranslatef(xSign * 0.026f, -0.008f, 0.005f);

    // Thumb rest angle: angled forward and pitched inward
    s_gl.glRotatef(xSign * 28.0f, 0.0f, 1.0f, 0.0f);
    s_gl.glRotatef(-18.0f, 1.0f, 0.0f, 0.0f);

    float curlFactor = (std::clamp)(curl, 0.0f, 1.0f);
    float angleProx = 8.0f + curlFactor * 45.0f;
    float angleDist = 10.0f + curlFactor * 55.0f;

    float lProx = 0.024f;
    float lDist = 0.020f;
    float w = 0.0056f; // GH #46: slim thumb (~25% narrower)

    // Proximal Thumb Segment (bare skin)
    s_gl.glRotatef(xSign * angleProx * 0.5f, 0.0f, 1.0f, 0.0f);
    s_gl.glRotatef(-angleProx, 1.0f, 0.0f, 0.0f);
    DrawBeveledBox(-w, -w, -lProx, w, w, 0.0f, 0.52f, 0.41f, 0.33f);

    s_gl.glTranslatef(0.0f, 0.0f, -lProx);

    // Distal Thumb Segment (folds over curled fingers when making a fist)
    s_gl.glRotatef(xSign * angleDist * 0.6f, 0.0f, 1.0f, 0.0f);
    s_gl.glRotatef(-angleDist, 1.0f, 0.0f, 0.0f);
    DrawBeveledBox(-w * 0.9f, -w * 0.9f, -lDist, w * 0.9f, w * 0.9f, 0.0f, 0.49f, 0.38f, 0.30f);

    // Thumb nail-side shading (subtle skin highlight, no armor)
    DrawBeveledBox(-w * 0.8f, w * 0.8f, -lDist * 0.85f, w * 0.8f, w * 1.2f, -lDist * 0.15f, 0.55f, 0.44f, 0.35f);

    s_gl.glPopMatrix();
}

void VRHandRenderer::DrawBeveledBox(float x0, float y0, float z0, float x1, float y1, float z1, float r, float g, float b) {
    // Per-hand fade uses alpha when blend dispatch is available, with darkening
    // as a conservative fallback for drivers without those core entry points.
    float f = (std::clamp)(m_currentFade, 0.0f, 1.0f);
    if (m_useAlphaFade) s_gl.glColor4f(r, g, b, f);
    else s_gl.glColor3f(r * f, g * f, b * f);
    s_gl.glBegin(GL_QUADS);

    // Front (+Z)
    s_gl.glNormal3f(0.0f, 0.0f, 1.0f);
    s_gl.glVertex3f(x0, y0, z1);
    s_gl.glVertex3f(x1, y0, z1);
    s_gl.glVertex3f(x1, y1, z1);
    s_gl.glVertex3f(x0, y1, z1);

    // Back (-Z)
    s_gl.glNormal3f(0.0f, 0.0f, -1.0f);
    s_gl.glVertex3f(x1, y0, z0);
    s_gl.glVertex3f(x0, y0, z0);
    s_gl.glVertex3f(x0, y1, z0);
    s_gl.glVertex3f(x1, y1, z0);

    // Top (+Y)
    s_gl.glNormal3f(0.0f, 1.0f, 0.0f);
    s_gl.glVertex3f(x0, y1, z1);
    s_gl.glVertex3f(x1, y1, z1);
    s_gl.glVertex3f(x1, y1, z0);
    s_gl.glVertex3f(x0, y1, z0);

    // Bottom (-Y)
    s_gl.glNormal3f(0.0f, -1.0f, 0.0f);
    s_gl.glVertex3f(x0, y0, z0);
    s_gl.glVertex3f(x1, y0, z0);
    s_gl.glVertex3f(x1, y0, z1);
    s_gl.glVertex3f(x0, y0, z1);

    // Right (+X)
    s_gl.glNormal3f(1.0f, 0.0f, 0.0f);
    s_gl.glVertex3f(x1, y0, z1);
    s_gl.glVertex3f(x1, y0, z0);
    s_gl.glVertex3f(x1, y1, z0);
    s_gl.glVertex3f(x1, y1, z1);

    // Left (-X)
    s_gl.glNormal3f(-1.0f, 0.0f, 0.0f);
    s_gl.glVertex3f(x0, y0, z0);
    s_gl.glVertex3f(x0, y0, z1);
    s_gl.glVertex3f(x0, y1, z1);
    s_gl.glVertex3f(x0, y1, z0);

    s_gl.glEnd();
}

void VRHandRenderer::DrawTaperedCylinder(float z0, float r0, float z1, float r1, int slices, float r, float g, float b) {
    float f = (std::clamp)(m_currentFade, 0.0f, 1.0f);
    if (m_useAlphaFade) s_gl.glColor4f(r, g, b, f);
    else s_gl.glColor3f(r * f, g * f, b * f);
    s_gl.glBegin(GL_QUAD_STRIP);
    for (int i = 0; i <= slices; ++i) {
        float angle = i * (6.2831853f / slices);
        float ca = cosf(angle);
        float sa = sinf(angle);

        s_gl.glNormal3f(ca, sa, (r0 - r1) / (z1 - z0));
        s_gl.glVertex3f(ca * r0, sa * r0, z0);
        s_gl.glVertex3f(ca * r1, sa * r1, z1);
    }
    s_gl.glEnd();
}

} // namespace RiddickVR
