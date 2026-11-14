# IKV enhanced PBR texture pack (NONCANONICAL, wave 29)

The original game ships no PBR maps. This pack adds albedo, normal, ORM and height maps to static zone surfaces for the
**enhanced appearance only** (OpenGL, Vulkan raster, Vulkan RT faithful/physical). Original appearance never reads it.
Sources and licences of every material: `SOURCES.md` (CC0 or generated procedurally).

## Layout

```
pbr/manifest.json
pbr/<material_id>/albedo.png   sRGB, "detail-normalised" (see Albedo)
pbr/<material_id>/normal.png   tangent space, OpenGL convention (+Y = image up), linear
pbr/<material_id>/orm.png      R ambient occlusion, G roughness, B metallic, linear
pbr/<material_id>/height.png   8/16-bit, first channel, white = high, linear
```

Every map is optional; at most 1024x1024 (larger maps are box-reduced at load). PNG 8/16-bit grey, grey+alpha, RGB,
RGBA or palette, non-interlaced. Installed next to the shaders: `share/ikv/pbr` (AppImage `usr/share/ikv/pbr`, Windows
`share\ikv\pbr`); the client looks in `$IKV_PBR_DIR`, the source tree's `assets/pbr`, `<exe>/pbr`, `<exe>/../share/ikv/pbr`.
`IKV_PBR_DIR=off` disables the pack (A/B captures).

## manifest.json

```json
{"version": 1,
 "materials": {"<material_id>": {"scale": [0.8, 0.8], "height_scale": 0.03, "tessellate": true, "parallax": true,
                                 "tint_keep_original": 0.0}},
 "textures": {"textures/arnavut_kaldirim.dds": "<material_id>"}}
```

- `scale`: metres per tile `[u, v]` — the maps are projected from **world position** (1 scene unit = 0.25 m): the plane
  is chosen per pixel by the surface normal's dominant axis (floors XZ, walls XY or ZY with image-up = world +Y), so the
  original UVs (often atlases) are ignored. `null`: the original UVs carry the maps, one pack tile per original UV tile.
- `height_scale`: relief depth in metres (0..0.5): parallax occlusion depth and tessellation displacement.
- `tessellate`: hardware tessellation of the surface (Vulkan and OpenGL 4.x; `--tessellation off` or no support falls
  back to parallax occlusion when `--parallax on`). Displacement is outward only, fades out between 10 and 20 m.
- `parallax`: parallax occlusion mapping with the height map (`--parallax on`, the default); fades out by ~20 m.
- `tint_keep_original`: 0..1 blend from the pack albedo (0) back to the original diffuse (1).

## Texture keys

A key is the material's original diffuse texture **exactly as the scene loader normalises `Material::diffuse`**: the
archive path lowercased with `/` separators, e.g. `textures/arnavut_kaldirim.dds` (not the basename). Only static zone
materials qualify (`include/ikv/pbr.hpp` `pbr_eligible_materials`: every placement an unanimated, unskinned, non-sky,
non-ocean zone instance; no blending, alpha cut-out or environment map) — water/ocean, sky, actors, foliage and glass are
never mapped. List every eligible key of the original zones with:

```
ikv-assets pbr-keys original/app/base > keys.json   # key -> zones, placements, original width/height
```

## Albedo

Pack albedo is authored detail-normalised: neutral grey mean, mean linear luminance 0.18 (sRGB ~118), local variation
kept. The renderer keeps each surface's own brightness and hue from the original art (pbr.glsl `pbrAlbedo`):

```
low   = original diffuse at its own UV, blurred (mip max(0, levels - 4))
base  = pack_albedo_linear * low_linear / 0.18
final = mix(base, original_linear, tint_keep_original)
```

Absent `albedo.png` = the original albedo. Absent `orm.png` = AO 1, roughness 0.65, metallic 0. Absent `normal.png` =
the vertex normal (tilted by the parallax relief when parallax is on). Absent `height.png` = no relief.
