// Headless tests for the baked native-hand texture asset (GH #61).
//
// Covers the BC1/DXT1 decoder (src/hook/bc1.h) on hand-computed blocks and
// cross-checks it against the *reference* pixels + checksum that
// tools/xtc_bake.py produced with the independent Python decoder
// (tools/mos_xtc.py). No GL context, no game install, no input synthesis.

#include "bc1.h"
#include "native_hand_mesh.h"
#include "native_hand_texture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using RiddickVR::Bc1DecodeBlock;
using RiddickVR::Bc1DecodeSurface;
using RiddickVR::Bc1Rgb565ToRgb;
using RiddickVR::Bc1SurfaceBytes;
using RiddickVR::FindNativeHandTexture;
using RiddickVR::NativeHandTexture;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "native hand texture: %s\n", what);
        ++g_failures;
    }
}

// Build a BC1 block with the given endpoint colours and a single 2-bit index
// written into every texel.
void MakeBlock(unsigned int c0, unsigned int c1, unsigned int index,
               unsigned char out[8]) {
    out[0] = (unsigned char)(c0 & 0xFF);
    out[1] = (unsigned char)((c0 >> 8) & 0xFF);
    out[2] = (unsigned char)(c1 & 0xFF);
    out[3] = (unsigned char)((c1 >> 8) & 0xFF);
    unsigned int bits = 0;
    for (int i = 0; i < 16; ++i) {
        bits |= (index & 3u) << (2 * i);
    }
    out[4] = (unsigned char)(bits & 0xFF);
    out[5] = (unsigned char)((bits >> 8) & 0xFF);
    out[6] = (unsigned char)((bits >> 16) & 0xFF);
    out[7] = (unsigned char)((bits >> 24) & 0xFF);
}

void CheckTexels(const unsigned char* rgba, int count,
                 unsigned char r, unsigned char g, unsigned char b,
                 unsigned char a, const char* what) {
    for (int i = 0; i < count; ++i) {
        const unsigned char* p = rgba + i * 4;
        if (p[0] != r || p[1] != g || p[2] != b || p[3] != a) {
            std::fprintf(stderr,
                         "native hand texture: %s texel %d is "
                         "(%u,%u,%u,%u), expected (%u,%u,%u,%u)\n",
                         what, i, p[0], p[1], p[2], p[3], r, g, b, a);
            ++g_failures;
            return;
        }
    }
}

} // namespace

