// GH #67 step 1: unit tests for the software GL_MODELVIEW stack
// (gl_matrix_tracker.h).
//
// These pin the semantics the draw-time modelview capture relies on:
//   - identity base; Load*/LoadMatrixf replace the current matrix,
//   - Translatef is a right-multiply by a translation (M' = M * T),
//   - Push/Pop duplicate/restore the top,
//   - overflow/underflow are ignored (never out of range),
//   - and the exact gap this closes: a camera modelview with a pushed,
//     translated sub-object (the viewmodel pass) must read back as
//     camera * translate -- not as the camera base the old `s_lastView`
//     (glLoadMatrixf only) recorded.
#include "gl_matrix_tracker.h"

#include <cstdio>
#include <cmath>

using RiddickVR::GlMatrixTracker;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "gl matrix tracker test failed: %s\n", message);
        ++g_failures;
    }
}

static bool Eq(const float* m, const float* e, float eps = 1e-5f) {
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(m[i] - e[i]) > eps) return false;
    }
    return true;
}

int main() {
    // Fresh tracker: identity base, depth 0.
    {
        GlMatrixTracker t;
        const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        Check(Eq(t.Current(), kIdentity), "a fresh tracker must be at the identity");
        Check(t.Depth() == 0, "a fresh tracker must have an empty stack");
    }

    // Translate on the identity base writes the translation into column 3.
    {
        GlMatrixTracker t;
        t.Translatef(1.0f, 2.0f, 3.0f);
        const float kExpected[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 2, 3, 1};
        Check(Eq(t.Current(), kExpected), "I * T must equal T");
    }

    // LoadMatrixf replaces the current matrix (not multiply).
    {
        GlMatrixTracker t;
        const float m[16] = {2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 10, 20, 30, 1};
        t.LoadMatrixf(m);
        Check(Eq(t.Current(), m), "LoadMatrixf must replace the top of the stack");
        t.LoadIdentity();
        const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        Check(Eq(t.Current(), kIdentity), "LoadIdentity must reset the top to identity");
    }

    // Push copies the top; a translate after the push must not disturb the base,
    // and Pop restores the pre-push matrix exactly.
    {
        GlMatrixTracker t;
        const float camera[16] = {0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 5, 6, 7, 1};
        t.LoadMatrixf(camera);

        t.Push();
        Check(t.Depth() == 1, "Push must increase the depth");
        t.Translatef(0.25f, -0.5f, 0.75f);

        // current == camera * T
        float expected[16];
        // camera * T: column 3 = camera * (0.25, -0.5, 0.75, 1)
        for (int i = 0; i < 16; ++i) expected[i] = camera[i];
        expected[12] = camera[0] * 0.25f + camera[4] * -0.5f + camera[8] * 0.75f + camera[12];
        expected[13] = camera[1] * 0.25f + camera[5] * -0.5f + camera[9] * 0.75f + camera[13];
        expected[14] = camera[2] * 0.25f + camera[6] * -0.5f + camera[10] * 0.75f + camera[14];
        expected[15] = camera[3] * 0.25f + camera[7] * -0.5f + camera[11] * 0.75f + camera[15];
        Check(Eq(t.Current(), expected), "a pushed + translated top must be base * translate");

        t.Pop();
        Check(t.Depth() == 0, "Pop must restore the depth");
        Check(Eq(t.Current(), camera), "Pop must restore the exact pre-push matrix");
    }

    // Nested pushes compose and unwind exactly. With an identity base the two
    // translations (1,0,0) then (0,2,0) must land in column 3 as (1,2,0).
    {
        GlMatrixTracker t;
        const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        t.Push();
        t.Translatef(1.0f, 0.0f, 0.0f);
        t.Push();
        t.Translatef(0.0f, 2.0f, 0.0f);
        Check(std::fabs(t.Current()[12] - 1.0f) < 1e-4f &&
              std::fabs(t.Current()[13] - 2.0f) < 1e-4f &&
              std::fabs(t.Current()[14] - 0.0f) < 1e-4f,
              "nested translations must accumulate in the composed matrix");
        t.Pop();
        t.Pop();
        Check(Eq(t.Current(), kIdentity), "unwinding all pushes must return to the base");
    }

    // Pop from the base is ignored (never underflows).
    {
        GlMatrixTracker t;
        const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        t.Pop();
        t.Pop();
        Check(t.Depth() == 0, "Pop from the base must not underflow");
        Check(Eq(t.Current(), kIdentity), "an ignored Pop must not change the base");
    }

    // Push overflow is ignored and leaves the current matrix usable; the matching
    // pops still unwind to the base.
    {
        GlMatrixTracker t;
        t.Translatef(4.0f, 5.0f, 6.0f); // base != identity so we can verify restore
        const float base[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 4, 5, 6, 1};
        for (int i = 0; i < GlMatrixTracker::kMaxDepth + 8; ++i) {
            t.Push();
        }
        Check(t.Depth() == GlMatrixTracker::kMaxDepth - 1,
              "Push must saturate at the bounded depth, never overflow");
        for (int i = 0; i < GlMatrixTracker::kMaxDepth + 8; ++i) {
            t.Pop();
        }
        Check(t.Depth() == 0 && Eq(t.Current(), base),
              "saturating pushes must still unwind to the base matrix");
    }

    // Multiply matches the closed-form product.
    {
        GlMatrixTracker t;
        const float a[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        const float b[16] = {16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
        t.LoadMatrixf(a);
        t.Multiply(b);
        float expected[16];
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                expected[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] +
                                      a[1 * 4 + r] * b[c * 4 + 1] +
                                      a[2 * 4 + r] * b[c * 4 + 2] +
                                      a[3 * 4 + r] * b[c * 4 + 3];
            }
        }
        Check(Eq(t.Current(), expected), "Multiply must compute M * m (column-major)");
    }

    // Reset returns a dirty tracker to the identity base.
    {
        GlMatrixTracker t;
        const float dirty[16] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};
        t.LoadMatrixf(dirty);
        t.Push();
        t.Translatef(1, 1, 1);
        t.Reset();
        const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        Check(t.Depth() == 0 && Eq(t.Current(), kIdentity),
              "Reset must clear the stack and restore the identity base");
    }

    if (g_failures == 0) {
        std::puts("gl matrix tracker tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
