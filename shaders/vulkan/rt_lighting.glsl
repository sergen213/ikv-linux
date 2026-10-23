#ifndef IKV_RT_LIGHTING_GLSL
#define IKV_RT_LIGHTING_GLSL
#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require
#include "frame.glsl"

// Include after #version 460, before shader declarations. No ray-tracing pipeline
// or shader binding table is used: every visibility/hit test below is a ray query.
#ifndef IKV_RT_SHADOWS
#define IKV_RT_SHADOWS 1
#endif
#ifndef IKV_RT_REFLECTIONS
#define IKV_RT_REFLECTIONS 1
#endif
#ifndef IKV_RT_GI
#define IKV_RT_GI 1
#endif
#ifndef IKV_RT_MAX_DISTANCE
#define IKV_RT_MAX_DISTANCE 100000.0
#endif

layout(set = 1, binding = 0) uniform accelerationStructureEXT sceneAS;
struct RtTriangle { vec4 a; vec4 b; vec4 c; vec4 color; };
layout(std430, set = 1, binding = 1) readonly buffer RtTriangles {
    RtTriangle rtTriangles[];
};
// Diffuse textures of alpha-tested materials (count: vulkan_rt.hpp rt_alpha_texture_slots).
layout(set = 1, binding = 2) uniform sampler2D rtAlphaTextures[64];

// Approximation, not original-game appearance: one-bounce stochastic diffuse
// probes and glossy reflections, analytic sky/sun, geometric hit normals and
// recovered material COLOR only. No hit texture/lightmap/emission sampling; only
// the source alpha test of alpha-tested materials (rtAlphaOpaque) is applied.
// ponytail: ocean AS triangles stay undisplaced; animated water visibility/hits
// are approximate until moving vertices are supplied to BLAS updates.
// The primary Sun visibility ray is the deterministic solar-centre ray: it needs no history, so a moving caster
// never leaves a held stale shadow. Indirect diffuse and glossy reflection are stochastic; per-pixel
// Cranley-Patterson-rotated R2 sequences (integer fixed point, no sin hash) decorrelate pixels and stratify each
// pixel over frames, and the guided resolve (temporal.frag) denoises only those two terms. Ray origins/offsets
// use the physical facet normal, hemisphere clipping the smooth vertex normal; BRDF shading uses the authored
// (mapped) normal.
// quality selects 1-4 samples PER enabled indirect effect. sunDir points from surface to sun; viewDir points
// from surface to eye. The result replaces raster sun/ambient lighting rather than adding it twice.

