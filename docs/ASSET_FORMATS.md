# Recovered assets and documented behavior

## Provenance and evidence boundary

The supplied `istanbulkur6_0.exe` was preserved; its recorded SHA256 is `34bb864018ad0c46a06a26d8bf0b4c48cd4bcf4c8db971c64b2ebda1b6fbdca7`. Native innoextract completed into `original/app/` before this inventory. Installer metadata reports İstanbul Kıyamet Vakti 6.0 and data version 5.2.0; these are different version fields, not an asserted game-data schema version. No Windows program was executed, no official service contacted, and no proprietary assets are included in the independent native implementation.

**Observed** means bytes or documentation in this supplied installation; **inferred** means an interpretation still needing loader/runtime evidence. Player-facing documentation does not supply authoritative server algorithms. The PDF explicitly describes version 6.0 and warns that subsequent versions can differ (PDF page 1).

## Reproducible inventory

```sh
python3 tools/asset_inventory.py original/app --output docs/evidence/asset_inventory.json
```

The metadata-only report contains relative paths, exact sizes, SHA256 and the first 16 bytes in hexadecimal. It refuses symbolic-link entries, rejects output inside the installation and detects individual files changing while hashed. Run only after extraction; this is not an atomic snapshot of a concurrently changing directory. The observed completed installation contains **759 files, 4,075,408,963 bytes**.

| Extension | Count | Extension | Count |
|---|---:|---|---:|
| `.raw` | 371 | `.dds` | 175 |
| `.ogg` | 77 | `.bin` | 36 |
| `.jpg` | 31 | `.vsh` | 20 |
| `.psh` | 18 | `.dat` | 13 |
| `.xml` | 4 | `.ttf` | 3 |
| `.exe` | 3 | no extension | 3 |
| `.dll` | 2 | `.pdf` | 1 |
| `.ico` | 1 | `.old` | 1 |

The extension totals count loose files, **not** members of DAT archives. Three extensionless files are legacy CVS metadata under an audio directory, not resource formats. All per-file hashes are in the report; selected anchors:

| File | Bytes | SHA256 |
|---|---:|---|
| `base/objects.dat` | 607138652 | `fd72b8e69d5d811bc4e9e1ca9bcc200d21c71497128072de2e2ca0e65a75afb2` |
| `base/textures_4.DAT` | 46945066 | `4239657d2ac74041ec3f1a153eee97df937d3c200df13c336af598cf1b5e2ca2` |
| `base/helps.xml` | 15938 | `b6807ea73359dabd1f96ebeab08d777f45a414afea1e11621d74087a04300bf8` |
| `ikv_oyun_rehberi.pdf` | 3001896 | `b93c1f6ab1f1ac76f74124cde9a21081b05fe72cb2da09ed0a3a39aa37f95fa3` |

## SKYF v1 container — established on all seven supplied archives

All seven root `base/*.DAT`/`objects.dat` archives have the same readable recursive index. Bounded Python inspection traversed every index, verified every member range, and found contiguous payload coverage through exact EOF in traversal order. Header bytes are `53 4b 59 46 01 00`.

All integers below are unsigned little-endian; structures are serialized without alignment padding:

| Structure | Layout |
|---|---|
| File header, 6 bytes | ASCII `SKYF`, `u16 version` = 1 |
| Directory, 68 bytes | NUL-padded `name[64]`, `u16 child_directory_count`, `u16 file_count` |
| File entry, 136 bytes | NUL-padded `name[128]`, `u32 absolute_file_offset`, `u32 stored_byte_length` |

The root directory follows the header. Each directory is followed by its file entries, then each child directory recursively. Paths include the root name (`textures/…`, `lightmaps/…`, `objects/…`). Names in this sample are usable as path components; their original general character encoding is not established. Payload offsets are absolute from archive start. The container adds no compression or encryption; a member may itself contain compressed data.

Independent original-code corroboration is recorded in Ghidra decompilations `analysis/trace-game/decompile/00461790.c` (read-only archive open, six-byte header read) and `analysis/dispatch-game/decompile/00461430.c` (68-byte directories, 136-byte entries, files before recursion, child/file counts at +0x40/+0x42). See [engine evidence and exact analyzed image](ENGINE_ARCHITECTURE.md).

| Archive | Bytes | Directories incl. root | Members | Index end / first payload |
|---|---:|---:|---:|---:|
| `textures_1.DAT` | 744969738 | 1 | 1484 | 201898 |
| `textures_2.DAT` | 186549994 | 1 | 1484 | 201898 |
| `textures_4.DAT` | 46945066 | 1 | 1484 | 201898 |
| `lightmaps_1.DAT` | 1215649206 | 16 | 18404 | 2504038 |
| `lightmaps_2.DAT` | 307712854 | 16 | 18404 | 2504038 |
| `lightmaps_4.DAT` | 80555214 | 16 | 18383 | 2501182 |
| `objects.dat` | 607138652 | 21 | 1482 | 202986 |

**Do not assume identical lightmap membership across variants:** `_4` has 21 fewer entries. First texture member `textures/045marbretile256.dds` has offset 201898 and sizes 43832, 11064, 2872 respectively. DDS dimensions are 256×256, 128×128, 64×64 with 9, 8, 7 mip levels. This establishes resolution variants for this member, not every texture selection rule.

### Native reader contract

`include/ikv/assets.hpp`, `src/assets.cpp`, `src/adf_codec.cpp`, `src/asset_main.cpp` implement the read-only C++20 `ikv-assets` tool; `scene.cpp`, `scene_animation.cpp`, `scene_resources.cpp` provide the original-resource scene consumer. Zlib is required for ADF decoding.

```sh
./build/ikv-assets list original/app/base/textures_4.DAT
./build/ikv-assets inspect original/app/base/textures_4.DAT textures/045marbretile256.dds
./build/ikv-assets list original/app/base/objects.dat
./build/ikv-assets inspect original/app/base/objects.dat objects/arsenal/ID_AGACTPGZ.ADF
./build/ikv-assets scene original/app objects/maps/eminonu.ADF
./build/ikv-assets scene original/app objects/npc/AGAH.ADF 1.5
./build/ikv-assets --self-test
```

`list` reports paths/offsets/sizes. `inspect` reports headers and now actually decodes supported DDS/ADF payloads; `scene` reconstructs geometry, materials, transforms and recovered animation from original members. Originals remain read-only; no extraction or overwriting is performed. SKYF retains its 64-level/64-MiB index/path budgets. Payload decoding has a 512-MiB aggregate budget, validated ranges/strides/scalars/indices/hierarchy, bounded codecs and explicit unsupported-semantics diagnostics. Native build/test/renderer proof belongs to integration and is not inferred from static Python corpus inspection.

