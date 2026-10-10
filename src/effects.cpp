// Original particle/effect system. Recovered from analysis/unpacked/istanbul.exe (imagebase 0x00400000);
// addresses and semantics are documented in docs/ASSET_FORMATS.md "Particle systems and effects".
#include "ikv/effects.hpp"
#include "ikv/assets.hpp"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace ikv {
namespace {
using Tree = boost::property_tree::ptree;
constexpr std::size_t max_xml = 8u << 20;

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string_view::npos ? std::string{} : std::string(s.substr(b, e - b + 1));
}
Tree parse(std::string_view xml) {
    if (xml.size() > max_xml) throw std::runtime_error("particle XML exceeds 8 MiB");
    std::istringstream in{std::string(xml)};
    Tree tree;
    boost::property_tree::read_xml(in, tree, boost::property_tree::xml_parser::no_comments);
    return tree;
}
// Locale-independent float stream; stops at the first token that is not a finite number (original istream failbit).
struct Numbers {
    const char* p; const char* end;
    explicit Numbers(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}
    void skip() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; }
    bool next(float& value) {
        skip();
        float v = 0;
        const auto [q, error] = std::from_chars(p, end, v);
        if (error != std::errc{} || !std::isfinite(v)) return false;
        p = q; value = v; return true;
    }
    template <std::size_t N> bool read(std::array<float, N>& out) { for (auto& v : out) if (!next(v)) return false; return true; }
};
// 00497a00 (scalar), 00497b20 (vec3), 00497e30 (vec4): "min [- max]"; a missing max repeats min.
template <std::size_t N> std::pair<std::array<float, N>, std::array<float, N>> range(const std::string& text) {
    Numbers in(text);
    std::array<float, N> a{}, b{};
    if (!in.read(a)) return {a, a};
    in.skip();
    if (in.p < in.end && *in.p == '-') { ++in.p; if (!in.read(b)) b = a; }
    else b = a;
    return {a, b};
}
ParticleRange scalar(const std::string& text) { auto [a, b] = range<1>(text); return {a[0], b[0]}; }
bool truth(const std::string& text) { return lower(trim(text)) == "true"; } // iequals(DAT_0058e7fc "true")
int integer(const std::string& text) {
    const auto t = trim(text);
    int v = 0;
    const auto [q, error] = std::from_chars(t.data(), t.data() + t.size(), v);
    return error == std::errc{} ? v : 0;
}
float number(const std::string& text) { Numbers in(text); float v = 0; in.next(v); return v; }
std::vector<float> flat(const std::string& text) { Numbers in(text); std::vector<float> out; float v; while (out.size() < 4096 && in.next(v)) out.push_back(v); return out; }
std::array<float, 3> normalize(std::array<float, 3> v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) for (auto& c : v) c /= l; // 0046ad90 leaves zero vectors unchanged.
    return v;
}
std::array<float, 3> cross(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {a[1] * b[2] - b[1] * a[2], b[0] * a[2] - a[0] * b[2], a[0] * b[1] - b[0] * a[1]};
}
// 00498cc0: box/sphere/cylinder carry extents then centre; mesh* generators bind to a mesh and are unused by the XML.
ParticleGenerator generator(const Tree& node, const std::string& system) {
    ParticleGenerator g;
    const auto type = lower(trim(node.get<std::string>("<xmlattr>.type", "")));
    const auto values = flat(node.get_value<std::string>());
    const auto at = [&](std::size_t i) { return i < values.size() ? values[i] : 0.f; };
    std::size_t centre = 3;
    if (type == "box") g.shape = ParticleGenerator::Shape::box;
    else if (type == "cylinder") g.shape = ParticleGenerator::Shape::cylinder;
    else if (type == "sphere") { g.shape = ParticleGenerator::Shape::sphere; centre = 2; }
    else { std::cerr << "particle system " << system << ": unknown generator '" << type << "'\n"; return g; }
    for (std::size_t i = 0; i < centre; ++i) g.size[i] = at(i);
    for (std::size_t i = 0; i < 3; ++i) g.center[i] = at(centre + i);
    return g;
}
// Authored "fx\x.dds" (loose base/fx) or "textures\x.dds" (textures_N.DAT); effects.xml doubles backslashes.
std::string resource_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string out;
    for (char c : trim(path)) if (c != '/' || out.empty() || out.back() != '/') out.push_back(c);
    return out;
}
std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() > max_xml) throw std::runtime_error(path.string() + " exceeds 8 MiB");
    return bytes;
}
} // namespace

