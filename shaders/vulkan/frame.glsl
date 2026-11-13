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
    vec4 groundMarks[64]; // x,z world, y ground height, w packed age/heading/foot (weather.glsl weatherStep)
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
// NONCANONICAL wave 28 (enhanced weather; renderer_gl.cpp IKV_GL_OCEAN_WIND keeps a copy): the wind sea over the source
// swell, displacing the ocean (the renderers upload it subdivided to <= 32-unit edges, renderer.hpp subdivide_ocean_mesh)
// and shading it (water.glsl). Three peaked crests (40, 26 and 19 m) run downwind, their height rising with the square of
// the wind (the longest about 1 m crest to trough in a 22 m/s gusting gale, ~20 cm at 10 m/s, none in still air). They
// travel with the wind phase (wind.w, wrapping at 1024 m: 8, 11 and 14 crests per wrap, so no jump) and fade with eye
// distance. waves: how many (the vertices take the two the subdivided mesh resolves, the shading all three). Returns
// height (units) and its world x/z gradient; 0 inactive.
vec3 oceanWindSwell(vec2 xz,vec3 eye,vec4 wind,vec4 misc,int waves){
    if(misc.w<=.5||wind.z<=0.)return vec3(0);
    float s=clamp(wind.z/25.,0.,1.),a=4.5*s*s*(1.+.5*misc.y),d=distance(eye.xz,xz);
    vec2 w=normalize(wind.xy+vec2(1e-6,0));
    vec3 r=vec3(0);
    for(int i=0;i<waves;++i){
        float k=float(i),lambda=160./(1.+.55*k),turn=k==1.?.35:k==2.?-.45:0.;
        vec2 dir=mat2(cos(turn),sin(turn),-sin(turn),cos(turn))*w;
        float amp=a*(k==0.?1.:k==1.?.55:.35)*(1.-smoothstep(5.*lambda,12.*lambda,d));
        float x=6.28318530718*(dot(xz,dir)/lambda-wind.w*(8.+3.*k)/1024.);
        float e=exp(sin(x)-1.); // afl_ext's peaked crest shape (water.glsl); .47 its mean, so the sea level stays
        r+=amp*vec3(e-.47,e*cos(x)*(6.28318530718/lambda)*dir);
    }
    return r;
}
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
    // NONCANONICAL wave 27/28 (enhanced weather): the swell eases to x0.6 in still air and grows to ~x2.8 in a gusting gale;
    // the wind sea (oceanWindSwell, its two longest crests) rises over it, everywhere (it fades with eye distance).
    if(abs(frame.eye.y-world.y)<100)world.y+=displacement*.5*(weatherOn(frame.weatherMisc)?.6+2.*weatherWindStrength(frame.wind,frame.weatherMisc):1.);
    if(weatherOn(frame.weatherMisc))world.y+=oceanWindSwell(world.xz,frame.eye.xyz,frame.wind,frame.weatherMisc,2).x;
    return world;
}
#endif