## Actual resource formats

### DDS textures and lightmaps

Every texture/lightmap archive member checked begins `DDS ` with a 124-byte DDS header (128 bytes including magic). Bounded header reads covered every member; no texture was rendered.

| Archive family | Header pixel formats |
|---|---|
| Each `textures_*` | 1040 DXT1, 166 DXT5, 276 uncompressed RGB16, 2 uncompressed RGB32 |
| `lightmaps_1` / `_2` | 18364 DXT1, 40 uncompressed RGB32 each |
| `lightmaps_4` | 18343 DXT1, 40 uncompressed RGB32 |

`decode_dds` produces every declared original mip level as one contiguous RGBA8 chain in source row order for DXT1/3/5 and mask-based RGB16/RGB32. `DdsMip` records each level's dimensions, offset and size; no base-level duplicate or generated mip substitutes authored pixels. Top-level RGB pitch, all source ranges, aggregate allocation, dimensions/masks/format and lower-level truncation are validated. Metadata inspection alone does not prove GPU sampling or colorspace parity. Loose DDS resources also provide UI atlases/effects and loose JPEGs provide loading/map images.

**Pack selection and GPU-native residency (wave 12).** The original opens exactly one texture and one lightmap pack (`004619d0`): `<kind>_4.dat`, `_2` or `_1` for its TextureDetail/LightmapDetail prefs 0/1/2; its launcher (`0x483330`) defaults to 2/1 on any machine with ≥1700 MB RAM. `ikv::texture_archive` follows the `texture_detail`/`lightmap_detail` settings (defaults 2 and 1; the client, launcher and particles share it) and only falls back to the nearest present pack when the selected one is missing. Scene and particle textures keep their authored DXT1/DXT3/DXT5 blocks (`decode_dds(bytes, true)`, `DdsFormat::bc1/bc2/bc3`, byte-identical to the DDS payload); renderers sample them as BC1-RGBA/BC2/BC3 or S3TC and decode with `decompress_dds` (exactly the CPU decoder) only when the device lacks BC sampling. UI images still decode to RGBA8. The 512 MiB scene texture budget now counts these resident bytes: every zone map at `textures_1`+`lightmaps_2` measures 7–119 MB (`karakoy` largest; `analysis/porting-wave12/Textures/impl/zone-resident.txt`, `ikv-assets scene` reports `texture_bytes`), where the same packs decoded to RGBA8 needed up to 920 MB.

### ADF geometry, materials, hierarchy and skeletal animation

All 1482 `objects.dat` members have `.ADF`/`.adf` names. Directory families include `arsenal`, `items`, `loc`, `maps`, `npc`, and `pc` character part directories. The 30 `objects/maps/` member names include `eminonu.ADF`, `karakoy.ADF`, `ada.adf`, `charsel.adf` and separate `_particles` resources; names do not prove collision/navigation semantics.

A bounded examination of `objects/arsenal/ID_AGACTPGZ.ADF` (offset 202986, 4632 bytes) establishes a chunk table for this resource:

- A 24-byte header begins `root`; `u32` at +4 = 5 and at +8 = 32.
- Five 32-byte records start at +24: `tag[4]`, identifier `u32`, stored length, decoded length, compression flag, resource-relative offset, and two opaque words. Opaque serialized words are never native pointers.
- Tags are `mate`, `item`, `vert`, `tris`, `qkey`. Offsets/lengths are 184/50, 234/76, 310/2607, 2917/1662, 4579/53.
- Native Python zlib successfully decoded all five bounded chunks to the advertised lengths 468, 256, 5808, 15040, 64. The `mate` chunk includes texture reference `agacyeni1.dds`.
- `objects/maps/ada.adf` begins `root` with +4 = 9528; its particle resource has +4 = 42. Static corpus inspection now validates this directory on all 1482 original members.

The native decoder uses the actual loaders, not geometric plausibility. The root codec selector at +12 is 255 for 1446 zlib members; 36 earlier members use the recovered adaptive 16-bit arithmetic codec. Its bits are LSB-first, model has 257 symbols and rescaling at total16383, and decoding stops at the declared output size without an unsafe redundant final-state read. Exact static evidence is in `analysis/assets-huffman/decompile/0040fcc0.c` and the four `0040f*` helpers. [Skeleton corpus proof](evidence/skeleton_decoder.json) records all 1482 read-only inspections; [selected mesh proof](evidence/adf_decoder.json) records hashes/counts for the real Eminönü map, tree prop and animated AGAH character.

