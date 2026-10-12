#pragma once
#include "ikv/assets.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <filesystem>
#include <string>
#include <optional>
#include <vector>
#include <memory>
#include <stdexcept>
namespace ikv {
struct Vertex { float x,y,z,nx,ny,nz,u,v; std::array<float,4> ambient{1,1,1,1}; std::array<float,3> tangent{}; std::array<float,2> uv1{}; std::array<float,4> color{1,1,1,1}; };
struct SkinInfluence { std::uint32_t vertex=0,node=0; float weight=0; std::array<float,3> position{},normal{}; };
struct Mesh { std::vector<Vertex> vertices; std::vector<std::uint32_t> indices; std::string texture; std::uint32_t material=0; std::vector<SkinInfluence> influences; std::uint64_t revision=0; std::uint32_t source_material_id=0; std::uint64_t key=0; }; // key: nonzero identity of an actor-assembly mesh, so renderers keep GPU resources across actor attach/detach
struct Texture { std::string name; std::uint32_t width=0,height=0; std::vector<std::uint8_t> data; std::vector<assets::DdsMip> mips; std::string source_path{}; assets::DdsFormat format=assets::DdsFormat::rgba8; }; // Original mip chain in `format` (authored BC blocks or RGBA8); source_path is archive basename:authored member path.
struct Material { std::string name,diffuse,normal,environment,luminance,lightmap; std::array<float,4> color{1,1,1,1}; bool alpha=false; float alpha_cutoff=0; std::uint32_t flags=0; bool double_sided=false; bool source_color_draw=true; bool source_ocean=false; };
struct Transform { std::array<float,16> matrix{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }; // column-major
struct Instance { std::uint32_t mesh=0; Transform transform; std::string name; std::uint32_t entity_id=0; std::uint32_t node=0; std::uint32_t material=UINT32_MAX; bool occlusion_proxy=false,sky=false,ocean=false; std::array<float,3> source_half_extents{}; bool source_support=false,source_nocamera=false; bool source_ordinary_color_participates=true; }; // Source collider classes and cached ordinary color reachability remain separate from material flags.
struct AnimationKey { float time=0; std::array<float,4> rotation{0,0,0,1}; std::array<float,3> position{}; };
struct AnimationTrack { std::uint32_t instance=0; std::vector<AnimationKey> keys; std::uint32_t controller=UINT32_MAX; AnimationKey cached_pose{},blend_pose{}; bool pose_initialized=false; }; // instance denotes scene node index; standalone tracks use raw source time
struct ActorClip { std::uint32_t id=0,next_id=0,next_frame=0; std::string name; float start=0,end=0,loop_start=0,loop_end=0; bool loops=false; std::vector<std::string> audio{}; std::string visual_effect{}; std::uint32_t state=0; float forward_speed=0; }; // audio: animation.dat audio/audio2/audio3 (0045dba0), raw source paths; 0045c890 picks one at random. state: animation.dat action class (0 when absent); forward_speed: forwardspeed (×50 = server wire units per tick)
struct ActorBinding { std::string path,model,class_id,biped; };
struct ActorLibrary { std::string model,class_id; std::vector<ActorClip> clips; std::string error; };
struct SourceActorRecord { unsigned uid=0; std::string tag,path,model,class_id,biped,object_type,bodypieces,suitable_slot; int icon=-1,pose=0; std::string icon_word,pose_word,error,shadow,diffuse; float bound_x=0,bound_y=0,bound_z=0; }; // shadow: shadowtype particle system (parser 0043a31d); diffuse: diffuse_texture (0043a90b), every material's diffuse on the entity (00434cd0); bound_x/y/z: boundx/boundy/boundz (0043a417/39/5b fill the per-classid bounds table 0x60acfc, 0 = absent)
struct ActorTables { std::filesystem::path base; bool loaded=false; std::vector<ActorBinding> bindings; std::vector<ActorLibrary> libraries; std::vector<SourceActorRecord> source_records; };
struct SourceSkill { int classindex=0,skillno=0; bool hasicon=false; int icon=-1; std::string sound,female_sound,name,detail; }; // skills.dat record (0044dad0); sounds are raw source paths, detail uses '|' line breaks
struct ActorAnimation { std::uint32_t library=UINT32_MAX,current_clip=UINT32_MAX; float previous_seconds=0,time=0,action_elapsed=0; bool initialized=false,stopped=false; float blend_duration=0,blend_elapsed=0,blend_from_time=0; std::uint32_t blend_from_clip=UINT32_MAX; bool action_held=false; std::uint32_t audio_cue=0; float rate=1; }; // action_held: looping clip requested by action; 0047bea0 gives it no jump deadline. audio_cue: bumped at 0045c980 non-looping clip entry and 0045cb9e loop wrap, where the original plays clip audio. rate: NONCANONICAL clip-clock multiplier (original 0047aa80 always advances real ms); the client sets it only for locomotion so authored stride matches authoritative ground speed.
struct SceneNode { std::uint32_t id=0; std::int32_t parent=-1; Transform local,world; std::int32_t source_reference=-1; std::array<float,3> source_bounds_center{}; std::string name; bool source_bone=false,source_has_geometry=false,source_attachment=false; std::string source_particle_system; }; // source_particle_system: type-11 item +128 particle_systems.xml name (zone *_particles.adf emitters).
struct Zone { std::uint32_t width=0,height=0; std::vector<std::uint8_t> cells; std::vector<float> heights; std::string source; float cell_size=1.5f; };
struct HelpParagraph { std::string text; std::uint32_t color=0xffffffff; };
struct HelpPage { std::string book,header; std::vector<HelpParagraph> paragraphs; };
// fog_record: original fog start/end distances (game-state record +0xec/+0xf0) before the Görüş Mesafesi scale;
// fog_start/fog_end/fog_far are the applied values (set_scene_fog).
struct SceneLighting { bool source_values=false; std::array<float,3> sun_direction{},ambient_multiplier{},diffuse_multiplier{},sky_b{},sky_c{}; bool fog_enabled=false; std::array<float,3> fog_color{}; std::array<float,2> fog_record{}; float fog_start=0,fog_end=0,fog_far=0,source_minute=749,lightmap_strength=0,sky_blend=0,sky_rotation=0,source_hour=-1,glow=0; unsigned source_zone=0; }; // source_hour: server clock (original thgf), -1 until received; glow: record HDR glow byte/255 (c308)
struct SourceSurfaceCorner { std::uint32_t mesh=0,vertex=0,next=UINT32_MAX; };
struct SourceSurface {
 std::vector<std::uint32_t> heads,indices,unweighted_corners;
 std::vector<SourceSurfaceCorner> corners;
 std::vector<std::array<std::uint16_t,2>> aliases;
 std::vector<std::array<float,3>> normals;
 bool recompute_normals=false,root_clears=false,normals_initialized=false,positions_changed=false;
};
enum class SourceScalarUse : std::uint8_t { ReplacedNormal,NoColorNormal,DegenerateNormal,InactiveLightmapUv,UnorderedBounds,InactiveSkinNormal,ZeroWeightPosition,UnreferencedNormal };
struct SourceScalarWord { std::uint32_t resource=0,chunk=0,offset=0,bits=0; SourceScalarUse use=SourceScalarUse::ReplacedNormal; };
// roofed (NONCANONICAL wave 17): drawn geometry covers the zone's walkable cells from above (an interior); such zones take
// the server's interior record (clock.interior) instead of the day/night cycle.
struct SourceSceneDescriptor { unsigned registry_id=0,support_id=0; std::string key,label,environment_key; float minute=749; std::string lightmap; std::vector<std::string> members; bool roofed=false; };
struct SourceEquipment { unsigned slot=0,uid=0; bool operator==(const SourceEquipment&) const = default; };
struct SourceAppearance { unsigned source_class=3,face=0,hair=0,hair_color=0,beard=0,beard_color=0; std::vector<SourceEquipment> equipment; bool operator==(const SourceAppearance&) const = default; };
// skin_held (set by the caller before update_scene): skip CPU skinning of this actor's meshes this frame, like original
// 00476eb0, which skins a root only while its +0xf4 frustum flag is set. The clip clock and bones keep advancing;
// skin_stale (update_scene bookkeeping) re-skins the actor on the first frame it is released.
struct SceneActorAssembly { std::uint32_t entity_id=0,controller=UINT32_MAX; SourceAppearance appearance; unsigned npc_uid=0; std::string member; std::array<std::size_t,8> begin{},end{}; std::vector<SourceEquipment> source_parts; std::array<std::size_t,2> weapon{}; bool skin_held=false,skin_stale=false; }; // meshes/materials/instances/nodes/tracks/surfaces/resources/scalars; weapon: slot-11 item instances, relative to begin[2]
struct Scene;
struct SceneModelResource { std::string member; std::shared_ptr<const Scene> decoded; };
struct Scene { std::vector<Mesh> meshes; std::vector<Texture> textures; std::vector<Material> materials; std::vector<Instance> instances; std::vector<AnimationTrack> animation; std::vector<SceneNode> nodes; std::vector<std::uint32_t> node_order; std::vector<HelpPage> help; Zone zone; SceneLighting lighting; std::vector<std::string> diagnostics; std::filesystem::path font; std::uint64_t geometry_revision=0,mesh_key=0; std::vector<SourceSurface> source_surfaces; std::vector<std::string> source_resources; std::vector<SourceScalarWord> source_scalar_words; ActorTables actor_tables; std::vector<ActorAnimation> actors; std::optional<unsigned> source_registry_id; unsigned source_support=UINT32_MAX; std::string source_environment_key,source_lightmap; std::vector<SceneActorAssembly> actor_assemblies; std::vector<SceneModelResource> model_resources; float animation_seconds=0; bool force_pose_update=false; };
// Validate placement overrides before combining source material eligibility and ordinary color reachability.
inline bool scene_source_color_draw(const Scene& scene,std::uint32_t mesh,
                                    std::uint32_t override_material=UINT32_MAX,bool ordinary_participates=true) {
    if(mesh>=scene.meshes.size())throw std::invalid_argument("scene color mesh index out of range");
    const auto material=override_material==UINT32_MAX?scene.meshes[mesh].material:override_material;
    if(scene.materials.empty())return ordinary_participates; // Preserve manually authored material-free previews.
    if(material>=scene.materials.size())throw std::invalid_argument("scene color material index out of range");
    return ordinary_participates&&scene.materials[material].source_color_draw;
}
std::vector<std::uint8_t> scene_material_usage(const Scene& scene);
// For each current mesh, its index in a renderer's previous mesh list (keys as uploaded), or SIZE_MAX if it must be uploaded.
// Keyed actor meshes may move when another actor detaches; unkeyed zone meshes are retained only at the same index.
std::vector<std::size_t> scene_mesh_remap(const std::vector<std::uint64_t>& previous_keys,const Scene& scene);
Scene load_scene(const std::filesystem::path& installation, const std::string& member="objects/maps/eminonu.ADF");
void attach_entity_model(Scene& scene,const std::filesystem::path& installation,const std::string& member,std::uint32_t entity_id);
const SourceSceneDescriptor& source_scene_descriptor(unsigned registry_id);
Scene load_source_scene(const std::filesystem::path& installation,unsigned registry_id);
Scene load_charsel_scene(const std::filesystem::path& installation); // Original character select/creation backdrop: slots c_character0..3, preview c_create_adamyarat.
void attach_source_actor(Scene& scene,const std::filesystem::path& installation,const SourceAppearance& appearance,std::uint32_t entity_id);
void attach_source_npc(Scene& scene,const std::filesystem::path& installation,unsigned source_uid,std::uint32_t entity_id,const std::vector<SourceEquipment>& equipment={});
void detach_source_actor(Scene& scene,std::uint32_t entity_id);
// The attach_* functions above are install_actor(scene, prepare_*(scene, ..., &scene)). Split, the prepare step reads and
// fills only `context` (actor tables, decoded model cache, diagnostics; a Scene owned by one thread at a time) and decodes
// the textures neither the model nor `resident` (names only) holds, so it can run off the frame thread. install_actor
// needs scene.actor_tables equal to the context's and the model's textures already in or moved into scene.textures.
struct PreparedActor { Scene model; ActorAnimation animation; SceneActorAssembly descriptor; bool entry=false; };
PreparedActor prepare_source_actor(Scene& context,const std::filesystem::path& installation,const SourceAppearance& appearance,std::uint32_t entity_id,const Scene* resident);
PreparedActor prepare_source_npc(Scene& context,const std::filesystem::path& installation,unsigned source_uid,std::uint32_t entity_id,const std::vector<SourceEquipment>& equipment,const Scene* resident);
PreparedActor prepare_entity_model(Scene& context,const std::filesystem::path& installation,const std::string& member,std::uint32_t entity_id,const Scene* resident);
void install_actor(Scene& scene,PreparedActor actor);
void set_actor_action(Scene& scene,std::uint32_t entity_id,std::uint32_t source_clip_id,float blend_seconds=.25f,bool restart=false,bool terminal=false); // Explicit restart/terminal seek are independent delivery policies; default retains source same-ID suppression.
Transform entity_instance_transform(const Scene& scene,const Instance& instance,float authority_x,float supported_ground,float authority_z,float orientation);
std::array<float,3> entity_source_bounds_center(const Scene& scene,const Instance& instance,float authority_x,float supported_ground,float authority_z,float orientation);
// The worn slot-11 weapon item's render-root instance (00495810 reads the item's +0x138 render node), or nullptr.
const Instance* source_actor_weapon(const Scene& scene,std::uint32_t entity_id);
Zone load_zone(const std::filesystem::path& installation,unsigned zone);
std::optional<float> zone_height(const Zone& zone,float x,float z);
void update_scene(Scene& scene,float seconds);
void scene_decoder_self_test();
void scene_animation_self_test();
std::vector<HelpPage> load_help(const std::filesystem::path& base);
std::vector<SourceSkill> load_source_skills(const std::filesystem::path& base);
std::vector<std::string> load_source_playlist(const std::filesystem::path& base,const char* name); // "playlist.dat"/"playlistsound.dat", file order
struct SourceRecord { std::string tag; std::map<std::string,std::string> attributes; std::vector<SourceRecord> children; }; // one data/*.dat element, UTF-8 attributes, file order
std::vector<SourceRecord> load_source_records(const std::filesystem::path& base,const char* name); // e.g. "classes.dat", file order
const SourceActorRecord& source_actor_record(const Scene& scene,unsigned source_uid);
std::string source_object_member(const std::string& path); // Canonical objects/ archive member; shares actor binding path validation.
ActorAnimation load_actor_animation(Scene& scene,const std::filesystem::path& base,const std::string& member);
ActorAnimation load_actor_animation(Scene& scene,const std::filesystem::path& base,const std::string& member,const SourceActorRecord& selected);
void scene_resources_self_test();
void set_scene_environment(Scene& scene,unsigned source_zone,std::optional<float> source_minute=std::nullopt);
// Server clock (original thgf): hour [0,24) and record ambient rgb, sun rgb, horizon rgb, lightmap shadow, HDR glow bytes.
void set_scene_clock(Scene& scene,float hour,const std::array<std::uint8_t,11>& record);
// 0046b630/0046b660 with the Görüş Mesafesi scale view: D3D linear fog start = record start·view (0x77c30c), end =
// record end·view + 110 (0x77c314); far = record end·view (0x77c31c, squared 0x77c320) bounds particle/model visibility.
void set_scene_fog(SceneLighting& light,float view);
// Original TextureDetail/LightmapDetail prefs (004619d0, globals 0x5eabb8/0x5eabbc): 0 = <kind>_4.DAT, 1 = _2, 2 = _1.
// Process-wide like the source prefs; applies to resources loaded afterwards. Default 2/1 (launcher >=1700 MB, 0x483330).
void set_texture_detail(unsigned textures,unsigned lightmaps);
// base/<kind>_N.DAT ("textures" or "lightmaps") for the selected detail, or the nearest present detail; empty when none.
std::filesystem::path texture_archive(const std::filesystem::path& base,const char* kind);
} // namespace ikv
