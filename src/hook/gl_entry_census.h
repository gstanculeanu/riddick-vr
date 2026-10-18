#pragma once

// GH #76 (epic #57 definition-of-done item 1) / GH #90 -- where the world array is
// set. ADR-0506 measured that *every* undecodable draw in the world span is
// `noarray`: the sampler is handed `GL_FLOAT` (`0x1406`), 3 components, a 56-byte
// stride and `GL_UNSIGNED_SHORT` (`0x1403`) indices, but a **null** client vertex
// pointer. The 2009 `RndrGL.dll`'s static OPENGL32 import table has no buffer-object
// entry point and no `glInterleavedArrays` (ADR-0482; `docs/rea/reference/RndrGL_GL_Path.txt`),
// so a null pointer with a real stride can only come from one of three places:
//
//   1. a **buffer object** (the canonical null-pointer + stride = offset 0 into a
//      bound VBO idiom), reached through a `wglGetProcAddress`'d core
//      `glBindBuffer` -- the ADR-0481 probe only ever looked for `*ARB*` names;
//   2. the **generic attribute** path (`glVertexAttribPointer[ARB]`), which
//      establishes no fixed-function vertex array at all; or
//   3. an array set by a call the hook does not instrument, in which case the
//      pointer the sampler reads is stale.
//
// This header is the pure, headless bookkeeping for the two census questions that
// discriminate those readings, fed from the hook's `wglGetProcAddress` interception
// and its `glBindBuffer`/`glVertexAttribPointer` wrappers:
//
//   * which names the engine *resolves* (`GlProcNameCensus`) -- a name it never asks
//     for cannot be the source; and
//   * whether a buffer object is bound for `GL_ARRAY_BUFFER` at the draw
//     (`ArrayBindingTracker`), plus whether the generic attribute path is called at
//     all (`AttributePathCensus`).
//
// No GL/Windows headers: plain types only, so it is unit-tested headlessly like
// vertex_submission_observe.h / depth_parallax.h.

#include <cstring>

namespace RiddickVR {

constexpr unsigned kGlArrayBuffer        = 0x8892u;
constexpr unsigned kGlElementArrayBuffer = 0x8893u;

// Bounded: a title that resolves more names than this stops recording new ones
// (the ones already recorded stay readable), so the census cannot grow unbounded.
constexpr unsigned kGlProcNameMax = 64;
constexpr unsigned kGlProcNameLen = 44;

class GlProcNameCensus {
public:
    // One `wglGetProcAddress(name)`. Returns true when `name` is seen for the
    // **first** time, so the caller logs each distinct name exactly once and a run
    // cannot flood the log with the per-frame re-resolutions.
    bool Note(const char* name) {
        if (!name) return false;
        ++calls_;
        for (unsigned i = 0; i < count_; ++i) {
            if (std::strcmp(names_[i], name) == 0) {
                ++callsPer_[i];
                return false;
            }
        }
        if (count_ >= kGlProcNameMax) return false;
        std::strncpy(names_[count_], name, kGlProcNameLen - 1);
        names_[count_][kGlProcNameLen - 1] = '\0';
        callsPer_[count_] = 1;
        ++count_;
        return true;
    }

    bool Has(const char* name) const { return CallsFor(name) > 0u; }

    unsigned CallsFor(const char* name) const {
        if (!name) return 0u;
        for (unsigned i = 0; i < count_; ++i) {
            if (std::strcmp(names_[i], name) == 0) return callsPer_[i];
        }
        return 0u;
    }

    unsigned TotalCalls() const { return calls_; }
    unsigned UniqueNames() const { return count_; }
    static constexpr unsigned MaxNames() { return kGlProcNameMax; }
    const char* NameAt(unsigned i) const { return i < count_ ? names_[i] : ""; }
    unsigned CallsAt(unsigned i) const { return i < count_ ? callsPer_[i] : 0u; }

    void Reset() { *this = GlProcNameCensus{}; }

private:
    unsigned calls_ = 0;
    unsigned count_ = 0;
    char names_[kGlProcNameMax][kGlProcNameLen] = {};
    unsigned callsPer_[kGlProcNameMax] = {};
};

// The buffer-object bindings in force. `arrayBufferId` is the id bound to
// `GL_ARRAY_BUFFER`; a non-zero id at a draw is what makes a null vertex pointer
// *valid* (offset 0 into the buffer) instead of unset -- the discriminator ADR-0506
// asked for.
struct ArrayBindingTracker {
    unsigned arrayBufferId = 0;
    unsigned elementBufferId = 0;
    unsigned bindCalls = 0;
    unsigned arrayBinds = 0;
    unsigned elementBinds = 0;
    bool sawArrayBuffer = false;  // a non-zero GL_ARRAY_BUFFER was ever bound

    void NoteBind(unsigned target, unsigned id) {
        ++bindCalls;
        if (target == kGlArrayBuffer) {
            ++arrayBinds;
            arrayBufferId = id;
            if (id != 0u) sawArrayBuffer = true;
        } else if (target == kGlElementArrayBuffer) {
            ++elementBinds;
            elementBufferId = id;
        }
    }