| Chunk | Recovered fields and native use |
|---|---|
| `mate`, stride468 | ID+0, flags+4, double-sided word+8, alpha-blend word+12; original machine `CMP` against zero tests words, not float values. Flag-gated diffuse string+20, normal+212, environment+276, source-named luminance+340. The original loader does not consume name+404; inactive storage is not required to be a string. Duplicate IDs use the first registration. Flag32/+340 binds material handle+0x14: the standard fade-secondary texture, not additive emission or a per-instance lightmap. With no higher-priority normal/environment variant, fade samples both textures at UV0 and mixes secondary→diffuse by vertex alpha before lighting/optional lightmap subtraction. Flag16/+84 binds handle+0x10, unused by the recovered standard selector. Original color drawing requires the first primary handle (flags&1); absent handles, including copied zero registry state, have `source_color_draw=false` in every class. Manual native materials default true. Texture-backed alpha test uses79/255. Binder/constructor confirms SRCALPHA/INVSRCALPHA, ADD and CCW culling (CW front faces). |
| `item`/`bone`,256 | Both consume ID+8, owner+12, parent+16 and own name+192. Geometry items additionally consume type+20 and clone flag+28; referenced geometry name+128 is consumed only for actual clones. Bone+20/+28/+128 storage is not geometry-item metadata and can contain stale addresses. Bone self-root resolves to owner; hierarchy is bounded and topologically ordered. |
| `vert`,48 | XYZ+12 scaled0.1, authored normal+24 normalized when consumed; source indices+0/+4 and flags+8 supply standard aliases (normal mask0xe, position mask2) and type4 aliases (normal mask0xc, position mask2). Rooted type4 geometry clears authored normals before face/ordered alias accumulation; rootless geometry retains its authored contribution. Nonfinite unused/replaced words retain source provenance, not a blanket finite-field requirement. |
| `mver`,16 | Packed normal11/11/10 bits+0, XYZfloat+4 scaled0.1; exact original normal scaling/normalization. |
| `pack`,32 | Source indices+0/+4, packed normal11/11/10+8, XYZfloat+16/+20/+24 scaled0.1, flags+28. Exact packed-normal multiplier float bits are `3a802008` (11-bit) and `3b004020` (10-bit), followed by source normalization on the standard path. Packed tangent+12 is not consumed by the recovered standard path; dynamic aliases use the type4 masks. `mver` has identity aliases. |
| `tris`,160 | U32 indices+0, material+156, per-corner UV0 at12/44/76, UV1 at20/52/84, RGBA colors+108. |
| `ptri`,116 | U16 indices0/2/4, material+112, UV0 at8/40/72, UV1 at16/48/80, ARGB4 colors104/106/108. Both triangle paths reverse index order and flip V as the original upload routine does. |
| `qkey`,64 | XYZ+0 scaled0.1, quaternion+32, time seconds+48; linear translation, shortest-arc Slerp and recovered quaternion axis mapping `(qx,-qz,-qy,qw)`. The original loader does not consume the apparent scale floats. Chunks append to their resolved model track; strict later-key search extrapolates the first pair before start and retains cached pose after the last key, including all-zero multikey tracks. |
| `wmap`,32 | VertexU32+0, weightfloat+4 and per-bone local XYZ+8 converted `(-x,z,y)` feed original00476eb0 weighted bone-world **positions only**; serialized normal words+20/+24/+28 are unread by that routine. Zero weight skips local position reads; root-clear leaves zero accumulated positions if all weights are zero. No invented inverse-bind matrix or position renormalization. Last map per bone wins as original004a0590 does. Dynamic normals come from face/alias passes, not transformed wmap normals. |
| `ambd` | ItemID+0, then `n=(decoded_bytes-4)>>2` raw packed color DWORDs copied from+4 (original004a1870). Independent per-instance COLOR1 stream uses model+0x260/+0x264 GPU, +0x268 stride4, +0x26c count and +0x270 CPU. Original0047e990 resizes absent/mismatched streams to referenced geometry vertex count via0047c690: preserve `min(old,new)` authored words and initialize new words with constructor0047c3d0's `ffffffff` (RGBA1). Longer unused words/partial trailing DWORD bytes are source-proven ignored and diagnosed, not a count-equality invariant; a truncated ID header still rejects. This is the actual source color constructor, not inherited prototype colors or a fabricated white material. Distinct instance streams remain separate in the native folded representation. |
| `lmpd`,76 | ModelID+0; original004a1930 returns on an absent model before consuming filename+4. A resolved target consumes the bounded NUL filename; remaining bytes are not atlas transforms. A separate instance override binds actual `lightmaps/<map>/<filename>` or root lightmap DDS with unmodified UV1. Shader subtraction is lightmap RGB multiplied by source lightmap strength, not diffuse×lightmap. UV1 is shader-inactive without an active authored-lightmap geometry consumer; this does not mean the original loader never copied UV1. |

The earlier two-scene diagnosis exposed native-only assumptions, not corrupt supplied resources. Eminönü material ID0 references occur on exactly192 triangles in16 type1 `occ_` objects (first records: item173, tris175, `occ_misir_03`, ID`f50a748b`), each with an authored lightmap. Original `004a0540` copies the zero/null registry state at `DAT_0077bf6c` on **any** lookup miss; this is not an `occ_`-only exception or evidence for rejecting a missing nominally visual material. The original registry resets for each ADF (`004a3770`), so missing IDs do not establish global material dependencies. Source evidence remains in `analysis/scene-registry-original/` and `analysis/scene-registry-default/`. The closure retains all geometry, triangle indices, `Mesh.source_material_id` and authored lightmaps, records `source_color_draw=false`, and invents no textures. Effective material overrides control forward GL/Vulkan draw, residency and picking eligibility. Replacement RT/shadow exclusion is an independent native policy, not proof of original shadow behavior.

AGAH's first failing field is bone directory record4 (`BIP_AGA`, ID`02697f86`), unused bytes+128 beginning `60937f3698937f36d0937f3608947f36`. Original `004a2c70` never treats this storage as a string or bone+28 as a clone flag. Genuinely consumed names and texture/clone paths retain strict bounded NUL validation. Its nine single-key records targeting absent nodes are original loader no-ops (`004a2890`, registry-miss return), not missing scene actors. A related supplied NPC, `ID_ZIRHLI_TUFEKCI01.ADF`, has nine triangle-referenced source vertices with two authored zero-weight entries apiece; these entries and corners must remain valid, with zero accumulated positions as in original `00476eb0`, rather than enforcing positive total weight.

The existing embedded decoder regression entrypoint is `./build/ikv-assets --self-test`. The closure updates active/inactive field boundaries, first-registration semantics, no-color material/lightmap retention across classes and authored zero-weight behavior; the former “reject missing visual materials” policy is obsolete. No builds, tests, formatters or render smokes were run for this documentation change. Parent integration must exercise the native entrypoint and full corpus probe before treating code closure as verified.

The subsequent Eminönü ambient-budget failure came from expanding every triangle corner into a vertex and then copying that redundant geometry for every authored instance color stream. Read-only source cardinalities are659 geometry sources,918770 source vertices,451250 triangles and3446 ambient-bearing cloned objects. Exact indexed reuse of the same source identity, material partition, UV0/UV1 and quantized authored color reduces base backend vertices from1353750 to918758 while retaining all1353750 indices and every distinct instance ambient stream. For the native84-byte `Vertex`/4-byte index layout, static accounting predicts441035976 geometry bytes instead of628052568, within the unchanged512-MiB geometry budget. A source-to-backend link is added only for each unique emitted vertex, so shared triangle indices cannot multiply authored bone influences. These figures describe representation accounting, not measured process RSS or a claim that the changed city loader has run.

The next missing-resource failure was likewise a registry-versus-residency assumption. Eminönü declares172 material IDs but triangles reference only78 of them. Dormant record10 (`2ae1e957`, flags13) names diffuse `merannormal R1.dds` and normal `merannormal1.dds`; dormant record134 (`1bd3aa54`, flags1) names diffuse `zemin_ingiliztasi.dds`. These files are absent from all three texture packs and loose supplied DDS files, but neither material is used by any triangle. Original `00462100` registers handles without loading image bytes, `00460070` permits inert source-mode0 handles, and `004623a0` requests residency on demand. Exact read-only exports are in `analysis/scene-resource-reachability/`. The shared `scene_material_usage` graph follows effective resolved instance overrides (or mesh materials for standalone preview), excluding occlusion/no-color consumers. Both GPU providers preserve dormant material slots without resolving paths or replacing them with white textures. All declared records, paths and authored lightmaps remain present; a genuinely active missing image still fails. Original selected-lightmap-variant policy remains separate and unchanged.