std::vector<ParticleSystemDef> parse_particle_systems(std::string_view xml) {
    const auto tree = parse(xml);
    const auto root = tree.find("ParticleSystems");
    if (root == tree.not_found()) throw std::runtime_error("particle_systems.xml lacks ParticleSystems root");
    std::vector<ParticleSystemDef> out;
    for (const auto& [key, node] : root->second) {
        if (lower(key) != "particlesystem") continue;
        ParticleSystemDef d;
        d.name = trim(node.get<std::string>("<xmlattr>.name", ""));
        // Element names compare with boost iequals (004552d0/00492c30/00497a60), so misspelled
        // "DieAtDestinaton" is ignored exactly like the original parser ignores it.
        for (const auto& [policy_key, policy] : node) {
            const auto policy_name = lower(policy_key);
            for (const auto& [element_key, element] : policy) {
                const auto k = lower(element_key);
                const auto text = element.get_value<std::string>();
                if (policy_name == "steppolicy") { // 004986b0
                    if (k == "gravity") std::tie(d.gravity_min, d.gravity_max) = range<3>(text);
                    else if (k == "colorstep") std::tie(d.color_step_min, d.color_step_max) = range<4>(text);
                    else if (k == "sizestep" || k == "sizestepvelocity") d.size_step = scalar(text);
                    else if (k == "spinstep") d.spin_step = scalar(text);
                    else if (k == "dietransparent") d.die_transparent = truth(text);
                } else if (policy_name == "renderpolicy") { // 004999e0
                    if (k == "particletexture") d.texture = resource_path(text);
                    else if (k == "alphamaskvalue") d.alpha_mask = number(text);
                    else if (k == "nospin") d.no_spin = truth(text);
                    else if (k == "dosort") d.sort = truth(text);
                    else if (k == "depthbias") d.depth_bias = number(text);
                    else if (k == "blendtype") {
                        const auto b = lower(trim(text));
                        d.blend = b == "blend" ? ParticleBlend::alpha : b == "subtract" ? ParticleBlend::subtract :
                                  b == "min" ? ParticleBlend::min : b == "max" ? ParticleBlend::max : ParticleBlend::add;
                    } else if (k == "heading") {
                        d.camera_heading = lower(trim(text)).starts_with("camera");
                        if (!d.camera_heading) {
                            Numbers in(text);
                            std::array<float, 3> a{}, b{};
                            // A non-numeric "Vector ..." prefix fails the stream: original falls back to (0,1,0),(1,0,0).
                            if (in.read(a) && in.read(b)) { d.heading = normalize(a); d.up = normalize(b); }
                            else { d.heading = {0, 1, 0}; d.up = {1, 0, 0}; }
                        }
                    } else if (k == "afterhdr") d.after_hdr = truth(text); // RenderPolicy +0x12a
                } else if (policy_name == "emiterpolicy") { // 0049e2f0
                    if (k == "maxparticles") d.max_particles = integer(text);
                    else if (k == "particlecount") d.count = integer(text);
                    else if (k == "creationrate") d.creation_rate = scalar(text);
                    else if (k == "startliferange") d.life = scalar(text);
                    else if (k == "startspinrange") d.spin = scalar(text);
                    else if (k == "startsizerange") d.size = scalar(text);
                    else if (k == "startcolorrange") std::tie(d.color_min, d.color_max) = range<4>(text);
                    else if (k == "dieatdestination") d.die_at_destination = truth(text);
                    else if (k == "positiongenerator") d.position = generator(element, d.name);
                    else if (k == "velocitygenerator") d.velocity = generator(element, d.name);
                    else if (k == "destinationgenerator") d.destination = generator(element, d.name);
                    else if (k == "textureset") {
                        ParticleTextureSet set;
                        set.chance = number(element.get<std::string>("<xmlattr>.chance", "0"));
                        set.ms_per_frame = integer(element.get<std::string>("<xmlattr>.FPms", "-1"));
                        const auto values = flat(text);
                        for (std::size_t i = 0; i + 4 <= values.size(); i += 4)
                            set.frames.push_back({values[i], values[i + 1], values[i + 2], values[i + 3]});
                        if (!set.frames.empty()) d.texture_sets.push_back(std::move(set));
                    }
                }
            }
        }
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<EffectDef> parse_effects(std::string_view xml) {
    const auto tree = parse(xml);
    const auto root = tree.find("Effects");
    if (root == tree.not_found()) throw std::runtime_error("effects.xml lacks Effects root"); // 00496020 "Node Name 'Effects' doesn't exist."
    std::vector<EffectDef> out;
    for (const auto& [key, node] : root->second) {
        if (key != "Effect") continue;
        const auto attr = [&](const char* name) { return trim(node.get<std::string>(std::string("<xmlattr>.") + name, "")); };
        EffectDef e;
        e.name = attr("name"); e.group = attr("groupname");
        e.system = attr("psystem_name"); e.system2 = attr("psystem2_name");
        e.head = attr("psystem_name_head"); e.trails = attr("psystem_name_trails");
        e.hit = {attr("psystem_name_hit"), attr("psystem_name_hit_2"), attr("psystem_name_hit_3")};
        const auto texture = attr("texture_name");
        e.texture = resource_path(texture);
        e.travel_speed = number(attr("travel_speed")); e.time_limit = number(attr("time_limit")); e.max_time_limit = number(attr("max_time_limit"));
        e.blend = lower(attr("blend_type"));
        e.attached = attr("attached") != "0";
        out.push_back(std::move(e));
    }
    return out;
}

EffectSystem::EffectSystem(const std::filesystem::path& asset_base) : base_(asset_base) {
    systems_ = parse_particle_systems(read_file(base_ / "particle_effects" / "systems" / "particle_systems.xml"));
    effects_ = parse_effects(read_file(base_ / "particle_effects" / "effects" / "effects.xml"));
    // ponytail: first definition wins for the three duplicated names (aportal, yildirim,
    // ls_fluoresant_yatay_doksan); the original container insertion order is unrecovered.
    for (std::size_t i = 0; i < systems_.size(); ++i) system_index_.emplace(systems_[i].name, i);
    for (std::size_t i = 0; i < effects_.size(); ++i) effect_index_.emplace(effects_[i].name, i);
}

float EffectSystem::random() {
    // MSVC CRT rand(): the original scales rand() by 1/32767 (DAT_00592ee0).
    random_ = random_ * 214013u + 2531011u;
    return static_cast<float>((random_ >> 16) & 0x7fffu) * (1.f / 32767.f);
}

std::array<float, 3> EffectSystem::generate(const ParticleGenerator& g) {
    std::array<float, 3> p{};
    switch (g.shape) {
    case ParticleGenerator::Shape::none: break;
    case ParticleGenerator::Shape::box: // 00498020
        for (unsigned i = 0; i < 3; ++i) p[i] = random() * g.size[i] - g.size[i] * .5f;
        break;
    case ParticleGenerator::Shape::sphere: { // 00498590: random direction in [-1,1]^3, radius in [r0,r1]
        std::array<float, 3> d{};
        for (auto& c : d) c = -1 + 2 * random();
        const float r = g.size[0] + (g.size[1] - g.size[0]) * random();
        d = normalize(d);
        for (unsigned i = 0; i < 3; ++i) p[i] = d[i] * r;
        break;
    }
    case ParticleGenerator::Shape::cylinder: { // 004981d0: vertical, angle*2pi, radius [r0,r1], height [-h/2,h/2]
        const float angle = random() * 6.2831855f;
        float radius = g.size[0];
        if (g.size[0] != g.size[1]) radius = random() * (g.size[1] - g.size[0]) + g.size[0];
        float y = g.size[2] * -.5f;
        if (g.size[2] != 0) y = random() * g.size[2] + y;
        p = {std::sin(angle) * radius, y, std::cos(angle) * radius};
        break;
    }
    }
    for (unsigned i = 0; i < 3; ++i) p[i] += g.center[i]; // 00492060
    return p;
}

void EffectSystem::report(std::string_view what, std::string_view name) {
    const auto key = std::string(what) + ':' + std::string(name);
    if (reported_.emplace(key, true).second) std::cerr << "effects: " << what << ' ' << name << '\n';
}

const ParticleSystemDef* EffectSystem::system(std::string_view name) {
    const auto found = system_index_.find(std::string(name));
    if (found == system_index_.end()) { report("missing particle system", name); return nullptr; }
    return &systems_[found->second];
}

std::uint32_t EffectSystem::texture(std::string_view name) {
    const auto key = lower(name);
    if (const auto found = texture_index_.find(key); found != texture_index_.end()) return static_cast<std::uint32_t>(found->second);
    std::uint32_t index = UINT32_MAX;
    try {
        std::vector<std::uint8_t> bytes;
        const auto slash = key.find('/');
        const auto directory = slash == std::string::npos ? std::string("fx") : key.substr(0, slash);
        const auto file = slash == std::string::npos ? key : key.substr(slash + 1);
        std::string source;
        if (directory == "textures") { // same archive preference as scene.cpp: _4, _2, _1
            for (const auto* suffix : {"_4.DAT", "_4.dat", "_2.DAT", "_1.DAT"}) {
                const auto path = base_ / (std::string("textures") + suffix);
                if (!std::filesystem::is_regular_file(path)) continue;
                std::ifstream in(path, std::ios::binary);
                const auto archive = assets::read_archive(in, std::filesystem::file_size(path));
                for (const auto& entry : archive.entries)
                    if (lower(entry.path) == key) { bytes = assets::read_entry(in, entry, 64u << 20); source = path.filename().string() + ':' + entry.path; break; }
                break;
            }
        } else if (directory == "fx") {
            for (const auto& entry : std::filesystem::directory_iterator(base_ / "fx"))
                if (lower(entry.path().filename().string()) == file) {
                    const auto text = read_file(entry.path());
                    bytes.assign(text.begin(), text.end());
                    source = entry.path().filename().string();
                    break;
                }
        }
        if (bytes.empty()) throw std::runtime_error("not found");
        auto image = assets::decode_dds(bytes);
        Texture t; t.name = key; t.width = image.width; t.height = image.height;
        t.rgba = std::move(image.rgba); t.mips = std::move(image.mips); t.source_path = source;
        textures_.push_back(std::move(t));
        index = static_cast<std::uint32_t>(textures_.size() - 1);
    } catch (const std::exception& e) { report(std::string("particle texture unavailable (") + e.what() + ")", name); }
    texture_index_.emplace(key, index);
    return index;
}

void EffectSystem::spawn(const ParticleSystemDef& def, std::array<float, 3> origin, std::uint32_t entity, float y_shift, float stop_after, std::uint32_t texture_override) {
    Emitter e{&def, texture_override == UINT32_MAX ? texture(def.texture) : texture_override, origin};
    e.entity = entity; e.y_shift = y_shift; e.stop_after = stop_after; e.remaining = def.count; e.blend = def.blend;
    e.origin[1] += y_shift;
    emitters_.push_back(std::move(e));
}

void EffectSystem::spawn_effect(const EffectDef& def, std::array<float, 3> origin, std::uint32_t entity) {
    // Group factory 004958f0 compares groupname against 12 strings (0x597e4c..0x597eec) and has no "lightning" case:
    // the original creates the effect node with no class, so effects.xml "lightning" draws nothing.
    if (def.group == "lightning") return;
    const float stop = def.time_limit > 0 ? def.time_limit : -1;
    std::vector<std::string_view> names;
    float lift = 0;
    if (def.group == "thrown_psystem") { names = {def.hit[0], def.hit[1]}; lift = 3; } // no source: impact only, at target Y+3.0
    else if (def.group == "delayed_effect") {
        // 004947c0: psystem_name as a free node at the effect position; 004948a0 adds the burst once elapsed > time_limit.
        if (const auto* d = system(def.system)) spawn(*d, origin, 0, 0, -1);
        delayed_.push_back({&def, origin});
        return;
    }
    else {
        names = {def.system, def.system2};
        // 00495620/00495400/00495210/00495330: these groups build a fixed system name instead of psystem_name,
        // placed at the actor position (00478030) + Y: kan 4.0 (0x595f94), level_up 3.0 (0x592f28), aura 4.5 (0x597dc4), Heal 0.
        if (def.group == "kan") { names.push_back("kan"); lift = 4; }
        else if (def.group == "level_up") { names.push_back("level_up"); lift = 3; }
        else if (def.group == "heal") names.push_back("Heal");
        else if (def.group == "aura") { names.push_back("aura"); lift = 4.5f; }
        else if (def.group == "attached_psystem_tex") {
            // 00494b30: "psystem_attached_tex" (its generator sits 8 units above the feet) as a child node of the target
            // (004a9cf0), so it rides with the actor; texture_name replaces its texture (004934a0) and a non-empty blend_type
            // its instance BlendType (00492eb0: blend 1, subtract 2, min 3, max 4, anything else 0). y_shift is never read.
            if (const auto* d = system("psystem_attached_tex")) {
                spawn(*d, origin, entity, 0, stop, def.texture.empty() ? UINT32_MAX : texture(def.texture));
                auto& e = emitters_.back();
                e.follow = true;
                if (!def.blend.empty())
                    e.blend = def.blend == "blend" ? ParticleBlend::alpha : def.blend == "subtract" ? ParticleBlend::subtract :
                              def.blend == "min" ? ParticleBlend::min : def.blend == "max" ? ParticleBlend::max : ParticleBlend::add;
            }
            return;
        }
    }
    bool any = false;
    for (auto name : names) {
        if (name.empty()) continue;
        if (const auto* d = system(name)) { spawn(*d, origin, def.attached ? entity : 0, lift, stop); any = true; }
    }
    if (!any) report("effect has no recoverable particle system", def.name);
}

void EffectSystem::trigger(std::string_view name, std::uint32_t entity) {
    const auto found = std::find_if(entities_.begin(), entities_.end(), [&](const RenderEntity& e) { return e.id == entity; });
    if (found == entities_.end()) { report("effect target not visible", std::to_string(entity)); return; }
    const std::array<float, 3> origin{found->x, found->ground_y, found->z};
    if (const auto e = effect_index_.find(std::string(name)); e != effect_index_.end()) spawn_effect(effects_[e->second], origin, entity);
    else if (const auto* d = system(name)) spawn(*d, origin, entity, 0, -1);
}

void EffectSystem::trigger_at(std::string_view name, std::array<float, 3> position) {
    if (const auto e = effect_index_.find(std::string(name)); e != effect_index_.end()) spawn_effect(effects_[e->second], position, 0);
    else if (const auto* d = system(name)) spawn(*d, position, 0, 0, -1);
}

void EffectSystem::attach(std::string_view name, std::uint32_t entity, std::array<float, 4> tint, bool weapon) {
    const auto* d = system(name);
    if (!d) return;
    for (auto& e : emitters_) if (e.follow && e.entity == entity && e.def == d && !e.stopped) { e.tint = tint; return; }
    const auto found = std::find_if(entities_.begin(), entities_.end(), [&](const RenderEntity& e) { return e.id == entity; });
    if (found == entities_.end()) return;
    spawn(*d, {found->x, found->ground_y, found->z}, entity, 0, -1);
    emitters_.back().follow = true;
    emitters_.back().tint = tint;
    emitters_.back().weapon = weapon;
}

void EffectSystem::detach(std::string_view name, std::uint32_t entity) {
    const auto found = system_index_.find(std::string(name));
    if (found == system_index_.end()) return;
    std::erase_if(emitters_, [&](const Emitter& e) { return e.follow && e.entity == entity && e.def == &systems_[found->second]; });
}

void EffectSystem::launch(std::string_view name, std::uint32_t source, std::uint32_t target) {
    const auto e = effect_index_.find(std::string(name));
    if (e == effect_index_.end() || effects_[e->second].group != "thrown_psystem") { trigger(name, target); return; }
    const auto& def = effects_[e->second];
    const auto find = [&](std::uint32_t id) { return std::find_if(entities_.begin(), entities_.end(), [&](const RenderEntity& r) { return r.id == id; }); };
    const auto from = find(source), to = find(target);
    if (from == entities_.end() || to == entities_.end()) { report("projectile endpoint not visible", def.name); return; }
    // 00494c00: start at the source position +5.5 Y (0x597e1c), goal at the target +3.0 Y (0x592f28); psystem_name
    // "yerden_effect" uses both ground positions. The flight lasts distance/travel_speed when that is below max_time_limit,
    // otherwise time_limit (no shipped thrown effect is speed-bound beyond 0.7 s).
    const bool ground = def.system == "yerden_effect";
    const std::array<float, 3> start{from->x, from->ground_y + (ground ? 0 : 5.5f), from->z};
    Projectile p{&def, next_projectile_++, target, start, {to->x, to->ground_y + (ground ? 0 : 3.f), to->z}, {to->x, to->ground_y, to->z}, start, 0, 0, false};
    float distance = 0;
    for (unsigned i = 0; i < 3; ++i) distance += (p.goal[i] - p.start[i]) * (p.goal[i] - p.start[i]);
    const float flight = std::sqrt(distance) / def.travel_speed;
    p.duration = flight < def.max_time_limit ? flight : def.time_limit;
    // The trails system (+0x94) only has its generator centre moved (00491c70), so its particles stay where they were
    // emitted; the head system (+0x98) is a node moved along (00491cc0), its particles travel with it.
    for (const auto* n : {&def.head, &def.trails}) {
        if (n->empty()) continue;
        if (const auto* d = system(*n)) { spawn(*d, p.start, 0, 0, -1); emitters_.back().projectile = p.id; emitters_.back().follow = n == &def.head; }
    }
    projectiles_.push_back(p);
}

void EffectSystem::attach_scene(const Scene& scene) {
    std::erase_if(emitters_, [](const Emitter& e) { return e.node != UINT32_MAX; });
    for (const auto& node : scene.nodes) {
        if (node.source_particle_system.empty()) continue;
        if (const auto* d = system(node.source_particle_system)) {
            // 004a2e70 type 11: the node's psystem copy emits at its generator centres in node space; 00478240 passes the node
            // world matrix to 0049a6b0, whose billboard 0049b6e0 transforms each particle position by it (rotation and scale
            // included, corner offsets stay camera/heading aligned). "particle_far_*" nodes get +0x21c = 3.05 (0x40433333).
            spawn(*d, {0, 0, 0}, 0, 0, -1);
            emitters_.back().node = static_cast<std::uint32_t>(&node - scene.nodes.data());
            if (lower(node.name).starts_with("particle_far_")) emitters_.back().far_scale = 3.05f;
        }
    }
    scene_ = &scene;
}

void EffectSystem::step(Emitter& e, float dt, std::array<float, 3> from) {
    const auto& d = *e.def;
    // 0049c240 per particle; gravity is a per-update velocity increment in the original.
    // ponytail: scaled to a 60 Hz update (the original's vsync rate) so it is frame-rate independent.
    const float gravity_scale = dt * 60;
    for (auto& p : e.particles) {
        for (unsigned i = 0; i < 3; ++i) p.position[i] += p.velocity[i] * dt;
        const float g = d.gravity_min == d.gravity_max ? 0 : random(); // 0049c440: one lerp factor for all axes
        for (unsigned i = 0; i < 3; ++i) p.velocity[i] += (d.gravity_min[i] + (d.gravity_max[i] - d.gravity_min[i]) * g) * gravity_scale;
        const float c = d.color_step_min == d.color_step_max ? 0 : random(); // 0049c500
        for (unsigned i = 0; i < 4; ++i) p.color[i] += (d.color_step_min[i] + (d.color_step_max[i] - d.color_step_min[i]) * c) * dt;
        p.spin += random(d.spin_step) * dt;
        p.size += random(d.size_step) * dt;
        if (d.die_transparent) p.color[3] = p.life / p.total;
        p.life -= dt;
    }
    std::erase_if(e.particles, [](const Particle& p) { return p.life <= 0; }); // 0049c750
    // 0049c7b0 emission.
    e.accumulator += dt;
    while ((e.remaining >= 1 || e.remaining == -1) && static_cast<int>(e.particles.size()) < d.max_particles &&
           e.accumulator >= e.interval && !e.stopped) {
        e.accumulator -= e.interval;
        e.interval = random(d.creation_rate);
        if (e.remaining != -1) --e.remaining;
        Particle p{};
        const auto local = generate(d.position);
        // Emitted when the accumulator crossed its interval during this update, so along the origin's path since `from`
        // (a moving trail stays continuous at any frame rate instead of clumping at the frame-end position).
        const float along = dt > 0 ? std::clamp(1 - e.accumulator / dt, 0.f, 1.f) : 1;
        for (unsigned i = 0; i < 3; ++i) p.position[i] = from[i] + (e.origin[i] - from[i]) * along + local[i];
        // 00491e40: a random index triangle of the item root's mesh, vertices weighted by three U(0,1) draws, through the
        // item node's world matrix. Without the item (none worn, or not assembled) the original attaches nothing.
        if (e.weapon) {
            const auto* item = scene_ ? source_actor_weapon(*scene_, e.entity) : nullptr;
            const auto at = std::find_if(entities_.begin(), entities_.end(), [&](const RenderEntity& r) { return r.id == e.entity; });
            if (!item || at == entities_.end() || scene_->meshes[item->mesh].indices.size() < 3) break;
            const auto& mesh = scene_->meshes[item->mesh];
            const auto triangle = std::min(static_cast<std::size_t>(random() * static_cast<float>(mesh.indices.size() / 3)), mesh.indices.size() / 3 - 1) * 3;
            const std::array<float, 3> w{random(), random(), random()};
            const float sum = std::max(w[0] + w[1] + w[2], 1e-6f);
            std::array<float, 3> point{};
            for (unsigned k = 0; k < 3; ++k) {
                const auto& v = mesh.vertices[mesh.indices[triangle + k]];
                const std::array<float, 3> xyz{v.x, v.y, v.z};
                for (unsigned i = 0; i < 3; ++i) point[i] += xyz[i] * w[k] / sum;
            }
            const auto m = entity_instance_transform(*scene_, *item, at->x, at->ground_y, at->z, at->yaw).matrix;
            for (unsigned i = 0; i < 3; ++i) p.position[i] = m[i] * point[0] + m[4 + i] * point[1] + m[8 + i] * point[2] + m[12 + i];
        }
        p.life = p.total = random(d.life);
        if (d.destination.shape == ParticleGenerator::Shape::none) p.velocity = generate(d.velocity);
        else {
            const auto goal = generate(d.destination);
            std::array<float, 3> delta{};
            for (unsigned i = 0; i < 3; ++i) delta[i] = e.origin[i] + goal[i] - p.position[i];
            if (d.die_at_destination && p.total > 0) for (unsigned i = 0; i < 3; ++i) p.velocity[i] = delta[i] / p.total;
            else p.velocity = normalize(delta);
        }
        p.spin = random(d.spin);
        const float c = d.color_min == d.color_max ? 0 : random();
        for (unsigned i = 0; i < 4; ++i) p.color[i] = d.color_min[i] + (d.color_max[i] - d.color_min[i]) * c;
        p.size = random(d.size);
        p.born_ms = now_ms_;
        p.set = -1;
        if (!d.texture_sets.empty()) { // weighted by chance (FUN_004449f0(0,sum) cumulative pick)
            const float total = std::accumulate(d.texture_sets.begin(), d.texture_sets.end(), 0.f, [](float s, const ParticleTextureSet& t) { return s + t.chance; });
            const float pick = total * random();
            float sum = 0; p.set = static_cast<std::int32_t>(d.texture_sets.size() - 1);
            for (std::size_t i = 0; i < d.texture_sets.size(); ++i) { sum += d.texture_sets[i].chance; if (pick < sum) { p.set = static_cast<std::int32_t>(i); break; } }
        }
        e.particles.push_back(p);
    }
}

void EffectSystem::update(float dt, std::span<const RenderEntity> entities) {
    if (!std::isfinite(dt) || dt < 0) throw std::invalid_argument("invalid effect delta");
    entities_.assign(entities.begin(), entities.end());
    now_ms_ += static_cast<std::uint32_t>(dt * 1000);
    const auto find = [&](std::uint32_t id) { return std::find_if(entities_.begin(), entities_.end(), [&](const RenderEntity& r) { return r.id == id; }); };
    for (auto& p : projectiles_) { // 00494e90
        p.elapsed += dt;
        if (p.elapsed > p.duration) {
            // Arrival: trails stop emitting (vtable +0xc 0049a510), the head's tint +0x70 is zeroed, and hit / hit_2 (never
            // hit_3) burst at the last goal.
            for (auto& e : emitters_) if (e.projectile == p.id) { e.stopped = true; if (e.follow) e.tint = {}; }
            for (const auto* hit : {&p.def->hit[0], &p.def->hit[1]}) if (!hit->empty()) if (const auto* d = system(*hit)) spawn(*d, p.goal, 0, 0, -1);
            p.done = true;
            continue;
        }
        const auto t = find(p.target);
        if (t == entities_.end() || p.duration == 0) { // +4 = 1: the effect and all its systems are deleted (00492260)
            std::erase_if(emitters_, [&](const Emitter& e) { return e.projectile == p.id; });
            p.done = true;
            continue;
        }
        // Linear from the start to the goal of the previous update, then the goal moves: "gercek_elbombasi" arcs above the
        // launch-time target by 0.7 × the remaining distance (0x597400), "yerden_effect" ends 0.8 (0x58f06c) above it,
        // anything else homes on the live target +3.0.
        const float k = p.elapsed / p.duration;
        for (unsigned i = 0; i < 3; ++i) p.position[i] = p.start[i] + (p.goal[i] - p.start[i]) * k;
        if (p.def->system == "gercek_elbombasi") {
            float remaining = 0;
            for (unsigned i = 0; i < 3; ++i) remaining += (p.target_start[i] - p.position[i]) * (p.target_start[i] - p.position[i]);
            p.goal = {p.target_start[0], p.target_start[1] + std::sqrt(remaining) * .7f, p.target_start[2]};
        } else if (p.def->system == "yerden_effect") p.goal = {p.target_start[0], p.target_start[1] + .8f, p.target_start[2]};
        else p.goal = {t->x, t->ground_y + 3, t->z};
    }
    for (auto& d : delayed_) { // 004948a0: one burst of psystem2, hit, hit_2, hit_3 once elapsed > time_limit
        d.elapsed += dt;
        if (d.elapsed <= d.def->time_limit) continue;
        for (const auto* name : {&d.def->system2, &d.def->hit[0], &d.def->hit[1], &d.def->hit[2]})
            if (!name->empty()) if (const auto* s = system(*name)) spawn(*s, d.at, 0, 0, -1);
        d.def = nullptr;
    }
    std::erase_if(delayed_, [](const Delayed& d) { return !d.def; });
    for (auto& e : emitters_) {
        const auto from = e.origin;
        e.age += dt;
        if (e.projectile) { // scene node emitters stay in node space; frame() applies the node matrix
            const auto p = std::find_if(projectiles_.begin(), projectiles_.end(), [&](const Projectile& q) { return q.id == e.projectile; });
            if (p != projectiles_.end()) e.origin = p->position;
            if (e.follow) for (auto& q : e.particles) for (unsigned i = 0; i < 3; ++i) q.position[i] += e.origin[i] - from[i];
        } else if (e.entity) {
            if (const auto t = find(e.entity); t != entities_.end()) {
                const std::array<float, 3> origin{t->x, t->ground_y + e.y_shift, t->z};
                // attach(): particles ride with the actor; an expired one-shot is re-created by the next attach().
                if (e.follow) for (auto& p : e.particles) for (unsigned i = 0; i < 3; ++i) p.position[i] += origin[i] - e.origin[i];
                e.origin = origin;
            } else { e.stopped = true; if (e.follow) e.particles.clear(); } // entity left visibility
        }
        if (e.stop_after >= 0 && e.age >= e.stop_after) e.stopped = true;
        step(e, dt, e.follow ? e.origin : from);
    }
    std::erase_if(projectiles_, [](const Projectile& p) { return p.done; });
    // 0049a520: finished when emission is exhausted or stopped and no particle remains (-1 never exhausts).
    std::erase_if(emitters_, [](const Emitter& e) {
        return e.particles.empty() && (e.stopped || (e.remaining != -1 && e.remaining < 1));
    });
}

std::size_t EffectSystem::particle_count() const {
    std::size_t n = 0;
    for (const auto& e : emitters_) n += e.particles.size();
    return n;
}

const ParticleFrame& EffectSystem::frame(const Camera& camera) {
    const auto basis = camera_basis(camera);
    const float tangent = std::tan(camera.fov * .008726646259971648f);
    frame_.textures = textures_;
    frame_.vertices.clear();
    frame_.batches.clear();
    std::vector<std::uint32_t> order;
    std::vector<std::array<float, 3>> at; // world positions
    for (const auto& e : emitters_) {
        if (e.particles.empty() || e.texture == UINT32_MAX) continue;
        const auto& d = *e.def;
        // 0049b6e0 @49ba09: particle position through the node world matrix (scene emitters; others are world space).
        const float* m = e.node != UINT32_MAX && scene_ && e.node < scene_->nodes.size() ? scene_->nodes[e.node].world.matrix.data() : nullptr;
        at.resize(e.particles.size());
        for (std::size_t k = 0; k < at.size(); ++k) {
            const auto& q = e.particles[k].position;
            at[k] = m ? std::array<float, 3>{m[0] * q[0] + m[4] * q[1] + m[8] * q[2] + m[12], m[1] * q[0] + m[5] * q[1] + m[9] * q[2] + m[13],
                                             m[2] * q[0] + m[6] * q[1] + m[10] * q[2] + m[14]} : q;
        }
        // Cull by the particles' bounding sphere (0049a5a0 computes the same AABB padded by size).
        std::array<float, 3> lo = at[0], hi = lo;
        float largest = 0;
        for (std::size_t k = 0; k < at.size(); ++k) {
            for (unsigned i = 0; i < 3; ++i) { lo[i] = std::min(lo[i], at[k][i]); hi[i] = std::max(hi[i], at[k][i]); }
            largest = std::max(largest, std::abs(e.particles[k].size) * 1.4143f);
        }
        std::array<float, 3> centre{}, rel{};
        float radius = largest;
        for (unsigned i = 0; i < 3; ++i) { centre[i] = (lo[i] + hi[i]) * .5f; radius += (hi[i] - lo[i]) * .5f; rel[i] = centre[i] - basis.eye[i]; }
        const float z = rel[0] * basis.forward[0] + rel[1] * basis.forward[1] + rel[2] * basis.forward[2];
        const float x = rel[0] * basis.right[0] + rel[1] * basis.right[1] + rel[2] * basis.right[2];
        const float y = rel[0] * basis.up[0] + rel[1] * basis.up[1] + rel[2] * basis.up[2];
        if (z + radius < camera.near_plane || z - radius > camera.far_plane) continue;
        const float distance2 = rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2];
        if (distance2 > ranges_[e.entity || e.projectile ? 1 : 0] || distance2 > ranges_[2] * .5f * e.far_scale) continue;
        const float slack = radius * 1.5f;
        if (std::abs(y) > z * tangent + slack || std::abs(x) > z * tangent * 2.4f + slack) continue; // up to ~2.4:1 aspect
        // Axes: camera billboard (0077c160.. camera basis) or authored heading/up (004999e0 +0x134/+0x14c).
        const auto normal = d.camera_heading ? basis.forward : d.heading;
        const auto up = d.camera_heading ? basis.up : d.up;
        const auto right = d.camera_heading ? basis.right : cross(d.heading, d.up);
        order.resize(e.particles.size());
        std::iota(order.begin(), order.end(), 0u);
        if (d.sort) {
            const auto distance = [&](std::uint32_t i) {
                float s = 0;
                for (unsigned k = 0; k < 3; ++k) { const float v = at[i][k] - basis.eye[k]; s += v * v; }
                return s;
            };
            std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return distance(a) > distance(b); });
        }
        const float reference = e.blend == ParticleBlend::alpha && d.alpha_mask != 0 ? d.alpha_mask : 0;
        // Node flag +0x215 (00478300) = psystem AfterHDR getter 0049a500 && non-"blend" BlendType 0049a560.
        const bool after_hdr = d.after_hdr && e.blend != ParticleBlend::alpha;
        if (frame_.batches.empty() || frame_.batches.back().texture != e.texture || frame_.batches.back().blend != e.blend ||
            frame_.batches.back().depth_bias != d.depth_bias || frame_.batches.back().alpha_reference != reference ||
            frame_.batches.back().after_hdr != after_hdr)
            frame_.batches.push_back({e.texture, e.blend, d.depth_bias, reference, static_cast<std::uint32_t>(frame_.vertices.size()), 0, after_hdr});
        for (const auto index : order) {
            const auto& p = e.particles[index];
            const auto& position = at[index];
            // 0049b6e0: corners are position +/- right*size +/- up*size, spun around the normal unless NoSpin.
            std::array<float, 3> r{}, u{};
            const float s = p.size;
            if (d.no_spin) for (unsigned i = 0; i < 3; ++i) { r[i] = right[i] * s; u[i] = up[i] * s; }
            else {
                const float c = std::cos(p.spin), sn = std::sin(p.spin);
                const auto nr = cross(normal, right), nu = cross(normal, up);
                for (unsigned i = 0; i < 3; ++i) { r[i] = (right[i] * c + nr[i] * sn) * s; u[i] = (up[i] * c + nu[i] * sn) * s; }
            }
            std::array<float, 4> uv{0, 0, 1, 1};
            if (p.set >= 0) {
                const auto& set = d.texture_sets[static_cast<std::size_t>(p.set)];
                const auto n = set.frames.size();
                std::size_t frame = 0;
                if (set.ms_per_frame == -1) frame = static_cast<std::size_t>(std::lround(static_cast<float>(n) * (1 - p.life / p.total)));
                else if (set.ms_per_frame > 0) frame = (now_ms_ - p.born_ms) / static_cast<std::uint32_t>(set.ms_per_frame) % n;
                uv = set.frames[std::min(frame, n - 1)];
            }
            std::array<float, 4> color{};
            for (unsigned i = 0; i < 4; ++i) color[i] = std::clamp(p.color[i], 0.f, 1.f) * e.tint[i];
            const auto corner = [&](float a, float b, float tu, float tv) {
                return ParticleVertex{position[0] + r[0] * a + u[0] * b, position[1] + r[1] * a + u[1] * b,
                                      position[2] + r[2] * a + u[2] * b, tu, tv, color};
            };
            const auto bl = corner(-1, -1, uv[0], uv[3]), tl = corner(-1, 1, uv[0], uv[1]),
                       tr = corner(1, 1, uv[2], uv[1]), br = corner(1, -1, uv[2], uv[3]);
            frame_.vertices.insert(frame_.vertices.end(), {bl, tl, tr, bl, tr, br});
        }
        frame_.batches.back().count = static_cast<std::uint32_t>(frame_.vertices.size()) - frame_.batches.back().first;
    }
    return frame_;
}

