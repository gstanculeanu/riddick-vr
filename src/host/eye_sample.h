#pragma once

// GH #79: the XRSIM eye-content sampler, as a pure helper.
//
// The content gate (GH #72 requirement 2) needs two numbers from an eye image:
// the full-image mean luma, to separate the all-black GL<->D3D11 interop warm-up
// from real content, and a 16x16 luma signature, to drop a static 2D page that
// repeats byte-identically. Both used to be computed by `SampleEyeContent`,
// which owned its own D3D11 staging copy -- so a single capture read each shared
// eye texture twice (once to sample, once to save) and, worse, sampled and
// saved at different moments. The hook blits both eyes on every swap with no
// synchronisation, so the two reads could straddle a write: measured, frame 540
// had a lit probe and an all-black *saved* right eye, and frame 1770 had an
// intra-image tear.
//
// The maths is split out here so it runs over the *same* private staged copy the
// BMP is written from (`StageEyePair` in `src/host/openxr_manager.cpp`) and so it
// is unit-testable without a GPU (tests/eye_capture_tests.cpp, ADR-0465).
//
// Pure and dependency-free.

#include <stdint.h>
#include <stddef.h>

namespace RiddickVR {

struct EyeImageSample {
    float meanLuma = 0.0f;   // (R+G+B)/3 averaged over every pixel (0..255)
    uint64_t signature = 0;  // FNV-1a 64 over the 16x16 luma grid
};

// Samples `width x height` texels of a mapped 32-bit (R8G8B8A8) eye image.
// `base` points at the first texel; `rowPitch` is the mapped row stride in
// bytes (>= width*4, so a padded pitch is honoured). The mean luma is plain
// (R+G+B)/3 -- the harness's near-black frames are ~0 under any weighting, so
// the threshold is unambiguous -- and is computed over *every* pixel (a sparse
// grid can miss a small bright element and misjudge a dark-but-real frame as
// blank). An empty image returns a zeroed sample.
inline EyeImageSample SampleEyeImage(const uint8_t* base, uint32_t rowPitch,
                                     uint32_t width, uint32_t height) {
    EyeImageSample out{};
    if (!base || width == 0 || height == 0) {
        return out;
    }

    uint64_t sum = 0;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = base + static_cast<size_t>(y) * rowPitch;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* px = row + static_cast<size_t>(x) * 4; // R8G8B8A8
            sum += (static_cast<uint32_t>(px[0]) + px[1] + px[2]) / 3; // R,G,B
        }
    }
    out.meanLuma = static_cast<float>(static_cast<double>(sum) /
                                      (static_cast<double>(width) * height));

    // 16x16 luma signature for the frozen (byte-identical repeat) check.
    const uint32_t kGrid = 16;
    uint64_t hash = 1469598103934665603ull; // FNV-1a 64-bit offset basis
    for (uint32_t gy = 0; gy < kGrid; ++gy) {
        const uint32_t y = static_cast<uint32_t>(
            static_cast<uint64_t>(gy) * height / kGrid);
        const uint8_t* row = base + static_cast<size_t>(y) * rowPitch;
        for (uint32_t gx = 0; gx < kGrid; ++gx) {
            const uint32_t x = static_cast<uint32_t>(
                static_cast<uint64_t>(gx) * width / kGrid);
            const uint8_t* px = row + static_cast<size_t>(x) * 4;
            const uint8_t luma = static_cast<uint8_t>(
                (static_cast<int>(px[0]) + px[1] + px[2]) / 3);
            hash = (hash ^ luma) * 1099511628211ull; // FNV-1a 64-bit prime
        }
    }
    out.signature = hash;
    return out;
}

}  // namespace RiddickVR
