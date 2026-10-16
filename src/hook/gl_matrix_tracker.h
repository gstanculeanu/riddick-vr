#pragma once

// GH #67 step 1: a software mirror of the fixed-function GL_MODELVIEW matrix
// stack.
//
// Why this exists (evidence: docs/rea/reference/RndrGL_GL_Path.txt section 3):
// RndrGL.dll composes object transforms from `glLoadMatrixf` + `glPushMatrix` +
// `glTranslatef` + `glPopMatrix` (it does **not** import `glMultMatrixf` /
// `glRotatef` / `glDepthRange`). Until this header the hook only observed
// `glLoadMatrixf` (in `OpenGLProxy::Hook_glLoadMatrixf`, mirrored into
// `s_lastView`), so the "modelview in effect at a draw" it recorded was at best
// the last *loaded* base -- never the matrix the engine actually rendered with,
// which is `base * translate(...)` for every pushed sub-object.
//
// The GH #67 step-2 observation probe (`ViewmodelSuppressor::SetObservedSample`,
// ADR-0450) feeds off that value, so without this tracker the captured
// viewmodel transform is wrong whenever the pass composes a sub-transform (the
// normal case for a multi-submesh arm/weapon viewmodel).
//
// Semantics match OpenGL's fixed-function stack for the calls RndrGL uses:
// column-major 4x4 matrices, `Load*` replace the top of the stack, `Translatef`
// is a right-multiply by a translation (`M' = M * T`), `Push` duplicates the top
// and `Pop` restores it. The stack is bounded; overflow/underflow are ignored
// (never dereference out of range) so an unbalanced engine call cannot corrupt
// state.
//
// Header-only and dependency-free so it is unit-tested headlessly
// (`tests/gl_matrix_tracker_tests.cpp`) with no GL context and no game launch.

namespace RiddickVR {

class GlMatrixTracker {
public:
    // OpenGL's minimum modelview stack depth is 32; a bound far above any real
    // engine frame is plenty and keeps the object small.
    static constexpr int kMaxDepth = 32;

    GlMatrixTracker() { Reset(); }

    // Fresh context: identity, empty stack.
    void Reset() {
        Identity(stack_[0]);
        depth_ = 0;
    }

    // The current modelview (top of stack). Always a valid 16-float buffer.
    const float* Current() const { return stack_[depth_]; }

    // glLoadMatrixf (GL_MODELVIEW): replace the current matrix.
    void LoadMatrixf(const float* m) {
        if (!m) return;
        Copy(stack_[depth_], m);
    }

    // glLoadIdentity.
    void LoadIdentity() { Identity(stack_[depth_]); }

    // glMultMatrixf: M' = M * m (not imported by RndrGL, provided for
    // completeness so the mirror is correct if that ever changes).
    void Multiply(const float* m) {
        if (!m) return;
        float out[16];
        MultiplyInto(stack_[depth_], m, out);
        Copy(stack_[depth_], out);
    }

    // glTranslatef: right-multiply by the translation (column-major).
    void Translatef(float x, float y, float z) {
        float* M = stack_[depth_];
        // M' = M * T; only column 3 changes: M'[:,3] = M * (x, y, z, 1).
        const float c0 = M[0] * x + M[4] * y + M[8] * z + M[12];
        const float c1 = M[1] * x + M[5] * y + M[9] * z + M[13];
        const float c2 = M[2] * x + M[6] * y + M[10] * z + M[14];
        const float c3 = M[3] * x + M[7] * y + M[11] * z + M[15];
        M[12] = c0;
        M[13] = c1;
        M[14] = c2;
        M[15] = c3;
    }

    // glPushMatrix: duplicate the top. Overflow is ignored (the matrix stays put)
    // so a matching pop later cannot underflow below the base.
    void Push() {
        if (depth_ + 1 >= kMaxDepth) return;
        Copy(stack_[depth_ + 1], stack_[depth_]);
        ++depth_;
    }

    // glPopMatrix: restore the previous top. Underflow is ignored.
    void Pop() {
        if (depth_ > 0) --depth_;
    }

    // Current stack depth (0 == base only). Diagnostic/tests.
    int Depth() const { return depth_; }

private:
    static void Identity(float* m) {
        for (int i = 0; i < 16; ++i) m[i] = 0.0f;
        m[0] = 1.0f;
        m[5] = 1.0f;
        m[10] = 1.0f;
        m[15] = 1.0f;
    }

    static void Copy(float* dst, const float* src) {
        for (int i = 0; i < 16; ++i) dst[i] = src[i];
    }

    // out = a * b, column-major (out[c*4 + r] = sum_k a[k*4 + r] * b[c*4 + k]).
    static void MultiplyInto(const float* a, const float* b, float* out) {
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                out[c * 4 + r] =
                    a[0 * 4 + r] * b[c * 4 + 0] +
                    a[1 * 4 + r] * b[c * 4 + 1] +
                    a[2 * 4 + r] * b[c * 4 + 2] +
                    a[3 * 4 + r] * b[c * 4 + 3];
            }
        }
    }

    int depth_ = 0;
    float stack_[kMaxDepth][16] = {};
};

} // namespace RiddickVR
