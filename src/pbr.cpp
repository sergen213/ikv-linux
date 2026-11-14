// NONCANONICAL wave 29: enhanced PBR texture pack loader (include/ikv/pbr.hpp, assets/pbr/README.md).
#include "ikv/pbr.hpp"
#include "ikv/renderer.hpp"

#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <future>
#include <map>
#include <stdexcept>

namespace ikv {
namespace {
[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("PBR pack: " + what); }
std::vector<std::uint8_t> read_file(const std::filesystem::path& path, std::uintmax_t limit) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) fail("cannot open " + path.string());
    const auto size = static_cast<std::uintmax_t>(in.tellg());
    if (size > limit) fail(path.string() + " exceeds " + std::to_string(limit) + " bytes");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) fail("cannot read " + path.string());
    return bytes;
}
std::uint32_t be32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }
constexpr std::uint32_t kMaxSide = 1024;
// Float RGBA level 0 -> RGBA8 texture with a full box-filtered mip chain (normal maps renormalised per level).
Texture rgba_texture(std::string name, std::string source, std::uint32_t w, std::uint32_t h, std::vector<float> level, bool normal_map) {
    // Oversized maps (contract: <= 1024) are box-reduced first.
    auto halve = [&](std::uint32_t& mw, std::uint32_t& mh, std::vector<float>& rgba) {
        const std::uint32_t nw = std::max(1u, mw / 2), nh = std::max(1u, mh / 2);
        std::vector<float> next(std::size_t(nw) * nh * 4);
        for (std::uint32_t y = 0; y < nh; ++y) for (std::uint32_t x = 0; x < nw; ++x) for (unsigned c = 0; c < 4; ++c) {
            float s = 0;
            for (unsigned k = 0; k < 4; ++k) s += rgba[(std::size_t(std::min(mh - 1, y * 2 + k / 2)) * mw + std::min(mw - 1, x * 2 + k % 2)) * 4 + c];
            next[(std::size_t(y) * nw + x) * 4 + c] = s / 4;
        }
        if (normal_map) for (std::size_t i = 0; i < next.size(); i += 4) {
            float v[3]{next[i] * 2 - 1, next[i + 1] * 2 - 1, next[i + 2] * 2 - 1};
            const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (l > 1e-6f) for (unsigned c = 0; c < 3; ++c) next[i + c] = v[c] / l * .5f + .5f;
        }
        rgba = std::move(next); mw = nw; mh = nh;
    };
    while (std::max(w, h) > kMaxSide) halve(w, h, level);
    Texture out{std::move(name), w, h, {}, {}, std::move(source), assets::DdsFormat::rgba8};
    out.data.reserve(std::size_t(w) * h * 4 * 4 / 3 + 64);
    for (std::uint32_t mw = w, mh = h;;) {
        const std::size_t offset = out.data.size();
        for (const float v : level) out.data.push_back(std::uint8_t(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)));
        out.mips.push_back({mw, mh, offset, out.data.size() - offset});
        if (mw == 1 && mh == 1) break;
        halve(mw, mh, level);
    }
    return out;
}
std::vector<float> png_rgba(const PngImage& image) {
    std::vector<float> rgba(std::size_t(image.width) * image.height * 4);
    for (std::size_t i = 0; i < std::size_t(image.width) * image.height; ++i) {
        const auto* s = &image.samples[i * image.channels];
        const auto at = [&](unsigned c) { return float(s[c]) / 65535.f; };
        switch (image.channels) {
        case 1: rgba[i*4] = rgba[i*4+1] = rgba[i*4+2] = at(0); rgba[i*4+3] = 1; break;
        case 2: rgba[i*4] = rgba[i*4+1] = rgba[i*4+2] = at(0); rgba[i*4+3] = at(1); break;
        case 3: for (unsigned c = 0; c < 3; ++c) rgba[i*4+c] = at(c); rgba[i*4+3] = 1; break;
        default: for (unsigned c = 0; c < 4; ++c) rgba[i*4+c] = at(c);
        }
    }
    return rgba;
}
// Height (first channel, white = high) -> derived-parallax layout: r height, g/b central differences + .5 (wrapping).
std::vector<float> height_layout(const PngImage& image) {
    const std::uint32_t w = image.width, h = image.height;
    std::vector<float> l(std::size_t(w) * h), out(l.size() * 4);
    for (std::size_t i = 0; i < l.size(); ++i) l[i] = float(image.samples[i * image.channels]) / 65535.f;
    const auto at = [&](std::uint32_t x, std::uint32_t y) { return l[std::size_t(y % h) * w + x % w]; };
    for (std::uint32_t y = 0; y < h; ++y) for (std::uint32_t x = 0; x < w; ++x) {
        float* p = &out[(std::size_t(y) * w + x) * 4];
        p[0] = at(x, y); p[1] = std::clamp(.5f + (at(x + 1, y) - at(x + w - 1, y)), 0.f, 1.f);
        p[2] = std::clamp(.5f + (at(x, y + 1) - at(x, y + h - 1)), 0.f, 1.f); p[3] = 1;
    }
    return out;
}
float number_in(const nlohmann::json& j, const char* key, float fallback, float lo, float hi, const std::string& where) {
    if (!j.contains(key) || j.at(key).is_null()) return fallback;
    if (!j.at(key).is_number()) fail(where + "." + key + " must be a number");
    const float v = j.at(key).get<float>();
    if (!std::isfinite(v) || v < lo || v > hi) fail(where + "." + key + " out of range");
    return v;
}
bool flag_in(const nlohmann::json& j, const char* key, bool fallback, const std::string& where) {
    if (!j.contains(key) || j.at(key).is_null()) return fallback;
    if (!j.at(key).is_boolean()) fail(where + "." + key + " must be a boolean");
    return j.at(key).get<bool>();
}
} // namespace