uint rtHash(uint value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}
// Point n of a per-pixel rotated R2 sequence in [0,1)^2; the rotation is a hash of pixel and effect.
vec2 rtSequence(uvec2 pixel, uint effect, uint n) {
    uint h = rtHash(pixel.x * 0x8da6b343u ^ pixel.y * 0xd8163841u ^ effect * 0xcb1ab31fu);
    uvec2 rotation = uvec2(h, rtHash(h));
    uvec2 point = rotation + n * uvec2(3242174889u, 2447445413u); // 2^32 * (0.7548776662, 0.5698402910)
    return vec2(point >> 8u) * (1.0 / 16777216.0);
}
vec3 rtUnit(vec3 value) {
    float squared = dot(value, value);
    return squared > 1e-20 ? value * inversesqrt(squared) : vec3(0.0, 1.0, 0.0);
}
float rtBias(vec3 point) { return max(0.002, length(point) * 0.000002); }
vec3 rtSky(vec3 direction) {
    return enhancedAmbientScale() * mix(vec3(0.035, 0.04, 0.05), vec3(0.42, 0.58, 0.85),
               smoothstep(-0.2, 0.8, direction.y));
}
vec3 rtSkyIrradiance(vec3 normal) {
    // Analytic hemispherical environment estimate, not a traced second bounce.
    // Noon values; the clock record scales sky and sun (frame.glsl, NONCANONICAL wave 19).
    return enhancedAmbientScale() * mix(vec3(0.08, 0.09, 0.11), vec3(0.32, 0.43, 0.62),
               clamp(normal.y * 0.5 + 0.5, 0.0, 1.0));
}
vec3 rtSunRadiance() { return vec3(2.8, 2.55, 2.2) * enhancedSunScale(); }
vec3 rtHemisphere(vec3 normal, vec2 u) {
    float phi = 6.28318530718 * u.x;
    float radius = sqrt(u.y);
    vec3 tangent = rtUnit(cross(abs(normal.y) < 0.99 ? vec3(0,1,0) : vec3(1,0,0), normal));
    vec3 bitangent = cross(normal, tangent);
    return tangent * (radius * cos(phi)) + bitangent * (radius * sin(phi))
         + normal * sqrt(max(1.0 - u.y, 0.0));
}
// Normal-mapped samples below the smooth surface would start inside it: mirror them across the smooth vertex normal's
// plane, which is continuous across triangles (mirroring across the facet made every triangle sample differently).
vec3 rtAboveSurface(vec3 direction, vec3 smoothNormal) {
    float below = dot(direction, smoothNormal);
    return below < 1e-4 ? rtUnit(direction - smoothNormal * (2.0 * below - 1e-3)) : direction;
}
// Source alpha test (scene.cpp cutoff 79/255) of a candidate hit on an alpha-tested placement (FORCE_NO_OPAQUE, its
// rtAlphaTextures slot + 1 in the SBT record offset; records carry half2 UVs in a/b/c.w: vulkan_rt.cpp alpha_test).
// Without it a ground decal's whole quad occluded the sun and every probe of the ground under it (black rectangles).
bool rtAlphaOpaque(uint index, uint slot, vec2 barycentrics) {
    RtTriangle triangle = rtTriangles[index];
    vec2 uv = unpackHalf2x16(floatBitsToUint(triangle.a.w)) * (1.0 - barycentrics.x - barycentrics.y)
            + unpackHalf2x16(floatBitsToUint(triangle.b.w)) * barycentrics.x
            + unpackHalf2x16(floatBitsToUint(triangle.c.w)) * barycentrics.y;
    // Mip 3: rays have no footprint; the coarse mip cut the alpha test's cost by ~0.5 ms per 1080p frame.
    return textureLod(rtAlphaTextures[nonuniformEXT(slot - 1u)], uv, 3.0).a > 79.0 / 255.0;
}
// Low-poly terminator: a direction above the smooth surface may still point below the physical facet (facet: triangle
// normal on the lit side). Such a ray starts just under the facet and skips back faces: inside a closed mesh only back
// faces are visible, so it leaves its own hull and only front faces beyond it occlude it, as for the smooth surface.
// Instances flip ray facing (vulkan_rt.cpp) so the source's raster front faces are ray front faces.
#define rtTrace(query, position, facet, direction, flags, mask) { \
    float bias = rtBias(position); \
    bool below = dot(direction, facet) < 0.0; \
    rayQueryInitializeEXT(query, sceneAS, \
        (flags) | (below ? gl_RayFlagsCullBackFacingTrianglesEXT : 0u), \
        (mask), position + facet * (below ? -bias : bias), bias, direction, IKV_RT_MAX_DISTANCE); \
    while (rayQueryProceedEXT(query)) \
        if (rtAlphaOpaque(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false) \
                          + rayQueryGetIntersectionPrimitiveIndexEXT(query, false), \
                          rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(query, false), \
                          rayQueryGetIntersectionBarycentricsEXT(query, false))) \
            rayQueryConfirmIntersectionEXT(query); }
// mask: TLAS instance mask (vulkan_rt.cpp: 1 static scenery, 2 actors). flags: extra ray flags.
float rtSunVisibilityMasked(vec3 position, vec3 facet, vec3 sunDir, uint mask, uint flags) {
    rayQueryEXT query;
    rtTrace(query, position, facet, sunDir, gl_RayFlagsTerminateOnFirstHitEXT | flags, mask);
    return rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}
