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
// Wind phase after one frame: integrated (continuous when the speed changes), never speed*time.
inline double advance_wind_phase(double phase, const SceneLighting& l, bool original_appearance, float dt) {
    if (!l.weather.active || original_appearance || l.roofed || !std::isfinite(dt) || dt <= 0) return phase;
    const float speed = std::isfinite(l.weather.wind_speed) ? std::clamp(l.weather.wind_speed, 0.f, 25.f) : 0.f;
    return std::fmod(phase + double(speed) * std::min(dt, 1.f), kWeatherPhasePeriod);
}
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
