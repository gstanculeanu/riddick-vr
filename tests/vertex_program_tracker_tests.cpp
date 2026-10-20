// GH #67 step 1 (corrected, ADR-0481): unit tests for the ARB vertex-program
// observation (vertex_program_observe.h).
//
// These pin the semantics the live capture relies on:
//   - inert until Configure(true): every Note* is a no-op and the snapshot is
//     all zeros (so enabling observation can never change a run that did not
//     ask for it),
//   - strings / binds / env-parameter counts and the last bound program id,
//   - four *consecutive* glProgramEnvParameter4fvARB indices assemble a matrix
//     (recorded in index order), a gap does not, and the last quad wins,
//   - ComputeSnapshot publishes and re-bases,
//   - Fnv1a gives a program string a stable, discriminating identity.
#include "vertex_program_observe.h"

#include <cstdio>
#include <cmath>

using RiddickVR::VertexProgramObserver;
using RiddickVR::VertexProgramSnapshot;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "vertex program observer test failed: %s\n", message);
        ++g_failures;
    }
}

static void Param4(VertexProgramObserver& o, unsigned index, float a, float b, float c, float d) {
    const float v[4] = {a, b, c, d};
    o.NoteEnvParameter(index, v);
}

int main() {
    // Inert by default: observation is opt-in (ViewmodelObserve=1).
    {
        VertexProgramObserver o;
        Check(!o.Observing(), "a fresh observer must not be observing");
        o.NoteProgramString(64, 0x1234u);
        o.NoteBindProgram(7u);
        Param4(o, 0u, 1.0f, 2.0f, 3.0f, 4.0f);
        const VertexProgramSnapshot s = o.ComputeSnapshot();
        Check(s.strings == 0 && s.binds == 0 && s.envParams == 0,
              "an inert observer must not count ARB program calls");
        Check(!s.matrixAssembled && s.lastProgramId == 0,
              "an inert observer must not publish a program or a matrix");
    }

    // Counting: strings, binds, env params, last program id, last string length.
    {
        VertexProgramObserver o;
        o.Configure(true);
        Check(o.Observing(), "Configure(true) must turn observation on");
        o.NoteProgramString(128, VertexProgramObserver::Fnv1a("!!ARBvp1.0", 10));
        o.NoteBindProgram(3u);
        o.NoteBindProgram(9u);
        Param4(o, 0u, 1.0f, 0.0f, 0.0f, 0.0f);
        Param4(o, 5u, 0.0f, 1.0f, 0.0f, 0.0f);
        const VertexProgramSnapshot s = o.ComputeSnapshot();
        Check(s.strings == 1, "one glProgramStringARB must be counted");
        Check(s.binds == 2, "two glBindProgramARB calls must be counted");
        Check(s.envParams == 2, "two env-parameter calls must be counted");
        Check(s.lastProgramId == 9u, "the last bound program id must be kept");
        Check(s.lastStringLen == 128u, "the last program string length must be kept");
        Check(s.lastStringHash == VertexProgramObserver::Fnv1a("!!ARBvp1.0", 10),
              "the program string hash must be published");
    }

    // A program string may be re-defined; the hash must be over its own bytes.
    {
        const unsigned a = VertexProgramObserver::Fnv1a("!!ARBvp1.0\n", 11);
        const unsigned b = VertexProgramObserver::Fnv1a("!!ARBfp1.0\n", 11);
        Check(VertexProgramObserver::Fnv1a("!!ARBvp1.0\n", 11) == a,
              "Fnv1a must be deterministic");
        Check(a != b, "different program strings must hash differently (a != b)");
    }

    // Four consecutive env-parameter indices assemble one matrix, in order.
    {
        VertexProgramObserver o;
        o.Configure(true);
        Param4(o, 20u, 1.0f, 2.0f, 3.0f, 4.0f);
        Param4(o, 21u, 5.0f, 6.0f, 7.0f, 8.0f);
        Param4(o, 22u, 9.0f, 10.0f, 11.0f, 12.0f);
        const VertexProgramSnapshot mid = o.ComputeSnapshot();
        Check(!mid.matrixAssembled,
              "three consecutive env parameters must not assemble a matrix yet");
        o.Configure(true);
        Param4(o, 20u, 1.0f, 2.0f, 3.0f, 4.0f);
        Param4(o, 21u, 5.0f, 6.0f, 7.0f, 8.0f);
        Param4(o, 22u, 9.0f, 10.0f, 11.0f, 12.0f);
        Param4(o, 23u, 13.0f, 14.0f, 15.0f, 16.0f);
        const VertexProgramSnapshot s = o.ComputeSnapshot();
        Check(s.matrixAssembled, "four consecutive env parameters must assemble a matrix");
        Check(s.matrixBase == 20u, "the matrix base must be the first index of the quad");
        const float kExpected[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        for (int i = 0; i < 16; ++i) {
            Check(std::fabs(s.matrix[i] - kExpected[i]) < 1e-6f,
                  "the assembled matrix must hold the four vec4s in index order");
        }
    }

    // A gap in the indices must NOT assemble (a per-object param interleaved
    // with others cannot silently become a matrix).
    {
        VertexProgramObserver o;
        o.Configure(true);
        Param4(o, 0u, 1.0f, 0.0f, 0.0f, 0.0f);
        Param4(o, 1u, 0.0f, 1.0f, 0.0f, 0.0f);
        Param4(o, 3u, 0.0f, 0.0f, 1.0f, 0.0f);
        Param4(o, 4u, 0.0f, 0.0f, 0.0f, 1.0f);
        const VertexProgramSnapshot s = o.ComputeSnapshot();
        Check(!s.matrixAssembled, "a non-consecutive index run must not assemble a matrix");
        Check(s.envParams == 4, "all four env parameters are still counted");
    }

    // The last complete quad wins (a program env block may re-upload per pass).
    {
        VertexProgramObserver o;
        o.Configure(true);
        for (unsigned i = 0; i < 4u; ++i) Param4(o, i, 1.0f, 0.0f, 0.0f, 0.0f);
        for (unsigned i = 4u; i < 8u; ++i) Param4(o, i, float(i), 0.0f, 0.0f, 0.0f);
        const VertexProgramSnapshot s = o.ComputeSnapshot();
        Check(s.matrixAssembled && s.matrixBase == 4u,
              "a second complete quad must replace the first and move the base");
        Check(s.matrix[0] == 4.0f && s.matrix[12] == 7.0f,
              "the published matrix must be the last complete quad");
    }

    // An index restart (wrap back to 0) starts a new, valid run.
    {
        VertexProgramObserver o;
        o.Configure(true);
        Param4(o, 0u, 1.0f, 0.0f, 0.0f, 0.0f);
        Param4(o, 1u, 0.0f, 1.0f, 0.0f, 0.0f);
        Param4(o, 2u, 0.0f, 0.0f, 1.0f, 0.0f);
        Param4(o, 0u, 4.0f, 0.0f, 0.0f, 0.0f);
        Param4(o, 1u, 0.0f, 4.0f, 0.0f, 0.0f);
        Param4(o, 2u, 0.0f, 0.0f, 4.0f, 0.0f);
        Param4(o, 3u, 0.0f, 0.0f, 0.0f, 4.0f);
        const VertexProgramSnapshot s = o.ComputeSnapshot();
        Check(s.matrixAssembled && s.matrixBase == 0u && s.matrix[0] == 4.0f,
              "a wrapped index run must assemble the new quad, not the stale one");
    }

    // ComputeSnapshot re-bases: the next frame starts from zero.
    {
        VertexProgramObserver o;
        o.Configure(true);
        o.NoteProgramString(8, 1u);
        o.NoteBindProgram(1u);
        (void)o.ComputeSnapshot();
        const VertexProgramSnapshot s = o.ComputeSnapshot();
        Check(s.strings == 0 && s.binds == 0 && s.envParams == 0 && !s.matrixAssembled,
              "ComputeSnapshot must re-base the frame counters");
    }

    // Configure(false) drops any partial state (a flag flip cannot leak a matrix).
    {
        VertexProgramObserver o;
        o.Configure(true);
        Param4(o, 0u, 1.0f, 0.0f, 0.0f, 0.0f);
        o.Configure(false);
        o.Configure(true);
        const VertexProgramSnapshot s = o.ComputeSnapshot();
        Check(!s.matrixAssembled && s.envParams == 0,
              "disabling observation must drop partial assembly state");
    }

    // GH #76/#90 (ADR-0515): Current() is a *live* read -- it names the program bound
    // at a draw without publishing or re-basing the frame's tallies.
    {
        VertexProgramObserver o;
        const RiddickVR::VertexProgramState untouched = o.Current();
        Check(!untouched.observing && untouched.programId == 0,
              "an observation-off Current() reports observing=false, not a silent zero");

        o.Configure(true);
        o.NoteProgramString(64, VertexProgramObserver::Fnv1a("!!ARBvp1.0", 10));
        o.NoteBindProgram(11u);
        Param4(o, 4u, 0.032f, 0.0f, 0.0f, 1.0f);
        Param4(o, 5u, 0.0f, 1.0f, 0.0f, 0.0f);
        Param4(o, 6u, 0.0f, 0.0f, 1.0f, 0.0f);
        Param4(o, 7u, 0.0f, 0.0f, 0.0f, 1.0f);

        const RiddickVR::VertexProgramState live = o.Current();
        Check(live.observing, "Current() reports the observation switch");
        Check(live.programId == 11u, "Current() carries the bound program id");
        Check(live.programHash == VertexProgramObserver::Fnv1a("!!ARBvp1.0", 10),
              "Current() carries the program string hash");
        Check(live.envParams == 4u && live.matrixAssembled && live.matrixBase == 4u,
              "Current() carries the assembled env-parameter matrix");
        Check(std::fabs(live.matrix[0] - 0.032f) < 1e-6f,
              "Current() carries the matrix values in index order");

        // Reading again must not re-base: the same values survive until a snapshot.
        const RiddickVR::VertexProgramState again = o.Current();
        Check(again.programId == 11u && again.envParams == 4u,
              "Current() must not re-base the frame tallies");
        (void)o.ComputeSnapshot();
        const RiddickVR::VertexProgramState after = o.Current();
        Check(after.programId == 0u && !after.matrixAssembled,
              "a snapshot re-bases what Current() later reports");
    }

    if (g_failures == 0) {
        std::printf("vertex program observer: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "vertex program observer: %d failure(s)\n", g_failures);
    return 1;
}
