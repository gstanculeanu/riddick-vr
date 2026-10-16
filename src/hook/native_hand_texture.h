#pragma once

// GH #61: native first-person hand/arm diffuse asset.
//
// The real Riddick skin/scar diffuse (``Riddick_ArmScar_D``) is baked out of the
// shipped ``MOS DATAFILE2.0`` ``.xtc`` texture pack by ``tools/xtc_bake.py``
// into ``native_hand_texture_data.h`` (generated; do not edit), so the game
// process never has to open the 685 MB ``Content/Models.xtc`` at runtime. The
// blocks are stored in the engine's own DXT1/BC1 codec and decoded by
// ``bc1.h``.
//
// This header is intentionally dependency-free and header-only so it can be
// unit-tested headlessly (``tests/native_hand_texture_tests.cpp``) without a GL
// context or the game. It is consumed by ``VRHandRenderer::DrawNativeHand``
// behind the ``NativeHands`` config flag.
//
// GH #61 (verified): the bake is warm skin/scar tones -- whole-surface mean
// luma ~139 with **zero** near-white texels -- and the native arm meshes' own
// UVs sample it across the atlas (mean luma ~127-147, real tonal spread). The
// earlier "baked NativeHandTexture decoded white" claim that disabled the bind
// (commit 52c2101) was a mis-attribution of the *0.25 fade* translucency fixed
// in the same commit; ``tests/native_hand_skin_tests.cpp`` and
// ``tools/test_xtc_bake.py`` pin the invariant so the feature is not reverted
// on that false premise.

namespace RiddickVR {

// One decoded reference pixel (proof the C++ BC1 decoder matches the Python
// one, without shipping the RGBA image).
struct NativeHandTextureSample {
    unsigned short x;
    unsigned short y;
    unsigned char rgba[4];
};

struct NativeHandTexture {
    const char* name;                        // surface name, e.g. "Riddick_ArmScar_D"
    const char* source;                      // pack path relative to the game root
    int mip;                                 // baked mip level
    unsigned int width;
    unsigned int height;
    const unsigned char* dxt1;               // BC1 blocks, Bc1SurfaceBytes(w,h) long
    unsigned int dxt1_bytes;
    const NativeHandTextureSample* samples;  // decoded reference pixels
    unsigned int sample_count;
    unsigned int rgba_checksum;              // sum of decoded RGBA bytes (test check)
};

} // namespace RiddickVR

#include "native_hand_texture_data.h"

#include <cstring>

namespace RiddickVR {

inline int NativeHandTextureCount() {
    return NativeHandTextureData::kTextureCount;
}

// Clamped accessor: never returns a dangling reference.
inline const NativeHandTexture& NativeHandTextureAt(int index) {
    if (index < 0 || index >= NativeHandTextureData::kTextureCount) {
        index = 0;
    }
    return NativeHandTextureData::kTextures[index];
}

inline const NativeHandTexture* FindNativeHandTexture(const char* name) {
    if (!name) {
        return nullptr;
    }
    for (int i = 0; i < NativeHandTextureData::kTextureCount; ++i) {
        if (std::strcmp(NativeHandTextureData::kTextures[i].name, name) == 0) {
            return &NativeHandTextureData::kTextures[i];
        }
    }
    return nullptr;
}

} // namespace RiddickVR