std::string pbr_texture_key(std::string_view diffuse) {
    std::string key(diffuse);
    for (auto& c : key) c = c == '\\' ? '/' : char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return key;
}

std::vector<bool> pbr_eligible_materials(const Scene& scene) {
    std::vector<std::uint8_t> state(scene.materials.size()); // 1 eligible, 2 excluded
    for (const auto& instance : scene.instances) {
        if (instance.mesh >= scene.meshes.size()) continue;
        const auto material = instance.material == UINT32_MAX ? scene.meshes[instance.mesh].material : instance.material;
        if (material >= state.size()) continue;
        const auto& m = scene.materials[material];
        const bool ok = !instance.sky && !instance.ocean && !instance.entity_id && !instance.occlusion_proxy &&
                        scene.meshes[instance.mesh].influences.empty() && !m.alpha && m.alpha_cutoff <= 0 && !m.source_ocean &&
                        m.environment.empty() && !m.diffuse.empty();
        state[material] = ok && state[material] != 2 ? 1 : 2;
    }
    std::vector<bool> out(state.size());
    for (std::size_t i = 0; i < state.size(); ++i) out[i] = state[i] == 1;
    return out;
}

const PbrMaterial* PbrPack::find(std::string_view diffuse) const {
    const auto it = textures.find(pbr_texture_key(diffuse));
    return it == textures.end() ? nullptr : &materials[it->second];
}

std::filesystem::path pbr_pack_root(const char* executable_base) {
    namespace fs = std::filesystem;
    const auto has = [](const fs::path& p) { std::error_code e; return fs::is_regular_file(p / "manifest.json", e); };
    if (const char* env = std::getenv("IKV_PBR_DIR")) {
        const std::string s(env);
        if (s.empty() || s == "off") return {};
        return has(s) ? fs::path(s) : fs::path();
    }
    std::vector<fs::path> roots;
#ifdef IKV_PBR_SOURCE_DIR
    roots.emplace_back(IKV_PBR_SOURCE_DIR);
#endif
    if (executable_base) { roots.emplace_back(fs::path(executable_base) / "pbr"); roots.emplace_back(fs::path(executable_base) / "../share/ikv/pbr"); }
    for (const auto& r : roots) if (has(r)) return r;
    return {};
}

