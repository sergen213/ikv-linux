# Engine architecture: observed original versus native implementation

## Scope and confidence

The original Windows package was extracted natively, preserved under `original/`, and inspected statically. No official server was contacted and no original Windows executable was run. [Asset formats and guide evidence](ASSET_FORMATS.md) document both known structures and unresolved semantics. Native multiplayer uses an explicitly independent protocol/test world; neither its transport nor its rendering is evidence of original behavior.

This document describes **observed original subsystems**, not a claim that the entire game has been reimplemented. Folder names, shader comments and guide descriptions can identify a subsystem but cannot establish its runtime initialization order, exact backend formula or interoperability.

## Original data-to-display path

The recoverable path is:

```text
user-supplied installation
  ├─ SKYF v1 archives → indexed member byte ranges
  │    ├─ textures/lightmaps → legacy DDS headers/payloads
  │    └─ objects → ADF chunk tables/compressed resources
  ├─ textual Direct3D vertex/pixel assembly
  ├─ particle-effect/system XML → DDS texture references
  ├─ zone/height/music/sound grid candidates
  ├─ Vorbis streams + headerless audio candidates
  └─ Turkish help, PDF guide, UI atlases/fonts, encoded game tables
```

SKYF indexing, legacy DDS decoding and the recovered ADF geometry/material/node/skin paths now have read-only native implementations. Static evidence establishes zone support/height lookup and original audio playback format. Full original shader/UI/effects behavior, live environment records and game-table-to-simulation compatibility remain separately bounded.

### Resource storage

SKYF contains a recursive directory/file index and absolute offset/length pairs. Texture/lightmap variants encode different DDS dimensions; `_4` lightmaps do not have exactly the same membership as `_1`/`_2`. A native implementation must select a supported variant and resolve names explicitly, not assume all archives can be substituted blindly.

ADF members contain actual maps, props, NPCs and character parts. Native `decode_adf` handles the 1446 zlib and36 recovered adaptive-arithmetic members; `load_scene` consumes original vertex/triangle records, texture material slots, geometry clones, ambient streams, node parents, real quaternion keys and bone-local weighted skin influences. [Asset schemas and corpus proof](ASSET_FORMATS.md) name the original routines and exact bounds. Opaque serialized words are never treated as native pointers.

Ghidra corroborates the index independently: archive opener `0x00461790` opens a Windows handle read-only, reads six header bytes, then calls `0x00461430`. The recursive parser reads 68-byte directory records, file count at +0x42, 136-byte file records, and child count at +0x40 after files. Exact decompilation records: `analysis/trace-game/decompile/00461790.c` and `analysis/dispatch-game/decompile/00461430.c`. The shown original opener does not visibly validate the SKYF magic; the independent reader deliberately validates supplied-format identity and bounds instead of reproducing unsafe assumptions.

### Original graphics evidence

The shipped shaders are Direct3D assembly: 20 `vs_1_1`, 15 `ps.1.1`, 3 `ps_2_0`. Cg comments expose position, normal, UV and tangent inputs and model/object/view/fog/light/camera constant names. Pixel-shader comments reference diffuse/environment/normal/lightmap-like textures. The assembly and constants establish an old Direct3D shader pipeline, not source-level portability.

BinaryEvidence's static Ghidra analysis of the unpacked game reports these virtual addresses:

| Address | Observed operation | Confidence / limit |
|---|---|---|
| `0x0045e8b0` | Calls `D3DXGetImageInfoFromFileInMemory` | Static image-metadata path; caller/complete native ABI not reconstructed here |
| `0x0045ef00` | Calls `D3DXCreateTextureFromFileInMemoryEx`, reference at `0x0045efbb` | Static texture upload path; caller functions reported at `0x00461c30`, `0x004623a0` |
| `0x00467a80` | Vertex-shader path using assembly-from-file API | Static shader-loading evidence, not translated GLSL |
| `0x00467c50` | Pixel-shader path using assembly-from-file API | Static shader-loading evidence, not translated GLSL |
| `0x004596f0`, `0x00453580` | Configuration references `base\\audio\\sounds.xml`; RAW WAVEFORMATEX uses PCMmono44100Hz16bit | Headerless RAW format confirmed;19 supplied odd-length audio members defensively reject; root grids excluded |

