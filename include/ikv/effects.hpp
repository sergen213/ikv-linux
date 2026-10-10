#pragma once

#include "ikv/renderer.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ikv {
// Original particle_effects/systems/particle_systems.xml record (parsers 004986b0/004999e0/0049e2f0).
struct ParticleRange { float min=0,max=0; };
struct ParticleGenerator {
    enum class Shape : std::uint8_t { none,box,sphere,cylinder } shape=Shape::none;
    std::array<float,3> size{},center{}; // box: extents; sphere: r0,r1; cylinder: r0,r1,height
};
struct ParticleTextureSet { float chance=0; int ms_per_frame=-1; std::vector<std::array<float,4>> frames; };
struct ParticleSystemDef {
    std::string name,texture; // texture: authored path, "fx/x.dds" (loose) or "textures/x.dds" (archive)
    std::array<float,3> gravity_min{},gravity_max{};
    std::array<float,4> color_step_min{},color_step_max{};
    ParticleRange size_step,spin_step;
    bool die_transparent=false;
    ParticleBlend blend=ParticleBlend::add;
    float alpha_mask=0,depth_bias=0;
    bool no_spin=true,sort=false,camera_heading=true; // RenderPolicy ctor 0049d710: NoSpin +0x128=1, DoSort +0x129=0, heading +0x130=1
    bool after_hdr=true; // RenderPolicy ctor 0049d710 @49d757 defaults AfterHDR (+0x12a) to true
    std::array<float,3> heading{0,1,0},up{1,0,0};
    int max_particles=0,count=0;
    ParticleRange creation_rate,life,size,spin;
    std::array<float,4> color_min{},color_max{};
    bool die_at_destination=false;
    ParticleGenerator position,velocity,destination;
    std::vector<ParticleTextureSet> texture_sets;
};
// Original particle_effects/effects/effects.xml record (parser 00496020 → 00493fd0, 0x130-byte record). y_shift (+0x90),
// hasPositionShift (+0xcc) and attached (+0x12c) are parsed but no effect class (004958f0/00495bc0) ever reads them.
struct EffectDef {
    std::string name,group,texture,system,system2,head,trails,blend; // blend: lower-case blend_type (+0x54)
    std::array<std::string,3> hit;
    float travel_speed=0,time_limit=0,max_time_limit=0; // +0xec, +0x8c, +0xf0
    bool attached=true;
};
std::vector<ParticleSystemDef> parse_particle_systems(std::string_view xml);
std::vector<EffectDef> parse_effects(std::string_view xml);

