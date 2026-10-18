// GH #76 (epic #57 DoD item 1) / GH #90: unit tests for the array-source census
// (gl_entry_census.h) and the sampler's recorded-pointer tally
// (vertex_submission_observe.h). Headless, no GL, no game launch.
//
// The decisive assertions are the ones that discriminate ADR-0506's leading
// explanation (the world array is established where the draw hook cannot see it):
//   * the dynamic entry-point census logs each distinct resolved name once;
//   * a non-zero GL_ARRAY_BUFFER binding is visible beside a null vertex pointer
//     (the buffer-object idiom), and is not confused with the element buffer;
//   * the generic-attribute path is counted separately from glInterleavedArrays;
//   * the sampler records how many glVertexPointer calls passed a null pointer; and
//   * the span census carries the array-buffer binding of its first rejection.
#include "gl_entry_census.h"
#include "stereo_dual.h"

#include <cstdio>

using RiddickVR::ArrayBindingTracker;
using RiddickVR::AttributePathCensus;
using RiddickVR::GlProcNameCensus;
using RiddickVR::kGlArrayBuffer;
using RiddickVR::kGlElementArrayBuffer;
using RiddickVR::SpanDrawCensus;
using RiddickVR::VertexSample;
using RiddickVR::VertexSampleReason;
using RiddickVR::VertexSubmissionObserver;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "gl entry census test failed: %s\n", message);
        ++g_failures;
    }
}