These are VAs in `analysis/unpacked/istanbul.exe`, image base `0x00400000`, SHA256 `44ffa8389a9f6e4c35841103ad4f40a90513e4deb518ab5605b0a1ebfae0be2d`, **not native C++ addresses**. Original packed `original/app/istanbul.exe` SHA256 is `bff4cffe753a04e237e3ed32c3798eda10a602d73424a2b228024762da44aba6`. The [annotated function map](FUNCTION_MAP.md) and [reverse-engineering record](REVERSE_ENGINEERING.md) own wider analysis/provenance; consult the exact image before interpreting any address. No initialization/main-loop/input/camera call graph is asserted from these five functions alone.

### Audio and effects

A supplied Ogg stream was identified by native ffprobe as mono Vorbis at44100Hz. Original RAW audio setup/direct-read evidence now establishes signedLE16 mono44100Hz; [audio metadata](evidence/audio-format.json) distinguishes347 audio RAW files from24 root selection grids and names19 odd-length native rejections. Stream-specific Vorbis metadata still governs its own decoding.

Original routine `0x00419a40` formats `music%.2d.raw`/`sound%.2d.raw` names and reads `0x400000` bytes into separate buffers (`analysis/trace-game/decompile/00419a40.c`). References are `0x00419a86`/`0x00419b3e`; format-string VAs are `0x0058eba4`/`0x0058eb94`. This corroborates zone-sized resource loading, but does not prove indexing order or playback/grid semantics.

Particle XML exposes step/render/emitter configuration, texture references, blending and visual attachment/projectile effects. Those visual parameters do not establish authoritative attack travel, hit timing, damage or status-effect formulas. Declared UTF-8 and ISO-8859-9 encodings differ by file.

### UI and documented rules

Loose DDS atlases, JPEG loading/map images and supplied fonts establish native asset candidates but not an original UI coordinate/layout definition. ISO-8859-9 help and the 74-page v6 PDF identify classes, skills, controls, inventory, professions, quests, groups, death and PvP. The image-only control page was rendered and read locally. In particular, original WASD controls are documented as **camera movement**, with mouse ground-click movement; do not relabel independent WASD test-world movement as preserved original input.

The guide and older help conflict on core-stat allocation and contain inconsistent skill/unlock/table values. A server must separate documentary rules from exact algorithms and independently observed behavior. No canonical XP, combat, aggro, loot or PvP formula is supplied by the native example world.

## Native boundary actually implemented

`ikv-assets` remains read-only but now performs actual DDS/ADF decoding and `scene ASSET_ROOT MEMBER [TIME_SECONDS]` loads genuine original-resource scene data. It supports direct imported base contents or original installation roots. The scene API supplies source materials/textures, original local transforms/hierarchy, real rigid/skeletal animation and mutable geometry revisions to both native render providers. Static buffers do not need per-frame uploads when only an animated character changes.

`load_zone`/`zone_height` preserve evidenced support-cell/height semantics for an explicitly configured zone. Client input retains documented ground-click movement and WASD camera behavior. Original ISO-8859-9 help is converted toUTF-8; the native client can render it with the supplied local font. Explicit entity-model member selection is an independent appearance configuration, not a recovered class/animation-state mapping.

Malformed inputs fail with bounds/codec/stride/index/hierarchy errors, not replacement shapes or successful no-op animation. Rendering and end-to-end server/client proof is exercised by the integration owner; metadata/static inspection is not a native execution or original visual-parity claim.

## Gates to real original-asset rendering

Remaining evidence gates after the native asset cutover:

1. **Scene fidelity:** original lightmap bindings, authored mip chains, class dispatch and fog bounds are recovered; exact live environment/preferences, complete shader variants/effects/UI and controller/appearance/action mappings remain evidence gates.
2. **World alignment:** support-grid lookup is established; full wall/physics/navigation rejection categories and physical meter units are not inferred from cells.
3. **GPU comparison:** both providers consume real recovered geometry/material/animation; native runs and screenshots must substantiate their output. Source startup lighting values are separate from live world-time/zone assignment.
4. **Audio selection:** PCM playback format is established; root spatial music/sound index behavior and server-event-to-sound bindings need their own evidence.
5. **Simulation:** opaque data serialization and documentary contradictions require legitimate static/runtime evidence, not guessed canonical formulas.

OpenGL compatibility, enhanced Vulkan and optional ray tracing are project goals, **not properties established by this inspection tool**. Better rendering should follow recovered geometry/material/state, not obscure missing original asset support with a synthetic scene.

## Preservation and legal handling

Keep installer and extracted files local; reports contain metadata and small structural observations, not redistributed game resources. Native source is independently written. Existing Direct3D assemblies, fonts, textures, music, guide and ADF data remain copyrighted original assets; provenance does not imply redistribution rights. Any future recording of original behavior must remain local/authorized and must not bypass authentication, anti-cheat or probe official infrastructure.