// Client-owned visual effect simulation. Particles are spawned in world space (scene units, Y up).
class EffectSystem {
public:
    explicit EffectSystem(const std::filesystem::path& asset_base);
    // Unknown names are reported once on stderr and ignored; a bare particle-system name is also accepted.
    void trigger(std::string_view effect_name,std::uint32_t entity_id);
    void trigger_at(std::string_view effect_name,std::array<float,3> position);
    // thrown_psystem 00494c00/00494e90: head/trails fly source->target over a fixed duration, then the two hit systems burst.
    void launch(std::string_view effect_name,std::uint32_t source_entity,std::uint32_t target_entity);
    // Persistent actor system (0047aa00 attaches classes.dat shadowtype, e.g. "golge" blob shadow, to the actor):
    // idempotent per (system, visible entity); particles move with the entity and the system re-arms when exhausted.
    // tint multiplies particle colour (0047b250 colour, e.g. the GroundHilite selection ring); a repeat call updates it.
    // weapon: weapon_gfx_psystem (00495810/00492130) replaces the position generator by random points on the worn slot-11
    // item mesh (00491e40), so the particles burn along the weapon instead of at the feet.
    void attach(std::string_view system_name,std::uint32_t entity_id,std::array<float,4> tint={1,1,1,1},bool weapon=false);
    // 0047b250(false): destroys the entity's attached system and its particles at once.
    void detach(std::string_view system_name,std::uint32_t entity_id);
    // Replaces scene emitters with the scene's type-11 "particle_*" nodes (SceneNode::source_particle_system); particles
    // are simulated in node space and drawn through the node's world matrix (0049b6e0 @49ba09 transforms by +0x58).
    void attach_scene(const Scene&);
    // 00478586: emitters farther than sqrt(range²) from the camera are skipped (Ayarlar Ortam Efektleri / Karakter Efektleri,
    // pref·160000), and every emitter farther than sqrt(.5·k·far²) (far = fog far distance 0x77c320, k = node +0x21c:
    // 1, 3.05 for "particle_far_*" scene nodes, 004a2e70).
    void set_ranges(float scene_squared,float entity_squared,float far_squared) { ranges_={scene_squared,entity_squared,far_squared}; }
    void update(float dt,std::span<const RenderEntity> entities);
    const ParticleFrame& frame(const Camera&);
    std::size_t particle_count() const;
    static void self_test();
private:
    struct Particle { std::array<float,3> position,velocity; std::array<float,4> color; float size,life,total,spin; std::uint32_t born_ms; std::int32_t set; };
    struct Emitter {
        const ParticleSystemDef* def; std::uint32_t texture; std::array<float,3> origin;
        std::uint32_t entity=0,projectile=0,node=UINT32_MAX; float y_shift=0,stop_after=-1,age=0,accumulator=0,interval=0;
        int remaining=0; bool stopped=false,follow=false,weapon=false; std::array<float,4> tint{1,1,1,1}; float far_scale=1;
        ParticleBlend blend=ParticleBlend::add; // instance BlendType +0x58 (0049daa0 copies the system's; 00492eb0 overrides)
        std::vector<Particle> particles{};
    };
    EffectSystem(const std::filesystem::path& asset_base,std::vector<ParticleSystemDef>,std::vector<EffectDef>);
    // thrown_psystem object: start +0x24, goal +0x30, target position at launch +0x3c, elapsed +0x88, duration +0x8c.
    struct Projectile { const EffectDef* def; std::uint32_t id,target; std::array<float,3> start,goal,target_start,position; float elapsed=0,duration=0; bool done=false; };
    struct Delayed { const EffectDef* def; std::array<float,3> at; float elapsed=0; }; // delayed_effect 004947c0/004948a0
    std::filesystem::path base_;
    std::vector<ParticleSystemDef> systems_;
    std::vector<EffectDef> effects_;
    std::unordered_map<std::string,std::size_t> system_index_,effect_index_,texture_index_;
    std::vector<Texture> textures_;
    std::vector<Emitter> emitters_;
    std::vector<Projectile> projectiles_;
    std::vector<Delayed> delayed_;
    std::vector<RenderEntity> entities_;
    std::unordered_map<std::string,bool> reported_;
    ParticleFrame frame_;
    const Scene* scene_=nullptr;
    std::array<float,3> ranges_{INFINITY,INFINITY,INFINITY}; // squared scene units: zone emitters, entity/projectile emitters, fog far
    std::uint32_t random_=1,now_ms_=0,next_projectile_=1;
    float random();
    float random(ParticleRange r) { return r.min==r.max?r.min:r.min+(r.max-r.min)*random(); }
    std::array<float,3> generate(const ParticleGenerator&);
    const ParticleSystemDef* system(std::string_view);
    std::uint32_t texture(std::string_view);
    void spawn(const ParticleSystemDef&,std::array<float,3> origin,std::uint32_t entity,float y_shift,float stop_after,std::uint32_t texture=UINT32_MAX);
    void spawn_effect(const EffectDef&,std::array<float,3> origin,std::uint32_t entity);
    void step(Emitter&,float dt,std::array<float,3> from); // from: origin at the start of the update (sub-frame emission)
    void report(std::string_view what,std::string_view name);
};
} // namespace ikv