int main() {
    // 1. The name census counts every call but reports each distinct name once, so
    //    a per-frame re-resolution cannot flood the log with duplicates.
    {
        GlProcNameCensus pc;
        Check(pc.Note("glBindBuffer") == true, "first resolve of a name is new");
        Check(pc.Note("glBindBuffer") == false, "a repeated resolve is not new");
        Check(pc.Note("glBindBuffer") == false, "a third resolve is not new");
        Check(pc.Note("glVertexAttribPointerARB") == true, "a second name is new");
        Check(pc.UniqueNames() == 2u, "two distinct names recorded");
        Check(pc.TotalCalls() == 4u, "all four resolutions counted");
        Check(pc.CallsFor("glBindBuffer") == 3u, "per-name call count");
        Check(pc.Has("glVertexAttribPointerARB"), "Has() finds a recorded name");
        Check(!pc.Has("glInterleavedArrays"), "Has() is false for an unrecorded name");
        Check(pc.CallsFor("glMapBuffer") == 0u, "an unrecorded name has zero calls");
        Check(pc.Note(nullptr) == false, "a null name is ignored");
        Check(pc.TotalCalls() == 4u, "a null name is not counted");
    }

    // 2. The name table is bounded: past the cap new names are dropped, not
    //    corrupting the table, and the recorded ones stay readable.
    {
        GlProcNameCensus pc;
        char name[32];
        for (unsigned i = 0; i < GlProcNameCensus::MaxNames(); ++i) {
            std::snprintf(name, sizeof(name), "glProc%u", i);
            Check(pc.Note(name) == true, "a name under the cap is recorded");
        }
        Check(pc.UniqueNames() == GlProcNameCensus::MaxNames(), "the table is full");
        Check(pc.Note("glOverflow") == false, "a name past the cap is dropped");
        Check(pc.UniqueNames() == GlProcNameCensus::MaxNames(), "the cap is not exceeded");
        Check(pc.NameAt(0) != nullptr && pc.NameAt(0)[0] != '\0', "recorded names stay readable");
        Check(pc.NameAt(GlProcNameCensus::MaxNames())[0] == '\0',
              "an out-of-range index reads an empty name");
    }

    // 3. The binding tracker distinguishes the array buffer from the element buffer
    //    and remembers "a non-zero ARRAY_BUFFER was ever bound" (the buffer-object
    //    idiom in front of a null vertex pointer).
    {
        ArrayBindingTracker ab;
        Check(ab.arrayBufferId == 0u && !ab.sawArrayBuffer, "nothing bound initially");
        ab.NoteBind(kGlElementArrayBuffer, 7u);
        Check(ab.arrayBufferId == 0u, "binding the element buffer leaves the array buffer alone");
        Check(ab.elementBufferId == 7u, "the element buffer id is remembered");
        Check(!ab.sawArrayBuffer, "the element buffer does not set sawArrayBuffer");
        ab.NoteBind(kGlArrayBuffer, 12u);
        Check(ab.arrayBufferId == 12u, "the array buffer id is remembered");
        Check(ab.sawArrayBuffer, "a non-zero array buffer sets sawArrayBuffer");
        Check(ab.bindCalls == 2u && ab.arrayBinds == 1u && ab.elementBinds == 1u,
              "the bind tallies are separate");
        ab.NoteBind(kGlArrayBuffer, 0u);
        Check(ab.arrayBufferId == 0u, "unbinding clears the live id");
        Check(ab.sawArrayBuffer, "the historical sighting is not cleared by an unbind");
    }

    // 4. The generic-attribute path and glInterleavedArrays are counted separately.
    {
        AttributePathCensus ap;
        Check(ap.attribPointerCalls == 0u && ap.interleavedCalls == 0u, "nothing called initially");
        ap.NoteAttribPointer(3u, 3u, 0x1406u, 56, reinterpret_cast<const void*>(0x40));
        Check(ap.attribPointerCalls == 1u, "a generic attribute pointer is counted");
        Check(ap.lastIndex == 3u && ap.lastSize == 3u && ap.lastType == 0x1406u && ap.lastStride == 56,
              "the last attribute pointer is recorded");
        Check(ap.lastPointerSet, "a non-null attribute pointer is recorded as set");
        ap.NoteAttribPointer(0u, 3u, 0x1406u, 56, nullptr);
        Check(!ap.lastPointerSet, "a null attribute pointer is recorded as unset");
        ap.NoteEnableArray();
        ap.NoteInterleaved();
        Check(ap.enableArrayCalls == 1u, "the enable-array call is counted");
        Check(ap.interleavedCalls == 1u, "glInterleavedArrays is counted separately");
        Check(ap.attribPointerCalls == 2u, "the attribute-pointer count is not disturbed");
    }

    // 5. The sampler's tally says how many glVertexPointer calls passed a null
    //    pointer -- the state ADR-0506 measured the world draws reading. The tally
    //    is unconditional (the pointer state is needed by the dual pass either way).
    {
        VertexSubmissionObserver obs;
        obs.NoteVertexPointer(3u, 0x1406u, 56, reinterpret_cast<const void*>(0x1000));
        obs.NoteVertexPointer(3u, 0x1406u, 56, nullptr);
        obs.NoteVertexPointer(3u, 0x1406u, 56, nullptr);
        const RiddickVR::VertexSubmissionSnapshot snap = obs.ComputeSnapshot();
        Check(snap.pointerCalls == 3u, "every glVertexPointer call is counted");
        Check(snap.pointerNullCalls == 2u, "the null-pointer subset is counted");
        Check(snap.pointerSize == 3u && snap.pointerType == 0x1406u && snap.pointerStride == 56,
              "the last recorded array state survives");
    }

    // 6. A `noarray` rejection carries the buffer binding that was in force, and the
    //    span census preserves the first rejection's binding (ADR-0507).
    {
        VertexSample bufBacked;
        bufBacked.vertexType = 0x1406u;
        bufBacked.vertexSize = 3u;
        bufBacked.stride = 56;
        bufBacked.pointerSet = false;
        bufBacked.count = 12u;
        bufBacked.indexType = 0x1403u;
        bufBacked.reason = VertexSampleReason::NoVertexArray;
        bufBacked.arrayBufferBound = true;
        bufBacked.arrayBufferId = 12u;
        SpanDrawCensus census;
        census.Add(bufBacked);
        Check(census.reasonNoArray == 1u, "the null-pointer draw is bucketed as noarray");
        Check(census.rejectArrayBufferBound, "the first rejection reports its array buffer binding");
        Check(census.rejectArrayBufferId == 12u, "the first rejection reports the buffer id");
        census.Reset();
        Check(census.rejectArrayBufferId == 0u && !census.rejectArrayBufferBound,
              "Reset clears the rejection signature");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "gl entry census: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("gl entry census: all checks passed\n");
    return 0;
}
