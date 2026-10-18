// GH #67 step 1 (second correction, ADR-0482): unit tests for the submitted-vertex
// observation (vertex_submission_observe.h).
//
// These pin the semantics the live capture relies on:
//   - inert until Configure(true): every Note* is a no-op and the snapshot is all
//     zeros (so enabling observation can never change a run that did not ask),
//   - glVertexPointer is remembered as client state (size/type/stride/pointer),
//   - a float client array is decoded: first vertex + axis-aligned bounds over a
//     spread of up to kMaxVertexSamples vertices, honouring the stride and all
//     three index widths,
//   - anything not decodable (non-float vertices, unknown index type, null
//     pointer, count 0) is reported invalid rather than guessed,
//   - the in-window sample is the window's largest decodable array draw (ADR-0483:
//     the arms mesh, not the window's first flat 2-D draw) and carries the draw's
//     index count; the first array draw outside the window is the world control;
//     display-list draws are counted by side,
//   - ComputeSnapshot publishes + re-bases but keeps the client pointer state.
#include "vertex_submission_observe.h"

#include <cstdio>
#include <cstring>

using RiddickVR::VertexSample;
using RiddickVR::VertexSubmissionObserver;
using RiddickVR::VertexSubmissionSnapshot;
using RiddickVR::SampleClientVertices;
using RiddickVR::kGlFloat;
using RiddickVR::kGlUnsignedByte;
using RiddickVR::kGlUnsignedShort;
using RiddickVR::kGlUnsignedInt;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "vertex submission observer test failed: %s\n", message);
        ++g_failures;
    }
}

static bool Near(float a, float b) { return (a - b) < 1e-5f && (b - a) < 1e-5f; }

static bool VecIs(const float* v, float x, float y, float z) {
    return Near(v[0], x) && Near(v[1], y) && Near(v[2], z);
}