int main() {
    // --- RGB565 expansion -------------------------------------------------
    unsigned char rgb[3];
    Bc1Rgb565ToRgb(0x07E0u, rgb); // pure green
    Check(rgb[0] == 0 && rgb[1] == 255 && rgb[2] == 0, "0x07E0 expands to green");
    Bc1Rgb565ToRgb(0xF800u, rgb); // pure red
    Check(rgb[0] == 255 && rgb[1] == 0 && rgb[2] == 0, "0xF800 expands to red");
    Bc1Rgb565ToRgb(0x0000u, rgb);
    Check(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0, "0x0000 expands to black");

    // --- BC1 block: c0 > c1 => opaque 4-colour mode -----------------------
    // c0 = red (255,0,0), c1 = blue (0,0,255).
    //   palette[2] = (2*c0+c1)/3 = (170,0,170), palette[3] = (85,0,170).
    unsigned char block[8];
    unsigned char texels[64];
    MakeBlock(0xF800u, 0x001Fu, 0u, block);
    Bc1DecodeBlock(block, texels);
    CheckTexels(texels, 16, 255, 0, 0, 255, "index 0 -> c0 (red)");

    MakeBlock(0xF800u, 0x001Fu, 2u, block);
    Bc1DecodeBlock(block, texels);
    // (2*red + blue)/3 = (170, 0, 85).
    CheckTexels(texels, 16, 170, 0, 85, 255, "index 2 -> (2c0+c1)/3");

    MakeBlock(0xF800u, 0x001Fu, 3u, block);
    Bc1DecodeBlock(block, texels);
    CheckTexels(texels, 16, 85, 0, 170, 255, "index 3 -> (c0+2c1)/3");

    // --- BC1 block: c0 < c1 => 3-colour mode, index 3 is transparent black -
    MakeBlock(0x001Fu, 0xF800u, 0u, block);
    Bc1DecodeBlock(block, texels);
    CheckTexels(texels, 16, 0, 0, 255, 255, "3-colour index 0 -> blue");

    MakeBlock(0x001Fu, 0xF800u, 2u, block);
    Bc1DecodeBlock(block, texels);
    CheckTexels(texels, 16, 127, 0, 127, 255, "3-colour index 2 -> average");

    MakeBlock(0x001Fu, 0xF800u, 3u, block);
    Bc1DecodeBlock(block, texels);
    CheckTexels(texels, 16, 0, 0, 0, 255, "3-colour index 3 -> black");

    // Bit order: index of pixel (px=1,py=0) sits in bits 2..3.
    MakeBlock(0xF800u, 0x001Fu, 0u, block);
    block[4] = (unsigned char)(block[4] | 0x0Cu); // pixel 1 -> index 3
    Bc1DecodeBlock(block, texels);
    Check(texels[4 + 0] == 85 && texels[4 + 2] == 170,
          "pixel 1 uses bits 2..3");
    Check(texels[0] == 255 && texels[2] == 0, "pixel 0 unchanged");

    // --- Surface size / degenerate guards ---------------------------------
    Check(Bc1SurfaceBytes(256, 512) == 65536, "256x512 surface is 65536 bytes");
    Check(Bc1SurfaceBytes(4, 4) == 8, "4x4 surface is one 8-byte block");
    Check(Bc1SurfaceBytes(5, 5) == 32, "5x5 surface needs 2x2 blocks");
    {
        unsigned char one[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        Bc1DecodeSurface(nullptr, 4, 4, texels);
        Bc1DecodeSurface(one, 4, 4, nullptr);
        Bc1DecodeSurface(one, 0, 4, texels);
        Check(true, "null/zero-size surface decode is a no-op");
    }

    // --- Non-multiple-of-4 edges are clipped, never overrun ---------------
    MakeBlock(0xFFFFu, 0x0000u, 0u, block); // all white
    std::vector<unsigned char> clipped(3 * 2 * 4 + 8, 0xAB);
    Bc1DecodeSurface(block, 3, 2, clipped.data());
    CheckTexels(clipped.data(), 6, 255, 255, 255, 255, "3x2 clipped surface");
    for (int i = 0; i < 8; ++i) {
        if (clipped[3 * 2 * 4 + i] != 0xAB) {
            Check(false, "3x2 decode wrote past the surface");
            break;
        }
    }

    // --- Baked asset: decoder must reproduce the Python reference ----------
    const NativeHandTexture* tex = FindNativeHandTexture("Riddick_ArmScar_D");
    Check(tex != nullptr, "baked Riddick_ArmScar_D texture present");
    if (tex) {
        Check(tex->mip >= 0, "baked mip is non-negative");
        Check(tex->width == 256 && tex->height == 512,
              "canonical bake is the 256x512 mip");
        Check(tex->dxt1_bytes == Bc1SurfaceBytes(tex->width, tex->height),
              "block byte count matches the surface size");
        Check(tex->sample_count >= 4, "reference samples are baked");

        std::vector<unsigned char> rgba(
            static_cast<size_t>(tex->width) * tex->height * 4u);
        Bc1DecodeSurface(tex->dxt1, tex->width, tex->height, rgba.data());

        unsigned int checksum = 0;
        for (size_t i = 0; i < rgba.size(); ++i) {
            checksum += rgba[i];
        }
        if (checksum != tex->rgba_checksum) {
            std::fprintf(stderr,
                         "native hand texture: decoded checksum 0x%08X != "
                         "baked 0x%08X (C++ and Python decoders disagree)\n",
                         checksum, tex->rgba_checksum);
            ++g_failures;
        }

        for (unsigned int s = 0; s < tex->sample_count; ++s) {
            const auto& sample = tex->samples[s];
            if (sample.x >= tex->width || sample.y >= tex->height) {
                Check(false, "reference sample inside the surface");
                continue;
            }
            const unsigned char* p =
                &rgba[(static_cast<size_t>(sample.y) * tex->width + sample.x) * 4u];
            if (p[0] != sample.rgba[0] || p[1] != sample.rgba[1] ||
                p[2] != sample.rgba[2] || p[3] != sample.rgba[3]) {
                std::fprintf(stderr,
                             "native hand texture: sample (%u,%u) decoded "
                             "(%u,%u,%u,%u) != baked (%u,%u,%u,%u)\n",
                             sample.x, sample.y, p[0], p[1], p[2], p[3],
                             sample.rgba[0], sample.rgba[1], sample.rgba[2],
                             sample.rgba[3]);
                ++g_failures;
            }
        }
    }

    // --- GH #61: every arm vertex UV must land on skin, not white ----------
    {
        const NativeHandTexture* tex = FindNativeHandTexture("Riddick_ArmScar_D");
        Check(tex != nullptr, "texture present for UV coverage check");
        if (tex) {
            std::vector<unsigned char> rgba(
                static_cast<size_t>(tex->width) * tex->height * 4u);
            Bc1DecodeSurface(tex->dxt1, tex->width, tex->height, rgba.data());
            const char* names[2] = {"Riddick_LeftArm", "Riddick_RightArm"};
            for (const char* name : names) {
                const RiddickVR::NativeHandMesh* mesh = RiddickVR::FindNativeHandMesh(name);
                Check(mesh != nullptr, "arm mesh present for UV coverage check");
                if (!mesh) continue;
                for (int flip = 0; flip < 2; ++flip) {
                    unsigned int whiteTexels = 0;
                    double sumR = 0, sumG = 0, sumB = 0;
                    unsigned int n = 0;
                    for (unsigned int i = 0; i < mesh->vertex_count; ++i) {
                        float u = mesh->vertices[i].uv[0];
                        float v = mesh->vertices[i].uv[1];
                        if (flip) v = 1.0f - v;
                        u -= std::floor(u);  // GL_REPEAT
                        v -= std::floor(v);
                        unsigned int x = (unsigned int)(u * tex->width);
                        unsigned int y = (unsigned int)(v * tex->height);
                        if (x >= tex->width) x = tex->width - 1;
                        if (y >= tex->height) y = tex->height - 1;
                        const unsigned char* p = &rgba[((size_t)y * tex->width + x) * 4u];
                        unsigned char mn = (std::min)(p[0], (std::min)(p[1], p[2]));
                        if (mn > 220) ++whiteTexels;
                        sumR += p[0]; sumG += p[1]; sumB += p[2];
                        ++n;
                    }
                    Check(n > 0, "arm mesh has vertices");
                    if (n == 0) continue;
                    const double mr = sumR / n, mg = sumG / n, mb = sumB / n;
                    std::printf("native hand texture: %s flip=%d mean=(%.0f,%.0f,%.0f) white=%u/%u\n",
                                name, flip, mr, mg, mb, whiteTexels, n);
                    Check(whiteTexels == 0, "no near-white texel at any arm UV");
                    Check(mr >= 130.0 && mr <= 210.0, "mean R at arm UVs is skin-range");
                    Check(mr > mg && mg > mb, "arm UV texels read R>G>B (skin)");
                }
            }
        }
    }

    // --- GH #61: texture integrity decision ------------------------------
    {
        using RiddickVR::CheckNativeTextureIntegrity;
        using RiddickVR::NativeTextureCheck;
        const unsigned W = 256, H = 512;
        const int F = 0x2601; // GL_LINEAR
        Check(CheckNativeTextureIntegrity(true, 256, 512, F, W, H, F) == NativeTextureCheck::Ok,
              "intact texture is Ok");
        Check(CheckNativeTextureIntegrity(false, 256, 512, F, W, H, F) == NativeTextureCheck::Reupload,
              "deleted id needs reupload");
        Check(CheckNativeTextureIntegrity(true, 0, 0, F, W, H, F) == NativeTextureCheck::Reupload,
              "incomplete texture (0x0) needs reupload");
        Check(CheckNativeTextureIntegrity(true, 256, 256, F, W, H, F) == NativeTextureCheck::Reupload,
              "wrong height needs reupload");
        Check(CheckNativeTextureIntegrity(true, 128, 512, F, W, H, F) == NativeTextureCheck::Reupload,
              "wrong width needs reupload");
        Check(CheckNativeTextureIntegrity(true, -1, 512, F, W, H, F) == NativeTextureCheck::Reupload,
              "negative size needs reupload");
        Check(CheckNativeTextureIntegrity(true, 256, 512, 0x2703, W, H, F) == NativeTextureCheck::Reparam,
              "min filter drift only needs reparam");
        Check(CheckNativeTextureIntegrity(false, 0, 0, 0x2703, W, H, F) == NativeTextureCheck::Reupload,
              "reupload wins over reparam");
    }

    // --- Lookup contract --------------------------------------------------
    Check(FindNativeHandTexture("nope") == nullptr, "unknown texture is null");
    Check(FindNativeHandTexture(nullptr) == nullptr, "null name is null");
    Check(RiddickVR::NativeHandTextureCount() >= 1, "at least one texture baked");

    if (g_failures != 0) {
        std::fprintf(stderr, "native hand texture: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("native hand texture: ok\n");
    return 0;
}
