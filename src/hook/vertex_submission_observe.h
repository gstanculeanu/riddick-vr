#pragma once

// GH #67 step 1 (second correction, ADR-0482) -- capture the transform where it
// actually *is*: in the engine's submitted vertex data.
//
// Measured (ADR-0481, read-only + live, `tools/gl_vocabulary_scan.py --imports`
// reproduces the import table):
//
//   * The shipped 2009 `RndrGL.dll`'s entire **static** OPENGL32 import table is
//     96 symbols and contains NO matrix composition beyond `glLoadMatrixf` +
//     `glPushMatrix` + `glTranslatef` + `glPopMatrix` -- in particular no
//     `glMultMatrixf`/`glMultMatrixd`/`glLoadMatrixd`/`glRotatef`/`glScalef`, and
//     **no ARB entry point at all** (as `vertex_program_observe.h` first claimed;
//     every ARB name could only be a `wglGetProcAddress` literal). That ARB path
//     was then instrumented and measured silent (`strings=0 binds=0 env=0`).
//   * A live run shows RndrGL loads GL_MODELVIEW exactly **once** (identity) and
//     never pushes/translates it, while the projection is reloaded per view.
//
// So with an identity modelview, a per-view projection and client-array /
// display-list draws, the object->view transform is applied **engine-side to the
// submitted vertices**. The observable surface is therefore the geometry itself:
//
//   * `glVertexPointer` defines the client vertex array. RndrGL imports it (and
//     `glNormalPointer`/`glColorPointer`), never `glDrawArrays` or
//     `glBindBuffer`, so the pointer is always a plain client pointer.
//   * `glDrawElements` / `glCallList` are the draw calls the hook already sees
//     (#63/#66), so a sample of the submitted vertices can be taken at draw time.
//
// This observer answers the two questions the next step depends on:
//   1. Are the submitted vertices in *object* space (small, near the origin,
//      per-mesh) or already in *view/camera* space (the view transform baked in)?
//   2. Are the first-person window's draws array draws or display-list draws? If
//      they are display lists, the vertex data is not observable at
//      `glDrawElements` at all and the capture must move into list compilation.
//
// It decodes only client, tightly-or-strided **`GL_FLOAT`** arrays. Anything else
// is reported *as such* (the pointer's type/size/stride in the log, `valid=false`)
// rather than guessed. No GL/Windows headers: plain types only, so it is
// unit-tested headlessly like gl_matrix_tracker.h / vertex_program_observe.h.

#include <cstring>

