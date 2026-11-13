#pragma once

#include "ikv/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <vector>

struct SDL_Window;

namespace ikv {
// Metres, Y up; yaw/pitch in radians, vertical field of view in degrees.
// Defaults frame a ~7.5-unit source actor from behind/above. Original LookAtRH/PerspectiveFovRH camera (004773e0/
// 00476640): look-at = player+6 up, up (0,1,0), fovy 50; pitch .27 (15.5 deg) is the median view pitch measured from
// the original gameplay video (10-25 deg); steeper pitches lean screen-edge verticals far more than the original did.
struct Camera {
    std::array<float, 3> target{0, 0, 0};
    float yaw = 0, pitch = 0.27f, distance = 22, fov = 50;
    float near_plane = 0.1f, far_plane = 10000;
};
enum class RayTracing { off, automatic, required };
// Vulkan RT lighting. faithful: the original lighting equation in every appearance (no denoiser); RT only adds actor
// cast shadows (deterministic Sun rays against TLAS mask 2). physical: enhanced-appearance GGX/GI/reflections + denoiser;
// with original appearance nothing traces, so no acceleration structure is built.
enum class RtStyle { faithful, physical };
// Vulkan only. fsr3: AMD FidelityFX Super Resolution 3.1.4 temporal upscaling (render_scale ignored); OpenGL renders natively.
enum class Upscaler { bilinear, fsr3 };
// FSR 3.1.4 presets: render resolution = output / {1, 1.5, 1.7, 2, 3}.
enum class FsrQuality { native_aa, quality, balanced, performance, ultra_performance };
struct RendererOptions {
    Camera camera;
    bool original_appearance = true;
    RayTracing ray_tracing = RayTracing::off;
    RtStyle rt_style = RtStyle::faithful;
    float render_scale = 1;
    Upscaler upscaler = Upscaler::bilinear;
    FsrQuality fsr_quality = FsrQuality::quality;
    float fsr_sharpness = .5f; // RCAS 0..1; 0 disables the sharpening pass
    // Vulkan + fsr3 only: FSR 3.1.4 frame generation (optical flow + frame interpolation) presents one generated frame
    // between every two rendered frames, paced by the renderer. Ignored (with a log line) without FSR or Vulkan.
    bool frame_generation = false;
    bool antialiasing = true;
    float exposure = 1;
    bool vsync = true;
    // NONCANONICAL (Ayarlar "Çizim Mesafesi"): draws whose bounding sphere lies wholly beyond this many units from the
    // eye are skipped in the main view (shadows, RT and sky unaffected); 0 = unlimited.
    float view_distance = 0;
    // NONCANONICAL enhanced parallax occlusion on static zone materials with a derived height (Material::height); original
    // appearance never applies it.
    bool parallax = true;
    // Main-thread transport progress only: must not reenter Scene/UI/Renderer; may throw.
    std::function<void()> progress;
};
// Resolved scene-space placement. Ground support and wire-unit conversion belong to the caller.
struct RenderEntity {
    std::uint32_t id = 0;
    float x = 0, ground_y = 0, z = 0, yaw = 0;
    // Original hover hilite colour (node +0xf8..+0x100, 0046ccf0 tail / 0047e1xx); all zero = no contour.
    std::array<float, 3> contour{};
};
// Straight-alpha RGBA8, top-left origin. Client rasterizes localized HUD/text.
struct Overlay {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};
using Matrix4 = std::array<float, 16>; // Column-major; column vectors.
inline Matrix4 multiply_matrix(const Matrix4& a, const Matrix4& b) {
    Matrix4 result{};
    for (unsigned c = 0; c < 4; ++c)
        for (unsigned r = 0; r < 4; ++r)
            for (unsigned k = 0; k < 4; ++k) result[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
    return result;
}
inline void validate_camera(const Camera& c) {
    for (float v : c.target) if (!std::isfinite(v)) throw std::invalid_argument("non-finite camera target");
    if (!std::isfinite(c.yaw) || !std::isfinite(c.pitch) || std::abs(c.pitch) >= 1.55f ||
        !std::isfinite(c.distance) || c.distance <= 0 || !std::isfinite(c.fov) || c.fov <= 1 || c.fov >= 175 ||
        !std::isfinite(c.near_plane) || !std::isfinite(c.far_plane) || c.near_plane <= 0 || c.far_plane <= c.near_plane)
        throw std::invalid_argument("invalid camera projection");
}
struct CameraBasis {
    std::array<float, 3> eye, right, up, forward;
};
inline CameraBasis camera_basis(const Camera& c) {
    validate_camera(c);
    const float sy = std::sin(c.yaw), cy = std::cos(c.yaw), sp = std::sin(c.pitch), cp = std::cos(c.pitch);
    CameraBasis b;
    b.forward = {-sy * cp, -sp, -cy * cp};
    b.right = {cy, 0, -sy};
    b.up = {-sy * sp, cp, -cy * sp};
    for (unsigned i = 0; i < 3; ++i) b.eye[i] = c.target[i] - b.forward[i] * c.distance;
    return b;
}
inline Matrix4 camera_view_projection(const Camera& c, int width, int height, bool vulkan = false) {
    if (width <= 0 || height <= 0) throw std::invalid_argument("empty render extent");
    const auto b = camera_basis(c);
    auto dot = [](const auto& a, const auto& v) { return a[0]*v[0]+a[1]*v[1]+a[2]*v[2]; };
    Matrix4 view{b.right[0], b.up[0], -b.forward[0], 0,
                 b.right[1], b.up[1], -b.forward[1], 0,
                 b.right[2], b.up[2], -b.forward[2], 0,
                 -dot(b.right,b.eye), -dot(b.up,b.eye), dot(b.forward,b.eye), 1};
    const float f = 1 / std::tan(c.fov * 0.008726646259971648f);
    Matrix4 projection{};
    projection[0] = f * static_cast<float>(height) / static_cast<float>(width);
    projection[5] = vulkan ? -f : f;
    projection[10] = vulkan ? c.far_plane / (c.near_plane - c.far_plane) : (c.far_plane+c.near_plane)/(c.near_plane-c.far_plane);
    projection[11] = -1;
    projection[14] = vulkan ? c.far_plane*c.near_plane/(c.near_plane-c.far_plane) : 2*c.far_plane*c.near_plane/(c.near_plane-c.far_plane);
    return multiply_matrix(projection, view);
}
// Enhanced raster sun: the source SunDir when known, otherwise a fixed fallback direction (towards the light).
inline std::array<float, 3> enhanced_sun(const SceneLighting& l) {
    return l.source_values ? l.sun_direction : std::array<float, 3>{.48f, .78f, .4f};
}
inline bool original_blob_shadows_enabled(bool original_appearance) { return original_appearance; }
// NONCANONICAL wave 27: SceneLighting::weather as shader inputs (Vulkan frame.glsl Frame wind/weather/weatherMisc, OpenGL
// weather_* uniforms). wind: xy unit direction (world X/Z the wind blows toward), z speed m/s, w accumulated wind phase in
// metres (renderer integrates speed*dt, wrapped at kWeatherPhasePeriod; every shader use is periodic in it). weather: x
// precipitation, y cloud cover, z wetness, w snow cover. misc: x fog density, y gust, z season 0..3, w kind+1 (0 inactive).
// All zero (the exact pre-wave shader paths) while inactive, with original appearance, or in a roofed zone (interiors get
// no precipitation, wind sway, wetness or snow).
constexpr double kWeatherPhasePeriod = 1024;
struct WeatherUniforms { std::array<float, 4> wind{}, weather{}, misc{}; bool active() const { return misc[3] > .5f; } };
inline WeatherUniforms weather_uniforms(const SceneLighting& l, bool original_appearance, double phase) {
    const auto& w = l.weather;
    if (!w.active || original_appearance || l.roofed) return {};
    auto unit = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.f; };
    float dx = w.wind_direction[0], dz = w.wind_direction[1];
    const float length = std::sqrt(dx*dx + dz*dz);
    if (std::isfinite(length) && length > 1e-6f) { dx /= length; dz /= length; } else { dx = 1; dz = 0; }
    const float speed = std::isfinite(w.wind_speed) ? std::clamp(w.wind_speed, 0.f, 25.f) : 0.f;
    return {{dx, dz, speed, float(std::fmod(std::max(phase, 0.), kWeatherPhasePeriod))},
            {unit(w.precipitation), unit(w.cloud_cover), unit(w.wetness), unit(w.snow_cover)},
            {unit(w.fog_density), unit(w.gust), float(std::min(unsigned(w.season), 3u)), float(std::min(unsigned(w.kind), 5u) + 1)}};
}
// Backdrop (clear colour, display-referred) of zones without a sky dome under weather: weather.glsl weatherSky's overcast
// pull toward the weather fog colour (weatherFogColor). Returns `clear` unchanged while weather is inactive.
inline std::array<float, 3> weather_backdrop(const WeatherUniforms& u, const SceneLighting& l, std::array<float, 3> clear) {
    if (!u.active()) return clear;
    const auto& s = u.weather; const auto& m = u.misc;
    std::array<float, 3> base = l.fog_enabled ? l.fog_color : std::array<float, 3>{.62f, .66f, .70f};
    const float grey = base[0]*.299f + base[1]*.587f + base[2]*.114f, g = std::clamp(.5f*s[1] + .4f*m[0], 0.f, 1.f);
    const float storm = m[3] > 3.5f && m[3] < 4.5f ? .75f : 1.f, amount = std::clamp(.9f*s[1] + .9f*m[0] + .2f*s[0], 0.f, 1.f);
    const std::array<float, 3> tint{.96f, 1.f, 1.04f};
    for (unsigned i = 0; i < 3; ++i) {
        const float fog = (base[i] + (grey*tint[i] - base[i])*g) * (1 - .35f*s[1]) * storm;
        clear[i] += (fog - clear[i]) * amount;
    }
    return clear;
}
// NONCANONICAL wave 28 (GL/Vulkan fog parity): the enhanced scene clear colour as linear HDR (what Vulkan clears its
// HDR target to; weather_backdrop under weather) and its display colour after the enhanced post.frag exposure, ACES and
// gamma. GL has no HDR target: it clears to enhanced_display(...) so its backdrop passes the same tonemap as the
// record/weather-fogged land meeting it at the horizon (until wave 28 GL cleared to the untonemapped colour, darker).
inline std::array<float, 3> enhanced_backdrop_linear(const WeatherUniforms& u, const SceneLighting& l) {
    std::array<float, 3> c{.04f, .05f, .065f};
    if (!u.active()) return c;
    const auto b = weather_backdrop(u, l, {std::pow(c[0], 1/2.2f), std::pow(c[1], 1/2.2f), std::pow(c[2], 1/2.2f)});
    for (unsigned i = 0; i < 3; ++i) c[i] = std::pow(b[i], 2.2f);
    return c;
}
inline std::array<float, 3> enhanced_display(std::array<float, 3> c, float exposure) {
    for (auto& v : c) {
        const float x = std::max(v, 0.f) * exposure;
        v = std::pow(std::clamp((x*(2.51f*x + .03f))/(x*(2.43f*x + .59f) + .14f), 0.f, 1.f), 1/2.2f);
    }
    return c;
}
// Wind phase after one frame: integrated (continuous when the speed changes), never speed*time.
inline double advance_wind_phase(double phase, const SceneLighting& l, bool original_appearance, float dt) {
    if (!l.weather.active || original_appearance || l.roofed || !std::isfinite(dt) || dt <= 0) return phase;
    const float speed = std::isfinite(l.weather.wind_speed) ? std::clamp(l.weather.wind_speed, 0.f, 25.f) : 0.f;
    return std::fmod(phase + double(speed) * std::min(dt, 1.f), kWeatherPhasePeriod);
}
// NONCANONICAL wave 28: recent character footsteps for the enhanced weather surfaces (weather.glsl: rings in rain
// puddles, prints pressed into snow cover). x/z world position (scene units), age seconds since the step, heading
// radians of travel (atan2(dx, dz)), side -1 left foot / +1 right foot. EffectSystem::ground_marks() produces them from
// actor movement; Renderer::set_ground_marks() uploads at most kMaxGroundMarks (newest first) for the next render().
struct GroundMark { float x = 0, z = 0, age = 0, heading = 0, side = 1; };
constexpr std::size_t kMaxGroundMarks = 64;
// CPU mirror of weather.glsl weatherHash/weatherNoise/weatherPuddle for flat upward ground (up.y = 1): puddle coverage
// 0..1 at world x/z for the given weather wetness. Must stay bit-for-bit in step with the shader (splashes, prints).
inline float weather_hash(float cx, float cz) {
    const auto qx = std::uint32_t(std::int32_t(std::floor(cx))), qz = std::uint32_t(std::int32_t(std::floor(cz)));
    std::uint32_t n = (qx * 1597334677u) ^ (qz * 3812015801u);
    n = (n ^ (n >> 15)) * 2246822519u;
    return float(n >> 8) * (1.f / 16777216.f);
}
inline float weather_noise(float x, float z) {
    const float ix = std::floor(x), iz = std::floor(z), fx = x - ix, fz = z - iz;
    const float ux = fx*fx*(3 - 2*fx), uz = fz*fz*(3 - 2*fz);
    const float a = weather_hash(ix, iz), b = weather_hash(ix + 1, iz), c = weather_hash(ix, iz + 1), d = weather_hash(ix + 1, iz + 1);
    return (a + (b - a)*ux) + ((c + (d - c)*ux) - (a + (b - a)*ux))*uz;
}
// NONCANONICAL wave 28: ~12% of flat ground at full wetness (weather.glsl weatherPuddle thresholds).
inline float weather_puddle(float x, float z, float wetness) {
    if (!(wetness > 0)) return 0;
    wetness = std::min(wetness, 1.f);
    const float m = weather_noise(x*.06f, z*.06f)*.65f + weather_noise(x*.23f, z*.23f)*.35f;
    const float lo = .84f - .16f*wetness, hi = .88f - .16f*wetness, t = std::clamp((m - lo)/(hi - lo), 0.f, 1.f);
    return wetness * t*t*(3 - 2*t);
}
// NONCANONICAL wave 28: GroundMarks as shader inputs (Vulkan frame.glsl groundMark*, OpenGL weather_mark* uniforms;
// decoded by weather.glsl weatherSteps). info x: count; bounds: xz min/max of every mark's reach (kGroundMarkReach);
// marks: x, z, ground height y, w = foot sign * (floor(age*64)*64 + heading in 64ths of a turn + 1). All zero (no marks:
// the wave-27 surface) while weather is inactive. Marks older than kGroundMarkLife (prints faded) or non-finite drop.
constexpr float kGroundMarkLife = 90, kGroundMarkReach = 4;
struct GroundMarkUniforms {
    std::array<float, 4> info{}, bounds{};
    std::array<std::array<float, 4>, kMaxGroundMarks> marks{};
};
inline float pack_ground_mark(float age, float heading, float side) {
    constexpr float turn = 6.2831853f;
    float h = std::fmod(heading, turn); if (h < 0) h += turn;
    const float step = std::fmod(std::round(h / turn * 64), 64.f);
    return (side < 0 ? -1.f : 1.f) * (std::floor(std::clamp(age, 0.f, kGroundMarkLife) * 64) * 64 + step + 1);
}
// heights: each mark's ground height (same order as marks).
inline GroundMarkUniforms ground_mark_uniforms(const WeatherUniforms& u, std::span<const GroundMark> marks, std::span<const float> heights) {
    GroundMarkUniforms g;
    if (!u.active()) return g;
    std::size_t n = 0;
    for (std::size_t i = 0; i < marks.size() && i < heights.size() && n < kMaxGroundMarks; ++i) {
        const auto& m = marks[i];
        if (!std::isfinite(m.x) || !std::isfinite(m.z) || !std::isfinite(m.age) || !std::isfinite(m.heading) || !std::isfinite(heights[i]) ||
            m.age < 0 || m.age >= kGroundMarkLife) continue;
        if (n == 0) g.bounds = {m.x, m.z, m.x, m.z};
        g.bounds = {std::min(g.bounds[0], m.x), std::min(g.bounds[1], m.z), std::max(g.bounds[2], m.x), std::max(g.bounds[3], m.z)};
        g.marks[n++] = {m.x, m.z, heights[i], pack_ground_mark(m.age, m.heading, m.side)};
    }
    if (n) g.bounds = {g.bounds[0] - kGroundMarkReach, g.bounds[1] - kGroundMarkReach, g.bounds[2] + kGroundMarkReach, g.bounds[3] + kGroundMarkReach};
    g.info[0] = float(n);
    return g;
}
// Per-renderer footstep state: Renderer::set_ground_marks() stores the marks; at render() each mark new since the last
// frame takes its ground height from the nearest entity (the walker, kGroundMarkReach*2 units), else the camera target,
// and keeps it while it lives. debug (IKV_RENDER_GROUND_MARKS, capture-only, any non-empty value): replaces the marks
// by a fabricated 24-step trail across the view through the camera target (in rain moved onto the nearest puddle,
// weather_puddle), the six newest stepping again every 1.5 s so their rings animate.
class GroundMarkTracker {
public:
    explicit GroundMarkTracker(const char* debug) : debug_(debug && *debug) {}
    void set(std::span<const GroundMark> marks) { marks_.assign(marks.begin(), marks.begin() + std::ptrdiff_t(std::min(marks.size(), kMaxGroundMarks))); }
    GroundMarkUniforms uniforms(const WeatherUniforms& u, std::span<const RenderEntity> entities, const Camera& camera, float seconds) {
        if (!u.active()) { heights_.clear(); return {}; }
        if (debug_) fabricate(u, camera, seconds);
        float fallback = camera.target[1];
        if (debug_) { // the fabricated trail lies on the ground of the entity nearest the camera target (the player)
            float best = INFINITY;
            for (const auto& e : entities) {
                const float dx = e.x - camera.target[0], dz = e.z - camera.target[2], d = dx*dx + dz*dz;
                if (std::isfinite(e.ground_y) && d < best) { best = d; fallback = e.ground_y; }
            }
        }
        std::vector<std::array<float, 3>> next;
        next.reserve(marks_.size());
        std::vector<float> heights;
        heights.reserve(marks_.size());
        for (const auto& m : marks_) {
            float y = fallback, best = 4*kGroundMarkReach*kGroundMarkReach;
            bool known = false;
            for (const auto& h : heights_) if (h[0] == m.x && h[1] == m.z) { y = h[2]; known = true; break; }
            if (!known) for (const auto& e : entities) {
                const float dx = e.x - m.x, dz = e.z - m.z, d = dx*dx + dz*dz;
                if (std::isfinite(e.ground_y) && d < best) { best = d; y = e.ground_y; }
            }
            next.push_back({m.x, m.z, y});
            heights.push_back(y);
        }
        heights_ = std::move(next);
        return ground_mark_uniforms(u, marks_, heights);
    }
private:
    void fabricate(const WeatherUniforms& u, const Camera& camera, float seconds) {
        float cx = camera.target[0], cz = camera.target[2];
        if (u.weather[2] > 0 && u.weather[3] <= 0) { // rain: centre the trail on the nearest puddle
            float best = INFINITY;
            for (int i = -40; i <= 40; ++i) for (int j = -40; j <= 40; ++j) {
                const float x = camera.target[0] + float(i), z = camera.target[2] + float(j), d = float(i*i + j*j);
                if (d < best && weather_puddle(x, z, u.weather[2]) > .9f*u.weather[2]) { best = d; cx = x; cz = z; }
            }
        }
        const float heading = camera.yaw + 1.5707963f, fx = std::sin(heading), fz = std::cos(heading);
        marks_.clear();
        for (int i = 0; i < 24; ++i) { // newest first, the walker heading +f, strides 2.8 units, feet 0.48 off the line
            const float along = 8.f - 2.8f*float(i), side = i % 2 ? -1.f : 1.f;
            const float age = i < 6 ? std::fmod(seconds + .25f*float(i), 1.5f) : 4.f*float(i - 5);
            marks_.push_back({cx + fx*along + fz*.48f*side, cz + fz*along - fx*.48f*side, age, heading, side});
        }
    }
    bool debug_ = false;
    std::vector<GroundMark> marks_;
    std::vector<std::array<float, 3>> heights_; // x, z, ground y of the marks last uploaded
};
// Mesh-space {lowest, highest} vertex y (wind sway weights vertices by height above the mesh base).
inline std::array<float, 2> mesh_height_range(const Mesh& mesh) {
    float lo = INFINITY, hi = -INFINITY;
    for (const auto& v : mesh.vertices) if (std::isfinite(v.y)) { lo = std::min(lo, v.y); hi = std::max(hi, v.y); }
    return lo <= hi ? std::array<float, 2>{lo, hi} : std::array<float, 2>{0, 0};
}
// NONCANONICAL wave 27 wind sway heuristic (enhanced): vegetation and cloth. The source has no such flag; alpha-tested
// materials alone (the source cutoff) also cover mosque walls, railings, signs and skyline cards, so the material must also
// name foliage or cloth in its diffuse texture or name (Turkish/English: agac tree, cimen grass, yaprak/leaf, cicek flower,
// bitki plant, palm, tree, bayrak flag, perde/curtain). Static (unskinned, non-entity) placements 1..80 units (0.25..20 m)
// tall bend downwind, weighted by the squared height above the mesh base; the bend at full wind is 3.5% of the height, at
// most 1.2 units. Returns the per-draw sway vector {mesh-space base y, 1/mesh height, world bend amplitude, placement
// seed}; zero = rigid. Colour and shadow passes share it; Vulkan RT draws stay rigid (renderer_vk.cpp).
inline bool weather_sways(const Material& material) {
    if (material.alpha_cutoff <= 0 || material.source_ocean) return false;
    for (const auto* text : {&material.diffuse, &material.name}) {
        std::string lower(*text);
        for (auto& c : lower) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        for (const char* hint : {"agac", "cimen", "yaprak", "leaf", "cicek", "bitki", "palm", "tree", "bayrak", "perde", "curtain"})
            if (lower.find(hint) != std::string::npos) return true;
    }
    return false;
}
inline std::array<float, 4> weather_sway(const WeatherUniforms& u, const Material& material, const Matrix4& model,
                                         const std::array<float, 2>& range, bool rigid) {
    if (!u.active() || u.wind[2] <= 0 || rigid || !weather_sways(material)) return {};
    const float height = range[1] - range[0];
    const float scale = std::sqrt(model[4]*model[4] + model[5]*model[5] + model[6]*model[6]), world = height * scale;
    if (!(world >= 1 && world <= 80) || !(height > 0)) return {};
    const float seed = std::fmod(std::abs(model[12]*.0173f + model[14]*.0291f), 1.f);
    return {range[0], 1 / height, std::min(world * .035f, 1.2f), std::isfinite(seed) ? seed : 0.f};
}
// Capture/test hook (NONCANONICAL wave 27): IKV_RENDER_WEATHER="season=winter,kind=snow,wind=1:0:8,gust=.3,precip=.8,
// cloud=.9,fog=.2,wet=.5,snow=1,temp=-3" replaces SceneLighting::weather in the renderers (renders weather without a server).
inline std::optional<SceneWeather> parse_weather_override(const char* text) {
    if (!text || !*text) return {};
    SceneWeather w; w.active = true;
    const std::string s(text);
    auto number = [](const std::string& v) {
        std::size_t used = 0; const float f = std::stof(v, &used);
        if (used != v.size() || !std::isfinite(f)) throw std::invalid_argument("IKV_RENDER_WEATHER number " + v);
        return f;
    };
    for (std::size_t at = 0; at < s.size();) {
        const auto end = std::min(s.find(',', at), s.size());
        const auto item = s.substr(at, end - at), key = item.substr(0, item.find('='));
        at = end + 1;
        if (item.empty()) continue;
        if (key.size() == item.size()) throw std::invalid_argument("IKV_RENDER_WEATHER item lacks '=': " + item);
        const auto value = item.substr(key.size() + 1);
        auto pick = [&](std::initializer_list<const char*> names) {
            unsigned i = 0; for (auto n : names) { if (value == n) return i; ++i; }
            throw std::invalid_argument("IKV_RENDER_WEATHER value " + value);
        };
        if (key == "season") w.season = Season(pick({"spring", "summer", "autumn", "winter"}));
        else if (key == "kind") w.kind = WeatherKind(pick({"clear", "cloudy", "rain", "storm", "snow", "fog"}));
        else if (key == "wind") {
            const auto a = value.find(':'), b = a == std::string::npos ? a : value.find(':', a + 1);
            if (b == std::string::npos) throw std::invalid_argument("IKV_RENDER_WEATHER wind=dx:dz:speed");
            w.wind_direction = {number(value.substr(0, a)), number(value.substr(a + 1, b - a - 1))}; w.wind_speed = number(value.substr(b + 1));
        }
        else if (key == "gust") w.gust = number(value); else if (key == "precip") w.precipitation = number(value);
        else if (key == "cloud") w.cloud_cover = number(value); else if (key == "fog") w.fog_density = number(value);
        else if (key == "wet") w.wetness = number(value); else if (key == "snow") w.snow_cover = number(value);
        else if (key == "temp") w.temperature = number(value);
        else throw std::invalid_argument("IKV_RENDER_WEATHER key " + key);
    }
    return w;
}
// Enhanced raster shadow map (2048^2): orthographic sun view around the camera, Vulkan clip space (y down, z 0..1).
inline Matrix4 shadow_light_matrix(const Camera& camera, const std::array<float, 3>& sun, float radius) {
    Camera light = camera;
    light.yaw = std::atan2(sun[0], sun[2]); light.pitch = std::atan2(sun[1], std::sqrt(sun[0]*sun[0]+sun[2]*sun[2]));
    light.pitch = std::clamp(light.pitch, -1.54f, 1.54f);
    const bool far = radius>std::max(30.f,camera.distance*2);
    light.distance = far?std::max({50.f,camera.distance*3,radius*2}):std::max(50.f,camera.distance*3);
    light.near_plane = .1f; light.far_plane = light.distance*4;
    const auto basis = camera_basis(light);
    auto dot = [](const auto& a, const auto& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
    Matrix4 view{basis.right[0], basis.up[0], -basis.forward[0], 0, basis.right[1], basis.up[1], -basis.forward[1], 0,
        basis.right[2], basis.up[2], -basis.forward[2], 0, -dot(basis.right,basis.eye), -dot(basis.up,basis.eye), dot(basis.forward,basis.eye), 1};
    // Whole shadow-map texels only: a window following the player by fractions of a texel re-rasterised every leaf
    // shadow at a new sub-texel phase each frame (crawling shadows while walking).
    const float texel = 2*radius/2048;
    for (std::size_t i : {12u, 13u}) view[i] = std::round(view[i]/texel)*texel;
    Matrix4 projection{}; projection[0] = 1/radius; projection[5] = -1/radius;
    projection[10] = 1/(light.near_plane-light.far_plane);
    projection[14] = light.near_plane/(light.near_plane-light.far_plane); projection[15] = 1;
    return multiply_matrix(projection, view);
}
inline Matrix4 shadow_light_matrix(const Camera& camera, const std::array<float, 3>& sun) {
    return shadow_light_matrix(camera,sun,std::max(30.f,camera.distance*2));
}
struct ShadowCascades { std::array<Matrix4, 2> light; float split; };
inline ShadowCascades shadow_light_cascades(const Camera& camera, const std::array<float, 3>& sun, float view_distance) {
    const float near = std::max(30.f, camera.distance*2);
    const float far = std::max(near*4, view_distance>0?view_distance:600.f);
    return {{shadow_light_matrix(camera,sun,near),shadow_light_matrix(camera,sun,far)},near};
}
// Conservative culling sphere {centre xyz, radius} in mesh space: AABB centre and half-diagonal.
// Non-finite vertices give an infinite radius, which is never culled.
inline std::array<float, 4> mesh_bounds(const Mesh& mesh) {
    if (mesh.vertices.empty()) return {0, 0, 0, 0};
    std::array<float, 3> lo{INFINITY, INFINITY, INFINITY}, hi{-INFINITY, -INFINITY, -INFINITY};
    for (const auto& v : mesh.vertices) {
        const std::array<float, 3> p{v.x, v.y, v.z};
        for (unsigned i = 0; i < 3; ++i) {
            if (!std::isfinite(p[i])) return {0, 0, 0, INFINITY}; // std::min/max would silently skip NaN
            lo[i] = std::min(lo[i], p[i]); hi[i] = std::max(hi[i], p[i]);
        }
    }
    std::array<float, 4> s{(lo[0]+hi[0])/2, (lo[1]+hi[1])/2, (lo[2]+hi[2])/2,
                           std::sqrt((hi[0]-lo[0])*(hi[0]-lo[0])+(hi[1]-lo[1])*(hi[1]-lo[1])+(hi[2]-lo[2])*(hi[2]-lo[2]))/2};
    for (float v : s) if (!std::isfinite(v)) return {0, 0, 0, INFINITY};
    return s;
}
// NONCANONICAL wave 28 (enhanced appearance only): the source ocean meshes are coarse (Sivri Ada mean edges 26-130 units,
// 6-32 m), too coarse to carry the wind sea's crests (frame.glsl oceanWindSwell). For any mesh drawn with a source_ocean
// material (ocean_wave_meshes) the renderers also upload a copy split by conforming longest-edge bisection until no edge
// exceeds max_edge (raised so that a mesh stays near max_triangles), and draw that copy only where ocean_waves_on and
// kOceanWaveRange allow; everywhere else the source mesh draws exactly as before the wave. Every attribute is
// interpolated linearly, so the copy differs only where it is displaced.
inline Mesh subdivide_ocean_mesh(const Mesh& mesh, float max_edge = 24.f, std::size_t max_triangles = 400000) {
    if (mesh.indices.size() % 3 || !mesh.influences.empty()) return mesh;
    for (const auto index : mesh.indices) if (index >= mesh.vertices.size()) return mesh; // the upload's validation reports it
    double area = 0;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const auto &a = mesh.vertices[mesh.indices[i]], &b = mesh.vertices[mesh.indices[i+1]], &c = mesh.vertices[mesh.indices[i+2]];
        area += 0.5*std::abs(double(b.x-a.x)*(c.z-a.z)-double(c.x-a.x)*(b.z-a.z));
    }
    if (!std::isfinite(area)) return mesh;
    // Bisected triangles average about max_edge^2/6.
    const float limit = std::max(max_edge, float(std::sqrt(area*6/double(std::max<std::size_t>(max_triangles, 1)))));
    Mesh out = mesh;
    std::vector<std::array<std::uint32_t, 3>> triangles(mesh.indices.size()/3), next;
    for (std::size_t i = 0; i < triangles.size(); ++i) triangles[i] = {mesh.indices[3*i], mesh.indices[3*i+1], mesh.indices[3*i+2]};
    std::unordered_map<std::uint64_t, std::uint32_t> midpoints;
    auto length = [&](std::uint32_t a, std::uint32_t b) {
        const auto &p = out.vertices[a], &q = out.vertices[b];
        return std::sqrt((p.x-q.x)*(p.x-q.x)+(p.y-q.y)*(p.y-q.y)+(p.z-q.z)*(p.z-q.z));
    };
    auto key = [](std::uint32_t a, std::uint32_t b) { return std::uint64_t(std::min(a, b)) << 32 | std::max(a, b); };
    auto unit = [](float& x, float& y, float& z) { const float l = std::sqrt(x*x+y*y+z*z); if (l > 0) { x /= l; y /= l; z /= l; } };
    auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
        const auto [it, added] = midpoints.try_emplace(key(a, b), std::uint32_t(out.vertices.size()));
        if (added) {
            const Vertex p = out.vertices[a], q = out.vertices[b];
            Vertex m = p;
            auto mix = [](float u, float v) { return 0.5f*(u+v); };
            m.x = mix(p.x, q.x); m.y = mix(p.y, q.y); m.z = mix(p.z, q.z);
            m.nx = mix(p.nx, q.nx); m.ny = mix(p.ny, q.ny); m.nz = mix(p.nz, q.nz); unit(m.nx, m.ny, m.nz);
            m.u = mix(p.u, q.u); m.v = mix(p.v, q.v);
            for (unsigned i = 0; i < 4; ++i) { m.ambient[i] = mix(p.ambient[i], q.ambient[i]); m.color[i] = mix(p.color[i], q.color[i]); }
            for (unsigned i = 0; i < 3; ++i) m.tangent[i] = mix(p.tangent[i], q.tangent[i]);
            unit(m.tangent[0], m.tangent[1], m.tangent[2]);
            for (unsigned i = 0; i < 2; ++i) m.uv1[i] = mix(p.uv1[i], q.uv1[i]);
            out.vertices.push_back(m);
        }
        return it->second;
    };
    // A triangle splits an edge another triangle already split (keeping the mesh conforming: no cracks once displaced),
    // else its longest edge when that exceeds the limit. Only edges longer than the limit are ever split, so it ends.
    for (bool changed = true; changed;) {
        changed = false; next.clear(); next.reserve(triangles.size()*2);
        for (const auto& t : triangles) {
            int edge = -1; float best = -1;
            for (int k = 0; k < 3; ++k)
                if (midpoints.contains(key(t[k], t[(k+1)%3]))) { const float l = length(t[k], t[(k+1)%3]); if (l > best) { best = l; edge = k; } }
            if (edge < 0) {
                for (int k = 0; k < 3; ++k) { const float l = length(t[k], t[(k+1)%3]); if (l > best) { best = l; edge = k; } }
                if (!(best > limit)) { next.push_back(t); continue; }
            }
            const std::uint32_t a = t[edge], b = t[(edge+1)%3], c = t[(edge+2)%3], m = midpoint(a, b);
            next.push_back({a, m, c}); next.push_back({m, b, c});
            changed = true;
        }
        triangles.swap(next);
    }
    out.indices.clear(); out.indices.reserve(triangles.size()*3);
    for (const auto& t : triangles) out.indices.insert(out.indices.end(), t.begin(), t.end());
    return out;
}
// Meshes drawn with a source_ocean material (renderers' draw material: the instance override, else the mesh's).
inline std::vector<bool> ocean_wave_meshes(const Scene& scene) {
    std::vector<bool> ocean(scene.meshes.size(), false);
    auto is_ocean = [&](std::uint32_t material) { return material < scene.materials.size() && scene.materials[material].source_ocean; };
    if (scene.instances.empty()) for (std::size_t i = 0; i < scene.meshes.size(); ++i) ocean[i] = is_ocean(scene.meshes[i].material);
    for (const auto& instance : scene.instances)
        if (instance.mesh < scene.meshes.size() && is_ocean(instance.material == UINT32_MAX ? scene.meshes[instance.mesh].material : instance.material))
            ocean[instance.mesh] = true;
    return ocean;
}
// The subdivided copy costs GPU time (Sivri Ada 40k -> ~650k ocean triangles), so it draws only while the wind sea exists:
// weather active and wind above 2 m/s, off again below 1.5 m/s (no flapping), and only for ocean placements whose culling
// sphere reaches within kOceanWaveRange of the eye (oceanWindWaves fades every wave out by then). At either switch the wind
// sea is at most ~4 cm high or faded out, so the change is the source swell's finer sampling only.
constexpr float kOceanWaveRange = 1920.f; // 8 x the longest wave: frame.glsl oceanWindWaves' fade end
inline bool ocean_waves_on(const WeatherUniforms& w, bool was_on) { return w.active() && w.wind[2] > (was_on ? 1.5f : 2.f); }
// NONCANONICAL wave 28: the bed under the ocean, so the wind waves die out toward shores, rocks, piers and hulls instead of
// rising through them. A top-down grid (<= 2048^2 cells, >= 2 units) of the highest static non-ocean geometry, stamped like
// effects.cpp's rain surface (cells whose centre a triangle covers, else its vertices' cells).
struct OceanBed {
    float x0 = 0, z0 = 0, cell = 1; std::uint32_t width = 0, depth = 0; std::vector<float> top;
    // Horizontal distance (units) from each cell to the nearest land: bed within 1 unit of the water level or above.
    std::vector<float> shore;
    float shore_at(float x, float z) const {
        if (shore.empty()) return INFINITY;
        const auto ix = std::clamp<std::int64_t>(std::int64_t(std::floor((x - x0) / cell)), 0, std::int64_t(width) - 1);
        const auto iz = std::clamp<std::int64_t>(std::int64_t(std::floor((z - z0) / cell)), 0, std::int64_t(depth) - 1);
        return shore[std::size_t(iz) * width + std::size_t(ix)];
    }
    // Highest bed within one cell around (x, z); -inf where nothing lies under the water.
    float near(float x, float z) const {
        if (top.empty()) return -INFINITY;
        const auto ix = std::int64_t(std::floor((x - x0) / cell)), iz = std::int64_t(std::floor((z - z0) / cell));
        float h = -INFINITY;
        for (std::int64_t dz = -1; dz <= 1; ++dz) for (std::int64_t dx = -1; dx <= 1; ++dx) {
            const auto cx = ix + dx, cz = iz + dz;
            if (cx >= 0 && cz >= 0 && cx < std::int64_t(width) && cz < std::int64_t(depth)) h = std::max(h, top[std::size_t(cz) * width + std::size_t(cx)]);
        }
        return h;
    }
};
inline OceanBed ocean_bed(const Scene& s) {
    OceanBed g;
    const auto ocean_draw = [&](const Instance& in) {
        const auto material = in.material == UINT32_MAX ? s.meshes[in.mesh].material : in.material;
        return in.ocean || (material < s.materials.size() && s.materials[material].source_ocean);
    };
    const auto usable = [&](const Instance& in) {
        return in.entity_id == 0 && !in.occlusion_proxy && !in.sky && in.mesh < s.meshes.size() && s.meshes[in.mesh].influences.empty() &&
               !ocean_draw(in) && scene_source_color_draw(s, in.mesh, in.material, in.source_ordinary_color_participates);
    };
    const auto world = [](const Matrix4& m, const Vertex& v) {
        return std::array<float, 3>{m[0]*v.x + m[4]*v.y + m[8]*v.z + m[12], m[1]*v.x + m[5]*v.y + m[9]*v.z + m[13], m[2]*v.x + m[6]*v.y + m[10]*v.z + m[14]};
    };
    float lo[2]{INFINITY, INFINITY}, hi[2]{-INFINITY, -INFINITY};
    for (const auto& in : s.instances) if (usable(in)) for (const auto& v : s.meshes[in.mesh].vertices) {
        const auto p = world(in.transform.matrix, v);
        if (!std::isfinite(p[0]) || !std::isfinite(p[2])) continue;
        lo[0] = std::min(lo[0], p[0]); lo[1] = std::min(lo[1], p[2]); hi[0] = std::max(hi[0], p[0]); hi[1] = std::max(hi[1], p[2]);
    }
    if (!(lo[0] <= hi[0] && lo[1] <= hi[1])) return g;
    g.cell = std::max(2.f, std::max(hi[0] - lo[0], hi[1] - lo[1]) / 2048); g.x0 = lo[0]; g.z0 = lo[1];
    g.width = std::uint32_t((hi[0] - lo[0]) / g.cell) + 1; g.depth = std::uint32_t((hi[1] - lo[1]) / g.cell) + 1;
    g.top.assign(std::size_t(g.width) * g.depth, -INFINITY);
    const auto cell_of = [&](float v, float origin, std::uint32_t n) { return std::clamp<std::int64_t>(std::int64_t((v - origin) / g.cell), 0, std::int64_t(n) - 1); };
    const auto stamp = [&](std::int64_t ix, std::int64_t iz, float y) { auto& t = g.top[std::size_t(iz) * g.width + std::size_t(ix)]; t = std::max(t, y); };
    for (const auto& in : s.instances) {
        if (!usable(in)) continue;
        const auto& mesh = s.meshes[in.mesh];
        for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            if (mesh.indices[t] >= mesh.vertices.size() || mesh.indices[t+1] >= mesh.vertices.size() || mesh.indices[t+2] >= mesh.vertices.size()) continue;
            const auto a = world(in.transform.matrix, mesh.vertices[mesh.indices[t]]), b = world(in.transform.matrix, mesh.vertices[mesh.indices[t+1]]),
                       c = world(in.transform.matrix, mesh.vertices[mesh.indices[t+2]]);
            if (!std::isfinite(a[0] + a[1] + a[2] + b[0] + b[1] + b[2] + c[0] + c[1] + c[2])) continue;
            const float area = (b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2]);
            bool covered = false;
            if (std::abs(area) > 1e-6f) {
                const auto x0 = cell_of(std::min({a[0], b[0], c[0]}), g.x0, g.width), x1 = cell_of(std::max({a[0], b[0], c[0]}), g.x0, g.width);
                const auto z0 = cell_of(std::min({a[2], b[2], c[2]}), g.z0, g.depth), z1 = cell_of(std::max({a[2], b[2], c[2]}), g.z0, g.depth);
                for (auto iz = z0; iz <= z1; ++iz) for (auto ix = x0; ix <= x1; ++ix) {
                    const float px = g.x0 + (float(ix) + .5f) * g.cell, pz = g.z0 + (float(iz) + .5f) * g.cell;
                    const float u = ((b[0] - px) * (c[2] - pz) - (c[0] - px) * (b[2] - pz)) / area, v = ((c[0] - px) * (a[2] - pz) - (a[0] - px) * (c[2] - pz)) / area, w = 1 - u - v;
                    if (u < 0 || v < 0 || w < 0) continue;
                    stamp(ix, iz, u * a[1] + v * b[1] + w * c[1]); covered = true;
                }
            }
            if (!covered) for (const auto& p : {a, b, c}) stamp(cell_of(p[0], g.x0, g.width), cell_of(p[2], g.z0, g.depth), p[1]);
        }
    }
    // Distance to shore: land cells at the highest ocean placement's level, then a two-pass chamfer distance transform.
    float level = -INFINITY;
    for (const auto& in : s.instances) if (in.mesh < s.meshes.size() && ocean_draw(in)) for (const auto& v : s.meshes[in.mesh].vertices) level = std::max(level, world(in.transform.matrix, v)[1]);
    if (!std::isfinite(level)) return g;
    const float straight = g.cell, diagonal = g.cell * 1.41421356f;
    g.shore.assign(g.top.size(), INFINITY);
    for (std::size_t i = 0; i < g.top.size(); ++i) if (g.top[i] >= level - 1) g.shore[i] = 0;
    const auto relax = [&](std::int64_t x, std::int64_t z, std::int64_t dx, std::int64_t dz, float w) {
        const auto nx = x + dx, nz = z + dz;
        if (nx < 0 || nz < 0 || nx >= std::int64_t(g.width) || nz >= std::int64_t(g.depth)) return;
        auto& d = g.shore[std::size_t(z) * g.width + std::size_t(x)];
        d = std::min(d, g.shore[std::size_t(nz) * g.width + std::size_t(nx)] + w);
    };
    for (std::int64_t z = 0; z < std::int64_t(g.depth); ++z) for (std::int64_t x = 0; x < std::int64_t(g.width); ++x) {
        relax(x, z, -1, 0, straight); relax(x, z, 0, -1, straight); relax(x, z, -1, -1, diagonal); relax(x, z, 1, -1, diagonal);
    }
    for (std::int64_t z = std::int64_t(g.depth) - 1; z >= 0; --z) for (std::int64_t x = std::int64_t(g.width) - 1; x >= 0; --x) {
        relax(x, z, 1, 0, straight); relax(x, z, 0, 1, straight); relax(x, z, 1, 1, diagonal); relax(x, z, -1, 1, diagonal);
    }
    return g;
}
// The subdivided wave copy of ocean mesh `mesh` (first ocean placement's transform) with each vertex's shore factor in
// uv1 = (-1, factor): 0 where the bed reaches the surface or land is near, 1 from kOceanShoreDepth of water and
// kOceanShoreReach from land (frame.glsl sourceOceanPosition scales the wind waves by it, so swells shoal and die before
// the beach instead of rising above it in narrow deep channels; the -1 marks the copy, whose lightmap UVs no water draw
// reads).
constexpr float kOceanShoreDepth = 16.f;  // units (4 m): full wind waves from this depth
constexpr float kOceanShoreReach = 200.f; // units (50 m): ... and this far from land
inline Mesh ocean_wave_mesh(const Scene& s, std::size_t mesh, const OceanBed& bed) {
    Mesh fine = subdivide_ocean_mesh(s.meshes[mesh]);
    const Matrix4* model = nullptr;
    for (const auto& in : s.instances)
        if (in.mesh == mesh) {
            const auto material = in.material == UINT32_MAX ? s.meshes[mesh].material : in.material;
            if (material < s.materials.size() && s.materials[material].source_ocean) { model = &in.transform.matrix; break; }
        }
    static constexpr Matrix4 identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const Matrix4& m = model ? *model : identity;
    for (auto& v : fine.vertices) {
        const float x = m[0]*v.x + m[4]*v.y + m[8]*v.z + m[12], y = m[1]*v.x + m[5]*v.y + m[9]*v.z + m[13], z = m[2]*v.x + m[6]*v.y + m[10]*v.z + m[14];
        const float d = y - bed.near(x, z), f = std::clamp(d / kOceanShoreDepth, 0.f, 1.f), r = std::clamp(bed.shore_at(x, z) / kOceanShoreReach, 0.f, 1.f);
        v.uv1 = {-1.f, (std::isfinite(d) ? f * f * (3 - 2 * f) : 1.f) * r * r * (3 - 2 * r)};
    }
    return fine;
}
using FrustumPlanes = std::array<std::array<float, 4>, 6>;
// Normalized clip planes of a column-major view-projection, GL depth range. For the Vulkan [0,1]-depth projection of the
// same camera this is a superset of the visible volume, so culling stays conservative.
inline FrustumPlanes frustum_planes(const Matrix4& m) {
    FrustumPlanes planes{};
    for (unsigned axis = 0; axis < 3; ++axis)
        for (unsigned side = 0; side < 2; ++side) {
            auto& p = planes[axis * 2 + side];
            for (unsigned c = 0; c < 4; ++c) p[c] = m[c * 4 + 3] + (side ? -1.f : 1.f) * m[c * 4 + axis];
            const float length = std::sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
            if (length > 0) for (auto& v : p) v /= length;
        }
    return planes;
}
// Directional shadow casters: cull only outside the light's X/Y volume. Depth planes stay disabled: near-plane
// casters are depth-clamped and can still shadow visible receivers; far casters are conservatively retained.
inline FrustumPlanes shadow_caster_planes(const Matrix4& light_vp) {
    auto planes = frustum_planes(light_vp);
    planes[4] = planes[5] = {0, 0, 0, 1};
    return planes;
}
// World-space bounding sphere {centre, radius} of an affine model matrix; non-finite bounds stay infinite.
inline std::array<float, 4> world_sphere(const Matrix4& model, const std::array<float, 4>& bounds) {
    if (!std::isfinite(bounds[3])) return {0, 0, 0, INFINITY};
    std::array<float, 4> s{};
    for (unsigned r = 0; r < 3; ++r)
        s[r] = model[r] * bounds[0] + model[4 + r] * bounds[1] + model[8 + r] * bounds[2] + model[12 + r];
    float scale = 0;
    for (unsigned c = 0; c < 3; ++c)
        scale = std::max(scale, model[c*4]*model[c*4] + model[c*4+1]*model[c*4+1] + model[c*4+2]*model[c*4+2]);
    s[3] = bounds[3] * std::sqrt(scale);
    return s;
}
// False only when the transformed bounding sphere lies wholly outside one plane (affine model matrices).
inline bool sphere_visible(const FrustumPlanes& planes, const Matrix4& model, const std::array<float, 4>& bounds) {
    const auto s = world_sphere(model, bounds);
    if (!std::isfinite(s[3])) return true;
    for (const auto& p : planes)
        if (p[0]*s[0] + p[1]*s[1] + p[2]*s[2] + p[3] < -s[3]) return false;
    return true;
}
// RendererOptions::view_distance test: false only when the sphere lies wholly beyond `distance` (0 = unlimited).
inline bool sphere_within(const std::array<float, 3>& eye, const Matrix4& model, const std::array<float, 4>& bounds, float distance) {
    if (distance <= 0) return true;
    const auto s = world_sphere(model, bounds);
    const float d = distance + s[3];
    return !std::isfinite(d) || (s[0]-eye[0])*(s[0]-eye[0]) + (s[1]-eye[1])*(s[1]-eye[1]) + (s[2]-eye[2])*(s[2]-eye[2]) <= d*d;
}
// Plane picking only; client must intersect recovered collision geometry before sending movement.
inline std::optional<std::array<float, 3>> camera_ground_point(const Camera& c, float pixel_x, float pixel_y,
                                                             int width, int height, float ground_y = 0) {
    if (width <= 0 || height <= 0 || !std::isfinite(pixel_x) || !std::isfinite(pixel_y) || !std::isfinite(ground_y)) return {};
    const auto b = camera_basis(c);
    const float scale = std::tan(c.fov * 0.008726646259971648f);
    const float x = (2 * pixel_x / static_cast<float>(width) - 1) * scale * static_cast<float>(width) / static_cast<float>(height);
    const float y = (1 - 2 * pixel_y / static_cast<float>(height)) * scale;
    std::array<float, 3> ray{};
    for (unsigned i = 0; i < 3; ++i) ray[i] = b.forward[i] + x*b.right[i] + y*b.up[i];
    if (std::abs(ray[1]) < 1e-6f) return {};
    const float t = (ground_y-b.eye[1])/ray[1];
    if (t < 0 || !std::isfinite(t)) return {};
    return std::array<float, 3>{b.eye[0]+t*ray[0], ground_y, b.eye[2]+t*ray[2]};
}
struct SourceScrollState {
    std::array<float,4> uv{},velocity{};
    std::uint32_t previous_tick=0,random_state=1;
    bool initialized=false;
};
// ponytail: seed1/native invocation order are independent inputs; original QPC seed/global CRT draw stream is unavailable.
inline const std::array<float,4>& advance_source_scroll(SourceScrollState& state,std::uint32_t now_ms) {
    if(!state.initialized){state.previous_tick=now_ms;state.initialized=true;}
    const auto elapsed=std::uint32_t(now_ms-state.previous_tick);
    for(unsigned i=0;i<4;++i) {
        state.random_state=state.random_state*214013U+2531011U;
        auto random=int((state.random_state>>16)&0x7fffU)%1000;
        auto& velocity=state.velocity[i];
        if(velocity>.5f)velocity-=float(random)*.00005f;
        else {if(velocity>=-.5f)random-=500;velocity+=float(random)*.00005f;}
        state.uv[i]+=velocity*float(elapsed)*.0001f;
    }
    state.previous_tick=now_ms;
    return state.uv;
}
// Original model fog predicate: world-basis corners around the separately recovered center.
// Matrix translation is intentionally ignored; the source center follows reference/parent links.
// Original 0046b780 propagates authored IEEE NaNs into corners; 0047cf50 uses
// an ordered distance>threshold comparison, so unordered corners do not enable fog.
// Preserve these source bounds (including signed extents), never clamp/recompute them.
inline bool source_fog_enabled(const std::array<float,3>& half_extents,const std::array<float,3>& center,
                               const Matrix4& world,const std::array<float,3>& eye,float start) {
    if(!std::isfinite(start))throw std::invalid_argument("non-finite source fog start");
    for(unsigned axis=0;axis<3;++axis) {
        if(!std::isfinite(center[axis])||!std::isfinite(eye[axis]))
            throw std::invalid_argument("non-finite source fog center/camera");
        for(unsigned basis=0;basis<3;++basis)if(!std::isfinite(world[basis*4+axis]))
            throw std::invalid_argument("non-finite source fog basis");
    }
    const float threshold=start*start;
    for(unsigned corner=0;corner<8;++corner) {
        float distance=0;
        for(unsigned axis=0;axis<3;++axis) {
            float coordinate=center[axis]-eye[axis];
            for(unsigned basis=0;basis<3;++basis)
                coordinate+=half_extents[basis]*world[basis*4+axis]*((corner&(1U<<basis))?1.0f:-1.0f);
            distance+=coordinate*coordinate;
        }
        if(distance>threshold)return true;
    }
    return false;
}
// Original particle BlendType 004999e0/render 0049a780: Sum/unknown=0, blend=1, subtract=2, min=3, max=4.
enum class ParticleBlend : std::uint8_t { add, alpha, subtract, min, max };
struct ParticleVertex { float x,y,z,u,v; std::array<float,4> color; }; // Straight RGBA, already clamped to 0..1.
// Six unindexed vertices per billboard. alpha_reference>0 enables the original alpha test (keep alpha > reference).
// after_hdr: original node flag +0x215 (AfterHDR && BlendType != blend), drawn by 0046cbe0 after the fake-HDR glow.
struct ParticleBatch { std::uint32_t texture=0; ParticleBlend blend=ParticleBlend::add; float depth_bias=0,alpha_reference=0; std::uint32_t first=0,count=0; bool after_hdr=false; };
// textures only grows while one EffectSystem lives; renderers upload new indices incrementally.
struct ParticleFrame { std::span<const Texture> textures; std::vector<ParticleVertex> vertices; std::vector<ParticleBatch> batches; };
// Top-down 8-bit RGB rows to .jpg/.jpeg, .tga or .bmp (original screenshot formats, 00468010), anything else binary PPM.
void write_rgb_image(const std::filesystem::path& path,int width,int height,std::span<const std::uint8_t> rgb);
class Renderer {
public:
    virtual ~Renderer() = default;
    // Draws and presents. Scene remains alive until provider destruction or next different Scene: render() with a Scene
    // other than the previous call's (by address) frees the previous one's GPU/RT resources and uploads the new one in full.
    virtual void render(const Scene&, std::span<const RenderEntity>, std::uint32_t self, float dt) = 0;
    virtual void resize(int width, int height) = 0;
    virtual void screenshot(const std::filesystem::path&) = 0;
    virtual void set_camera(const Camera&) = 0;
    virtual const Camera& camera() const = 0;
    // Uploads pixels synchronously; caller may release or mutate Overlay immediately afterwards. `changed` lists the x,y,w,h
    // rectangles that differ from the previous call (non-overlapping); empty = the whole overlay.
    virtual void set_overlay(const Overlay&, std::span<const std::array<int, 4>> changed = {}) = 0;
    // Uploads billboards for the next render(): non-AfterHDR batches after scene alpha (glow input), AfterHDR batches after
    // the glow composite, both before HUD; alpha is fogged like v_particles.vsh. Caller may mutate the frame afterwards.
    virtual void set_particles(const ParticleFrame&) = 0;
    // NONCANONICAL wave 28: footsteps for the next render()'s enhanced weather surfaces (GroundMarkTracker; newest first,
    // at most kMaxGroundMarks used). Caller may mutate them afterwards.
    virtual void set_ground_marks(std::span<const GroundMark>) = 0;
    // Original death view (VOD 34:00): desaturates the 3D scene and particles for following render() calls; HUD stays in colour.
    virtual void set_grayscale(bool) = 0;
    // Vulkan applies upscaler/quality/sharpness/frame generation from the next render() (targets rebuilt on change);
    // OpenGL renders natively.
    virtual void set_upscaling(Upscaler, FsrQuality, float /*sharpness*/, bool /*frame_generation*/) {}
    virtual void set_view_distance(float) = 0; // RendererOptions::view_distance from the next render()
};
// Window lifetime belongs to caller and must exceed renderer lifetime. Factories throw on unsupported providers.
std::unique_ptr<Renderer> make_gl_renderer(SDL_Window*, const RendererOptions& = {});
#if IKV_HAS_VULKAN
std::unique_ptr<Renderer> make_vk_renderer(SDL_Window*, const RendererOptions& = {});
#endif
} // namespace ikv