PngImage decode_png(std::span<const std::uint8_t> b) {
    static constexpr std::uint8_t signature[8]{137, 80, 78, 71, 13, 10, 26, 10};
    if (b.size() < 8 || !std::equal(signature, signature + 8, b.begin())) fail("not a PNG");
    std::uint32_t w = 0, h = 0; unsigned depth = 0, type = 0;
    std::vector<std::uint8_t> idat;
    std::vector<std::array<std::uint8_t, 3>> palette;
    bool header = false, end = false;
    for (std::size_t at = 8; !end;) {
        if (b.size() - at < 12) fail("truncated PNG chunk");
        const std::uint32_t length = be32(&b[at]);
        if (length > b.size() - at - 12) fail("truncated PNG chunk data");
        const std::string_view kind(reinterpret_cast<const char*>(&b[at + 4]), 4);
        const std::uint8_t* data = &b[at + 8];
        if (kind == "IHDR") {
            if (length != 13) fail("bad IHDR");
            w = be32(data); h = be32(data + 4); depth = data[8]; type = data[9];
            if (data[10] || data[11]) fail("unsupported PNG compression/filter");
            if (data[12]) fail("interlaced PNG unsupported");
            header = true;
        } else if (kind == "PLTE") {
            for (std::uint32_t i = 0; i + 2 < length; i += 3) palette.push_back({data[i], data[i + 1], data[i + 2]});
        } else if (kind == "IDAT") idat.insert(idat.end(), data, data + length);
        else if (kind == "IEND") end = true;
        at += 12 + std::size_t(length);
    }
    if (!header || !w || !h || w > 8192 || h > 8192) fail("PNG dimensions");
    const unsigned channels = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : type == 6 ? 4 : 0;
    const bool ok_depth = depth == 8 || (depth == 16 && type != 3) || ((type == 0 || type == 3) && (depth == 1 || depth == 2 || depth == 4));
    if (!channels || !ok_depth) fail("unsupported PNG colour type/depth");
    if (type == 3 && palette.empty()) fail("palette PNG without PLTE");
    const std::size_t bits = std::size_t(channels) * depth, stride = (std::size_t(w) * bits + 7) / 8, step = std::max<std::size_t>(1, bits / 8);
    std::vector<std::uint8_t> raw(std::size_t(h) * (stride + 1));
    uLongf size = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &size, idat.data(), static_cast<uLong>(idat.size())) != Z_OK || size != raw.size()) fail("PNG inflate failed");
    std::vector<std::uint8_t> previous(stride), row(stride);
    PngImage out{w, h, type == 3 ? 3u : channels, {}};
    out.samples.resize(std::size_t(w) * h * out.channels);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* in = &raw[std::size_t(y) * (stride + 1)];
        const unsigned filter = in[0];
        if (filter > 4) fail("bad PNG filter");
        for (std::size_t x = 0; x < stride; ++x) {
            const int a = x >= step ? row[x - step] : 0, up = previous[x], c = x >= step ? previous[x - step] : 0;
            int v = in[1 + x];
            if (filter == 1) v += a;
            else if (filter == 2) v += up;
            else if (filter == 3) v += (a + up) / 2;
            else if (filter == 4) {
                const int p = a + up - c, pa = std::abs(p - a), pb = std::abs(p - up), pc = std::abs(p - c);
                v += pa <= pb && pa <= pc ? a : pb <= pc ? up : c;
            }
            row[x] = std::uint8_t(v);
        }
        for (std::uint32_t x = 0; x < w; ++x) for (unsigned ch = 0; ch < channels; ++ch) {
            const std::size_t bit = (std::size_t(x) * channels + ch) * depth;
            std::uint32_t v;
            if (depth == 16) v = std::uint32_t(row[bit / 8]) << 8 | row[bit / 8 + 1];
            else if (depth == 8) v = row[bit / 8];
            else v = (row[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
            auto* dst = &out.samples[(std::size_t(y) * w + x) * out.channels];
            if (type == 3) {
                if (v >= palette.size()) fail("PNG palette index");
                for (unsigned c = 0; c < 3; ++c) dst[c] = std::uint16_t(palette[v][c] * 257u);
            } else dst[ch] = std::uint16_t(depth == 16 ? v : depth == 8 ? v * 257u : v * 65535u / ((1u << depth) - 1));
        }
        std::swap(previous, row);
    }
    return out;
}