The first actual GPU-binding failure after the scene CLI succeeded exposed a separate case-identity mismatch: material record113 (`e25eda2e`, flags9) referenced `textures/Bina_01.dds`, while the decoded image key was `textures/bina_01.dds`. Both GPU providers intentionally perform exact prepared-key lookup. Actual archive indices retain `textures/Bina_01.dds` in all three resolution variants; independently read DDS headers give1024×1024,512×512 and256×256 for `_1`, `_2` and `_4`. The fix applies the existing archive ASCII-case/slash canonicalization once to loaded bound material channels and legacy mesh texture keys, not during drawing. `Texture.name` is the runtime key; appended `Texture.source_path` records the selected archive basename and original-case virtual member path, preserving actual source and variant provenance without copying image payloads. Dormant declarations are untouched. Actor attachment reuses the same loader and canonical image keys; shader/provider lookup behavior remains unchanged. Source inspection found22 triangle-referenced channel declarations affected, so this is not a Bina-specific exception.

Actor attachment mutates the existing scene's mesh, texture and material vectors. OpenGL already invalidated its resource cache on these size changes; Vulkan now uses the same resource-cardinality invalidation before its existing scene reload, rather than allowing appended canonical actor keys to reach stale image or descriptor tables. This changes resource synchronization only, not shaders or scene geometry.

Evidence routines include `004a3570` dispatch, `00475fe0`/`00476190` vertices, `00469580`/`00469850` triangles, `004a2320` materials, `004444a0` quaternion matrices and `00476eb0` CPU skinning. Separate copied Ghidra projects preserve originals and the shared project. Geometry binds the current item, not a globally unique chunk ID: all three original Eminönü skydome occurrences and other repeated IDs are retained; deferred keys/ambient/lightmaps use the last registered matching model as the original does. Skin positions and face-derived normals/UV tangents update only on genuine changes, with per-mesh revisions retaining static city buffers. Dynamic source normal processing preserves raw face-area sums and ordered repeated-alias passes, not normalized generated normals: original0047dab0 copies CPU vertex bytes to the GPU and recurses without a final normalization. Rootless models retain their authored normalized initial contribution plus faces. Shader normalization parity is a separate consumer question. Corner remapping uses bounded contiguous arrays rather than allocating a container per source vertex. `attach_entity_model` accepts an explicitly selected original member and authority ID, not a recovered appearance/class mapping. Original controller cursor/loop/blend/action state and live environment assignments remain unresolved rather than invented.

Case-sensitive type1/4 prefixes select actual source sky/ocean/`isik_huzme`/`occ_` classes. Occlusion geometry remains retained but its original render virtual is a no-op; this does not prove wall collision. Fog half-extents are serialized signed IEEE fields scaled0.05, not positive-only mesh AABBs. `K_AYK_001_x00.ADF` has a legal negative extent: no abs/clamp. `K_GVD_S01_x00.ADF` item+116 raw`ffffffff` is a NaN extent: original0046b780 propagates corners, then0047cf50's ordered distance>threshold comparison is false when unordered. Preserve these source words and signed/NaN behavior; centers, basis, camera and transforms remain strictly validated. Center follows the reference/parent translation rule, while world3×3 rotates extents. Source startup lighting, clock defaults, black fog distance-scale1 and sky blend remain explicit. Original0046d680's unsigned tick gives sky rotation `tick_ms*5e-6`; native wrapping establishes the equation, not original clock-epoch or preference synchronization. [Class/duplicate/bounds proof](evidence/scene_classes.json) and [lightmap proof](evidence/lightmap_decoder.json) remain static evidence; all4101 Eminönü lightmap references resolve in each supplied variant. Actually absent lightmap images are diagnosed and retain non-lightmap drawing, never fabricated.

#### Semantic closure evidence — 2026-10-08

The **original full native corpus probe** recorded51 semantic errors:19 nonfinite-scalar resources,1 negative-half-extent resource,26 missing-material resources,4 unhandled `pack` geometry resources and1 absent-lightmap-target resource (`artifact://475:41587-41841`). This is observed pre-closure output, **not** a passing post-change corpus run. [ADF evidence](evidence/adf_decoder.json) retains all51 named paths, first implicated tag/chunk/decoded offsets/raw words and secondary nonfinite-field counts. The four pack failures identify unsupported geometry, not an invalid scalar offset: bounded first-record words and the negative-extent word are separately observed in `artifact://534`. The first nonfinite fields split into14 vertex-normal cases,4 inactive UV1 cases and1 source-bounds case; byte reachability plus the source root census (`artifact://515`) establish rooted type4 clearing for all17 nonstatic scalar resources. The two static resources are rootless and require their actual no-color/degenerate/unreferenced consumer classification rather than blanket authored-normal clearing. The398-word secondary census is static field evidence, not a full native consumer pass.

The code closure retains raw nonfinite IEEE metadata as `SourceScalarWord` (resource/chunk/decoded offset/bits/use); `source_resources` paths are remapped on attachment. Classified uses are `ReplacedNormal`, `NoColorNormal`, `DegenerateNormal`, `InactiveLightmapUv`, `UnorderedBounds`, `InactiveSkinNormal`, `ZeroWeightPosition` and `UnreferencedNormal`. Active XYZ, UV0, colors, indices, transforms and security limits remain strict; all indices, including zero-area triangles, remain intact. Original material sorting is unsigned-ID descending with the exact unstable00463150/004630f0 permutation, not an arbitrary stable sort. Source evidence is in `analysis/scene-semantic-source/`, `analysis/scene-semantic-sort-median/` and the observed byte-copy finalizer `analysis/scene-normal-finalizer/decompile/0047dab0.c`.

All26 first missing-material words are ID`00000000`; `ID_CEMBER_TRAP_DOOR.ADF` additionally has one triangle with ID`3f000000`, which uses the same registry-miss semantics. `misir.ADF` lmpd chunk1901 has payload/directory ID0, current model`69e78956`, stored offset911655 and filename`l_.dds`; original004a1930 returns for that target miss before reading the filename. This is a diagnosed no-op, not an invented binding. Structurally valid empty decode returns an explicit diagnostic, while standalone `load_scene` refuses an empty/blank render scene. Corpus categories `standalone_nonrenderable`, `structure_no_color_draw` and `renderable_structure_passed` distinguish outcomes; no blank model counts as visual proof.

