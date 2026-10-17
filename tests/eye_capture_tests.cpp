// GH #79: unit tests for the pure XRSIM capture helpers -- the eye-content
// sampler (src/host/eye_sample.h) and the BMP sidecar writer layout
// (src/host/bmp_writer.h).
//
// The mock host used to sample each shared eye texture and then re-read it to
// write the BMP, so the pixels that were *graded* could differ from the pixels
// that were *saved* (measured: frame 540, a lit probe against an all-black
// saved right eye; frame 1770, an intra-image tear). The host now stages both
// eyes into one private copy and runs both the gate and the save over it; the
// maths and the on-disk layout are factored out here so they are pinned
// without a GPU (ADR-0465).
#include "bmp_writer.h"
#include "eye_sample.h"

#include <cstdio>
#include <vector>

using RiddickVR::BmpFileHeader;
using RiddickVR::ConvertRgbaRowToBgra;
using RiddickVR::EyeImageSample;
using RiddickVR::MakeBmpHeader;
using RiddickVR::SampleEyeImage;

static int g_failures = 0;

static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "eye capture test failed: %s\n", message);
        ++g_failures;
    }
}

int main() {
    // --- BMP header layout ------------------------------------------------
    Check(sizeof(BmpFileHeader) == 54,
          "BMP header must be BITMAPFILEHEADER + BITMAPINFOHEADER (54 bytes)");
    const BmpFileHeader h = MakeBmpHeader(4, 3);
    Check(h.bfType == 0x4D42, "BMP magic must be 'BM'");
    Check(h.bfOffBits == 54, "pixel data must start at offset 54");
    Check(h.biSize == 40, "BITMAPINFOHEADER must be 40 bytes");
    Check(h.biWidth == 4 && h.biHeight == -3,
          "BMP must be top-down (negative height) at the requested size");
    Check(h.biPlanes == 1 && h.biBitCount == 32,
          "BMP must be a 1-plane 32-bpp image");
    Check(h.biCompression == 0, "BMP must be uncompressed (BI_RGB)");
    Check(h.biSizeImage == 4u * 3u * 4u, "BMP image size must be width*height*4");
    Check(h.bfSize == 54u + 4u * 3u * 4u, "BMP file size must be header + pixels");

    // --- R8G8B8A8 -> BMP (B,G,R,A) row swizzle ----------------------------
    const uint8_t src[8] = { 10, 20, 30, 255, 40, 50, 60, 128 };
    uint8_t dst[8] = { 0 };
    ConvertRgbaRowToBgra(src, dst, 2);
    Check(dst[0] == 30 && dst[1] == 20 && dst[2] == 10 && dst[3] == 255,
          "first texel R/B must swap to B,G,R,A and keep alpha");
    Check(dst[4] == 60 && dst[5] == 50 && dst[6] == 40 && dst[7] == 128,
          "second texel R/B must swap to B,G,R,A and keep alpha");
    uint8_t untouched[4] = { 7, 7, 7, 7 };
    ConvertRgbaRowToBgra(src, untouched, 0);
    Check(untouched[0] == 7 && untouched[3] == 7, "width 0 must be a no-op");

    // --- eye sampler: mean luma, rowPitch, frozen signature ---------------
    // 2x2 R8G8B8A8 image with a padded row pitch (8 bytes of pixels, 12 bytes
    // per row) to prove the sampler honours the mapped RowPitch.
    const uint32_t pitch = 12;
    std::vector<uint8_t> img(pitch * 2, 0xAA);
    // texel(0,0) R10 G20 B30 -> luma 20 ; texel(1,0) R60 G60 B60 -> luma 60
    img[0] = 10; img[1] = 20; img[2] = 30; img[3] = 255;
    img[4] = 60; img[5] = 60; img[6] = 60; img[7] = 255;
    // texel(0,1) R0 G0 B0 -> luma 0 ; texel(1,1) R30 G30 B30 -> luma 30
    img[pitch + 0] = 0;  img[pitch + 1] = 0;  img[pitch + 2] = 0;  img[pitch + 3] = 255;
    img[pitch + 4] = 30; img[pitch + 5] = 30; img[pitch + 6] = 30; img[pitch + 7] = 255;

    const EyeImageSample s = SampleEyeImage(img.data(), pitch, 2, 2);
    Check(s.meanLuma > 27.0f && s.meanLuma < 28.0f,
          "mean luma must average (R+G+B)/3 over every texel: (20+60+0+30)/4 = 27.5");
    Check(SampleEyeImage(img.data(), pitch, 2, 2).signature == s.signature,
          "the frozen-page signature must be deterministic");

    // An alpha-only change must not move the mean or the signature (the luma
    // ignores alpha), while an RGB change must move the signature.
    std::vector<uint8_t> alpha = img;
    alpha[3] = 0; alpha[pitch + 3] = 1;
    const EyeImageSample sAlpha = SampleEyeImage(alpha.data(), pitch, 2, 2);
    Check(sAlpha.signature == s.signature && sAlpha.meanLuma == s.meanLuma,
          "alpha must not affect the luma mean or the signature");
    std::vector<uint8_t> changed = img;
    changed[pitch + 4] = 200; changed[pitch + 5] = 200; changed[pitch + 6] = 200;
    Check(SampleEyeImage(changed.data(), pitch, 2, 2).signature != s.signature,
          "a changed texel must change the frozen-page signature");

    // A uniform dark image is the interop warm-up: mean 0 (below the blank
    // threshold), and an empty/failed image samples to zero rather than
    // crashing.
    std::vector<uint8_t> black(pitch * 2, 0);
    Check(SampleEyeImage(black.data(), pitch, 2, 2).meanLuma == 0.0f,
          "an all-black eye must sample to mean luma 0");
    const EyeImageSample empty = SampleEyeImage(nullptr, 0, 0, 0);
    Check(empty.meanLuma == 0.0f && empty.signature == 0,
          "an empty image must sample to a zeroed record");

    // --- graded pixel == saved pixel (GH #79) -----------------------------
    // The gate samples the mapped row and the BMP writer swizzles that same
    // row; the per-texel luma of the saved BMP row must match the source row
    // the sampler saw, so the graded pixels and the saved pixels agree.
    uint8_t swizzled[8] = { 0 };
    ConvertRgbaRowToBgra(img.data(), swizzled, 2); // first row (2 texels)
    Check(swizzled[0] == img[2] && swizzled[2] == img[0],
          "the saved row must carry the same texels as the sampled row (B/R swap)");
    const int savedLuma0 = (swizzled[2] + swizzled[1] + swizzled[0]) / 3;
    const int srcLuma0 = (img[0] + img[1] + img[2]) / 3;
    const int savedLuma1 = (swizzled[6] + swizzled[5] + swizzled[4]) / 3;
    const int srcLuma1 = (img[4] + img[5] + img[6]) / 3;
    Check(savedLuma0 == srcLuma0 && savedLuma1 == srcLuma1,
          "the saved BMP texel's luma must equal the sampled source texel's luma");

    if (g_failures == 0) {
        std::puts("eye capture tests passed");
    }
    return g_failures == 0 ? 0 : 1;
}
