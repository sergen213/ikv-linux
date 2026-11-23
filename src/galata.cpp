// NONCANONICAL Galata Bridge: see include/ikv/galata.hpp.
#include "ikv/galata.hpp"
#include "ikv/scene.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>
namespace ikv::galata {
namespace {
// Measured from the original stubs (analysis/bridge-c): the Eminönü deck's broken end and its seaward axis, the
// Karaköy deck's broken end, and the rotation that turns the Karaköy stub's axis onto the Eminönü one.
constexpr float eminonu_end_x = 6.08f, eminonu_end_z = 1227.8f;
constexpr float axis_x = -0.524837f, axis_z = 0.851203f; // Eminönü seaward
constexpr float karakoy_end_x = -1307.1f, karakoy_end_z = -770.4f;
constexpr double rotation_degrees = 70.0002;
struct Frame { float c, s, tx, tz; };
const Frame& frame() {
    static const Frame f = [] {
        const double a = rotation_degrees * std::numbers::pi / 180;
        const float c = float(std::cos(a)), s = float(std::sin(a));
        const float rx = c * karakoy_end_x - s * karakoy_end_z, rz = s * karakoy_end_x + c * karakoy_end_z;
        return Frame{c, s, eminonu_end_x + gap * axis_x - rx, eminonu_end_z + gap * axis_z - rz};
    }();
    return f;
}
} // namespace

Point karakoy_to_eminonu(float x, float z) { const auto& f = frame(); return {f.c * x - f.s * z + f.tx, f.s * x + f.c * z + f.tz}; }
Point eminonu_to_karakoy(float x, float z) {
    const auto& f = frame(); const float dx = x - f.tx, dz = z - f.tz;
    return {f.c * dx + f.s * dz, -f.s * dx + f.c * dz};
}
BridgeCoordinates bridge_coordinates(float x, float z) {
    const float dx = x - eminonu_end_x, dz = z - eminonu_end_z;
    return {dx * axis_x + dz * axis_z, dx * axis_z - dz * axis_x};
}
float deck_height(float s) { return deck_eminonu + (deck_karakoy - deck_eminonu) * std::clamp(s / gap, 0.f, 1.f); }
bool linked(unsigned registry) { return registry == eminonu_registry || registry == karakoy_registry; }
unsigned scene_registry(unsigned registry) { return registry == karakoy_registry ? eminonu_registry : registry; }
Point to_scene(unsigned registry, float x, float z) { return registry == karakoy_registry ? karakoy_to_eminonu(x, z) : Point{x, z}; }
Point from_scene(unsigned registry, float x, float z) { return registry == karakoy_registry ? eminonu_to_karakoy(x, z) : Point{x, z}; }
float native_yaw(unsigned registry, float yaw) { return registry == karakoy_registry ? yaw + float(rotation_degrees * std::numbers::pi / 180) : yaw; }

void paint_bridge(Zone& zone, unsigned support_id) {
    if ((support_id != eminonu_support && support_id != karakoy_support) || zone.width != 2048 || zone.height != 2048 ||
        zone.cells.size() != 4194304 || zone.heights.size() != 4194304) return;
    const bool karakoy = support_id == karakoy_support;
    const float from = karakoy ? seam_to_eminonu - 9 : -12, to = karakoy ? gap + 12 : seam_to_karakoy + 6;
    for (std::size_t iz = 0; iz < 2048; ++iz) for (std::size_t ix = 0; ix < 2048; ++ix) {
        const float x = (float(ix) - 1024) * zone.cell_size, z = (float(iz) - 1024) * zone.cell_size;
        const auto p = karakoy ? karakoy_to_eminonu(x, z) : Point{x, z};
        const auto b = bridge_coordinates(p.x, p.z);
        if (b.s < from || b.s > to || std::abs(b.w) > walk_half_width) continue;
        const auto i = iz * 2048 + ix;
        if (zone.cells[i] == 255) continue; // the original deck and quays keep their own heights
        zone.cells[i] = 255; zone.heights[i] = deck_height(b.s) - (karakoy ? height_offset : 0);
    }
}

std::optional<Crossing> crossing(unsigned registry, std::int32_t x, std::int32_t y) {
    if (!linked(registry)) return {};
    const auto p = to_scene(registry, float(x) / 1000, float(y) / 1000);
    const auto b = bridge_coordinates(p.x, p.z);
    if (std::abs(b.w) > walk_half_width + 5) return {};
    if (registry == eminonu_registry && b.s >= seam_to_karakoy && b.s <= seam_to_karakoy + 40) {
        const auto k = eminonu_to_karakoy(p.x, p.z);
        return Crossing{karakoy_registry, std::int32_t(std::lround(k.x * 1000)), std::int32_t(std::lround(k.z * 1000))};
    }
    if (registry == karakoy_registry && b.s <= seam_to_eminonu && b.s >= seam_to_eminonu - 40)
        return Crossing{eminonu_registry, std::int32_t(std::lround(p.x * 1000)), std::int32_t(std::lround(p.z * 1000))};
    return {};
}

std::optional<float> scene_ground(const Scene& scene, float x, float z) {
    if (auto h = zone_height(scene.zone, x, z)) return h;
    if (!scene.linked_zone) return {};
    const auto k = eminonu_to_karakoy(x, z);
    if (auto h = zone_height(*scene.linked_zone, k.x, k.z)) return *h + height_offset;
    return {};
}

namespace {
using Matrix = std::array<float, 16>;
Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix r{};
    for (int c = 0; c < 4; ++c) for (int row = 0; row < 4; ++row) {
        float v = 0; for (int k = 0; k < 4; ++k) v += a[k * 4 + row] * b[c * 4 + k]; r[c * 4 + row] = v;
    }
    return r;
}
Matrix karakoy_matrix() {
    const auto& f = frame();
    return {f.c, 0, f.s, 0, 0, 1, 0, 0, -f.s, 0, f.c, 0, f.tx, height_offset, f.tz, 1};
}
// Raw source quaternion (rx,ry,rz,w) -> the renderer quaternion key_transform builds its matrix from: (rx,-rz,-ry,w).
std::array<float, 4> premultiply_key_rotation(const std::array<float, 4>& raw) {
    const auto& f = frame();
    // Y rotation by phi with x' = cos(phi) x + sin(phi) z: ours has sin(phi) = -s, so phi = -theta.
    const double phi = -std::atan2(double(f.s), double(f.c));
    const float qy = float(std::sin(phi / 2)), qw = float(std::cos(phi / 2));
    const float x = raw[0], y = -raw[2], z = -raw[1], w = raw[3];
    // (0,qy,0,qw) * (x,y,z,w)
    const float nx = qw * x + qy * z, ny = qw * y + qy * w, nz = qw * z - qy * x, nw = qw * w - qy * y;
    return {nx, -nz, -ny, nw};
}
std::array<float, 3> apply(const Matrix& m, float x, float y, float z, float w) {
    return {m[0] * x + m[4] * y + m[8] * z + m[12] * w, m[1] * x + m[5] * y + m[9] * z + m[13] * w, m[2] * x + m[6] * y + m[10] * z + m[14] * w};
}
Vertex lerp(const Vertex& a, const Vertex& b, float t) {
    const auto l = [t](float p, float q) { return p + (q - p) * t; };
    Vertex v = a;
    v.x = l(a.x, b.x); v.y = l(a.y, b.y); v.z = l(a.z, b.z); v.nx = l(a.nx, b.nx); v.ny = l(a.ny, b.ny); v.nz = l(a.nz, b.nz);
    v.u = l(a.u, b.u); v.v = l(a.v, b.v);
    for (int i = 0; i < 4; ++i) { v.ambient[i] = l(a.ambient[i], b.ambient[i]); v.color[i] = l(a.color[i], b.color[i]); }
    for (int i = 0; i < 3; ++i) v.tangent[i] = l(a.tangent[i], b.tangent[i]);
    for (int i = 0; i < 2; ++i) v.uv1[i] = l(a.uv1[i], b.uv1[i]);
    return v;
}
// Keeps the part of a polygon where along(v) >= limit (keep_above) or <= limit.
std::vector<Vertex> clip(const std::vector<Vertex>& polygon, float limit, bool keep_above) {
    std::vector<Vertex> out;
    const auto value = [&](const Vertex& v) { const float s = bridge_coordinates(v.x, v.z).s - limit; return keep_above ? s : -s; };
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const auto& a = polygon[i]; const auto& b = polygon[(i + 1) % polygon.size()];
        const float va = value(a), vb = value(b);
        if (va >= 0) out.push_back(a);
        if ((va >= 0) != (vb >= 0)) out.push_back(lerp(a, b, va / (va - vb)));
    }
    return out;
}
std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
std::uint32_t material_of(const Scene& scene, const Instance& i) { return i.material == UINT32_MAX ? scene.meshes[i.mesh].material : i.material; }
// A world-space mesh of the given instance's geometry, triangles clipped to one section [-section, 0] of the Eminönü
// stub, moved `offset` along the bridge and raised onto the new deck's slope.
Mesh section_copy(const Scene& scene, const Instance& instance, float offset, bool clip_section) {
    const auto& source = scene.meshes[instance.mesh];
    const auto& m = instance.transform.matrix;
    std::vector<Vertex> world(source.vertices.size());
    for (std::size_t i = 0; i < world.size(); ++i) {
        auto v = source.vertices[i];
        const auto p = apply(m, v.x, v.y, v.z, 1), n = apply(m, v.nx, v.ny, v.nz, 0), t = apply(m, v.tangent[0], v.tangent[1], v.tangent[2], 0);
        v.x = p[0]; v.y = p[1]; v.z = p[2]; v.nx = n[0]; v.ny = n[1]; v.nz = n[2]; v.tangent = t;
        world[i] = v;
    }
    Mesh out; out.texture = source.texture; out.material = material_of(scene, instance); out.source_material_id = source.source_material_id;
    const auto place = [&](Vertex v) {
        const float s = bridge_coordinates(v.x, v.z).s + offset;
        v.x += offset * axis_x; v.z += offset * axis_z; v.y += deck_height(s) - deck_eminonu;
        out.indices.push_back(std::uint32_t(out.vertices.size())); out.vertices.push_back(v);
    };
    for (std::size_t k = 0; k + 2 < source.indices.size(); k += 3) {
        std::vector<Vertex> polygon{world[source.indices[k]], world[source.indices[k + 1]], world[source.indices[k + 2]]};
        if (clip_section) {
            // End caps lying in a section plane would stand inside the new deck.
            bool cap = true;
            for (const auto& v : polygon) { const float s = bridge_coordinates(v.x, v.z).s; cap &= std::abs(s) < .05f || std::abs(s + section) < .05f; }
            if (cap) continue;
            polygon = clip(clip(polygon, -section, true), 0, false);
            if (polygon.size() < 3) continue;
        }
        for (std::size_t i = 1; i + 1 < polygon.size(); ++i) { place(polygon[0]); place(polygon[i]); place(polygon[i + 1]); }
    }
    return out;
}
} // namespace