The parent subsequently exercised the full repaired corpus (`artifact://545`,90.79349261s probe;98.94s compile+run): all7 archive structures,59818 DDS resources/390678 mip levels,1482 ADF chunk streams and18 zone pairs passed their respective checks, with full retained source-word actual-byte validation. Native semantic categories are1444 `renderable_structure_passed`,25 `structure_no_color_draw`,12 `standalone_nonrenderable` and **1 failure**: `objects/maps/buyukhol.ADF`, `ADF ambient vertex count`. Thus50 of the original51 semantic failures are resolved in this **intermediate observed run**, not an all-pass result. Bounded bytes show1379 ambient records and67 clone-count mismatches; first chunk7936 targets`d286582f`, with494 color words against496 prototype vertices, all496 referenced by active color geometry. At this probe the mismatch remained unresolved; it cannot be dismissed as unused/no-color. None of these structural categories establishes original visual/animation parity. Partial original shadows, particles, action/appearance mappings and renderer normalization/parity remain explicit unknowns; this documentation worker ran no checks, Windows program or network endpoint.

**Subsequent source/code closure:** complete primary exports in `analysis/scene-ambient-consumer-complete/decompile/004a1870.c`, `0047e990.c`, `0047c690.c`, `0047c3d0.c` and `0047ee50.c` establish the independent COLOR1 stream and preserving resize described above. Earlier false no-return typing had hidden the post-free loader body; `tools/ghidra/AssetReturnTypes.java` now restores actual body ranges through machine `RET`, without modifying the original image. Native clone ambient streams initialize independently, preserve the authored prefix and use the actual constructor for missing tails rather than inheriting prototype colors. Default clones of a colored prototype share one independent default-ambient geometry copy per prototype, not a copy per instance; an already all-white prototype needs none. Existing geometry caps are charged and source aliases retained. Grow/shrink, partial-tail, independent/shared-default-stream and malformed-header regressions have been added; this documentation worker did not run them. The original-corpus ambient correction was subsequently exercised in artifact560 below; artifact545 remains the historical50/51 intermediate result.

**Final observed original-corpus proof:** parent artifact560 exited0 (91.371940866s probe;99.62s compile+run), passing7 SKYF archives,59818 DDS resources/390678 mip levels,1482 ADF streams/226936 chunks and18 zones. All1482 native ADF structures classify as1445 `renderable_structure_passed`,25 `structure_no_color_draw` and12 `standalone_nonrenderable`; consumer failures and `first_failures` are zero/empty. Full retained `SourceScalarWord` exact-original-byte validation passed without raising existing caps. This closes **51/51 original semantic failures** and the active buyukhol ambient mismatch; it does **not** establish texture/archive-context drawing, animation action mappings or original visual parity, nor does a data-only/no-color classification count as a blank visual pass.

Separate selected-regression status remains explicit: Debug `asset-boundaries` then failed `ADF ambient size` because an obsolete first-pass modulo4 predicate rejected the source-proven13-byte COLOR1 fixture. Only that alignment predicate was removed; header>=4 and contextual header/chunk/id/size diagnostics remain. No fixture reduction or material fallback was introduced. This latest narrow correction awaits selected-regression validation; no selected CTest, ASan or GL success is asserted here. Every original AMBD payload is aligned, so that subsequent predicate removal does not invalidate artifact560's actual original-corpus proof.

Parent verification handoff (not executed here): first freshly build the native assets library, then run the existing decoder entrypoint above and the corrected full-corpus runner below. The runner privately includes `scene.cpp`; animation/resources compile directly, without linking `libikv-scene.a`.

```sh
/usr/bin/c++ -std=c++20 -O2 -I/mnt/2nd4TB/IKV/include \
  /mnt/2nd4TB/IKV/analysis/native_corpus_probe.cpp \
  /mnt/2nd4TB/IKV/src/scene_animation.cpp \
  /mnt/2nd4TB/IKV/src/scene_resources.cpp \
  /mnt/2nd4TB/IKV/build-full/libikv-assets-lib.a -lz \
  -o /tmp/ikv-native-corpus-probe && \
  timeout 930s /tmp/ikv-native-corpus-probe /mnt/2nd4TB/IKV/original/app/base
```

### Zone grids versus audio

All 18 supplied pairs are established as 2048×2048 row-major grids (`z*2048+x`), byte support cells and unscaled LEfloat32 worldY heights. Original generation uses four downward scene queries per cell;255 means all accepted,0 absent/rejected support, **not a proven complete wall/navigation mask**. World pitch is1.5; nominal originXZ(-1536,-1536). The bounded runtime query separately rounds float32 `coordinate*(2/3)` and `+1024`, then rounds half-away from zero, bounds each axis and accepts only255. `load_zone`/`zone_height` mirror that evidenced path, without interpolation or guessed unit conversions. [Zone proof](evidence/zone_decoder.json) records exact VAs, all18 hashes,75,497,472 finite heights and6,203,985 support cells.

Root `musicNN.raw`/`soundNN.raw` are each 4194304 bytes. `music00.raw` starts with 255 sentinel-like values and an interior sample contains 14; `sound00.raw` contains 255, 1 and 2. **[INFERENCE]** these are spatial music/sound selection grids, not playable raw audio. Do not send them to a PCM decoder based on extension alone.

Under `base/audio/`, 77 files begin `OggS`. Native `ffprobe` identified `audio/atmospheric/antipater.ogg` as **Vorbis, 44100 Hz, mono, 210.295374 s**:

```sh
ffprobe -v error -show_entries stream=codec_name,sample_rate,channels:format=duration \
  -of json original/app/base/audio/atmospheric/antipater.ogg
```

Original audio-directory RAW dispatch/setup is now recovered as **headerless signed LE16 PCM, mono,44100Hz** through the original WAVEFORMATEX and direct fread/DirectSound-lock path. Native readers reject odd-length files rather than losing a partial sample;328 of347 supplied audio RAW files are even-sized,19 rejected. Root music/sound grids are excluded. See [audio format evidence](evidence/audio-format.md) and [metadata](evidence/audio-format.json). `audio/sounds.xml` is ISO-8859-9; its sample weights need not be normalized.

### Shaders and particle configuration

The installation includes **20 textual `vs_1_1` vertex shaders**, **15 textual `ps.1.1` pixel shaders**, and **3 textual `ps_2_0` pixel shaders**. These are Direct3D assembly, **not GLSL/SPIR-V or original Cg source**. Compiler comments identify NVIDIA Cg 1.2.1001, March 17 2004; that dates the compiler string, not the installer.

