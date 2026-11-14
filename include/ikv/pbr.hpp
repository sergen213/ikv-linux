#pragma once
// NONCANONICAL wave 29: enhanced-appearance PBR texture pack (assets/pbr, docs in assets/pbr/README.md). The original game
// has no PBR maps; original appearance never reads the pack.
//
// Key derivation (the manifest's "textures" keys): the material's diffuse texture name exactly as the scene loader
// normalises Material::diffuse — the original archive path lowercased with '/' separators, e.g.
// "textures/zemin_ingiliztasi.dds" (pbr_texture_key also lowercases and converts '\\' for robustness). Only static zone
// materials (pbr_eligible_materials) are ever mapped; ocean/water, sky, actors, cut-out and blended materials never are.
// `ikv-assets pbr-keys <original/app/base>` lists every eligible key of every original zone.
#include "ikv/scene.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ikv {
std::string pbr_texture_key(std::string_view diffuse);
// Per scene material: true for opaque static zone materials (every placement an unanimated, unskinned, non-sky,
// non-ocean zone instance; no blend, cut-out or environment map), the set derive_parallax_heights also uses.
std::vector<bool> pbr_eligible_materials(const Scene& scene);
// A material whose original shading blends the MAT+14 secondary fade layer (Material::luminance) under the diffuse by
// vertex alpha (renderers: fade map present, no normal or environment map). Terrains: the visible ground is often that
// layer (dirt, grass), so its own pack material (keyed by Material::luminance) is blended in the same way.
inline bool pbr_fade_layer(const Material& m) { return !m.luminance.empty() && m.normal.empty() && m.environment.empty(); }
// Tessellated draws only within this many scene units of the eye: pbr.glsl kPbrTessFadeEnd (displacement faded out).
constexpr float kPbrTessRange = 80.f;
struct PbrMaterial {
    std::string id;
    // world_uv: manifest scale non-null — every pack map is projected from world position (triplanar, scale = metres per
    // tile; 1 scene unit = 0.25 m) instead of the original UVs.
    bool world_uv = false;
    std::array<float, 2> scale{1, 1};
    float height_scale = 0; // metres
    bool tessellate = false, parallax = false;
    float tint_keep_original = 0;
    // RGBA8 full mip chains; empty data = absent map. albedo sRGB, normal +Y (OpenGL) in rgb, orm r AO g roughness b
    // metallic, height in the derived-parallax layout (scene.cpp parallax_height: r height 1 = top, g/b central
    // differences + .5).
    Texture albedo, normal, orm, height;
};
// Per-draw shader vector (pbr.glsl `pbr`: Vulkan push constant, GL uniform) of a draw with pack material p; {0,0,0,-1}
// without one. tessellated: the draw displaces its geometry (no parallax then); a tessellation material drawn without
// tessellation falls back to parallax occlusion when RendererOptions::parallax allows.
inline std::array<float, 4> pbr_draw_params(const PbrMaterial* p, bool parallax_option, bool tessellated) {
    if (!p) return {0, 0, 0, -1};
    const bool relief = !tessellated && parallax_option && (p->parallax || p->tessellate) && !p->height.data.empty();
    return {p->world_uv ? .25f / p->scale[0] : 0.f, p->world_uv ? .25f / p->scale[1] : 0.f, p->height_scale * 4.f,
            p->tint_keep_original + (relief ? 2.f : 0.f)};
}
struct PbrPack {
    std::filesystem::path root;
    std::vector<PbrMaterial> materials;
    std::unordered_map<std::string, std::uint32_t> textures; // pbr_texture_key -> materials index
    std::vector<std::string> diagnostics;
    const PbrMaterial* find(std::string_view diffuse) const;
};
// First existing <root>/manifest.json of: $IKV_PBR_DIR (set but empty or "off": no pack), compiled-in source tree
// (IKV_PBR_SOURCE_DIR), <base>/pbr, <base>/../share/ikv/pbr. Empty path: none.
std::filesystem::path pbr_pack_root(const char* executable_base);
// Throws on an unreadable/invalid manifest; individual unreadable maps are skipped with a diagnostic.
std::shared_ptr<const PbrPack> load_pbr_pack(const std::filesystem::path& root);
// Decoded PNG (8/16-bit gray, gray+alpha, RGB, RGBA, palette; non-interlaced) as 16-bit samples, `channels` per pixel.
struct PngImage { std::uint32_t width = 0, height = 0, channels = 0; std::vector<std::uint16_t> samples; };
PngImage decode_png(std::span<const std::uint8_t> bytes);
// Tessellation copy of a static mesh (PbrMaterial::tessellate): split until no edge exceeds max_edge units, then each
// vertex's tangent = its welded smoothed normal (area weighted, vertices welded by position, plus UV unless world_uv)
// scaled by the displacement weight: 0 on open/seam edges, so tessellated surfaces stay crack-free against their
// neighbours and themselves (shared edges get identical positions, normals, weights and tessellation factors).
Mesh pbr_tessellation_mesh(const Mesh& mesh, bool world_uv, float max_edge = 8.f);
void pbr_self_test();
} // namespace ikv