void merge(Scene& scene, Scene karakoy, void (*append)(Scene&, Scene)) {
    // --- Karaköy into the Eminönü frame: its sky goes (Eminönü's stays), its roots take the rigid transform.
    std::erase_if(karakoy.instances, [](const Instance& i) { return i.sky; });
    const auto t = karakoy_matrix();
    for (auto& node : karakoy.nodes) {
        node.world.matrix = multiply(t, node.world.matrix);
        if (node.parent < 0) node.local.matrix = multiply(t, node.local.matrix);
    }
    for (auto& track : karakoy.animation) {
        if (track.instance >= karakoy.nodes.size() || karakoy.nodes[track.instance].parent >= 0) continue;
        for (auto& key : track.keys) {
            const auto p = apply(t, key.position[0], key.position[1], key.position[2], 1);
            key.position = p; key.rotation = premultiply_key_rotation(key.rotation);
        }
        track.pose_initialized = false;
    }
    for (auto& instance : karakoy.instances)
        if (karakoy.meshes[instance.mesh].influences.empty()) instance.transform.matrix = multiply(t, instance.transform.matrix);
    // Karaköy's water where Eminönü's already lies would fight it on the same plane.
    std::array<float, 4> water{1e9f, 1e9f, -1e9f, -1e9f};
    for (const auto& i : scene.instances) if (lower(i.name).starts_with("ocean")) {
        for (const auto& v : scene.meshes[i.mesh].vertices) {
            const auto p = apply(i.transform.matrix, v.x, v.y, v.z, 1);
            water = {std::min(water[0], p[0]), std::min(water[1], p[2]), std::max(water[2], p[0]), std::max(water[3], p[2])};
        }
    }
    for (const auto& i : karakoy.instances) if (lower(i.name).starts_with("ocean")) {
        auto& mesh = karakoy.meshes[i.mesh];
        std::vector<std::uint32_t> kept;
        for (std::size_t k = 0; k + 2 < mesh.indices.size(); k += 3) {
            float cx = 0, cz = 0;
            for (int j = 0; j < 3; ++j) { const auto& v = mesh.vertices[mesh.indices[k + j]]; const auto p = apply(i.transform.matrix, v.x, v.y, v.z, 1); cx += p[0] / 3; cz += p[2] / 3; }
            if (cx > water[0] && cx < water[2] && cz > water[1] && cz < water[3]) continue;
            kept.insert(kept.end(), mesh.indices.begin() + std::ptrdiff_t(k), mesh.indices.begin() + std::ptrdiff_t(k + 3));
        }
        mesh.indices = std::move(kept);
    }
    // --- Eminönü: the stub's end cap and the painted Karaköy backdrop cards stand where the bridge and Karaköy now are.
    std::vector<Instance> deck, piers;
    std::erase_if(scene.instances, [&](const Instance& i) {
        const auto name = lower(i.name);
        if (name == "galata_yol") deck.push_back(i);
        if (name == "galata_ayak" || name.starts_with("galata_korkuluk")) piers.push_back(i);
        return name == "kopru_kapatma" || (name.starts_with("kara_0") && name.size() == 7);
    });
    if (deck.empty() || piers.empty()) throw std::runtime_error("Galata Bridge: Eminönü stub not found");
    // Clicks pick only the invisible walk set (SourceSupport colliders); the new deck needs its own strip in it.
    const auto walkset = std::find_if(scene.instances.begin(), scene.instances.end(),
        [](const Instance& i) { return i.source_support && lower(i.name).starts_with("walkset"); });
    if (walkset == scene.instances.end()) throw std::runtime_error("Galata Bridge: Eminönü walk set not found");
    const Instance walk_template = *walkset;
    append(scene, std::move(karakoy));
    // --- The new sections: copies of the stub's last section, each one section further and on the rising deck.
    const auto node = std::uint32_t(scene.nodes.size());
    SceneNode root; root.id = node; root.name = "galata_koprusu_yeni"; scene.nodes.push_back(root); scene.node_order.push_back(node);
    for (int k = 1; k <= sections; ++k) {
        const float offset = section * float(k);
        const auto add = [&](const Instance& from, bool clip_section) {
            auto mesh = section_copy(scene, from, offset, clip_section);
            if (mesh.indices.empty()) return;
            Instance copy; copy.mesh = std::uint32_t(scene.meshes.size()); copy.node = node; copy.name = from.name + "_yeni" + std::to_string(k);
            copy.source_support = from.source_support; copy.source_nocamera = from.source_nocamera;
            scene.meshes.push_back(std::move(mesh)); scene.instances.push_back(std::move(copy));
        };
        for (const auto& d : deck) add(d, true);
        for (const auto& p : piers) {
            float s = 0; const auto& mesh = scene.meshes[p.mesh]; if (mesh.vertices.empty()) continue;
            for (const auto& v : mesh.vertices) { const auto q = apply(p.transform.matrix, v.x, v.y, v.z, 1); s += bridge_coordinates(q[0], q[2]).s; }
            s /= float(mesh.vertices.size());
            if (s >= -section && s < 0) add(p, false);
        }
    }
    {
        Mesh strip = scene.meshes[walk_template.mesh];
        strip.vertices.clear(); strip.indices.clear(); strip.influences.clear();
        constexpr int steps = 64;
        const float from = -6, to = gap + 6;
        for (int k = 0; k <= steps; ++k) {
            const float s = from + (to - from) * float(k) / steps;
            for (const float w : {-walk_half_width, walk_half_width}) {
                Vertex v{};
                v.x = eminonu_end_x + s * axis_x + w * axis_z; v.z = eminonu_end_z + s * axis_z - w * axis_x;
                v.y = deck_height(s); v.ny = 1;
                strip.vertices.push_back(v);
            }
            if (k) {
                const auto a = std::uint32_t(2 * k - 2);
                strip.indices.insert(strip.indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
            }
        }
        Instance walk = walk_template;
        walk.mesh = std::uint32_t(scene.meshes.size()); walk.node = node; walk.name = "walkset_galata_yeni"; walk.transform = Transform{};
        scene.meshes.push_back(std::move(strip)); scene.instances.push_back(std::move(walk));
    }
    ++scene.geometry_revision;
}
} // namespace ikv::galata