`shaders/v_dif_env_norm_lit.vsh` documents position/normal/UV/tangent inputs and object/view/fog/sun/camera constants. `t_dif_env_norm_lit.psh` names diffuse/environment/normal/luminance samplers. Ocean, fog, lighting, particles, contour and pseudo-HDR-blur program names exist; names do not prove every path is used by the executable. Shader translation requires original attribute declarations, constant binding/transposition, state and texture sampling evidence.

`particle_effects/effects/effects.xml` is UTF-8 effect-to-system configuration with travel/attachment/hit-system fields. `systems/particle_systems.xml` declares ISO-8859-9 (all bytes are ASCII) and contains `StepPolicy`, `RenderPolicy`, original misspelling `EmiterPolicy`, texture paths, blend modes, generators and lifetimes. These document visual effects, **not server damage or projectile validation**.

### Particle systems and effects (recovered; implemented in `src/effects.cpp`)

Static evidence from `analysis/unpacked/istanbul.exe` (exports under `analysis/porting-wave1/Particles/ghidra*/`). `0041de60` loads `base\particle_effects\effects\effects.xml` and `...\systems\particle_systems.xml`. Per `ParticleSystem`, `0049ebe0` dispatches `StepPolicy`→`004986b0`, `EmiterPolicy`→`0049e2f0`, `RenderPolicy`→`004999e0`. Element and keyword comparisons use boost `iequals` with a locale (`004552d0`/`00492c30`/`00497a60`), so case is ignored, and an unknown element is ignored. That includes `DieAtDestinaton`, the misspelling used by 210 systems: only `DieAtDestination` (37 systems) has any effect.

| Element | Original semantics |
|---|---|
| `Gravity` vec3 or `min - max` | Each update adds `lerp(min,max,rand)` to velocity (`0049c440`) **without dt**, so it is per update. NONCANONICAL: the port scales it by `dt·60`, assuming the original's 60 Hz update. |
| `SizeStep`/`SizeStepVelocity`, `SpinStep` | Per second, as a range; random within the range on every step (`0049c240`). |
| `ColorStep` rgba range | Per second; a single lerp factor is shared by all channels (`0049c500`). |
| `DieTransparent` | Each step sets `alpha = life/total`. |
| `ParticleTexture` | `fx\x.dds` is a loose file; `textures\x.dds` is read from `textures_N.DAT`. |
| `BlendType` → `+0x120` | `blend`=1; `subtract`=2; `min`=3; `max`=4; anything else (`Sum`/`sum`/`sume`) =0. The draw (`0049a780`, which calls SetBlend `00443ec0` with enable, src, dst, op) uses: 0 → SRCALPHA,ONE,ADD; 1 → SRCALPHA,INVSRCALPHA,ADD; 2 → SRCALPHA,ONE,REVSUBTRACT; 3 → MIN; 4 → MAX. |
| `AlphaMaskValue` | Only for type 1, and only when non-zero: alpha test with ALPHAREF=`mask·255`, GREATER. |
| `DepthBias` | D3DRS_DEPTHBIAS (`0xc3`), added to depth in [0,1]. |
| `NoSpin`, `DoSort` | Booleans. |
| `AfterHDR` → `+0x12a` | Boolean, **default true** (ctor `0049d710`): 254 of 284 systems omit it. Systems with AfterHDR and a BlendType other than `blend` are drawn by `doRenderAfterHDR` (`0046cbe0`) after the fake-HDR glow composite, so they do not feed the glow; all others are drawn with the transparent models before it (`00478300` node flag +0x215). Ported as `ParticleBatch.after_hdr`. |
| `Heading` | `camera…` gives camera-facing billboards. Otherwise six floats give heading and up. A non-numeric `Vector 0 1 0` fails the stream and falls back to heading (0,1,0), up (1,0,0), i.e. a quad lying flat on the ground. |
| `MaxParticles`, `ParticleCount` | Concurrent cap; total emission count (-1 = infinite). |
| `CreationRate` range | Seconds between emissions. 0 fills up to the cap in one update. |
| `StartLife/Size/Spin/ColorRange` | Spawn ranges. |
| `TextureSet chance FPms` | Flat float list, 4 floats per frame (`u0 v0 u1 v1`). The set is picked by cumulative chance. With `FPms=-1` the frame is `round(n·(1-life/total))`; otherwise it is `(now_ms-birth)/FPms mod n` (`0049b6e0`). |
| `Position/Velocity/DestinationGenerator type=` | `box` (`00498020`): extents, then centre; uniform in centre±extent/2. `sphere` (`00498590`): r0 r1, then centre; random direction in [-1,1]³, normalised, radius in [r0,r1]. `cylinder` (`004981d0`): r0 r1 h, then centre; vertical, angle·2π, y in [-h/2,h/2]. `mesh*` exists but is unused by the XML. With a destination and `DieAtDestination`, velocity = (dest-pos)/life; otherwise velocity = normalize(dest-pos). |

Emission (`0049c7b0`) emits while all of these hold: emission remains (or the count is -1), the live count is below the cap, the accumulator is at least the interval, and the system is not stopped. A system is finished (`0049a520`) when its emission is exhausted or stopped and no particle remains. The billboard (`0049b6e0`) has corners `pos ± right·size ± up·size`, rotated about the normal by the spin unless `NoSpin`. Vertex colour is clamped to 0..255 and modulated by the texture. Randomness uses MSVC `rand()·(1/32767)`.

`effects.xml` is parsed by `00496020`. Attributes: `groupname`, `psystem_name`, `psystem2_name`, `psystem_name_head/trails/hit/hit_2/hit_3`, `texture_name`, `travel_speed`, `time_limit`, `y_shift`, `attached`. Group factory `004958f0`: the `kan`, `level_up`, `heal` and `aura` classes (`00495620`/`00495400`/`00495210`/`00495330`) ignore `psystem_name`. They create the fixed systems `kan`, `level_up`, `Heal` and `aura` at the actor position (`00478030`) raised in Y by +4.0, +3.0, 0 and +4.5 respectively. The `rtea` stat handler `004a59d0` fires `kan` on every HP decrease and `level_up` on a level gain, by calling `EffectManager 0x77d37c::create 00493ba0` and then `00491b40(1,entity,entity,0.5,0)`. `004a6a80` (`woey`) maps skills to effect names.