int main() {
    const float tri[9] = {0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 3.0f, 10.0f, -5.0f, 4.0f};

    // Inert by default: observation is opt-in (ViewmodelObserve=1).
    {
        VertexSubmissionObserver o;
        Check(!o.Observing(), "a fresh observer must not be observing");
        o.NoteVertexPointer(3, kGlFloat, 0, tri);
        const unsigned char idx[3] = {0, 1, 2};
        o.NoteArrayDraw(true, 3, kGlUnsignedByte, idx);
        o.NoteArrayDraw(false, 3, kGlUnsignedByte, idx);
        o.NoteListDraw(true);
        const VertexSubmissionSnapshot s = o.ComputeSnapshot();
        Check(s.arrayInside == 0 && s.arrayOutside == 0 && s.listInside == 0 && s.listOutside == 0,
              "an inert observer must not count draws");
        Check(!s.hasInsideSample && !s.hasOutsideSample,
              "an inert observer must not sample geometry");
    }

    // A float array with ubyte indices: first vertex + bounds, spread sampling.
    {
        VertexSubmissionObserver o;
        o.Configure(true);
        o.NoteVertexPointer(3, kGlFloat, 0, tri);
        const unsigned char idx[3] = {2, 0, 1};
        o.NoteArrayDraw(true, 3, kGlUnsignedByte, idx);
        const VertexSubmissionSnapshot s = o.ComputeSnapshot();
        Check(s.arrayInside == 1 && s.arrayOutside == 0, "the in-window array draw is counted");
        Check(s.pointerSet && s.pointerSize == 3 && s.pointerType == kGlFloat && s.pointerStride == 0,
              "glVertexPointer state must be published");
        Check(s.hasInsideSample && s.insideSample.valid && s.insideSample.read == 3,
              "the in-window sample must be valid and cover all three vertices");
        Check(VecIs(s.insideSample.first, 10.0f, -5.0f, 4.0f),
              "the first sampled vertex must be index[0] of the index list");
        Check(VecIs(s.insideSample.min, 0.0f, -5.0f, 0.0f) &&
              VecIs(s.insideSample.max, 10.0f, 2.0f, 4.0f),
              "bounds must cover every sampled vertex");
    }

    // A stride is honoured (an interleaved array must not be read as packed).
    {
        const float interleaved[12] = {1, 2, 3, 99, 99, 99, 4, 5, 6, 88, 88, 88};
        const VertexSample s = SampleClientVertices(2, kGlUnsignedByte, nullptr, 3, kGlFloat, 24, interleaved);
        Check(s.valid && s.read == 2, "a strided float array must decode");
        Check(VecIs(s.first, 1.0f, 2.0f, 3.0f), "the first vertex must be read from the array base");
        Check(VecIs(s.max, 4.0f, 5.0f, 6.0f),
              "the second vertex must be read at base + stride, not base + 12 bytes");
    }

    // A large draw is sampled with a spread that reaches the last vertex.
    {
        float big[3 * 100];
        for (int i = 0; i < 100; ++i) {
            big[i * 3 + 0] = static_cast<float>(i);
            big[i * 3 + 1] = 0.0f;
            big[i * 3 + 2] = 0.0f;
        }
        const VertexSample s = SampleClientVertices(100, kGlUnsignedByte, nullptr, 3, kGlFloat, 0, big);
        Check(s.valid && s.read == RiddickVR::kMaxVertexSamples,
              "a 100-vertex draw must be capped at kMaxVertexSamples");
        Check(Near(s.min[0], 0.0f) && Near(s.max[0], 99.0f),
              "the spread must reach both the first and the last vertex");
    }

    // All three index widths are decoded (ubyte/short/uint).
    {
        const unsigned short idx16[2] = {2, 0};
        const unsigned int idx32[2] = {2, 1};
        const VertexSample a = SampleClientVertices(2, kGlUnsignedShort, idx16, 3, kGlFloat, 0, tri);
        const VertexSample b = SampleClientVertices(2, kGlUnsignedInt, idx32, 3, kGlFloat, 0, tri);
        Check(a.valid && VecIs(a.first, 10.0f, -5.0f, 4.0f), "unsigned-short indices must decode");
        Check(b.valid && VecIs(b.first, 10.0f, -5.0f, 4.0f), "unsigned-int indices must decode");
    }

    // A 2-component array yields z = 0 rather than a garbage third component.
    {
        const float pos2[4] = {7.0f, 8.0f, -1.0f, -2.0f};
        const VertexSample s = SampleClientVertices(2, kGlUnsignedByte, nullptr, 2, kGlFloat, 0, pos2);
        Check(s.valid && VecIs(s.first, 7.0f, 8.0f, 0.0f), "a 2-component array must decode x/y with z = 0");
    }

    // Not decodable -> invalid (never a plausible-looking wrong number).
    {
        const unsigned char idx[1] = {0};
        const VertexSample shortVerts =
            SampleClientVertices(1, kGlUnsignedByte, idx, 3, 0x1402u /* GL_SHORT */, 0, tri);
        const VertexSample noPtr = SampleClientVertices(1, kGlUnsignedByte, idx, 3, kGlFloat, 0, nullptr);
        const VertexSample badIdx = SampleClientVertices(1, 0x1407u, idx, 3, kGlFloat, 0, tri);
        const VertexSample empty = SampleClientVertices(0, kGlUnsignedByte, idx, 3, kGlFloat, 0, tri);
        Check(!shortVerts.valid && shortVerts.read == 0, "a non-float array must not be decoded");
        Check(!noPtr.valid, "a null vertex pointer must not be decoded");
        Check(!badIdx.valid, "an unknown index type must not be decoded");
        Check(!empty.valid, "an empty draw must not be decoded");
    }

    // The first array draw outside the window is the world control; the in-window
    // sample is the window's largest decodable draw (ADR-0483).
    {
        VertexSubmissionObserver o;
        o.Configure(true);
        o.NoteVertexPointer(3, kGlFloat, 0, tri);
        const unsigned char first[1] = {0};   // -> vertex 0
        const unsigned char later[1] = {2};   // -> vertex 2
        o.NoteArrayDraw(false, 1, kGlUnsignedByte, first);
        o.NoteArrayDraw(false, 1, kGlUnsignedByte, later);
        o.NoteArrayDraw(true, 1, kGlUnsignedByte, later);
        o.NoteListDraw(true);
        o.NoteListDraw(false);
        const VertexSubmissionSnapshot s = o.ComputeSnapshot();
        Check(s.arrayOutside == 2 && s.arrayInside == 1, "every array draw must be counted");
        Check(s.listInside == 1 && s.listOutside == 1, "display-list draws must be counted by side");
        Check(s.hasOutsideSample && VecIs(s.outsideSample.first, 0.0f, 0.0f, 0.0f),
              "the first world array draw is the world sample");
        Check(s.hasInsideSample && VecIs(s.insideSample.first, 10.0f, -5.0f, 4.0f),
              "a single in-window array draw is the first-person sample");
        Check(s.insideSample.count == 1, "the sample must carry the draw's index count");
    }

    // The in-window sample prefers the LARGEST decodable draw: a flat screen-space
    // quad first, then the arms mesh, must yield the mesh (ADR-0483).
    {
        // A screen-space quad (the window's first draw) and a 6-vertex mesh.
        const float quad[12] = {0.5f, 0.5f, 0.0f, -0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.0f,
                                -0.5f, 0.5f, 0.0f};
        const float mesh[18] = {0.1f, 0.2f, 1.0f, -0.3f, 0.4f, 1.5f, 0.7f, -0.8f, 2.0f,
                                0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 2.0f, 2.0f, 2.0f};
        VertexSubmissionObserver o;
        o.Configure(true);
        // The quad first (count 4), then the mesh (count 6): the mesh must win.
        o.NoteVertexPointer(3, kGlFloat, 0, quad);
        o.NoteArrayDraw(true, 4, kGlUnsignedByte, nullptr);
        o.NoteVertexPointer(3, kGlFloat, 0, mesh);
        o.NoteArrayDraw(true, 6, kGlUnsignedByte, nullptr);
        const VertexSubmissionSnapshot s = o.ComputeSnapshot();
        Check(s.arrayInside == 2, "both in-window draws must be counted");
        Check(s.hasInsideSample && s.insideSample.count == 6,
              "the in-window sample must be the largest draw, not the first");
        Check(VecIs(s.insideSample.first, 0.1f, 0.2f, 1.0f) &&
              VecIs(s.insideSample.max, 2.0f, 2.0f, 2.0f),
              "the largest in-window draw's geometry must be the sample");
    }

    // A larger but undecodable in-window draw must not clobber a valid smaller one.
    {
        const float mesh[9] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f};
        VertexSubmissionObserver o;
        o.Configure(true);
        o.NoteVertexPointer(3, kGlFloat, 0, mesh);
        o.NoteArrayDraw(true, 3, kGlUnsignedByte, nullptr);      // valid, count 3
        o.NoteVertexPointer(3, 0x1402u /* GL_SHORT */, 0, mesh); // undecodable type
        o.NoteArrayDraw(true, 900, kGlUnsignedByte, nullptr);    // larger, invalid
        const VertexSubmissionSnapshot s = o.ComputeSnapshot();
        Check(s.hasInsideSample && s.insideSample.valid && s.insideSample.count == 3,
              "an undecodable larger draw must not replace a valid in-window sample");
    }

    // ComputeSnapshot re-bases the interval but keeps the client pointer state.
    {
        VertexSubmissionObserver o;
        o.Configure(true);
        o.NoteVertexPointer(3, kGlFloat, 0, tri);
        const unsigned char idx[1] = {1};
        o.NoteArrayDraw(false, 1, kGlUnsignedByte, idx);
        (void)o.ComputeSnapshot();
        o.NoteArrayDraw(false, 1, kGlUnsignedByte, idx);
        const VertexSubmissionSnapshot s = o.ComputeSnapshot();
        Check(s.arrayOutside == 1 && !s.hasInsideSample,
              "ComputeSnapshot must re-base the interval counters");
        Check(s.pointerSet && s.pointerSize == 3,
              "the client vertex-array state must survive ComputeSnapshot");
    }

    // Configure(false) drops everything (a flag flip cannot leak a sample).
    {
        VertexSubmissionObserver o;
        o.Configure(true);
        o.NoteVertexPointer(3, kGlFloat, 0, tri);
        const unsigned char idx[1] = {0};
        o.NoteArrayDraw(true, 1, kGlUnsignedByte, idx);
        o.Configure(false);
        o.Configure(true);
        const VertexSubmissionSnapshot s = o.ComputeSnapshot();
        Check(!s.pointerSet && !s.hasInsideSample && s.arrayInside == 0,
              "disabling observation must drop the pointer state and any sample");
    }

    if (g_failures == 0) {
        std::printf("vertex submission observer: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "vertex submission observer: %d failure(s)\n", g_failures);
    return 1;
}