namespace RiddickVR {

// The handful of GL enums that must be recognised without a GL header.
constexpr unsigned kGlUnsignedByte  = 0x1401u;
constexpr unsigned kGlUnsignedShort = 0x1403u;
constexpr unsigned kGlUnsignedInt   = 0x1405u;
constexpr unsigned kGlFloat         = 0x1406u;

// Bounded so a very large draw cannot make the hook slow: the bounds of a mesh
// are well represented by a spread of vertices, not by every one.
constexpr unsigned kMaxVertexSamples = 24;

struct VertexSample {
    bool valid = false;       // the array was decodable and at least one vertex was read
    unsigned read = 0;        // vertices actually read
    unsigned indexType = 0;   // as passed to glDrawElements
    unsigned vertexType = 0;  // as recorded by glVertexPointer
    float first[3] = {};      // the first sampled vertex
    float min[3] = {};        // axis-aligned bounds over the sampled vertices
    float max[3] = {};
};

// Reads a spread of up to kMaxVertexSamples vertices from a client array draw.
// `vertices` is the glVertexPointer pointer, `indices` the glDrawElements index
// buffer (null = sequential). Returns `valid=false` when the arrays are not a
// decodable float array (a non-float vertex type, an unknown index type, a null
// vertex pointer, `count == 0`), so the caller logs a type/stride it could not
// decode instead of a plausible-looking wrong number.
inline VertexSample SampleClientVertices(unsigned count, unsigned indexType, const void* indices,
                                         unsigned vertexSize, unsigned vertexType, int stride,
                                         const void* vertices) {
    VertexSample s;
    s.indexType = indexType;
    s.vertexType = vertexType;
    if (vertexType != kGlFloat) return s;          // only float arrays are decoded
    if (!vertices || count == 0u) return s;        // nothing to read
    if (vertexSize < 2u || vertexSize > 4u) return s;
    unsigned indexSize;
    switch (indexType) {
        case kGlUnsignedByte:  indexSize = 1u; break;
        case kGlUnsignedShort: indexSize = 2u; break;
        case kGlUnsignedInt:   indexSize = 4u; break;
        default: return s;                          // unknown index type
    }
    const unsigned step = stride > 0
        ? static_cast<unsigned>(stride)
        : vertexSize * static_cast<unsigned>(sizeof(float));
    const unsigned char* base = static_cast<const unsigned char*>(vertices);
    const unsigned char* idx = static_cast<const unsigned char*>(indices);
    const unsigned n = count < kMaxVertexSamples ? count : kMaxVertexSamples;
    for (unsigned k = 0; k < n; ++k) {
        // Which element of the index list this sample reads: all of them when the
        // draw is small, otherwise an even spread across [0, count-1].
        const unsigned long long position = (count <= kMaxVertexSamples)
            ? k
            : (static_cast<unsigned long long>(k) * (count - 1u)) / (kMaxVertexSamples - 1u);
        unsigned vi = static_cast<unsigned>(position);
        if (idx) {
            const unsigned char* ip = idx + position * indexSize;
            vi = 0;
            for (unsigned b = 0; b < indexSize; ++b) {
                vi |= static_cast<unsigned>(ip[b]) << (8u * b);
            }
        }
        const unsigned char* vp = base + static_cast<size_t>(vi) * step;
        float xyz[3] = {0.0f, 0.0f, 0.0f};
        const unsigned comps = vertexSize < 3u ? vertexSize : 3u;
        for (unsigned c = 0; c < comps; ++c) {
            std::memcpy(&xyz[c], vp + c * sizeof(float), sizeof(float));
        }
        if (s.read == 0u) {
            for (unsigned c = 0; c < 3u; ++c) {
                s.first[c] = xyz[c];
                s.min[c] = xyz[c];
                s.max[c] = xyz[c];
            }
        } else {
            for (unsigned c = 0; c < 3u; ++c) {
                if (xyz[c] < s.min[c]) s.min[c] = xyz[c];
                if (xyz[c] > s.max[c]) s.max[c] = xyz[c];
            }
        }
        ++s.read;
    }
    s.valid = s.read > 0u;
    return s;
}

// One sampled frame's submitted-geometry tallies (interval counts) plus the
// first array-draw sample taken inside and outside the first-person window.
struct VertexSubmissionSnapshot {
    unsigned arrayInside = 0;     // glDrawElements calls with the window open
    unsigned arrayOutside = 0;    // glDrawElements calls with the window closed
    unsigned listInside = 0;      // glCallList calls with the window open
    unsigned listOutside = 0;     // glCallList calls with the window closed
    bool pointerSet = false;      // a client vertex array was recorded
    unsigned pointerSize = 0;     // components per vertex (2/3/4)
    unsigned pointerType = 0;     // GL type enum of the array
    int pointerStride = 0;        // bytes between vertices (0 = tightly packed)
    bool hasInsideSample = false; // the first in-window array draw was sampled
    bool hasOutsideSample = false;
    VertexSample insideSample;    // the first-person candidate geometry
    VertexSample outsideSample;   // world geometry (the control)
};

class VertexSubmissionObserver {
public:
    // Observation is opt-in (ViewmodelObserve=1) and inert otherwise: the draw
    // notes are no-ops and ComputeSnapshot reports zeros.
    void Configure(bool observe) {
        observe_ = observe;
        if (!observe) Reset();
    }
    bool Observing() const { return observe_; }