`00493fd0` stores each record as 0x130 bytes: strings `groupname` +0x00, `psystem_name` +0x1c, `texture_name` +0x38, `blend_type` +0x54, `psystem2_name` +0x70, `psystem_name_trails` +0x94, `psystem_name_hit` +0xb0, `psystem_name_head` +0xd0, `psystem_name_hit_2` +0xf4, `psystem_name_hit_3` +0x110; floats `time_limit` +0x8c, `y_shift` +0x90, `travel_speed` +0xec, `max_time_limit` +0xf0; bools `hasPositionShift` +0xcc, `attached` +0x12c. No group class reads `y_shift`, `hasPositionShift` or `attached`. Each class gets the record by value in its init (`004958f0`) and per-frame update (`00495bc0`, dt in seconds); an effect object is deleted with all its systems (`00492260`) once its kill byte +4 is set or none of its systems is alive (`004919d0`).
- `delayed_effect` (`004947c0`/`004948a0`): `psystem_name` starts at once as a free node at the effect position. On the first update with elapsed > `time_limit`, `psystem2_name`, `hit`, `hit_2` and `hit_3` start at the same position; `psystem_name_head` is never used. `zeplin_bomba` (3 s, `woey` type 0x42 sub 0x2e4) is the only shipped one.
- `attached_psystem_tex` (`00494b30`, no update): `psystem_attached_tex` (generator 8 units above the feet) as a child node of the target entity (`00494360` → `004a9cf0`), `texture_name` as its texture (`004934a0`) and, when `blend_type` is non-empty, its instance BlendType +0x58 (`00492eb0`: blend 1, subtract 2, min 3, max 4, otherwise 0). `bg_direnc`/`bg_kritik`/`bg_hasar` (`woey` 0x3f/0x41/0x40) set `blend`; their `y_shift` 70/50/−50 has no effect.
- `thrown_psystem` (`00494c00`/`00494e90`): start = source position +5.5 Y (`0x597e1c`), goal = target +3.0 Y (`0x592f28`); `psystem_name` `yerden_effect` uses both ground positions. Duration = distance/`travel_speed` if that is below `max_time_limit`, else `time_limit` (all shipped thrown effects: 0.7 / 1.0, fireball 0.8). Each update: elapsed += dt; while elapsed ≤ duration, position = start + (previous goal − start)·elapsed/duration, then the goal moves to the live target +3.0 (`gercek_elbombasi`: launch-time target + 0.7·remaining distance in Y, `0x597400`; `yerden_effect`: launch-time target +0.8, `0x58f06c`); the trails generator centre (`00491c70`) and the head node (`00491cc0`) take the position. A vanished target or zero duration kills the effect. On arrival (elapsed > duration) the trails system stops emitting (vtable +0xc `0049a510`), the head's tint +0x70 is zeroed, and `hit` and `hit_2` (never `hit_3`) start at the last goal.
- `zehirleme` (`004954d0`) places both systems at the target position, without the authored `y_shift` 2.

Zone `objects/maps/*_particles.adf` files hold type-11 `item` records: name at +192, particle-system name at +128. Thirteen zones carry them, for example `ada` (19), `labirent` (440), `buyukhol` (~2000). `eminonu_particles.adf` is absent. Seven referenced names are missing from the XML: `devana_taht_elektrik`, `kkoyisikhuzmetunel`, `kkoyisiktunel`, `ls_bulp`, `ls_huzur`, `ls_jenerator`, `seal`. The scene loader keeps the name in `SceneNode::source_particle_system`, and `EffectSystem::attach_scene` emits at the node's world translation. Node rotation is not applied: separately authored rotated variants such as `*_doksan` and `Mum Atesi 90` suggest the original also ignores it (**I**). Animation clips in `animation.dat` with `visual_effect` (`dev tavan mesale` ×8, `anten_explode`, `gustav_top`, `perihan_mektup`) fire with the clip cue.

Remaining unknowns:
- `DieAtDestination`'s default is assumed false.
- Duplicate system names (`aportal`, `yildirim`, `ls_fluoresant_yatay_doksan`) resolve to the first definition.
- `lightning`'s group class has no recovered system.
- Billboards are depth-tested with no soft-particle fade (ZENABLE on, ZWRITE off in `0049a780`). Ground-level systems such as `ates_cemberi` (centre Y 1.0, half-size 2.3–2.8) therefore show hard horizontal cuts where the quads enter the terrain; this is original behaviour, not an alpha or blend error.

### Game tables/localization

`data/animation.dat`, `classes.dat`, `skills.dat`, `prefixes.dat`, `playlist.dat`, and `playlistsound.dat` are not SKYF. Their shared prefix `c3 39 d1 9d 77 82 83 79 d5 bd 78 d7 ad 57 9f f5` is encoded XML, not evidence of an unknown compression algorithm: original `00450120` decodes byte `i` as `encoded[i] ^ (((i * 0x57) + 0x55) & 255) ^ 0xaa`, then invokes its XML parser. The actual classes/animation decode produced ISO-8859-9 XML; this does not establish gameplay formulas or complete schemas for the other tables.

Original `00439bd0` binds a class's model path, `meshid`, raw `classid` string and biped; the action-library identity hashes the concatenated `meshid`/`classid` strings. Original `0045dba0` reads named clip IDs and frame ranges, loops, loop bounds and jump ID/frame. Numeric clip fields accept ASCII surrounding whitespace, as original `_atol` does; this does not normalize the raw model/class identity strings. The source loop predicate is exactly `value == "true"`: actual `"False"` values are false, not malformed booleans. Frames multiply by the recovered float word `0x3d088889` (`0.033333335`, 30 Hz); no frame-minus-one offset is applied. The original generic actor constructor selects action **2**, not a guessed idle/default clip. For the actual Agah library (`ID_AGA`, class `16`), action 2 is `Agah_Hail` frames 75–135, followed by the authored jump to `Agah_Wait` (ID 1), frame 10, looping frames 10–70. `defaultclip="0"` is not a valid clip-0 record in that library; `defaultanimation="kAnimationClip_NULL"` does not authorize inventing one.

Attached actors use scene-owned decoded metadata and independent local clocks; standalone ADF/map sampling retains raw source time. Original `0045c860` gives generic startup/immediate transitions a minimum action duration of one second, including zero-span clips. Sampling independently uses the authored loop endpoint for both looping wrap and nonlooping clamp; its loop bounds need not be contained in the action start/end range. The original actor transition discards elapsed overshoot and performs one authored jump per update; it does not wrap the entire ADF timeline. Missing/ambiguous bindings or required active fields retain the original initialized pose with an explicit diagnostic. Constructor `0045e640` does not initialize absent loop-start/jump fields, so those cannot be assigned invented source defaults. This routing does **not** establish canonical self/player appearance assignment, live server-event/action selection, movement/audio triggers or crossfade parity. Native build, behavioral and GPU evidence for this cutover must be recorded separately from pre-cutover screenshots.