void EffectSystem::self_test() {
    const auto require = [](bool ok, const char* what) { if (!ok) throw std::runtime_error(std::string("effect self-test: ") + what); };
    const auto systems = parse_particle_systems(R"(<?xml version="1.0" encoding="iso-8859-9" ?>
<ParticleSystems><ParticleSystem name = "fixture">
 <StepPolicy><Gravity>0.0 -1.0 0.0</Gravity><SizeStep>2.0</SizeStep><ColorStep>0 0 0 0</ColorStep><SpinStep>0.0</SpinStep><DieTransparent>true</DieTransparent></StepPolicy>
 <RenderPolicy><ParticleTexture>fx\Fixture.dds</ParticleTexture><BlendType>Blend</BlendType><AlphaMaskValue>0.25</AlphaMaskValue><NoSpin>true</NoSpin><Heading>vector 0 1 0</Heading><DepthBias>-0.00035</DepthBias></RenderPolicy>
 <EmiterPolicy><TextureSet chance="0.5" FPms="-1">0 0 .5 .5  .5 0 1 .5 1</TextureSet><MaxParticles>3</MaxParticles><ParticleCount>3</ParticleCount>
  <CreationRate>0.0 - 0.0</CreationRate><DieAtDestinaton>true</DieAtDestinaton>
  <PositionGenerator type="box">0 0 0  0 1 0</PositionGenerator><VelocityGenerator type="box">0 0 0  0 0 2</VelocityGenerator>
  <StartSpinRange>0.0</StartSpinRange><StartLifeRange>1.0-</StartLifeRange><StartSizeRange>0.07-0.12</StartSizeRange>
  <StartColorRange>1 1 1 1</StartColorRange></EmiterPolicy></ParticleSystem></ParticleSystems>)");
    require(systems.size() == 1 && systems[0].name == "fixture" && systems[0].texture == "fx/Fixture.dds", "system identity");
    const auto& d = systems[0];
    require(d.blend == ParticleBlend::alpha && !d.camera_heading && d.heading == std::array<float, 3>{0, 1, 0} && d.up == std::array<float, 3>{1, 0, 0} && d.after_hdr, "render policy");
    require(!d.die_at_destination && d.size.min == .07f && d.size.max == .12f && d.life.min == 1 && d.life.max == 1, "ranges");
    require(d.texture_sets.size() == 1 && d.texture_sets[0].frames.size() == 2 && d.texture_sets[0].ms_per_frame == -1, "texture set");
    require(d.gravity_min[1] == -1 && d.gravity_max[1] == -1 && d.color_min[3] == 1, "step policy");
    const auto effects = parse_effects(R"(<Effects><Effect name="fx" groupname="kan" psystem_name="" texture_name="fx\\a.dds" y_shift="2" max_time_limit="0.7" blend_type="Blend" attached="0"/></Effects>)");
    require(effects.size() == 1 && effects[0].group == "kan" && effects[0].texture == "fx/a.dds" && effects[0].max_time_limit == .7f &&
            effects[0].blend == "blend" && !effects[0].attached, "effect");

    EffectSystem fx(std::filesystem::path{}, systems, effects);
    fx.texture_index_.emplace("fx/fixture.dds", 0); // no texture decode in the self-test
    fx.textures_.push_back(Texture{"fx/fixture.dds", 1, 1, {255, 255, 255, 255}, {{1, 1, 0, 4}}});
    const RenderEntity actor{7, 10, 2, -5, 0};
    fx.update(0, std::span(&actor, 1));
    fx.trigger("fixture", 7);
    fx.update(0, std::span(&actor, 1)); // CreationRate 0: MaxParticles emitted at once, then exhausted
    require(fx.particle_count() == 3, "emission count");
    const auto& p = fx.emitters_[0].particles[0];
    require(p.position == std::array<float, 3>{10, 3, -5} && p.velocity == std::array<float, 3>{0, 0, 2}, "spawn position/velocity");
    fx.update(.5f, std::span(&actor, 1));
    const auto& q = fx.emitters_[0].particles[0];
    require(std::abs(q.position[2] + 4) < 1e-5f && std::abs(q.velocity[1] + 30) < 1e-3f && std::abs(q.color[3] - 1) < 1e-6f && std::abs(q.life - .5f) < 1e-6f, "step");
    const auto& frame = fx.frame(Camera{{10, 3, -5}});
    require(frame.batches.size() == 1 && frame.vertices.size() == 18 && frame.batches[0].blend == ParticleBlend::alpha &&
            frame.batches[0].alpha_reference == .25f && frame.batches[0].count == 18 && !frame.batches[0].after_hdr, "billboard batch");
    require(std::abs(frame.vertices[0].y - q.position[1]) < 1e-5f && frame.vertices[0].u == .5f, "ground-facing quad and life frame");
    fx.update(.6f, std::span(&actor, 1));
    fx.update(0, std::span(&actor, 1));
    require(fx.particle_count() == 0 && fx.emitters_.empty(), "expiry");
    fx.trigger("fx", 7); // kan group falls back to the fixed "kan" system, absent from the fixture
    require(fx.emitters_.empty(), "missing hard-coded system ignored");
    require(parse_particle_systems(R"(<ParticleSystems><ParticleSystem name="d"/></ParticleSystems>)")[0].no_spin, "NoSpin default (0049d710)");
    EffectDef lightning; lightning.name = "lightning"; lightning.group = "lightning"; lightning.system = "fixture";
    EffectSystem bolt(std::filesystem::path{}, systems, {lightning});
    bolt.update(0, std::span(&actor, 1));
    bolt.trigger("lightning", 7);
    require(bolt.emitters_.empty(), "lightning group has no 004958f0 class");
    // Scene emitter: node-space particles drawn through the node matrix (90 degrees about Z, then +(10,2,-5)).
    Scene scene;
    scene.nodes.resize(1);
    scene.nodes[0].name = "Particle_Far_x"; scene.nodes[0].source_particle_system = "fixture";
    scene.nodes[0].world.matrix = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 10, 2, -5, 1};
    EffectSystem zone(std::filesystem::path{}, systems, {});
    zone.texture_index_.emplace("fx/fixture.dds", 0);
    zone.attach_scene(scene);
    zone.update(0, {});
    require(zone.emitters_.size() == 1 && zone.emitters_[0].far_scale == 3.05f && zone.particle_count() == 3, "scene emitter");
    const Camera view{{9, 2, -5}}; // local (0,1,0) -> (9,2,-5), 22 units from the eye
    const auto& placed = zone.frame(view);
    require(placed.vertices.size() == 18 && std::abs((placed.vertices[0].x + placed.vertices[2].x) * .5f - 9) < 1e-5f &&
            std::abs((placed.vertices[0].y + placed.vertices[2].y) * .5f - 2) < 1e-5f, "node rotation applied");
    zone.set_ranges(INFINITY, INFINITY, 300); // 00478586: 22² > .5·3.05·300
    require(zone.frame(view).vertices.empty(), "fog far cull");
    // Thrown trail: emissions inside one update spread along the projectile's path; the head rides with it.
    auto trail = systems[0];
    trail.name = "trail"; trail.creation_rate = {.125f, .125f}; trail.max_particles = trail.count = 20; trail.gravity_min = trail.gravity_max = {};
    EffectDef thrown; thrown.name = "throw"; thrown.group = "thrown_psystem"; thrown.trails = "trail"; thrown.head = "fixture"; thrown.travel_speed = 10; thrown.max_time_limit = 3;
    EffectSystem shot(std::filesystem::path{}, {trail, systems[0]}, {thrown});
    shot.texture_index_.emplace("fx/fixture.dds", 0);
    const std::array<RenderEntity, 2> ends{{{7, 10, 2, -5, 0}, {8, 30, 2, -5, 0}}};
    shot.update(0, ends);
    shot.launch("throw", 7, 8);
    shot.update(.5f, ends); // 5 units toward the target: trail particles spread over the path, head particles at its end
    const float end = 10 + 5 * 20 / std::sqrt(400 + 2.5f * 2.5f); // start Y+5.5, goal Y+3
    float lo = INFINITY, hi = -INFINITY;
    for (const auto& t : shot.emitters_[1].particles) { lo = std::min(lo, t.position[0]); hi = std::max(hi, t.position[0]); }
    require(shot.emitters_[1].particles.size() == 5 && std::abs(lo - 10) < 1e-4f && std::abs(hi - end) < 1e-4f, "continuous trail");
    require(std::all_of(shot.emitters_[0].particles.begin(), shot.emitters_[0].particles.end(), [&](const Particle& h) { return std::abs(h.position[0] - end) < 1e-4f; }), "head rides projectile");
    // 00494c00: 20.16 units at speed 10 exceed max_time_limit .7, so the flight takes time_limit 1 s. 00494e90: linear toward
    // the previous update's goal, then the goal follows the live target +3; arrival after elapsed > duration.
    auto core = systems[0];
    core.name = "core"; core.count = -1; core.max_particles = 1; core.life = {50, 50}; core.gravity_min = core.gravity_max = {}; core.velocity = {};
    auto burst = core; burst.name = "burst"; burst.count = 1;
    auto third = burst; third.name = "third";
    thrown.head = "core"; thrown.max_time_limit = .7f; thrown.time_limit = 1; thrown.hit = {"burst", "burst", "third"};
    EffectSystem fly(std::filesystem::path{}, {trail, core, burst, third}, {thrown});
    fly.texture_index_.emplace("fx/fixture.dds", 0);
    std::array<RenderEntity, 2> moving{{{7, 10, 2, -5, 0}, {8, 30, 2, -5, 0}}};
    fly.update(0, moving);
    fly.launch("throw", 7, 8);
    require(fly.projectiles_.size() == 1 && fly.projectiles_[0].duration == 1, "time_limit flight beyond max_time_limit");
    const auto head_x = [&] { return fly.emitters_[0].particles.at(0).position[0]; };
    fly.update(.25f, moving);
    require(std::abs(head_x() - 15) < 1e-4f, "quarter of the way");
    moving[1].x = 40; // the target walks on: this update still lerps toward the old goal, then re-aims
    fly.update(.25f, moving);
    require(std::abs(head_x() - 20) < 1e-4f && fly.projectiles_[0].goal == std::array<float, 3>{40, 5, -5}, "lerp to previous goal");
    fly.update(.25f, moving);
    require(std::abs(head_x() - 32.5f) < 1e-4f, "re-aimed lerp");
    fly.update(.25f, moving); // elapsed == duration: still flying
    require(fly.projectiles_.size() == 1 && fly.emitters_.size() == 2, "arrival needs elapsed > duration");
    fly.update(.01f, moving);
    const auto named = [&](const char* n) { return std::count_if(fly.emitters_.begin(), fly.emitters_.end(), [&](const Emitter& e) { return e.def->name == n; }); };
    require(fly.projectiles_.empty() && named("burst") == 2 && named("third") == 0, "hit and hit_2 only");
    require(fly.emitters_[0].tint == std::array<float, 4>{} && fly.emitters_[0].stopped && fly.emitters_[1].stopped, "head hidden, trail stopped");
    require(std::all_of(fly.emitters_.begin(), fly.emitters_.end(), [](const Emitter& e) { return e.def->name != std::string("burst") || e.particles.at(0).position == std::array<float, 3>{40, 6, -5}; }), "burst at the goal");
    fly.launch("throw", 7, 8);
    fly.update(.1f, std::span(moving.data(), 1)); // target gone mid-flight: the whole effect is deleted
    require(fly.projectiles_.empty() && named("core") == 1 && named("trail") == 1, "vanished target deletes the projectile");
    thrown.system = "gercek_elbombasi";
    EffectSystem lob(std::filesystem::path{}, {trail, core}, {thrown});
    lob.texture_index_.emplace("fx/fixture.dds", 0);
    lob.update(0, moving);
    lob.launch("throw", 7, 8);
    lob.update(.5f, moving); // position (25, 6.25, -5) toward the launch goal (40,5,-5); goal arcs .7 x 15.13 above (40,2,-5)
    require(std::abs(lob.projectiles_[0].goal[1] - (2 + .7f * std::sqrt(225 + 4.25f * 4.25f))) < 1e-4f && lob.projectiles_[0].goal[0] == 40, "grenade arc");
    thrown.system = "yerden_effect";
    EffectSystem low(std::filesystem::path{}, {trail, core}, {thrown});
    low.texture_index_.emplace("fx/fixture.dds", 0);
    low.update(0, moving);
    low.launch("throw", 7, 8);
    require(low.projectiles_[0].start[1] == 2 && low.projectiles_[0].goal[1] == 2, "ground start and goal");
    low.update(.1f, moving);
    require(low.projectiles_[0].goal[1] == 2 + .8f, "ground goal +0.8");
    // delayed_effect: psystem_name at once, psystem2 + hit + hit_2 + hit_3 once elapsed > time_limit, at the trigger point.
    EffectDef zeplin; zeplin.name = "zep"; zeplin.group = "delayed_effect"; zeplin.system = "core"; zeplin.system2 = "burst";
    zeplin.hit = {"burst", "burst", "third"}; zeplin.time_limit = 3; zeplin.head = "trail";
    EffectSystem later(std::filesystem::path{}, {trail, core, burst, third}, {zeplin});
    later.texture_index_.emplace("fx/fixture.dds", 0);
    later.update(0, std::span(&actor, 1));
    later.trigger("zep", 7);
    later.update(3, std::span(&actor, 1));
    require(later.emitters_.size() == 1 && later.emitters_[0].def->name == std::string("core"), "delay holds at elapsed == time_limit");
    later.update(.01f, std::span(&actor, 1));
    require(later.emitters_.size() == 5 && later.delayed_.empty() && later.emitters_[4].particles.at(0).position == std::array<float, 3>{10, 3, -5}, "delayed burst");
    // attached_psystem_tex: psystem_attached_tex rides with the target; blend_type overrides the instance blend; y_shift unused.
    auto icon = core; icon.name = "psystem_attached_tex"; icon.blend = ParticleBlend::add; icon.count = 1;
    EffectDef tex; tex.name = "bg"; tex.group = "attached_psystem_tex"; tex.blend = "blend";
    EffectSystem badge(std::filesystem::path{}, {icon}, {tex});
    badge.texture_index_.emplace("fx/fixture.dds", 0);
    RenderEntity walker = actor;
    badge.update(0, std::span(&walker, 1));
    badge.trigger("bg", 7);
    badge.update(0, std::span(&walker, 1));
    walker.x += 5;
    badge.update(0, std::span(&walker, 1));
    require(badge.emitters_[0].blend == ParticleBlend::alpha && badge.emitters_[0].particles.at(0).position == std::array<float, 3>{15, 3, -5}, "attached icon");
    require(badge.frame(Camera{{15, 3, -5}}).batches.at(0).blend == ParticleBlend::alpha, "instance blend drawn");
}

EffectSystem::EffectSystem(const std::filesystem::path& asset_base, std::vector<ParticleSystemDef> systems, std::vector<EffectDef> effects)
    : base_(asset_base), systems_(std::move(systems)), effects_(std::move(effects)) {
    for (std::size_t i = 0; i < systems_.size(); ++i) system_index_.emplace(systems_[i].name, i);
    for (std::size_t i = 0; i < effects_.size(); ++i) effect_index_.emplace(effects_[i].name, i);
}
} // namespace ikv