    // glVertexPointer(size, type, stride, pointer). Client state persists across
    // draws, so this is remembered (not per-draw) -- and it is read only while
    // observing, so it can never change a run that did not ask for it.
    void NoteVertexPointer(unsigned size, unsigned type, int stride, const void* pointer) {
        pointerSize_ = size;
        pointerType_ = type;
        pointerStride_ = stride;
        pointer_ = pointer;
        pointerSet_ = pointer != nullptr;
    }

    // One glDrawElements call, tagged by whether it fell inside the first-person
    // window. The first array draw on each side is sampled (a whole pass shares
    // its buffer, so later draws would only repeat it).
    void NoteArrayDraw(bool insideWindow, unsigned count, unsigned indexType, const void* indices) {
        if (!observe_) return;
        if (insideWindow) {
            ++arrayInside_;
            if (!hasInsideSample_) {
                insideSample_ = SampleClientVertices(count, indexType, indices,
                                                     pointerSize_, pointerType_, pointerStride_, pointer_);
                hasInsideSample_ = true;
            }
        } else {
            ++arrayOutside_;
            if (!hasOutsideSample_) {
                outsideSample_ = SampleClientVertices(count, indexType, indices,
                                                      pointerSize_, pointerType_, pointerStride_, pointer_);
                hasOutsideSample_ = true;
            }
        }
    }

    // One glCallList call, tagged the same way. A display list's vertices are
    // replayed inside the driver, so counting these is what decides whether the
    // first-person geometry is observable at all at the draw entry point.
    void NoteListDraw(bool insideWindow) {
        if (!observe_) return;
        if (insideWindow) ++listInside_;
        else ++listOutside_;
    }

    // Publishes this interval's tallies/samples and re-bases the interval state.
    // The recorded vertex-pointer state is client state, so it survives.
    VertexSubmissionSnapshot ComputeSnapshot() {
        VertexSubmissionSnapshot snap;
        snap.arrayInside = arrayInside_;
        snap.arrayOutside = arrayOutside_;
        snap.listInside = listInside_;
        snap.listOutside = listOutside_;
        snap.pointerSet = pointerSet_;
        snap.pointerSize = pointerSize_;
        snap.pointerType = pointerType_;
        snap.pointerStride = pointerStride_;
        snap.hasInsideSample = hasInsideSample_;
        snap.hasOutsideSample = hasOutsideSample_;
        snap.insideSample = insideSample_;
        snap.outsideSample = outsideSample_;
        arrayInside_ = arrayOutside_ = listInside_ = listOutside_ = 0u;
        hasInsideSample_ = hasOutsideSample_ = false;
        insideSample_ = VertexSample{};
        outsideSample_ = VertexSample{};
        return snap;
    }

    void Reset() {
        arrayInside_ = arrayOutside_ = listInside_ = listOutside_ = 0u;
        hasInsideSample_ = hasOutsideSample_ = false;
        insideSample_ = VertexSample{};
        outsideSample_ = VertexSample{};
        pointerSize_ = pointerType_ = 0u;
        pointerStride_ = 0;
        pointer_ = nullptr;
        pointerSet_ = false;
    }

private:
    bool observe_ = false;
    unsigned arrayInside_ = 0;
    unsigned arrayOutside_ = 0;
    unsigned listInside_ = 0;
    unsigned listOutside_ = 0;
    bool hasInsideSample_ = false;
    bool hasOutsideSample_ = false;
    VertexSample insideSample_;
    VertexSample outsideSample_;
    unsigned pointerSize_ = 0;
    unsigned pointerType_ = 0;
    int pointerStride_ = 0;
    const void* pointer_ = nullptr;
    bool pointerSet_ = false;
};

} // namespace RiddickVR
