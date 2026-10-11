#ifndef IKV_RT_GUIDE_GLSL
#define IKV_RT_GUIDE_GLSL
// Guided-RT surface guide (scene location 1, RGBA32F): xy octahedral interpolated vertex normal (normal-map detail
// would stop the spatial filter on every bumpy texel), z packed 8-bit artwork colour + 1 (0: no RT decomposition,
// e.g. sky/ocean/untextured translucent), w signed view depth (<0: never history).
vec2 rtGuideEncodeNormal(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 e = n.xy;
    if (n.z < 0.0) e = (1.0 - abs(n.yx)) * vec2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
    return e;
}
vec3 rtGuideDecodeNormal(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}
// Integers below 2^24 are exact in float32.
float rtGuidePackArtwork(vec3 c) {
    vec3 q = floor(clamp(c, 0.0, 1.0) * 255.0 + 0.5);
    return 1.0 + q.r * 65536.0 + q.g * 256.0 + q.b;
}
// Linear albedo, matching lighting.glsl's pow(texel, 2.2).
vec3 rtGuideAlbedo(float packed) {
    float v = packed - 1.0;
    vec3 q = vec3(floor(v / 65536.0), mod(floor(v / 256.0), 256.0), mod(v, 256.0));
    return pow(q / 255.0, vec3(2.2));
}
#endif
