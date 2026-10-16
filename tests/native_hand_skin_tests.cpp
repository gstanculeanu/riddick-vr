// Headless regression test for the native hand *skin* path (GH #61).
//
// Commit 52c2101 disabled the real diffuse (`NativeHandTexture=0`) with the
// claim that "the baked NativeHandTexture decoded white". That attribution is
// false and is checked here so it can never silently justify re-disabling the
// feature: the baked asset (src/hook/native_hand_texture_data.h) is warm
// skin/scar tones with ZERO near-white texels, and the native arm meshes'
// baked UVs (src/hook/native_hand_mesh_data.h) sample that asset's skin tones
// across the whole surface -- with a healthy per-vertex luminance spread, i.e.
// real texture detail, not a flat patch.
//
// Everything is compiled-in (no GL context, no game install, no input
// synthesis): the DXT1 blocks are decoded with the same bc1.h the renderer
// uses, then sampled at the mesh's own UVs exactly like
// VRHandRenderer::DrawNativeHand does (glTexCoord2f(v.uv[0], v.uv[1], optionally
// V-flipped)). The companion Python mirror lives in tools/test_xtc_bake.py.

#include "bc1.h"
#include "native_hand_mesh.h"
#include "native_hand_texture.h"

#include <cmath>
#include <cstdio>
#include <vector>

using RiddickVR::Bc1DecodeSurface;
using RiddickVR::Bc1SurfaceBytes;
using RiddickVR::FindNativeHandMesh;
using RiddickVR::FindNativeHandTexture;
using RiddickVR::NativeHandMesh;
using RiddickVR::NativeHandTexture;
using RiddickVR::NativeHandVertex;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "native hand skin: %s\n", what);
        ++g_failures;
    }
}

int Luma(const unsigned char* p) {
    return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000;
}

bool NearWhite(const unsigned char* p) {
    return p[0] > 200 && p[1] > 200 && p[2] > 200;
}

double Mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (double x : v) s += x;
    return s / static_cast<double>(v.size());
}

double StdDev(const std::vector<double>& v) {
    if (v.size() < 2) return 0.0;
    const double m = Mean(v);
    double s = 0.0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / static_cast<double>(v.size()));
}

// Nearest-texel sample with WRAP (the renderer uses GL_REPEAT); UVs here live
// in [0,1] but the wrap keeps the mirror in step with the GL path.
void SampleNearest(const std::vector<unsigned char>& rgba, unsigned int w,
                   unsigned int h, float u, float v, unsigned char out[3]) {
    long x = static_cast<long>(std::lround(u * static_cast<float>(w - 1)));
    long y = static_cast<long>(std::lround(v * static_cast<float>(h - 1)));
    x = ((x % static_cast<long>(w)) + static_cast<long>(w)) % static_cast<long>(w);
    y = ((y % static_cast<long>(h)) + static_cast<long>(h)) % static_cast<long>(h);
    const unsigned char* p = &rgba[(static_cast<size_t>(y) * w + static_cast<size_t>(x)) * 4u];
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
}

} // namespace