std::shared_ptr<const PbrPack> load_pbr_pack(const std::filesystem::path& root) {
    auto pack = std::make_shared<PbrPack>();
    pack->root = root;
    const auto bytes = read_file(root / "manifest.json", 4u << 20);
    const auto manifest = nlohmann::json::parse(bytes.begin(), bytes.end());
    if (!manifest.is_object() || manifest.value("version", 0) != 1) fail("manifest version must be 1");
    const auto& materials = manifest.at("materials");
    if (!materials.is_object()) fail("materials must be an object");
    std::map<std::string, std::uint32_t> ids;
    // Capture/test hook (NONCANONICAL wave 29): IKV_PBR_HEIGHT_SCALE multiplies every height_scale (exaggerated relief makes
    // tessellated silhouettes and shadows visible in captures). Unset: 1.
    float relief_gain = 1;
    if (const char* gain = std::getenv("IKV_PBR_HEIGHT_SCALE")) {
        relief_gain = std::strtof(gain, nullptr);
        if (!std::isfinite(relief_gain) || relief_gain < 0 || relief_gain > 100) fail("IKV_PBR_HEIGHT_SCALE out of range 0..100");
    }
    for (const auto& [id, j] : materials.items()) {
        const std::string where = "materials." + id;
        if (!j.is_object() || id.empty() || id.find_first_of("/\\.") != std::string::npos) fail(where + " invalid");
        PbrMaterial m;
        m.id = id;
        if (j.contains("scale") && !j.at("scale").is_null()) {
            const auto& s = j.at("scale");
            if (!s.is_array() || s.size() != 2 || !s[0].is_number() || !s[1].is_number()) fail(where + ".scale must be [u,v] or null");
            m.scale = {s[0].get<float>(), s[1].get<float>()};
            for (const float v : m.scale) if (!std::isfinite(v) || v < .01f || v > 100) fail(where + ".scale out of range");
            m.world_uv = true;
        }
        m.height_scale = number_in(j, "height_scale", 0, 0, .5f, where) * relief_gain;
        m.tessellate = flag_in(j, "tessellate", false, where);
        m.parallax = flag_in(j, "parallax", false, where);
        m.tint_keep_original = number_in(j, "tint_keep_original", 0, 0, 1, where);
        ids.emplace(id, std::uint32_t(pack->materials.size()));
        pack->materials.push_back(std::move(m));
    }
    // Map decode in parallel (one job per material): every map optional.
    std::vector<std::future<std::vector<std::string>>> jobs;
    for (auto& m : pack->materials) jobs.push_back(std::async(std::launch::async, [&root, &m] {
        std::vector<std::string> notes;
        const auto dir = root / m.id;
        const auto load = [&](const char* file, Texture& target, int kind) { // 0 colour, 1 normal, 2 height
            const auto path = dir / file;
            std::error_code e;
            if (!std::filesystem::is_regular_file(path, e)) return;
            try {
                const auto png = decode_png(read_file(path, 64u << 20));
                target = rgba_texture("pbr:" + m.id + "/" + file, "pbr/" + m.id + "/" + file, png.width, png.height,
                    kind == 2 ? height_layout(png) : png_rgba(png), kind == 1);
            } catch (const std::exception& error) { notes.push_back(path.string() + ": " + error.what()); }
        };
        load("albedo.png", m.albedo, 0); load("normal.png", m.normal, 1); load("orm.png", m.orm, 0); load("height.png", m.height, 2);
        if (m.height.data.empty() || m.height_scale <= 0) m.tessellate = m.parallax = false; // nothing to displace
        return notes;
    }));
    for (auto& job : jobs) for (auto& note : job.get()) pack->diagnostics.push_back(std::move(note));
    if (manifest.contains("textures")) {
        const auto& textures = manifest.at("textures");
        if (!textures.is_object()) fail("textures must be an object");
        for (const auto& [key, id] : textures.items()) {
            if (!id.is_string() || !ids.contains(id.get<std::string>())) { pack->diagnostics.push_back("textures." + key + ": unknown material"); continue; }
            pack->textures[pbr_texture_key(key)] = ids.at(id.get<std::string>());
        }
    }
    return pack;
}

Mesh pbr_tessellation_mesh(const Mesh& mesh, bool world_uv, float max_edge) {
    if (mesh.indices.size() % 3 || !mesh.influences.empty()) return mesh;
    for (const auto index : mesh.indices) if (index >= mesh.vertices.size()) return mesh;
    Mesh out = subdivide_ocean_mesh(mesh, max_edge, 200000);
    struct Key { std::int64_t p[5]; bool operator<(const Key& o) const { return std::lexicographical_compare(p, p + 5, o.p, o.p + 5); } };
    std::map<Key, std::uint32_t> welds;
    std::vector<std::uint32_t> id(out.vertices.size());
    for (std::size_t i = 0; i < out.vertices.size(); ++i) {
        const auto& v = out.vertices[i];
        const auto q = [](float f, double s) { return std::int64_t(std::llround(double(f) * s)); };
        const Key k{{q(v.x, 64), q(v.y, 64), q(v.z, 64), world_uv ? 0 : q(v.u, 4096), world_uv ? 0 : q(v.v, 4096)}};
        id[i] = welds.try_emplace(k, std::uint32_t(welds.size())).first->second;
    }
    std::vector<std::array<double, 3>> face(welds.size()), vertex(welds.size());
    std::map<std::uint64_t, int> edges;
    for (std::size_t t = 0; t + 2 < out.indices.size(); t += 3) {
        const auto &a = out.vertices[out.indices[t]], &b = out.vertices[out.indices[t + 1]], &c = out.vertices[out.indices[t + 2]];
        const double e1[3]{b.x - a.x, b.y - a.y, b.z - a.z}, e2[3]{c.x - a.x, c.y - a.y, c.z - a.z};
        const double n[3]{e1[1]*e2[2] - e1[2]*e2[1], e1[2]*e2[0] - e1[0]*e2[2], e1[0]*e2[1] - e1[1]*e2[0]};
        for (unsigned k = 0; k < 3; ++k) {
            const auto vi = out.indices[t + k], wi = id[vi], next = id[out.indices[t + (k + 1) % 3]];
            const auto& v = out.vertices[vi];
            for (unsigned c3 = 0; c3 < 3; ++c3) face[wi][c3] += n[c3];
            vertex[wi][0] += v.nx; vertex[wi][1] += v.ny; vertex[wi][2] += v.nz;
            if (wi != next) ++edges[std::uint64_t(std::min(wi, next)) << 32 | std::max(wi, next)];
        }
    }
    std::vector<bool> open(welds.size());
    for (const auto& [edge, count] : edges) if (count != 2) { open[edge >> 32] = true; open[edge & 0xffffffffu] = true; }
    for (std::size_t i = 0; i < out.vertices.size(); ++i) {
        auto n = face[id[i]];
        const auto& s = vertex[id[i]];
        if (n[0]*s[0] + n[1]*s[1] + n[2]*s[2] < 0) for (auto& c : n) c = -c; // follow the authored normals' side
        const double l = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        const float weight = open[id[i]] || !(l > 1e-12) ? 0.f : 1.f;
        out.vertices[i].tangent = {float(n[0] / std::max(l, 1e-12)) * weight, float(n[1] / std::max(l, 1e-12)) * weight, float(n[2] / std::max(l, 1e-12)) * weight};
    }
    return out;
}

