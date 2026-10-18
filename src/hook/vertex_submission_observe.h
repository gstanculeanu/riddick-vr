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
//
// ADR-0483 (third correction): a live run showed the depth-only-clear window's
// FIRST in-window array draw is a flat, screen-space 2-D mesh, not the arms, so
// the in-window sample is now the window's **largest decodable array draw** (the
// arms mesh is the pass's biggest mesh) and the draw's index `count` is logged so
// a 4-vertex quad is distinguishable from a real mesh.
//
// ADR-0490 (eighth correction): both of the samples above are classified by the
// *window*, and two live runs measured the same 14688-count volumetric mesh inside
// BOTH window openers (ADR-0489), so the window is not what isolates the arms. This
// adds a **window-independent** candidate: the interval's largest decodable array
// draw whose sampled bounds are entirely *near-field* (within kNearFieldMaxAbs of
// the origin on every axis), regardless of the window state. The measured
// separation is wide: the near-field meshes sit at ~±0.45 while the view-space
// world/overlay draws sit at |x| ~ 1.6e3..3.5e3, so a threshold of 16 units
// (~0.5 m at the provisional 30.3 units/m of viewmodel_drive.h) is comfortably
// between them. Each candidate carries whether the window happened to be open at
// that draw (`inside`), so a run can state directly whether the window mattered.
// The threshold is PROVISIONAL and re-measured on a headset; the harness
// re-checks the logged bounds against its own copy of the constant.

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

// GH #67 step 1 (eighth correction, ADR-0490): what counts as "near-field" for the
// window-independent candidate. Engine units; see the header comment. PROVISIONAL
// (measured: near-field meshes ~±0.45, view-space world/overlay draws ~1.6e3..3.5e3).
constexpr float kNearFieldMaxAbs = 16.0f;

// GH #76/#90 (ADR-0506): *why* an array draw's vertices could not be decoded. The
// span census (stereo_dual.h) counts these so a run names the rejected signature
// (vertex type/size/stride/pointer, index type) instead of only "unknown" -- which
// is what decides whether the per-eye pass can see the world geometry at all.
enum class VertexSampleReason : unsigned {
    Ok = 0,
    NonFloatVertexType,     // glVertexPointer type != GL_FLOAT
    NoVertexArray,          // no client vertex array pointer recorded (null)
    ZeroCount,              // glDrawElements count == 0
    UnsupportedVertexSize,  // glVertexPointer size outside 2..4
    UnknownIndexType,       // index type not GL_UNSIGNED_BYTE/SHORT/INT
};

struct VertexSample {
    bool valid = false;       // the array was decodable and at least one vertex was read
    unsigned read = 0;        // vertices actually read
    unsigned count = 0;       // the draw's index count (total vertices, not just sampled)
    unsigned indexType = 0;   // as passed to glDrawElements
    unsigned vertexType = 0;  // as recorded by glVertexPointer
    unsigned vertexSize = 0;  // components per vertex as recorded by glVertexPointer
    int stride = 0;           // bytes between vertices (0 = tightly packed)
    bool pointerSet = false;  // a non-null client vertex array pointer was recorded
    // GH #76/#90 (ADR-0507): whether a buffer object was bound to GL_ARRAY_BUFFER
    // when this draw was sampled. A non-zero id beside a null vertex pointer is the
    // buffer-object idiom (offset 0 into the buffer), i.e. the array *is* set -- just
    // not in client memory -- which is the discriminator ADR-0506 needed.
    bool arrayBufferBound = false;
    unsigned arrayBufferId = 0;
    // GH #76/#90 (ADR-0508): the generic-attribute path at this draw. ADR-0507
    // measured that the null fixed-function vertex pointer is the engine's own
    // doing (83% of glVertexPointer calls pass NULL) and that the whole core
    // buffer-object set plus glVertexAttribPointerARB are used ~700x/frame, so the
    // world geometry is on this path, not the client array the sampler reads. These
    // are stamped by the proxy at the draw (the pure sampler does not know them),
    // so an undecodable draw can still be classified as the engine's 3-D geometry
    // rather than guessed at.
    bool genericAttributeEnabled = false;   // a generic attribute array was enabled
    unsigned genericPositionBufferId = 0;   // its position array's buffer (0 = client)
    VertexSampleReason reason = VertexSampleReason::Ok;  // why, when !valid
    float first[3] = {};      // the first sampled vertex
    float min[3] = {};        // axis-aligned bounds over the sampled vertices
    float max[3] = {};
};