int main() {
    const NativeHandTexture* tex = FindNativeHandTexture("Riddick_ArmScar_D");
    Check(tex != nullptr, "baked Riddick_ArmScar_D diffuse present");
    if (!tex) {
        std::fprintf(stderr, "native hand skin: %d failure(s)\n", g_failures);
        return 1;
    }

    Check(tex->width > 0 && tex->height > 0, "texture has a non-zero size");
    Check(tex->dxt1 != nullptr &&
              tex->dxt1_bytes >= Bc1SurfaceBytes(tex->width, tex->height),
          "texture blocks cover the whole surface");

    std::vector<unsigned char> rgba(
        static_cast<size_t>(tex->width) * tex->height * 4u);
    Bc1DecodeSurface(tex->dxt1, tex->width, tex->height, rgba.data());

    // --- Whole-surface stats: a skin diffuse must be mid-tone and never white.
    const size_t texels = static_cast<size_t>(tex->width) * tex->height;
    std::vector<double> surface_luma;
    surface_luma.reserve(texels);
    size_t surface_white = 0;
    for (size_t i = 0; i < texels; ++i) {
        const unsigned char* p = &rgba[i * 4u];
        surface_luma.push_back(Luma(p));
        if (NearWhite(p)) ++surface_white;
    }
    {
        char msg[160];
        const double white_frac =
            static_cast<double>(surface_white) / static_cast<double>(texels);
        std::snprintf(msg, sizeof(msg),
                      "baked diffuse must not decode white (%.2f%% near-white)",
                      white_frac * 100.0);
        Check(white_frac < 0.02, msg);
    }
    Check(Mean(surface_luma) > 90.0 && Mean(surface_luma) < 200.0,
          "baked diffuse mean luminance is a plausible skin mid-tone");
    Check(StdDev(surface_luma) > 15.0,
          "baked diffuse has real tonal variation (not a flat fill)");

    // --- Mesh-UV sampling (exactly the fixed-function path's texcoords) -----
    // Only the meshes the native path actually draws are required to be
    // textured skin: the Riddick arms (surface_index 1/2 == the RIDDICK_ARMSCAR
    // arm surfaces of Riddick_B05, which the renderer maps to the ArmScar_D
    // diffuse). Pa2_Hand_Detail carries no real UVs and is a fallback for a
    // different character, so it is not part of this invariant.
    int textured_meshes = 0;
    for (int mi = 0; mi < RiddickVR::NativeHandMeshCount(); ++mi) {
        const NativeHandMesh& mesh = RiddickVR::NativeHandMeshAt(mi);
        if (mesh.surface_index < 1 || mesh.surface_index > 2 ||
            mesh.vertices == nullptr || mesh.vertex_count == 0) {
            continue;
        }
        ++textured_meshes;

        // UVs must actually span the atlas (a degenerate patch would sample a
        // single texel and read as flat, which is what #61/#64 reported).
        float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f;
        for (unsigned int i = 0; i < mesh.vertex_count; ++i) {
            const NativeHandVertex& v = mesh.vertices[i];
            if (v.uv[0] < umin) umin = v.uv[0];
            if (v.uv[0] > umax) umax = v.uv[0];
            if (v.uv[1] < vmin) vmin = v.uv[1];
            if (v.uv[1] > vmax) vmax = v.uv[1];
        }
        const float span_u = umax - umin;
        const float span_v = vmax - vmin;
        {
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          "%s UVs must span the atlas (u=%.3f v=%.3f)",
                          mesh.name, static_cast<double>(span_u),
                          static_cast<double>(span_v));
            Check(span_u > 0.5f || span_v > 0.5f, msg);
        }

        for (int flip = 0; flip <= 1; ++flip) {
            std::vector<double> luma;
            luma.reserve(mesh.vertex_count);
            size_t white = 0;
            for (unsigned int i = 0; i < mesh.vertex_count; ++i) {
                const NativeHandVertex& v = mesh.vertices[i];
                const float tv = flip ? (1.0f - v.uv[1]) : v.uv[1];
                unsigned char rgb[3];
                SampleNearest(rgba, tex->width, tex->height, v.uv[0], tv, rgb);
                luma.push_back(Luma(rgb));
                if (NearWhite(rgb)) ++white;
            }
            char msg[224];
            std::snprintf(msg, sizeof(msg),
                          "%s (VFlip=%d) must sample skin tones, not white "
                          "(mean=%.1f, %zu/%u near-white)",
                          mesh.name, flip, Mean(luma), white, mesh.vertex_count);
            Check(Mean(luma) > 90.0 && Mean(luma) < 200.0 && white == 0, msg);
            std::snprintf(msg, sizeof(msg),
                          "%s (VFlip=%d) must sample varied texels "
                          "(stddev=%.1f)",
                          mesh.name, flip, StdDev(luma));
            Check(StdDev(luma) > 15.0, msg);
        }
    }
    Check(textured_meshes >= 2,
          "both native arm meshes are textured (surface_index 1)");

    if (g_failures != 0) {
        std::fprintf(stderr, "native hand skin: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("native hand skin: ok\n");
    return 0;
}
