#pragma once

// GH #79: the XRSIM BMP sidecar writer, as pure helpers.
//
// The capture writes one 32-bit BMP per eye. The file header layout and the
// R8G8B8A8 -> BMP (BGRA byte order) row swizzle used to live inline inside
// `SaveTextureToBMP`, which mapped its own D3D11 staging copy per eye -- so a
// captured frame read each shared eye texture twice (the content gate's sample,
// then the save) and wrote the BMP from the second, later read. The gate probe
// and the saved pixels could therefore disagree (GH #79: frame 540). The layout
// is split out here so the BMP is written from the *same* private staged copy
// the gate sampled, and so it is unit-testable without a GPU
// (tests/eye_capture_tests.cpp, ADR-0465).
//
// Pure and dependency-free.

#include <stdint.h>
#include <stddef.h>

namespace RiddickVR {

#pragma pack(push, 1)
// BITMAPFILEHEADER (14) + BITMAPINFOHEADER (40). Top-down (negative height) so
// the BMP row order matches the mapped D3D11 staging rows directly.
struct BmpFileHeader {
    uint16_t bfType = 0x4D42;   // "BM"
    uint32_t bfSize = 0;
    uint16_t bfReserved1 = 0;
    uint16_t bfReserved2 = 0;
    uint32_t bfOffBits = 54;
    uint32_t biSize = 40;
    int32_t biWidth = 0;
    int32_t biHeight = 0;
    uint16_t biPlanes = 1;
    uint16_t biBitCount = 32;
    uint32_t biCompression = 0;  // BI_RGB
    uint32_t biSizeImage = 0;
    int32_t biXPelsPerMeter = 2835;
    int32_t biYPelsPerMeter = 2835;
    uint32_t biClrUsed = 0;
    uint32_t biClrImportant = 0;
};
#pragma pack(pop)

static_assert(sizeof(BmpFileHeader) == 54,
              "BMP header must be BITMAPFILEHEADER + BITMAPINFOHEADER (54 bytes)");

inline BmpFileHeader MakeBmpHeader(uint32_t width, uint32_t height) {
    BmpFileHeader header{};
    header.biWidth = static_cast<int32_t>(width);
    header.biHeight = -static_cast<int32_t>(height); // top-down
    header.biSizeImage = width * height * 4;
    header.bfSize = sizeof(BmpFileHeader) + header.biSizeImage;
    return header;
}

// A mapped R8G8B8A8 row -> the BMP's on-disk 32-bpp order (B, G, R, A): the
// first and third channels swap, green and alpha are preserved. `width` is in
// texels; both buffers hold at least width*4 bytes.
inline void ConvertRgbaRowToBgra(const uint8_t* src, uint8_t* dst, uint32_t width) {
    for (uint32_t x = 0; x < width; ++x) {
        dst[x * 4 + 0] = src[x * 4 + 2];
        dst[x * 4 + 1] = src[x * 4 + 1];
        dst[x * 4 + 2] = src[x * 4 + 0];
        dst[x * 4 + 3] = src[x * 4 + 3];
    }
}

}  // namespace RiddickVR
