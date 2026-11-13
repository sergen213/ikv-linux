#ifndef IKV_FRAME_GLSL
#define IKV_FRAME_GLSL
layout(set=0,binding=0,std140) uniform Frame {
    mat4 vp;
    mat4 lightVP;
    vec4 eye;
    vec4 sun;
    vec4 params;
    vec4 viewport;
    vec4 ambient;
    vec4 diffuse;
    vec4 fogColor;
    vec4 fog;
    vec4 skyB;
    vec4 skyC;
    mat4 previousVP;
    // xyz: unit camera basis; w: horizontal/vertical tan(half FOV).
    vec4 cameraRight;
    vec4 cameraUp;
    vec4 cameraForward;
    vec4 previousEye;
    vec4 previousForward;
    // Original C38.xy: native low-32 millisecond tick and seconds. z: death-view grayscale; w: fullscreen (gamma ramp).
    vec4 sourceTime;
    // xy: this frame's sub-pixel projection jitter in NDC (vp includes it); zw: the previous frame's (previousVP).
    vec4 jitter;
    mat4 farLightVP;
    // x: near eye-distance limit.
    vec4 shadowParams;
    // NONCANONICAL wave 27 weather (renderer.hpp weather_uniforms; all zero while inactive, original appearance or roofed):
    vec4 wind; // xy unit dir (world X/Z), z speed m/s, w accumulated wind phase in metres (renderer integrates speed*dt)
    vec4 weather; // x precipitation, y cloud cover, z wetness, w snow cover
    vec4 weatherMisc; // x fog density, y gust, z season index 0..3, w kind index+1 (0 = inactive)
    // NONCANONICAL wave 28 footsteps (renderer.hpp ground_mark_uniforms; count 0 while inactive = the pre-wave paths):
    vec4 groundMarkInfo; // x mark count (<= 64)
    vec4 groundMarkBounds; // xz min, xz max (units) of every mark's reach
    vec4 groundMarks[64]; // x,z world, y ground height, w packed age/heading/foot (weather.glsl weatherSteps)
} frame;
#include "weather.glsl"
// Original fullscreen gamma ramp 0x467030(1,1,1.2) -> SetGammaRamp @0x46711f: ramp[i] = pow(i/255,1/1.2)*1.005, clamped.
// Applied once to display values on every present path (scene in post.frag, HUD in overlay.frag), fullscreen only.
vec3 sourceGammaRamp(vec3 c){return frame.sourceTime.w>.5?min(pow(max(c,vec3(0)),vec3(1./1.2))*1.005,vec3(1)):c;}
// NONCANONICAL wave 19: enhanced and RT physical light follows the server clock record (004b6970 multipliers_2 sun,
// multipliers_1 ambient) relative to the noon record (config clock 8..18 h = the 0041c9a0 Eminönü zone-load constants),
// linearised like the authored colours: night has no full-strength sun or sun shadows. Without source values: 1.
// NONCANONICAL wave 27: the weather tints and dims the enhanced sun and sky light (weather.glsl weatherSunTint/AmbientTint).
vec3 enhancedSunScale(){return (frame.sun.w>.5?pow(max(frame.diffuse.rgb*(255./vec3(126,105,110)),vec3(0)),vec3(2.2)):vec3(1))*weatherSunTint(frame.weather,frame.weatherMisc);}
vec3 enhancedAmbientScale(){return (frame.sun.w>.5?pow(max(frame.ambient.rgb*(255./vec3(200,157,142)),vec3(0)),vec3(2.2)):vec3(1))*weatherAmbientTint(frame.weather,frame.weatherMisc,frame.sourceTime.y);}
// NONCANONICAL wave 27: share of sun shadow washed out by cloud/fog light (callers with a local `frame` use this).
float frameShadowSoftness(){return weatherShadowSoftness(frame.weather,frame.weatherMisc);}
// frame.ambient.w: 1 in a roofed zone (SceneLighting::roofed): enhanced keeps the original baked light, no shadow-mapped sun.
bool sceneRoofed(){return frame.ambient.w>.5;}
// Independently expressed v_ocean.vsh EXPP fraction and authored polynomial.
vec4 sourceOceanPosition(vec4 world) {
    float phase=fract((world.x/32+world.y/13+frame.sourceTime.y)*.15915494-.25);
    vec3 q=vec3(0,.5,1)-phase;
    q*=q;
    vec3 wave=vec3(24.980804,-24.980804,24.980804)*q+
        vec3(-60.145809,60.145809,-60.145809);
    wave=wave*q+vec3(85.453789,-85.453789,85.453789);
    wave=wave*q+vec3(-64.939354,64.939354,-64.939354);
    wave=wave*q+vec3(19.739208,-19.739208,19.739208);
    wave=wave*q+vec3(-1,1,-1);
    float first=phase<.25?1:0,third=phase>=.75?1:0;
    float displacement=-dot(wave,vec3(first,1-first-third,third));
    // NONCANONICAL wave 27 (enhanced weather): the swell grows with the wind, up to x2.5 in a gale.
    if(abs(frame.eye.y-world.y)<100)world.y+=displacement*.5*(weatherOn(frame.weatherMisc)?1.+1.5*weatherWindStrength(frame.wind,frame.weatherMisc):1.);
    return world;
}
#endif
