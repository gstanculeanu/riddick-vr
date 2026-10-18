#pragma once

// GH #67 step 1 (corrected) -- where the engine's transforms actually live.
//
// Measured, on the live 2009 target (`System/Win32_x86`, the install the
// harness runs), read-only (ADR-0481):
//
//   1. The shipped `RndrGL.dll` (939248 bytes, 05-Mar-12) does **not** import
//      `glMultMatrixf` / `glMultMatrixd` / `glLoadMatrixd` / `glRotatef` /
//      `glScalef` / `glDepthRange` -- the same negatives the 2004 recon found
//      (`docs/rea/reference/RndrGL_GL_Path.txt` section 3, ADR-0413 target
//      mismatch). So the fixed-function modelview *cannot* be composed with
//      those calls; the matrix-stack mirror (gl_matrix_tracker.h) can only ever
//      see what `glLoadMatrixf`/`glPushMatrix`/`glTranslatef` set.
//   2. A live run (8790 frames, RIDDICKVR_SIM=1, shadow install) shows the
//      modelview stack is a dead end as a capture: `glLoadMatrixf
//      [MODELVIEW #1]` is RndrGL's *only* modelview load in the whole run
//      (identity), and `glPushMatrix`/`glTranslatef`/`glPopMatrix` are never
//      called (the probe reads pushes=0 translates=0 on every sampled frame)
//      while the projection is reloaded ~14x per frame.
//   3. That same `RndrGL.dll` **does** import the ARB_vertex_program entry
//      points -- `glProgramStringARB`, `glBindProgramARB`, `glGenProgramARB`,
//      `glProgramEnvParameter4fvARB` -- plus the generic-attribute path
//      (`glVertexAttribPointerARB`, and no `glTexCoordPointer`). The engine
//      renders through vertex programs and feeds the object/view transforms in
//      as *program environment parameters*, not through GL_MODELVIEW.
//
// Consequence: the transform GH #67 wants to re-express in head/controller
// space is a program parameter, so it must be captured on the ARB program path.
// This observer is that capture: it counts the ARB program calls per frame,
// remembers the last bound program, and reconstructs a 4x4 from four
// *consecutive* `glProgramEnvParameter4fvARB` indices (matrixBase is the first
// index). It does not claim a row/column convention: it records the four
// vectors in index order, exactly as uploaded.
//
// Header-only and dependency-free (no GL/Windows headers) so it is unit-tested
// headlessly, like gl_matrix_tracker.h / viewmodel_suppressor.h.

namespace RiddickVR {

// One sampled frame's ARB vertex-program tallies. `matrixAssembled` is true
// when four consecutive environment-parameter indices were seen; `matrix` then
// holds those four vec4s in index order (matrixBase .. matrixBase+3).
struct VertexProgramSnapshot {
    unsigned strings = 0;        // glProgramStringARB calls
    unsigned binds = 0;          // glBindProgramARB calls
    unsigned envParams = 0;      // glProgramEnvParameter4fvARB calls
    unsigned lastProgramId = 0;  // last glBindProgramARB id (0 = none bound)
    unsigned lastStringLen = 0;  // last glProgramStringARB string length
    unsigned lastStringHash = 0; // FNV-1a of the last program string (identifies it)
    bool matrixAssembled = false;
    unsigned matrixBase = 0;     // first environment-parameter index of the quad
    float matrix[16] = {};       // the four consecutive vec4s, in index order
};

class VertexProgramObserver {
public:
    // Observation is opt-in (ViewmodelObserve=1) and inert otherwise: every
    // Note* below is a no-op and ComputeSnapshot reports zeros.
    void Configure(bool observe) {
        observe_ = observe;
        if (!observe) Reset();
    }
    bool Observing() const { return observe_; }

    // glProgramStringARB(target, format, len, string) -- `hash` is Fnv1a over
    // the program string (the caller hashes it; capped by `hashLen` when set).
    void NoteProgramString(int stringLen, unsigned hash) {
        if (!observe_) return;
        ++strings_;
        lastStringLen_ = stringLen > 0 ? static_cast<unsigned>(stringLen) : 0u;
        lastStringHash_ = hash;
    }

    // glBindProgramARB(target, program).
    void NoteBindProgram(unsigned program) {
        if (!observe_) return;
        ++binds_;
        lastProgramId_ = program;
    }

    // glProgramEnvParameter4fvARB(target, index, params[4]).
    void NoteEnvParameter(unsigned index, const float* params) {
        if (!observe_ || !params) return;
        ++envParams_;
        if (hasLastIndex_ && index == lastIndex_ + 1u && rows_ < 4u) {
            ++rows_;
        } else {
            rows_ = 1u;
        }
        const unsigned row = rows_ - 1u;
        for (unsigned c = 0; c < 4u; ++c) pending_[row * 4u + c] = params[c];
        if (rows_ == 4u) {
            for (unsigned i = 0; i < 16u; ++i) matrix_[i] = pending_[i];
            matrixBase_ = index - 3u;
            matrixAssembled_ = true;
        }
        lastIndex_ = index;
        hasLastIndex_ = true;
    }

    // Publishes this frame's tallies and re-bases them.
    VertexProgramSnapshot ComputeSnapshot() {
        VertexProgramSnapshot snap;
        snap.strings = strings_;
        snap.binds = binds_;
        snap.envParams = envParams_;
        snap.lastProgramId = lastProgramId_;
        snap.lastStringLen = lastStringLen_;
        snap.lastStringHash = lastStringHash_;
        snap.matrixAssembled = matrixAssembled_;
        snap.matrixBase = matrixBase_;
        for (unsigned i = 0; i < 16u; ++i) snap.matrix[i] = matrix_[i];
        Reset();
        return snap;
    }

    void Reset() {
        strings_ = binds_ = envParams_ = 0;
        lastProgramId_ = lastStringLen_ = lastStringHash_ = 0;
        matrixAssembled_ = false;
        matrixBase_ = 0;
        rows_ = 0;
        hasLastIndex_ = false;
        lastIndex_ = 0;
        for (unsigned i = 0; i < 16u; ++i) { matrix_[i] = 0.0f; pending_[i] = 0.0f; }
    }

    // FNV-1a over a byte range, so a program string has a stable small identity
    // in the log (the caller caps `len` if the program is huge).
    static unsigned Fnv1a(const void* data, int len) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        unsigned h = 2166136261u;
        for (int i = 0; i < len; ++i) {
            h ^= p[i];
            h *= 16777619u;
        }
        return h;
    }

private:
    bool observe_ = false;
    unsigned strings_ = 0;
    unsigned binds_ = 0;
    unsigned envParams_ = 0;
    unsigned lastProgramId_ = 0;
    unsigned lastStringLen_ = 0;
    unsigned lastStringHash_ = 0;
    bool matrixAssembled_ = false;
    unsigned matrixBase_ = 0;
    float matrix_[16] = {};
    // Consecutive-index assembly state (four env params in a row form a matrix).
    unsigned rows_ = 0;
    bool hasLastIndex_ = false;
    unsigned lastIndex_ = 0;
    float pending_[16] = {};
};

} // namespace RiddickVR