The scene resource reader converts actual ISO-8859-9 `helps.xml` to UTF-8 with original Book/Page/Paragraph text and colors (11 books,42 pages,64 paragraphs), bounded XML/no DTD or custom entities. Human HUD text uses original Verdana; Sobee's observed uppercase glyph slots are icons rather than a general text font. Original fonts are selected locally; their presence grants no redistribution permission. UI-atlas coordinate/layout recovery remains distinct from the working help/font and recovered actor-table resource paths.

## Documented version-6 gameplay, not backend formulas

**G** below is the supplied PDF, cited by PDF page; **H** is `base/helps.xml`, located by `Library/Book[@name=…]/Page[n]`. These are design evidence for later compatible work, not features asserted to exist in the independent native test world.

| System | Documented behavior | Evidence |
|---|---|---|
| Characters/classes | Savaşçı, Büyücü, Şifacı; warrior tank/damage roles, mage damage/control, healer support/resurrection. Four characters/account; sex changes appearance rather than strength. | G 15–19, 43 |
| Stats/combat | Energy is survivability, kudret powers abilities; attack/defense affect weapon hit chance. Armor reduces physical damage; fire/ice/electricity/acid/poison use matching resistances. One resistance point reduces matching damage by 1/1000. Displayed crit chance/multiplier are not a complete calculation pipeline. | G 22–24; H `Stats` 3–9 |
| Progression/skills | XP from kills/quests; core stats grow automatically in G. One skill point per level; skill rows unlock at 1/10/20/30/40; confirm point allocation. Free resets every five levels through 30, then 40; paid resets also described. Rank tables 1–15 are present, not proof of every runtime cap. | G 21–22, 30–31, 40, 51–74; H `Skills` 1 |
| Monster ownership/AI | First successful damage claims XP/loot; claimant death can transfer ownership to next attacker. Lower-level creatures yield reduced XP; threat from damage/abilities/healing selects targets. Leashing clears effects/threat/ownership and restores energy. | G 40–42 |
| Inventory/equipment | Bag pickup/equip, requirements, stacks/splitting, bank, confirmed deletion when dragged outside bag. Durability 1000 intact/0 unusable; paid repairs. Talisman slots 2 major + 2 minor; fitting zeros kudret. | G 25–29; H `Inventory` 1–6 |
| Trading | Vendors and player exchange; both parties confirm, changed offers invalidate confirmation, inventory space required. Vendor return 1/5 regular price or 1/3 with Plus. | G 29–30, 35 |
| Craft/gather | Silahtar/Zırhçı/Kimyacı craft; Madenci/Sarraf/Lokman gather. Two profession points per level; recipes learned/consumed, material consumption, loot-bag output; 100 learned recipes across professions. Shared nodes temporarily exhaust. | G 24, 28, 31–32, 39–40, 47; H `Craft` 1 |
| Quests | NPC acceptance, journal, chains, abandonment, timed failure/retry, XP/items; rewards diminish above quest level. Some abandoned non-NPC chains use GB-ANI recovery. | G 28, 32–33, 49; H `Quests` 1 |
| Groups | Maximum six; leader adds/removes, successor when leader leaves. Shared kill XP; random member loot assignment; shared kill objectives, individual other tasks; leaving region removes membership. | G 38, 43 |
| Death | Nearest safe/faction respawn, money and durability loss; healer resurrection. No death-XP penalty established. | G 45–46, 71; H `Inventory` 5 |
| PvP/factions | Kabadayı opt-in and Arzın Çocukları/Lodos warfare; faction access at level five. Charisma/glory/arena behavior described, including safe areas; examples do not give full glory formula. | G 44–46 |
| Controls | Ground left-click movement, entity selection, right-drag camera, F1 help, Esc menu, O sitting; ten quickbar slots via 1–9/0. Both H and G's illustrated chart assign WASD to camera, not locomotion. | G 14, 17, 31–37, 46, 48, 51; H `Quickbar` 1, `Seviye1a` 2–3 |

### Illustrated control chart, PDF page 37

Native `pdfinfo` confirms **74 PDF pages**; metadata dates the document February 28, 2018 (not independently verified publication date). Page 37 contains an image with no extractable text. Native `pdftoppm -f 37 -l 37 -singlefile -png` rendering was read locally, not redistributed. It confirms:

- WASD/arrows/right-drag move the camera; wheel and +/− zoom; F4 sets a locked elevated camera angle.
- Left-click ground runs/selects; right-click an enemy attacks or opens an entity interaction menu; Shift+left-click walks, Ctrl+left-click runs without selecting, Alt+left-click selects without moving.
- Space runs toward the selected target and attacks when in range; Tab cycles enemies near-to-far, Shift reverses; O toggles sitting.
- F5 toggles UI, F8 names, F10 performance/system display; Y skills, U production, G quests, H minimap, K character information, C inventory.
- Enter sends chat; Ctrl+C replies to last incoming whisper; Ctrl+X repeats the last outgoing whisper. Shift+left-click splits stacks; item context determines Ctrl+left-click linking and pickup/equip controls.
- Print Screen saves JPEG, Shift+Print Screen BMP, Ctrl+Print Screen TGA. These are original documented bindings, not necessarily the new native client's bindings.

### Contradictions and exact limits

- H `Stats` page 2 describes manual strength/intelligence/constitution/dexterity allocation, whereas G 21–22 describes automatic core growth. Do not combine them into one v6 implementation.
- Mage dispel unlock is 30 in G page 18 and 20 in page 62's table. Warrior defensive stance headings say 25% while page 52 table rows say 5%. These values are unresolved.
- G page 30 assigns Alt to both 100× and 1000× money adjustments. Do not invent the second key.
- Seven chat channels are stated, but additional guild chat is documented. G page 26 says player level must be higher than item level; exact equality behavior is not established.
- No full XP/stat curves, hit-chance function, resistance clamp/ordering, threat coefficients, drop odds, gathering cooldown, crafting probability, bag/stack limits, repair pricing or death-money-loss formula was recovered. Explicit guide percentages and examples are documentary claims, not measured server behavior.

## Remaining interoperability prerequisites

The read-only archive/DDS/ADF scene, recovered rigid/skeletal sampling, real lightmap bindings, original class/bounds metadata, zone support/height and Turkish help/font paths have native implementations plus runnable decoder checks and static corpus proof. Integration must exercise those native checks and both actual renderers before reporting success. Remaining evidence includes live environment/preferences, complete shader/effect/UI behavior, controller cursor/loop/blend/action/appearance mappings, opaque game-table serialization and documentary rule contradictions. None is replaced by invented canonical mechanics or renamed generated geometry.