void pbr_self_test() {
    const auto need = [](bool ok, const char* what) { if (!ok) throw std::runtime_error(std::string("pbr self-test: ") + what); };
    need(pbr_texture_key("Textures\\Zemin.DDS") == "textures/zemin.dds", "key normalisation");
    // 2x2 16-bit grey PNG with Sub/Up/Paeth filters, encoded here with zlib.
    std::vector<std::uint8_t> raw{1, 0x10, 0x00, 0x10, 0x00, 4, 0x20, 0x00, 0x00, 0x00};
    uLongf zsize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> z(zsize);
    need(compress(z.data(), &zsize, raw.data(), static_cast<uLong>(raw.size())) == Z_OK, "zlib");
    z.resize(zsize);
    std::vector<std::uint8_t> png{137, 80, 78, 71, 13, 10, 26, 10};
    const auto chunk = [&](const char* kind, const std::vector<std::uint8_t>& data) {
        for (int s = 24; s >= 0; s -= 8) png.push_back(std::uint8_t(data.size() >> s));
        png.insert(png.end(), kind, kind + 4); png.insert(png.end(), data.begin(), data.end());
        for (int i = 0; i < 4; ++i) png.push_back(0); // CRC is not verified
    };
    chunk("IHDR", {0, 0, 0, 2, 0, 0, 0, 2, 16, 0, 0, 0, 0}); chunk("IDAT", z); chunk("IEND", {});
    const auto image = decode_png(png);
    // row 0 Sub: 0x1000, 0x1000+0x1000; row 1 Paeth over row 0: 0x1000+0x2000, 0x2000+0 (paeth picks up/left)
    need(image.width == 2 && image.channels == 1 && image.samples[0] == 0x1000 && image.samples[1] == 0x2000 && image.samples[2] == 0x3000,
         "PNG unfilter");
    const auto tex = rgba_texture("t", "t", 4, 2, std::vector<float>(32, .5f), false);
    need(tex.mips.size() == 3 && tex.mips[2].width == 1 && tex.data.size() == 32 + 8 + 4, "mip chain");
    // Two quads sharing an edge, 16 units wide: after splitting to 8-unit edges, interior vertices displace, rim does not.
    Mesh m;
    for (int z2 = 0; z2 <= 1; ++z2) for (int x = 0; x <= 2; ++x) m.vertices.push_back(Vertex{float(x * 16), 0, float(z2 * 16), 0, 1, 0, float(x), float(z2)});
    m.indices = {0, 3, 1, 1, 3, 4, 1, 4, 2, 2, 4, 5};
    const auto t = pbr_tessellation_mesh(m, true, 8);
    bool interior = false, rim = true;
    for (const auto& v : t.vertices) {
        const bool edge = v.x == 0 || v.x == 32 || v.z == 0 || v.z == 16;
        if (edge) rim = rim && v.tangent == std::array<float, 3>{0, 0, 0};
        else interior = interior || std::abs(v.tangent[1] - 1) < 1e-5f;
    }
    need(t.indices.size() > m.indices.size() && interior && rim, "tessellation weights");
}
} // namespace ikv