    void Reset() { *this = ArrayBindingTracker{}; }
};

// The generic-attribute path (`glVertexAttribPointer[ARB]` +
// `glEnableVertexAttribArray[ARB]`) and `glInterleavedArrays`. Any of these being
// called says the fixed-function `glVertexPointer` the sampler records is not the
// array the draw actually used.
//
// ADR-0508 (GH #76/#90 Next after ADR-0507): a *per-index* record, because the
// generic-attribute path establishes the array without any fixed-function
// `glVertexPointer` at all, and the decisive question is not "was the path called"
// but "at THIS draw, which array would a re-issued `glDrawElements` read, and is
// its position data in a buffer object". GL keeps an array buffer *per attribute*
// at the moment the pointer is set, so this records the `GL_ARRAY_BUFFER` binding
// in force at each `NoteAttribPointer` rather than the live binding at the draw
// (which is a different attribute's buffer by the time the draw happens).
constexpr unsigned kGlFloatAttrib = 0x1406u;   // GL_FLOAT, for the attribute type
constexpr unsigned kGenericAttribMax = 16;     // the GL minimum; bounded the same way

struct AttributePathCensus {
    unsigned attribPointerCalls = 0;
    unsigned enableArrayCalls = 0;
    unsigned disableArrayCalls = 0;
    unsigned enableClientStateCalls = 0;
    unsigned disableClientStateCalls = 0;
    unsigned interleavedCalls = 0;

    // The last call's shape, kept for the evidence line (unchanged from ADR-0507).
    unsigned lastIndex = 0;
    unsigned lastSize = 0;
    unsigned lastType = 0;
    int lastStride = 0;
    bool lastPointerSet = false;
    unsigned lastArrayBufferId = 0;

    // Per-index generic-attribute state. This is GL state: it persists across
    // draws until the engine disables it, exactly like the fixed-function pointer.
    bool enabled[kGenericAttribMax] = {};
    unsigned size[kGenericAttribMax] = {};
    unsigned type[kGenericAttribMax] = {};
    int stride[kGenericAttribMax] = {};
    bool pointerSet[kGenericAttribMax] = {};
    unsigned arrayBufferId[kGenericAttribMax] = {};  // GL_ARRAY_BUFFER bound at set time

    void NoteAttribPointer(unsigned index, unsigned attrSize, unsigned attrType, int attrStride,
                           const void* pointer, unsigned boundArrayBufferId) {
        ++attribPointerCalls;
        lastIndex = index;
        lastSize = attrSize;
        lastType = attrType;
        lastStride = attrStride;
        lastPointerSet = pointer != nullptr;
        lastArrayBufferId = boundArrayBufferId;
        if (index < kGenericAttribMax) {
            size[index] = attrSize;
            type[index] = attrType;
            stride[index] = attrStride;
            pointerSet[index] = pointer != nullptr;
            arrayBufferId[index] = boundArrayBufferId;
        }
    }
    void NoteEnableArray(unsigned index) {
        ++enableArrayCalls;
        if (index < kGenericAttribMax) enabled[index] = true;
    }
    void NoteDisableArray(unsigned index) {
        ++disableArrayCalls;
        if (index < kGenericAttribMax) enabled[index] = false;
    }
    // glEnableClientState/glDisableClientState (the fixed-function path); counted
    // so a run can state whether the fixed-function vertex array is even enabled.
    void NoteEnableClientState() { ++enableClientStateCalls; }
    void NoteDisableClientState() { ++disableClientStateCalls; }
    void NoteInterleaved() { ++interleavedCalls; }

    unsigned EnabledCount() const {
        unsigned n = 0;
        for (unsigned i = 0; i < kGenericAttribMax; ++i) {
            if (enabled[i]) ++n;
        }
        return n;
    }
    bool AnyEnabled() const { return EnabledCount() != 0u; }

    // The position-like generic attribute: the lowest enabled index holding a
    // 3+-component GL_FLOAT array -- the shape every measured world draw uses
    // (GL_FLOAT, 3 components, stride 56; ADR-0506/0507). -1 when there is none.
    int PositionAttribIndex() const {
        int best = -1;
        unsigned bestSize = 0;
        for (unsigned i = 0; i < kGenericAttribMax; ++i) {
            if (!enabled[i] || type[i] != kGlFloatAttrib || size[i] < 3u) continue;
            if (best < 0 || size[i] > bestSize) {
                best = static_cast<int>(i);
                bestSize = size[i];
            }
        }
        return best;
    }
    // The buffer object that holds that attribute's data (0 = client memory).
    unsigned PositionBufferId() const {
        const int i = PositionAttribIndex();
        return i >= 0 ? arrayBufferId[i] : 0u;
    }
    bool PositionBufferBacked() const { return PositionBufferId() != 0u; }

    void Reset() { *this = AttributePathCensus{}; }
};

} // namespace RiddickVR
