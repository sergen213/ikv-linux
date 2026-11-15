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

#include <algorithm>
#include <cmath>
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
    // NONCANONICAL wave 32: manifest "height": "derived" — the material has no maps of its own: it keeps the original
    // texture whole (tint_keep_original 1) and its relief (parallax occlusion and tessellated displacement, both on the
    // original UVs) is the original art's derived relief (Material::height, scene.cpp parallax_height). Drawn only on
    // scene materials that have one (pbr_main_material).
    bool derived_height = false;
    float tint_keep_original = 0;
    // RGBA8 full mip chains; empty data = absent map. albedo sRGB, normal +Y (OpenGL) in rgb, orm r AO g roughness b
    // metallic, height in the derived-parallax layout (scene.cpp parallax_height: r height 1 = top, g/b central
    // differences + .5).
    Texture albedo, normal, orm, height;
    bool has_height() const { return derived_height || !height.data.empty(); }
};
// Per-draw shader vector (pbr.glsl `pbr`: Vulkan push constant, GL uniform) of a draw with pack material p; {0,0,0,-1}
// without one. tessellated: the draw displaces its geometry; NONCANONICAL wave 31: it keeps parallax occlusion (relief
// flag) where RendererOptions::parallax allows, and pbr.glsl pbrReliefShare splits the relief between the displaced
// geometry (coarse, near) and parallax occlusion (fine detail, and all of it where the displacement fades out). A
// tessellation material drawn without tessellation uses parallax occlusion alone. w = tint_keep_original + 2 x relief
// + 4 x tessellated + 8 x round(low_gain x 256) (pbr.glsl pbrTintKeep, pbrParallax, pbrTessellated, pbrLowGain).
// pbr.x = -1 (derived_height): the original UVs carry the relief, and pbr.glsl never applies the generic derived relief of
// whole-original-albedo materials (pbrDerivedRelief) to it, so --parallax off leaves only the tessellated displacement.
inline std::array<float, 4> pbr_draw_params(const PbrMaterial* p, bool parallax_option, bool tessellated, float low_gain = 1.f) {
    if (!p) return {0, 0, 0, -1};
    const bool relief = parallax_option && (p->parallax || p->tessellate) && p->has_height();
    const float gain = std::round(std::clamp(low_gain, 1.f / 256.f, 16.f) * 256.f);
    const float u = p->world_uv ? .25f / p->scale[0] : p->derived_height ? -1.f : 0.f, v = p->world_uv ? .25f / p->scale[1] : 0.f;
    return {u, v, p->height_scale * 4.f, p->tint_keep_original + (relief ? 2.f : 0.f) + (tessellated ? 4.f : 0.f) + 8.f * gain};
}
// NONCANONICAL wave 29: linear brightness gain of the blurred-original tint (pbr.glsl pbrAlbedo). The shader samples a
// coarse authored mip of the original diffuse and linearises it, but the authored mips average sRGB values: for high-
// contrast textures (cobbles: bright stones, dark joints) that is ~20% darker than the mean linear colour the original
// shading shows, so every pack surface came out darker. Gain = mean linear luminance of the top mip / mean linearised
// luminance of the mip pbrLowLod mostly picks (the coarsest for world-projected materials, levels-4 otherwise).
float pbr_low_gain(const Texture& diffuse, bool world_uv);
// Per scene material {main gain (Material::diffuse), fade layer gain (Material::luminance)}; 1 where unmapped.
std::vector<std::array<float, 2>> pbr_low_gains(const Scene& scene, std::span<const PbrMaterial* const> pbr,
                                                std::span<const PbrMaterial* const> fade);
struct PbrPack {
    std::filesystem::path root;
    std::vector<PbrMaterial> materials;
    std::unordered_map<std::string, std::uint32_t> textures; // pbr_texture_key -> materials index
    std::vector<std::string> diagnostics;
    const PbrMaterial* find(std::string_view diffuse) const;
};
// The pack material of an eligible scene material (manifest key Material::diffuse); a derived-height material only where
// the scene derived a relief for it (flat art has none: displacing by the 1x1 white would lift the whole surface).
inline const PbrMaterial* pbr_main_material(const PbrPack& pack, const Material& m) {
    const auto* p = pack.find(m.diffuse);
    return p && p->derived_height && m.height.empty() ? nullptr : p;
}
// The pack material of a fade-layer scene material's secondary layer (Material::luminance) under main. NONCANONICAL wave 32:
// none under a derived-height main material: the whole original terrain blend is kept (the cobble streets' toprak layer
// mapped to the gravel pack greyed the original cobbles).
inline const PbrMaterial* pbr_fade_material(const PbrPack& pack, const Material& m, const PbrMaterial& main) {
    return main.derived_height ? nullptr : pack.find(m.luminance);
}
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