float rtSunVisibility(vec3 position, vec3 facet, vec3 sunDir, uint flags) {
#if IKV_RT_SHADOWS
    return rtSunVisibilityMasked(position, facet, sunDir, 0xffu, flags);
#else
    return 1.0;
#endif
}
vec3 rtIncoming(vec3 position, vec3 facet, vec3 direction, vec3 sunDir) {
    rayQueryEXT query;
    rtTrace(query, position, facet, direction, 0u, 0xffu);
    if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return rtSky(direction);
    uint index = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true)
               + rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
    RtTriangle triangle = rtTriangles[index];
    mat4x3 objectToWorld = rayQueryGetIntersectionObjectToWorldEXT(query, true);
    vec3 a = objectToWorld * vec4(triangle.a.xyz, 1.0);
    vec3 b = objectToWorld * vec4(triangle.b.xyz, 1.0);
    vec3 c = objectToWorld * vec4(triangle.c.xyz, 1.0);
    vec3 hitNormal = rtUnit(cross(b - a, c - a));
    if (dot(hitNormal, direction) > 0.0) hitNormal = -hitNormal;
    vec2 barycentrics = rayQueryGetIntersectionBarycentricsEXT(query, true);
    vec3 hit = a * (1.0 - barycentrics.x - barycentrics.y)
             + b * barycentrics.x + c * barycentrics.y;
    float cosine = max(dot(hitNormal, sunDir), 0.0);
    // Second-order term: alpha-tested placements stay opaque quads here (their alpha test cost ~1 ms per frame).
    float visibility = cosine > 0.0 ? rtSunVisibility(hit, hitNormal, sunDir, gl_RayFlagsOpaqueEXT) : 0.0;
    return max(triangle.color.rgb, vec3(0.0))
         * (rtSkyIrradiance(hitNormal) + rtSunRadiance() * (cosine * visibility))
         * (1.0 / 3.14159265359);
}
// Linear lighting split for the guided resolve: radiance = albedo * (sun + diffuse) + reflection.
// sun: deterministic direct gain; diffuse: stochastic indirect gain; reflection: stochastic additive radiance.
struct RtLight { vec3 sun; vec3 diffuse; vec3 reflection; };
// facet: physical triangle normal facing the eye; smoothNormal: interpolated vertex normal; normal: authored
// (normal-mapped) shading normal.
RtLight rtLighting(vec3 position, vec3 facet, vec3 smoothNormal, vec3 normal, vec3 viewDir, float roughness,
                   vec3 sunDir, uvec2 pixel, uint frame, float quality) {
    facet = rtUnit(facet);
    smoothNormal = rtUnit(dot(smoothNormal, facet) < 0.0 ? -smoothNormal : smoothNormal);
    normal = rtUnit(normal);
    viewDir = rtUnit(viewDir);
    sunDir = rtUnit(sunDir);
    roughness = clamp(roughness, 0.04, 1.0);
    RtLight light = RtLight(vec3(0.0), vec3(0.0), vec3(0.0));
    float cosine = max(dot(normal, sunDir), 0.0);
    // Behind the physical facet AND the smooth vertex normal (flat or back-lit surface): self-shadowed whatever the
    // mapped normal says. Behind the facet only: low-poly terminator, traced by rtSunVisibility.
    float visibility = dot(facet, sunDir) > 0.0 || dot(smoothNormal, sunDir) > 0.0 ? 1.0 : 0.0;
#if IKV_RT_SHADOWS
    if (cosine > 0.0 && visibility > 0.0)
        visibility = rtSunVisibility(position, facet, sunDir, 0u);
#endif
    light.sun = rtSunRadiance() * (cosine * visibility * 0.96 / 3.14159265359);
    int samples = int(clamp(ceil(quality), 1.0, 4.0));
#if IKV_RT_GI
    for (int sampleIndex = 0; sampleIndex < samples; ++sampleIndex) {
        vec3 direction = rtAboveSurface(rtHemisphere(normal, rtSequence(pixel, 1u, frame * uint(samples) + uint(sampleIndex))), smoothNormal);
        light.diffuse += rtIncoming(position, facet, direction, sunDir);
    }
    // Cosine-weighted estimator: cosine / PDF cancels pi for Lambertian BRDF.
    light.diffuse *= 0.96 / float(samples);
#else
    light.diffuse = rtSkyIrradiance(normal) * (0.96 / 3.14159265359);
#endif
#if IKV_RT_REFLECTIONS
    // GGX importance-sampled half vectors, Schlick Fresnel, Smith masking.
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float nDotV = max(dot(normal, viewDir), 0.0001);
    vec3 tangent = rtUnit(cross(abs(normal.y) < 0.99 ? vec3(0,1,0) : vec3(1,0,0), normal));
    vec3 bitangent = cross(normal, tangent);
    for (int sampleIndex = 0; sampleIndex < samples; ++sampleIndex) {
        vec2 u = rtSequence(pixel, 2u, frame * uint(samples) + uint(sampleIndex));
        float phi = 6.28318530718 * u.x;
        float cosTheta = sqrt((1.0 - u.y) / (1.0 + (alphaSquared - 1.0) * u.y));
        float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
        vec3 halfVector = rtUnit(tangent * (cos(phi) * sinTheta)
                              + bitangent * (sin(phi) * sinTheta) + normal * cosTheta);
        float vDotH = max(dot(viewDir, halfVector), 0.0);
        vec3 direction = reflect(-viewDir, halfVector);
        float nDotL = max(dot(normal, direction), 0.0);
        if (vDotH <= 0.0 || nDotL <= 0.0) continue;
        float nDotH = max(dot(normal, halfVector), 0.0001);
        float gV = 2.0 * nDotV / (nDotV + sqrt(alphaSquared + (1.0 - alphaSquared) * nDotV * nDotV));
        float gL = 2.0 * nDotL / (nDotL + sqrt(alphaSquared + (1.0 - alphaSquared) * nDotL * nDotL));
        float fresnel = 0.04 + 0.96 * pow(1.0 - vDotH, 5.0);
        // The estimator's mean (directional albedo) is at most one; grazing mapped normals made single
        // samples hundreds of times brighter (white actor sparkles), so each sample weight is capped at one.
        float weight = min(fresnel * gV * gL * vDotH / (nDotV * nDotH), 1.0);
        light.reflection += rtIncoming(position, facet, rtAboveSurface(direction, smoothNormal), sunDir) * weight;
    }
    light.reflection /= float(samples);
#endif
    return light;
}
#endif