// True when a *valid* sample's sampled bounds lie entirely within
// [-kNearFieldMaxAbs, +kNearFieldMaxAbs] on every axis. Pure; used both by the
// observer (to pick the candidate) and by the harness (to re-check the logged
// bounds against its own copy of the constant).
inline bool IsNearFieldSample(const VertexSample& s) {
    if (!s.valid) return false;
    for (int c = 0; c < 3; ++c) {
        if (!(s.min[c] >= -kNearFieldMaxAbs) || !(s.max[c] <= kNearFieldMaxAbs)) return false;
    }
    return true;
}

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
    s.vertexSize = vertexSize;
    s.stride = stride;
    s.pointerSet = vertices != nullptr;
    s.count = count;                               // the draw's size, logged even when undecodable
    if (vertexType != kGlFloat) {                  // only float arrays are decoded
        s.reason = VertexSampleReason::NonFloatVertexType;
        return s;
    }
    if (!vertices) {                               // nothing to read
        s.reason = VertexSampleReason::NoVertexArray;
        return s;
    }
    if (count == 0u) {
        s.reason = VertexSampleReason::ZeroCount;
        return s;
    }
    if (vertexSize < 2u || vertexSize > 4u) {
        s.reason = VertexSampleReason::UnsupportedVertexSize;
        return s;
    }
    unsigned indexSize;
    switch (indexType) {
        case kGlUnsignedByte:  indexSize = 1u; break;
        case kGlUnsignedShort: indexSize = 2u; break;
        case kGlUnsignedInt:   indexSize = 4u; break;
        default:                                       // unknown index type
            s.reason = VertexSampleReason::UnknownIndexType;
            return s;
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
// first array-draw sample taken outside the first-person window (the World
// control) and the window's largest decodable array draw (the arms candidate,
// ADR-0483), plus the window-independent near-field candidate (ADR-0490).
struct VertexSubmissionSnapshot {
    unsigned arrayInside = 0;     // glDrawElements calls with the window open
    unsigned arrayOutside = 0;    // glDrawElements calls with the window closed
    unsigned listInside = 0;      // glCallList calls with the window open
    unsigned listOutside = 0;     // glCallList calls with the window closed
    bool pointerSet = false;      // a client vertex array was recorded
    unsigned pointerSize = 0;     // components per vertex (2/3/4)
    unsigned pointerType = 0;     // GL type enum of the array
    int pointerStride = 0;        // bytes between vertices (0 = tightly packed)
    // GH #76/#90 (ADR-0507): glVertexPointer calls seen this interval, and how many
    // of them passed a null pointer -- so a run can state whether the null the
    // world draws read is ever *set* by the engine or is only ever stale.
    unsigned pointerCalls = 0;
    unsigned pointerNullCalls = 0;
    bool hasInsideSample = false; // an in-window array draw was sampled
    bool hasOutsideSample = false;
    VertexSample insideSample;    // the first-person candidate (largest in-window mesh)
    VertexSample outsideSample;   // world geometry (the control)
    // GH #67 step 1 (eighth correction, ADR-0490): window-independent.
    bool hasNearSample = false;   // a decodable *near-field* array draw was sampled
    bool nearSampleInside = false;// ... and the window happened to be open for it
    VertexSample nearSample;      // the interval's largest near-field decodable draw
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

    // GH #76/#90 (ADR-0505): a read-only sample of the current client array, for
    // the dual per-eye pass. The pointer state is client GL state and is recorded
    // by NoteVertexPointer whether or not observation is enabled, so this neither
    // enables observation nor changes a draw.
    VertexSample SampleCurrent(unsigned count, unsigned indexType, const void* indices) const {
        return SampleClientVertices(count, indexType, indices, pointerSize_, pointerType_,
                                    pointerStride_, pointer_);
    }

    // glVertexPointer(size, type, stride, pointer). Client state persists across
    // draws, so this is remembered (not per-draw) -- and it is read only while
    // observing, so it can never change a run that did not ask for it.
    void NoteVertexPointer(unsigned size, unsigned type, int stride, const void* pointer) {
        pointerSize_ = size;
        pointerType_ = type;
        pointerStride_ = stride;
        pointer_ = pointer;
        pointerSet_ = pointer != nullptr;
        // GH #76/#90 (ADR-0507): the null/non-null tally. Counted whether or not
        // observation is on (the pointer state above is needed by the dual pass
        // either way); it is a read-only counter and changes no draw.
        ++pointerCalls_;
        if (!pointer) ++pointerNullCalls_;
    }

    // One glDrawElements call, tagged by whether it fell inside the first-person
    // window. The window's *largest* array draw is the first-person candidate
    // (ADR-0483): the arms mesh is the biggest mesh the pass submits, while the
    // FIRST in-window draw was measured to be a flat, screen-space 2-D mesh
    // (`first=(0.5,0.5,0) min=(-0.5,-0.5,0) max=(0.5,0.5,0)`, ADR-0482), so
    // "first" picked the wrong draw. Outside the window the first array draw is
    // the World control and is kept.
    void NoteArrayDraw(bool insideWindow, unsigned count, unsigned indexType, const void* indices) {
        if (!observe_) return;
        if (insideWindow) {
            ++arrayInside_;
            const VertexSample s = SampleClientVertices(count, indexType, indices,
                                                        pointerSize_, pointerType_, pointerStride_, pointer_);
            // Keep the largest *decodable* in-window draw; an undecodable larger
            // draw must not overwrite a valid smaller one (and the first draw is
            // kept when nothing bigger decodes, so `valid=0` is still reported).
            if (!hasInsideSample_ ||
                (s.valid && (!insideSample_.valid || s.count > insideSample_.count))) {
                insideSample_ = s;
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
        // GH #67 step 1 (eighth correction, ADR-0490): the window-independent
        // candidate -- the interval's largest decodable *near-field* array draw.
        // The cheap `count >` guard means only a draw that could win is decoded,
        // so sampling every draw costs almost nothing once one has been found.
        if (!hasNearSample_ || count > nearSample_.count) {
            const VertexSample ns = SampleClientVertices(count, indexType, indices,
                                                         pointerSize_, pointerType_, pointerStride_, pointer_);
            if (IsNearFieldSample(ns) &&
                (!hasNearSample_ || ns.count > nearSample_.count)) {
                nearSample_ = ns;
                nearSampleInside_ = insideWindow;
                hasNearSample_ = true;
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
        snap.pointerCalls = pointerCalls_;
        snap.pointerNullCalls = pointerNullCalls_;
        snap.hasInsideSample = hasInsideSample_;
        snap.hasOutsideSample = hasOutsideSample_;
        snap.insideSample = insideSample_;
        snap.outsideSample = outsideSample_;
        snap.hasNearSample = hasNearSample_;
        snap.nearSampleInside = nearSampleInside_;
        snap.nearSample = nearSample_;
        arrayInside_ = arrayOutside_ = listInside_ = listOutside_ = 0u;
        pointerCalls_ = 0;
        pointerNullCalls_ = 0;
        hasInsideSample_ = hasOutsideSample_ = false;
        insideSample_ = VertexSample{};
        outsideSample_ = VertexSample{};
        hasNearSample_ = false;
        nearSampleInside_ = false;
        nearSample_ = VertexSample{};
        return snap;
    }

    void Reset() {
        arrayInside_ = arrayOutside_ = listInside_ = listOutside_ = 0u;
        hasInsideSample_ = hasOutsideSample_ = false;
        insideSample_ = VertexSample{};
        outsideSample_ = VertexSample{};
        hasNearSample_ = false;
        nearSampleInside_ = false;
        nearSample_ = VertexSample{};
        pointerSize_ = pointerType_ = 0u;
        pointerStride_ = 0;
        pointer_ = nullptr;
        pointerSet_ = false;
        pointerCalls_ = 0;
        pointerNullCalls_ = 0;
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
    bool hasNearSample_ = false;
    bool nearSampleInside_ = false;
    VertexSample nearSample_;
    unsigned pointerSize_ = 0;
    unsigned pointerType_ = 0;
    int pointerStride_ = 0;
    const void* pointer_ = nullptr;
    bool pointerSet_ = false;
    unsigned pointerCalls_ = 0;
    unsigned pointerNullCalls_ = 0;
};

} // namespace RiddickVR
