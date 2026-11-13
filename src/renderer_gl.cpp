#include "ikv/renderer.hpp"
#include "ikv_gl_water.h" // generated from shaders/vulkan/water.glsl (CMakeLists.txt): #define IKV_GL_WATER
#include "ikv_gl_weather.h" // generated from shaders/vulkan/weather.glsl: #define IKV_GL_WEATHER (NONCANONICAL wave 27)
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengl_glext.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

namespace ikv {
namespace {
// Independently written GLSL. Original shader evidence is documented in GRAPHICS_ROADMAP.
// NONCANONICAL wave 28 (enhanced weather): the wind sea, a copy of Vulkan frame.glsl oceanWindSwell (keep identical):
// height (units) and world x/z gradient of up to three downwind crests travelling with the wind phase; zero while
// inactive. The scene vertex shader displaces the (subdivided) ocean by the two longest, water.glsl shades all three.
#define IKV_GL_OCEAN_WIND R"GLSL(
vec3 oceanWindSwell(vec2 xz,vec3 eye,vec4 wind,vec4 misc,int waves){
    if(misc.w<=0.5||wind.z<=0.0)return vec3(0.0);
    float s=clamp(wind.z/25.0,0.0,1.0),a=4.5*s*s*(1.0+0.5*misc.y),d=distance(eye.xz,xz);
    vec2 w=normalize(wind.xy+vec2(1e-6,0.0));
    vec3 r=vec3(0.0);
    for(int i=0;i<waves;++i){
        float k=float(i),lambda=160.0/(1.0+0.55*k),turn=k==1.0?0.35:k==2.0?-0.45:0.0;
        vec2 dir=mat2(cos(turn),sin(turn),-sin(turn),cos(turn))*w;
        float amp=a*(k==0.0?1.0:k==1.0?0.55:0.35)*(1.0-smoothstep(5.0*lambda,12.0*lambda,d));
        float x=6.28318530718*(dot(xz,dir)/lambda-wind.w*(8.0+3.0*k)/1024.0);
        float e=exp(sin(x)-1.0);
        r+=amp*vec3(e-0.47,e*cos(x)*(6.28318530718/lambda)*dir);
    }
    return r;
}
)GLSL"
// Independently expressed v_ocean.vsh EXPP fraction and authored polynomial (Vulkan frame.glsl sourceOceanPosition):
// scene vertex shader; needs uniforms camera_position and source_time.
#define IKV_GL_SOURCE_OCEAN R"GLSL(
vec4 source_ocean_position(vec4 world_position) {
    float phase=fract((world_position.x/32.0+world_position.y/13.0+source_time.y)*0.15915494-0.25);
    vec3 squared=vec3(0.0,0.5,1.0)-phase;
    squared*=squared;
    vec3 polynomial=vec3(24.980804,-24.980804,24.980804)*squared+vec3(-60.145809,60.145809,-60.145809);
    polynomial=polynomial*squared+vec3(85.453789,-85.453789,85.453789);
    polynomial=polynomial*squared+vec3(-64.939354,64.939354,-64.939354);
    polynomial=polynomial*squared+vec3(19.739208,-19.739208,19.739208);
    polynomial=polynomial*squared+vec3(-1.0,1.0,-1.0);
    float first=phase<0.25?1.0:0.0,third=phase>=0.75?1.0:0.0;
    float wave=-dot(polynomial,vec3(first,1.0-first-third,third));
    // NONCANONICAL wave 27/28 (enhanced weather, Vulkan frame.glsl): the swell eases to x0.6 in still air and grows to
    // ~x2.8 in a gusting gale; the wind sea (oceanWindSwell, IKV_GL_OCEAN_WIND, two longest crests) rises over it.
    if(abs(camera_position.y-world_position.y)<100.0)world_position.y+=wave*0.5*(weatherOn(weather_misc)?0.6+2.0*weatherWindStrength(weather_wind,weather_misc):1.0);
    if(weatherOn(weather_misc))world_position.y+=oceanWindSwell(world_position.xz,camera_position,weather_wind,weather_misc,2).x;
    return world_position;
}
)GLSL"
constexpr const char* scene_vertex = R"GLSL(#version 330 core
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 uv;
layout(location=3) in vec4 ambient;
layout(location=4) in vec3 tangent;
layout(location=5) in vec2 uv_lightmap;
layout(location=6) in vec4 diffuse_color;
uniform mat4 view_projection;
uniform mat4 object_matrix;
uniform bool source_lighting;
uniform vec3 sun_direction;
uniform vec3 ambient_multiplier;
uniform vec3 diffuse_multiplier;
uniform bool fog_enabled;
uniform float fog_start;
uniform float fog_end;
uniform vec3 camera_position;
uniform bool has_normal_map;
uniform bool sky;
uniform bool original_appearance;
uniform bool has_environment_map;
uniform bool source_ocean;
uniform vec4 source_time;
uniform vec4 source_scroll;
uniform mat4 light_view_projection;
uniform mat4 far_light_view_projection;
uniform mat3 normal_matrix; // transpose(inverse(mat3(object_matrix))), per draw on the CPU (set for enhanced non-sky draws)
out vec3 fragment_position;
out vec4 shadow_position;
out vec4 far_shadow_position;
out vec2 texcoord;
out vec3 world_normal;
out vec3 world_tangent;
out vec3 source_vertex_light;
out float fog_blend;
out vec2 environment_uv;
out vec2 lightmap_uv;
out vec3 source_tangent_sun;
out vec3 source_tangent_eye;
out vec3 source_ambient_light; // source vertex light without the directional term (enhanced ambient, wave 19)
out float sky_scatter;
out float fade_blend;
// NONCANONICAL wave 27 weather (renderer.hpp weather_uniforms; zero = the pre-wave paths) and this draw's wind sway vector.
uniform vec4 weather_wind;
uniform vec4 weather_misc;
uniform vec4 weather_sway;
)GLSL" IKV_GL_WEATHER IKV_GL_OCEAN_WIND IKV_GL_SOURCE_OCEAN R"GLSL(
void main() {
    if(source_ocean) {
        vec4 world_position=object_matrix*vec4(position,1.0);
        world_position=source_ocean_position(world_position);
        gl_Position=view_projection*world_position;
        fragment_position=world_position.xyz;shadow_position=light_view_projection*world_position;far_shadow_position=far_light_view_projection*world_position; // enhanced water_surface
        texcoord=uv+source_scroll.xy;
        lightmap_uv=uv+source_scroll.zw;
        source_tangent_eye=normalize(camera_position-world_position.xyz);
        source_vertex_light=source_ambient_light=ambient_multiplier;
        world_normal=normal;world_tangent=tangent;
        source_tangent_sun=vec3(0.0);environment_uv=vec2(0.0);
        sky_scatter=0.0;fade_blend=1.0;
        float source_clip_z=(gl_Position.z+gl_Position.w)*0.5;
        fog_blend=fog_enabled?(fog_end-source_clip_z)/(fog_end-fog_start):1.0;
        return;
    }
    vec4 world_position=object_matrix*vec4(position,1.0);
    if(!sky)world_position.xyz+=weatherSway(position.y,world_position.xyz,weather_sway,weather_wind,weather_misc,source_time.y); // wave 27
    gl_Position=view_projection*world_position;
    fragment_position=world_position.xyz;shadow_position=light_view_projection*world_position;far_shadow_position=far_light_view_projection*world_position;
    if(!original_appearance&&!sky) {
        // Normal-offset receiver: push the lookup up to 2 shadow texels out along the normal as the surface turns from the
        // sun, so curved actor skin stops self-shadowing against its own neighbouring shadow-map texels.
        vec3 n=normal_matrix*normal;float nl=length(n);
        float texel=2.0/(2048.0*max(length(vec3(light_view_projection[0][0],light_view_projection[1][0],light_view_projection[2][0])),1e-6));
        float far_texel=2.0/(2048.0*max(length(vec3(far_light_view_projection[0][0],far_light_view_projection[1][0],far_light_view_projection[2][0])),1e-6));
        if(nl>0.00001){n/=nl;shadow_position+=light_view_projection*vec4(n*texel*2.0*(1.0-max(dot(n,normalize(sun_direction)),0.0)),0.0);
            far_shadow_position+=far_light_view_projection*vec4(n*far_texel*2.0*(1.0-max(dot(n,normalize(sun_direction)),0.0)),0.0);}
    }
    texcoord=uv;
    world_normal=normal;world_tangent=tangent;
    if(!original_appearance&&!sky) {
        world_normal=normal_matrix*normal;
        world_tangent=mat3(object_matrix)*tangent;
    }
    lightmap_uv=uv_lightmap;
    fade_blend=diffuse_color.a;
    vec3 eye_direction=vec3(0.0);environment_uv=vec2(0.0);
    if(sky||has_normal_map||has_environment_map) {
        eye_direction=camera_position-world_position.xyz;
        if(length(eye_direction)>0.00001)eye_direction=normalize(eye_direction);
    }
    // Original terms are also evaluated in enhanced: its ambient is the original light without the sun (wave 19).
    if(has_environment_map&&!sky) {
        vec3 local_normal=normal;
        if(has_normal_map&&length(local_normal)>0.00001)local_normal=normalize(local_normal);
        environment_uv=(eye_direction-2.0*local_normal*dot(local_normal,eye_direction)).xy;
    }
    source_tangent_sun=vec3(0.0);source_tangent_eye=vec3(0.0);
    if(has_normal_map&&!sky&&length(normal)>0.00001&&length(tangent)>0.00001) {
        vec3 n=mat3(object_matrix)*normalize(normal);
        vec3 t=mat3(object_matrix)*normalize(tangent);
        vec3 b=cross(n,t);t=cross(b,n);
        source_tangent_sun=vec3(dot(t,-sun_direction),dot(b,-sun_direction),dot(n,-sun_direction));
        vec3 source_eye=vec3(dot(t,eye_direction),dot(b,eye_direction),dot(n,eye_direction));
        if(length(source_eye)>0.00001)source_tangent_eye=normalize(source_eye);
    }
    source_vertex_light=source_ambient_light=ambient.rgb;
    if(source_lighting&&!sky&&(!original_appearance||!has_normal_map)) {
        float directional=clamp(dot(mat3(object_matrix)*normal,sun_direction),0.0,1.0);
        // D3D vs_1_1 saturates oD0 before the pixel shader consumes it.
        source_vertex_light=clamp(ambient.rgb*ambient_multiplier+directional*diffuse_multiplier,0.0,1.0);
        source_ambient_light=clamp(ambient.rgb*ambient_multiplier,0.0,1.0);
    }
    sky_scatter=0.0;
    if(sky) {
        float mu=dot(eye_direction,-sun_direction);
        float phase=0.039999995*0.995/(1.64-1.6*mu);
        sky_scatter=(0.001*(1.0+mu*mu)+phase)
            *(1.0-pow(2.7182817,-0.001)*pow(2.7182817,-0.995))/0.99599999;
        source_vertex_light=0.80000001*sqrt(max(ambient_multiplier,vec3(0.0)));
    }
    // Original fog consumes D3D homogeneous clip Z, not NDC Z or eye distance.
    float source_clip_z=(gl_Position.z+gl_Position.w)*0.5;
    fog_blend=fog_enabled?clamp((fog_end-source_clip_z)/(fog_end-fog_start),0.0,1.0):1.0;
}
)GLSL";
constexpr const char* scene_fragment = R"GLSL(#version 330 core
in vec2 texcoord;
in vec3 world_normal;
in vec3 world_tangent;
uniform sampler2D diffuse_map;
uniform sampler2D normal_map;
uniform bool has_normal_map;
uniform sampler2D environment_map;
uniform sampler2D lightmap_map;
uniform bool has_environment_map;
uniform bool has_lightmap;
uniform sampler2D fade_map;
uniform bool has_fade_map;
in float fade_blend;
uniform bool source_lighting;
uniform vec3 sky_b;
uniform vec3 sky_c;
uniform float lightmap_strength;
in vec2 environment_uv;
in vec2 lightmap_uv;
in vec3 source_tangent_sun;
in vec3 source_tangent_eye;
uniform vec4 material_color;
uniform bool has_texture;
uniform bool original_appearance;
uniform float exposure;
uniform float alpha_cutoff;
in vec3 source_vertex_light;
in vec3 source_ambient_light;
uniform vec3 ambient_multiplier;
uniform vec3 diffuse_multiplier;
uniform bool scene_roofed; // SceneLighting::roofed
in float fog_blend;
uniform vec3 fog_color;
uniform bool sky;
uniform bool source_ocean;
uniform vec3 sun_direction;
uniform float sky_blend;
uniform bool grayscale; // original death view: 3D scene desaturated, HUD untouched
uniform sampler2DShadow shadow_map; // GL_LINEAR + GL_COMPARE_REF_TO_TEXTURE: each tap is a bilinear 2x2 depth test
uniform sampler2DShadow shadow_far_map;
uniform float shadow_split;
uniform vec3 camera_position;
in vec3 fragment_position;
in vec4 shadow_position;
in vec4 far_shadow_position;
in float sky_scatter;
// NONCANONICAL wave 27 weather (renderer.hpp weather_uniforms; zero = the pre-wave paths); scene_fog: SceneLighting::fog_enabled.
uniform vec4 weather_wind;
uniform vec4 weather_state;
uniform vec4 weather_misc;
uniform bool scene_fog;
uniform vec4 source_time;
)GLSL" IKV_GL_WEATHER R"GLSL(
// Enhanced raster lighting: Vulkan lighting.glsl (non-RT enhanced) plus its post.frag ACES tonemap.
const float PI=3.14159265359;
vec3 enhanced_environment(vec3 direction) {
    if(!has_environment_map)return mix(vec3(0.055,0.07,0.09),vec3(0.32,0.44,0.62),clamp(direction.y*0.5+0.5,0.0,1.0));
    return textureLod(environment_map,vec2(atan(direction.z,direction.x)/(2.0*PI)+0.5,acos(clamp(direction.y,-1.0,1.0))/PI),0.0).rgb;
}
// Shadow positions use Vulkan light clip space; the GL shadow pass maps z to window depth 0..1 unchanged.
float shadow_pcf(sampler2DShadow map,vec3 p,vec3 n,vec3 l) {
    vec2 uv=p.xy*0.5+0.5;
    if(any(lessThan(uv,vec2(0.0)))||any(greaterThan(uv,vec2(1.0)))||p.z<0.0||p.z>1.0)return 1.0;
    float bias=max(0.0003,0.002*(1.0-dot(n,l)));vec2 texel=1.0/vec2(textureSize(map,0));float visible=0.0;
    for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++)visible+=texture(map,vec3(uv+vec2(x,y)*texel,p.z-bias));
    return visible/9.0;
}
float shadow_visibility(vec3 n,vec3 l) {
    vec3 near=shadow_position.xyz/shadow_position.w;vec2 uv=near.xy*0.5+0.5;
    bool near_valid=distance(camera_position,fragment_position)<=shadow_split&&all(greaterThanEqual(uv,vec2(0.0)))&&all(lessThanEqual(uv,vec2(1.0)))&&near.z>=0.0&&near.z<=1.0;
    if(near_valid)return shadow_pcf(shadow_map,near,n,l);
    return shadow_pcf(shadow_far_map,far_shadow_position.xyz/far_shadow_position.w,n,l);
}
out vec4 output_color;
// NONCANONICAL parallax occlusion: Vulkan lighting.glsl parallax() (derived relief of static zone materials, enhanced only).
uniform sampler2D height_map;
uniform bool has_height_map;
vec2 surface_uv;
vec3 parallax_gradient=vec3(0.0);
void parallax() {
    surface_uv=texcoord;
    if(!has_height_map||original_appearance||sky||source_ocean)return;
    vec3 dp1=dFdx(fragment_position),dp2=dFdy(fragment_position),nf=cross(dp1,dp2),v=camera_position-fragment_position;
    vec2 du1=dFdx(texcoord),du2=dFdy(texcoord);float det=du1.x*du2.y-du2.x*du1.y,dist=length(v);
    if(abs(det)<1e-12||dot(nf,nf)<1e-20||dist<1e-4)return;
    nf=normalize(nf);v/=dist;if(dot(nf,v)<0.0)nf=-nf;
    vec3 pu=(dp1*du2.y-dp2*du1.y)/det,pv=(dp2*du1.x-dp1*du2.x)/det;
    vec3 gu=cross(pv,nf),gv=cross(nf,pu);gu/=dot(pu,gu);gv/=dot(pv,gv);
    float c=dot(v,nf),depth=smoothstep(0.05,0.18,c)*(1.0-smoothstep(50.0,90.0,dist))*min(12.0*min(length(pu),length(pv))/float(textureSize(height_map,0).x),0.6);
    if(!(depth>1e-3))return;
    vec2 shift=-vec2(dot(v,gu),dot(v,gv))/c*depth;
    int steps=int(mix(24.0,8.0,c));float t=0.0,h=textureGrad(height_map,texcoord,du1,du2).r,last_t=0.0,last_f=h-1.0;
    for(int i=0;i<steps&&t<1.0-h;++i){last_t=t;last_f=t-(1.0-h);t+=1.0/float(steps);h=textureGrad(height_map,texcoord+shift*t,du1,du2).r;}
    float f=t-(1.0-h);if(f>0.0&&last_f<0.0)t=mix(last_t,t,last_f/(last_f-f));
    surface_uv=texcoord+shift*t;
    vec2 slope=(textureGrad(height_map,surface_uv,du1,du2).gb-0.5)*0.5*vec2(textureSize(height_map,0));
    parallax_gradient=0.7*depth*(slope.x*gu+slope.y*gv);
}
bool source_normal_path(){return has_normal_map&&source_lighting&&length(source_tangent_eye)>0.00001;}
// Original lit surface (display-referred). sun_visibility 0 drops the source directional term (Vulkan originalSurface).
vec4 original_surface(vec4 surface,float sun_visibility) {
    if(source_normal_path()) {
        vec4 mapped_texel=texture(normal_map,surface_uv);
        vec2 xy=mapped_texel.xy*2.0-1.0;
        vec3 mapped=normalize(vec3(xy,1.0-dot(xy,xy)));
        float factor=mix(1.0,dot(mapped,source_tangent_sun)*0.5+0.5,sun_visibility);
        vec3 half_direction=mapped+source_tangent_eye;
        float specular=length(half_direction)>0.00001
            ? pow(max(dot(mapped,normalize(half_direction)),0.0),has_environment_map?64.0:48.0):0.0;
        if(factor<=0.0)specular=0.0;
        vec3 illumination=mix(sky_c,sky_b,factor)*mapped_texel.a;
        vec3 specular_color=vec3(mapped_texel.b);
        if(has_environment_map)specular_color+=texture(environment_map,environment_uv).rgb*surface.a;
        vec3 result=(surface.rgb+specular*specular_color)*illumination;
        if(!has_environment_map)result+=surface.rgb*(1.0-surface.a);
        return vec4(result,1.0);
    }
    if(has_environment_map) {
        vec3 environment=texture(environment_map,environment_uv).rgb;
        if(has_lightmap)surface.rgb=mix(surface.rgb,environment,surface.a)
            -texture(lightmap_map,lightmap_uv).rgb*(1.0-clamp(surface.a,0.0,1.0))*lightmap_strength;
        else surface.rgb+=environment*surface.a;
        surface.a=1.0;
    } else if(has_lightmap)surface.rgb-=texture(lightmap_map,lightmap_uv).rgb*lightmap_strength;
    surface.rgb*=sun_visibility>0.5?source_vertex_light:source_ambient_light;
    return surface;
}
// Vulkan frame.glsl enhancedSunScale/enhancedAmbientScale (NONCANONICAL wave 19): clock record relative to noon, linear.
// NONCANONICAL wave 27: the weather tints and dims the sun and sky light (weather.glsl weatherSunTint/AmbientTint).
vec3 enhanced_sun_scale(){return (source_lighting?pow(max(diffuse_multiplier*(255.0/vec3(126.0,105.0,110.0)),vec3(0.0)),vec3(2.2)):vec3(1.0))*weatherSunTint(weather_state,weather_misc);}
vec3 enhanced_ambient_scale(){return (source_lighting?pow(max(ambient_multiplier*(255.0/vec3(200.0,157.0,142.0)),vec3(0.0)),vec3(2.2)):vec3(1.0))*weatherAmbientTint(weather_state,weather_misc,source_time.y);}
// Vulkan lighting.glsl weatherFogLinear.
vec3 weather_fog_linear(){
    vec3 c=pow(max(weatherFogColor(fog_color,scene_fog,weather_state,weather_misc),vec3(0.0)),vec3(2.2));
    return scene_fog?c:c*min(enhanced_ambient_scale(),vec3(1.0));
}
// NONCANONICAL: enhanced source ocean, the shared OpenUnderworld water (shaders/vulkan/water.glsl, embedded at build time
// as IKV_GL_WATER) over the opaque scene copy (water_*_map, blitted before the water draws). The copy holds tonemapped
// display colour: inverted to linear before mixing.
uniform sampler2D water_color_map;
uniform sampler2D water_depth_map;
uniform mat4 view_projection;
uniform mat4 light_view_projection;
uniform mat4 far_light_view_projection;
vec3 water_linear(vec3 y){y=min(pow(y,vec3(2.2)),vec3(0.99));vec3 a=2.43*y-2.51,b=0.59*y-0.03,c=0.14*y;return (-b-sqrt(b*b-4.0*a*c))/(2.0*a)/exposure;}
vec3 waterSceneColor(vec2 uv){return water_linear(textureLod(water_color_map,uv,0.0).rgb);}
float waterSceneDepth(vec2 uv){return textureLod(water_depth_map,uv,0.0).r;}
float waterNdcZ(float depth){return depth*2.0-1.0;}
float waterBedSun(vec3 p){
    vec3 l=normalize(sun_direction),up=vec3(0.0,1.0,0.0);
    vec4 c=light_view_projection*vec4(p,1.0);vec3 near=c.xyz/c.w;vec2 uv=near.xy*0.5+0.5;
    bool near_valid=distance(camera_position,p)<=shadow_split&&all(greaterThanEqual(uv,vec2(0.0)))&&all(lessThanEqual(uv,vec2(1.0)))&&near.z>=0.0&&near.z<=1.0;
    if(near_valid)return shadow_pcf(shadow_map,near,up,l);
    vec4 f=far_light_view_projection*vec4(p,1.0);
    return shadow_pcf(shadow_far_map,f.xyz/f.w,up,l);
}
bool waterRayReflection(vec3 origin,vec3 direction,inout vec3 colour){return false;}
)GLSL" IKV_GL_OCEAN_WIND IKV_GL_WATER R"GLSL(
vec3 water_surface() {
    vec3 p=fragment_position,l=normalize(sun_direction),sun_scale=enhanced_sun_scale();
    vec3 light=source_lighting?pow(min(ambient_multiplier+diffuse_multiplier*max(l.y,0.0),vec3(1.0)),vec3(2.2)):vec3(1.0);
    float sun=scene_roofed?0.0:shadow_visibility(vec3(0.0,1.0,0.0),l);
    float caustic=scene_roofed?0.0:0.8*min(max(sun_scale.r,max(sun_scale.g,sun_scale.b)),1.0);
    sun=mix(sun,1.0,weatherShadowSoftness(weather_state,weather_misc)); // NONCANONICAL wave 27 (0 without weather)
    vec3 c=waterShade(p,camera_position,view_projection,gl_FragCoord.xy/vec2(textureSize(water_depth_map,0)),source_time.y,l,light,
        fog_color,vec3(2.8,2.55,2.2)*sun_scale,sun,caustic,weather_wind,weather_state,weather_misc);
    c=mix(pow(max(fog_color,vec3(0.0)),vec3(2.2)),c,clamp(fog_blend,0.0,1.0));
    if(weatherOn(weather_misc))c=mix(weather_fog_linear(),c,weatherFogTransmit(distance(camera_position,p),weather_state,weather_misc));
    c*=exposure;c=clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14),0.0,1.0);
    return pow(c,vec3(1.0/2.2));
}
void main() {
    if(source_ocean&&!original_appearance){vec3 c=water_surface();if(grayscale)c=vec3(dot(c,vec3(0.299,0.587,0.114)));output_color=vec4(c,1.0);return;}
    parallax();
    vec4 surface=material_color;
    if(has_texture&&!source_ocean) surface*=texture(diffuse_map,surface_uv);
    // Evaluates source constants when available; otherwise explicitly reduced ambient-only rendering.
    if(source_ocean) {
        // t_ocean.psh uses only diffuse and normal samplers; authored alpha is one.
        vec3 diffuse=texture(diffuse_map,texcoord).rgb*source_vertex_light;
        vec3 first=texture(normal_map,lightmap_uv*0.80000001).rgb*2.0-1.0;
        vec3 second=texture(normal_map,texcoord*0.23999999).rgb*2.0-1.0;
        vec3 mapped=normalize(first+second).xzy;
        float facing=dot(mapped,source_tangent_eye);
        vec3 reflection=source_tangent_eye-2.0*mapped*facing;
        float specular=pow(clamp(dot(reflection,-sun_direction),0.0,1.0),64.0)
            *(1.0-clamp(facing,0.0,1.0));
        surface=vec4(diffuse+specular*sky_c*0.80000001,1.0);
    } else if(sky) {
        // NONCANONICAL wave 27 (enhanced weather): clouds drift with the wind; overcast, fog and lightning (weather.glsl).
        bool weather=weatherOn(weather_misc);
        vec2 sky_uv=weather?texcoord+weatherSkyScroll(weather_wind):texcoord;
        if(weather&&has_texture)surface=material_color*texture(diffuse_map,sky_uv);
        vec3 second=has_environment_map?texture(environment_map,sky_uv).rgb:vec3(0.0);
        vec3 clouds=surface.rgb*(1.0-clamp(sky_blend,0.0,1.0))+second*sky_blend;
        surface=vec4(mix(fog_color,clouds*source_vertex_light+vec3(sky_scatter),surface.a),1.0);
        if(weather)surface.rgb=weatherSky(surface.rgb,weatherFogColor(fog_color,scene_fog,weather_state,weather_misc),weather_state,weather_misc,source_time.y);
    } else if(original_appearance) {
        if(!source_normal_path()&&!has_environment_map&&has_fade_map)surface=mix(texture(fade_map,texcoord),surface,fade_blend);
        surface=original_surface(surface,1.0);
    } else {
        // Source MAT+14 secondary fade applies only without higher-priority normal/environment maps.
        if(has_fade_map&&!has_normal_map&&!has_environment_map)surface=mix(texture(fade_map,surface_uv)*material_color,surface,fade_blend);
        // Vulkan lighting.glsl (NONCANONICAL wave 19): ambient = original light without its sun term (roofed: with it, and
        // no shadow-mapped sun), linearised; sun and sky reflection scaled by the clock record; record fog in linear.
        vec3 baked=pow(max(original_surface(surface,scene_roofed?1.0:0.0).rgb,vec3(0.0)),vec3(2.2));
        // Direct sun albedo keeps the original subtractive lightmap (Vulkan enhancedLightmapShadow; not normal-mapped).
        vec3 lightmap_shadow=has_lightmap&&!has_normal_map?texture(lightmap_map,lightmap_uv).rgb*lightmap_strength
            *(has_environment_map?1.0-clamp(surface.a,0.0,1.0):1.0):vec3(0.0);
        if(has_normal_map||has_environment_map)surface.a=1.0;
        float nlength=length(world_normal);
        vec3 n=nlength>0.00001?world_normal/nlength:vec3(0.0,1.0,0.0);
        if(has_normal_map) {
            vec3 t=world_tangent-n*dot(n,world_tangent);
            // Source normal maps carry only xy (b specular, a illumination): z rebuilt as original_surface (Vulkan mappedNormal).
            if(dot(t,t)>1e-12){t=normalize(t);vec2 xy=texture(normal_map,surface_uv).xy*2.0-1.0;n=normalize(mat3(t,cross(n,t),n)*normalize(vec3(xy,1.0-dot(xy,xy))));}
        } else n=normalize(n-parallax_gradient);
        vec3 v=normalize(camera_position-fragment_position),l=normalize(sun_direction),h=normalize(v+l);
        float roughness=0.65;
        vec3 albedo=pow(max(surface.rgb-lightmap_shadow,vec3(0.0)),vec3(2.2));
        // NONCANONICAL wave 27 (Vulkan lighting.glsl): season tint, rain wetness and snow cover on albedo and baked light.
        bool weather=weatherOn(weather_misc);
        float direct_roughness=roughness,puddle=0.0;
        if(weather) {
            WeatherSurface w=weatherSurface(albedo,n,nlength>0.00001?world_normal/nlength:n,fragment_position,roughness,source_time.y,weather_state,weather_misc);
            baked=weatherBaked(baked,albedo,w,weather_misc)*weatherAmbientTint(weather_state,weather_misc,source_time.y);
            puddle=w.puddle;
            albedo=w.albedo;n=w.normal;roughness=w.roughness;direct_roughness=w.directRoughness;
        }
        float ndl=max(dot(n,l),0.0),ndv=max(dot(n,v),0.001),ndh=max(dot(n,h),0.0),vdh=max(dot(v,h),0.0);
        float a2=direct_roughness*direct_roughness*direct_roughness*direct_roughness,dd=ndh*ndh*(a2-1.0)+1.0,d=a2/(PI*dd*dd);
        float k=(direct_roughness+1.0)*(direct_roughness+1.0)/8.0,g=(ndv/(ndv*(1.0-k)+k))*(ndl/(ndl*(1.0-k)+k));
        vec3 f=vec3(0.04)+0.96*pow(1.0-vdh,5.0);
        vec3 direct=((1.0-f)*albedo/PI+d*g*f/max(4.0*ndv*ndl,0.001))*ndl*3.2;
        // Weather (Vulkan lighting.glsl): softened sun shadows; puddles mirror the weather-tinted sky strongly.
        float sun_shadow=shadow_visibility(n,l),reflection_gain=0.4;vec3 sky_reflection=enhanced_environment(reflect(-v,n));
        if(weather){sun_shadow=mix(sun_shadow,1.0,weatherShadowSoftness(weather_state,weather_misc));reflection_gain=mix(0.4,2.5,puddle);
            sky_reflection=mix(sky_reflection,weather_fog_linear(),max(weather_state.y,weather_misc.x));}
        vec3 c=1.5*((scene_roofed?vec3(0.0):direct*enhanced_sun_scale()*sun_shadow)+baked+sky_reflection*f*(1.0-roughness)*reflection_gain*enhanced_ambient_scale());
        c=mix(pow(max(fog_color,vec3(0.0)),vec3(2.2)),c,clamp(fog_blend,0.0,1.0));
        if(weather) { // weather fog
            c=mix(weather_fog_linear(),c,weatherFogTransmit(distance(camera_position,fragment_position),weather_state,weather_misc));
        }
        c*=exposure;c=clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14),0.0,1.0);
        surface.rgb=pow(c,vec3(1.0/2.2));
    }
    if((original_appearance||source_ocean)&&!sky)surface.rgb=mix(fog_color,surface.rgb,clamp(fog_blend,0.0,1.0));
    if(surface.a<=alpha_cutoff&&alpha_cutoff>0.0)discard;
    if(grayscale)surface.rgb=vec3(dot(surface.rgb,vec3(0.299,0.587,0.114)));
    output_color=surface;
}
)GLSL";
// Enhanced sun shadow map (Vulkan shadow.vert/.frag): light clip z 0..1 is mapped to GL's -1..1, so the stored window
// depth equals the Vulkan depth the scene shader compares against.
constexpr const char* shadow_vertex = R"GLSL(#version 330 core
layout(location=0) in vec3 position;
layout(location=2) in vec2 uv;
layout(location=6) in vec4 diffuse_color;
uniform mat4 light_view_projection;
uniform mat4 object_matrix;
// NONCANONICAL wave 27: swaying draws bend as in scene_vertex.
uniform vec4 weather_wind;
uniform vec4 weather_misc;
uniform vec4 weather_sway;
uniform vec4 source_time;
out vec2 texcoord;
out float fade_blend;
)GLSL" IKV_GL_WEATHER R"GLSL(
void main(){
    texcoord=uv;fade_blend=diffuse_color.a;
    vec3 sway=weatherSway(position.y,(object_matrix*vec4(position,1.0)).xyz,weather_sway,weather_wind,weather_misc,source_time.y);
    vec4 p=sway==vec3(0.0)?light_view_projection*object_matrix*vec4(position,1.0):light_view_projection*(object_matrix*vec4(position,1.0)+vec4(sway,0.0));
    gl_Position=vec4(p.xy,p.z*2.0-p.w,p.w);
}
)GLSL";
constexpr const char* shadow_fragment = R"GLSL(#version 330 core
in vec2 texcoord;
in float fade_blend;
uniform sampler2D diffuse_map;
uniform sampler2D fade_map;
uniform bool has_texture;
uniform bool has_fade_map;
uniform bool opaque_alpha;
uniform vec4 material_color;
uniform float alpha_cutoff;
void main(){
    if(alpha_cutoff<=0.0)return;
    float alpha=has_texture?texture(diffuse_map,texcoord).a:1.0;
    if(opaque_alpha)alpha=1.0;
    else if(has_fade_map)alpha=mix(texture(fade_map,texcoord).a,alpha,fade_blend);
    if(alpha*material_color.a<=alpha_cutoff)discard;
}
)GLSL";
// Casters without an alpha cutoff: no texture fetch and no discard, so early depth stays on (same depth as shadow_fragment).
constexpr const char* shadow_depth_fragment = R"GLSL(#version 330 core
void main(){}
)GLSL";
constexpr const char* overlay_vertex = R"GLSL(#version 330 core
out vec2 texcoord;
void main(){
    vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);
    texcoord=vec2(p.x,1.0-p.y);
    gl_Position=vec4(p*2.0-1.0,0.0,1.0);
}
)GLSL";
constexpr const char* overlay_fragment = R"GLSL(#version 330 core
in vec2 texcoord;
uniform sampler2D overlay_map;
out vec4 output_color;
void main(){output_color=texture(overlay_map,texcoord);}
)GLSL";
// Original fake HDR pass (0046fdb0 @470d2f, "doing fake HDR pass"): two 128x128 X8R8G8B8 targets bb38/bb3c (00466e40).
// mode 0 = t_half_intensity: bb3c = .45*scene + .55*bb38 (last frame); mode 1 = t_pseudo_HDR_blur: three taps one
// 128-texel apart weighted (.25+c3.x, .5, .25+c3.x), c3.x = record glow*.02; mode 2 = fixed-function composite with
// quad diffuse 0xffcccccc, blended SRCCOLOR/ONE onto the frame (0046fdb0 @4716a2).
constexpr const char* glow_vertex = R"GLSL(#version 330 core
out vec2 texcoord;
void main(){
    vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);
    texcoord=p;
    gl_Position=vec4(p*2.0-1.0,0.0,1.0);
}
)GLSL";
constexpr const char* glow_fragment = R"GLSL(#version 330 core
in vec2 texcoord;
uniform sampler2D first;
uniform sampler2D second;
uniform int mode;
uniform vec4 step_gain;
out vec4 output_color;
void main(){
    vec3 c=texture(first,texcoord).rgb;
    if(mode==0)c=c*0.45+texture(second,texcoord).rgb*0.55;
    else if(mode==1)c=(0.25+step_gain.z)*(texture(first,texcoord-step_gain.xy).rgb+texture(first,texcoord+step_gain.xy).rgb)+0.5*c;
    else if(mode==2)c*=0.8;
    else c=min(pow(c,vec3(1.0/1.2))*1.005,vec3(1.0)); // mode 3: fullscreen gamma ramp 0x467030(1,1,1.2), final pass
    output_color=vec4(clamp(c,0.0,1.0),1.0);
}
)GLSL";
// Original particle stage: texture*vertex colour (0049a780 texture stages), D3DRS_DEPTHBIAS 0xc3 added to NDC depth,
// alpha test ALPHAREF only for BlendType "blend" with a nonzero mask (keep alpha > ref).
constexpr const char* particle_vertex = R"GLSL(#version 330 core
layout(location=0) in vec3 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec4 color;
uniform mat4 view_projection;
uniform float depth_bias;
uniform bool fog_enabled;
uniform float fog_start;
uniform float fog_end;
out vec2 texcoord;
out vec4 tint;
void main(){
    texcoord=uv;tint=color;
    gl_Position=view_projection*vec4(position,1.0);
    // v_particles.vsh fogs alpha only: oD0.w=sat(a*(c20.z-clipZ)/(c20.z-c20.y)), D3D clip Z before DEPTHBIAS.
    if(fog_enabled)tint.a=clamp(color.a*(fog_end-(gl_Position.z+gl_Position.w)*0.5)/(fog_end-fog_start),0.0,1.0);
    gl_Position.z+=depth_bias*2.0*gl_Position.w; // D3D [0,1] depth bias in GL's [-1,1] NDC
}
)GLSL";
constexpr const char* particle_fragment = R"GLSL(#version 330 core
in vec2 texcoord;
in vec4 tint;
uniform sampler2D particle_map;
uniform float alpha_reference;
uniform bool grayscale;
out vec4 output_color;
void main(){
    vec4 color=texture(particle_map,texcoord)*tint;
    if(alpha_reference>0.0&&color.a<=alpha_reference)discard;
    if(grayscale)color.rgb=vec3(dot(color.rgb,vec3(0.299,0.587,0.114)));
    output_color=color;
}
)GLSL";
// Original hover hilite (0046ccf0 tail "rendering hilite of models"; node draw 0047e1xx): v_contour.vsh pushes each
// vertex along its normal by c8.w, t_contour.psh outputs oD0 = (c8.rgb, .2). No fog (FOGENABLE cleared at 46d0ba).
constexpr const char* contour_vertex = R"GLSL(#version 330 core
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
uniform mat4 view_projection;
uniform mat4 object_matrix;
uniform float offset;
void main(){
    // ponytail: v_contour adds the unrotated object normal in world space; the world normal keeps rotated shells outward.
    vec3 n=mat3(object_matrix)*normal;
    n=length(n)>0.00001?normalize(n)*offset:vec3(0.0);
    gl_Position=view_projection*(object_matrix*vec4(position,1.0)+vec4(n,0.0));
}
)GLSL";
constexpr const char* contour_fragment = R"GLSL(#version 330 core
uniform vec3 contour;
uniform bool grayscale;
out vec4 output_color;
void main(){
    output_color=vec4(grayscale?vec3(dot(contour,vec3(0.299,0.587,0.114))):contour,0.2);
}
)GLSL";

class GlRenderer final : public Renderer {
#define GL_ENTRY(type, name) type name##_ = nullptr
    GL_ENTRY(PFNGLCREATESHADERPROC, glCreateShader);
    GL_ENTRY(PFNGLSHADERSOURCEPROC, glShaderSource);
    GL_ENTRY(PFNGLCOMPILESHADERPROC, glCompileShader);
    GL_ENTRY(PFNGLGETSHADERIVPROC, glGetShaderiv);
    GL_ENTRY(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog);
    GL_ENTRY(PFNGLDELETESHADERPROC, glDeleteShader);
    GL_ENTRY(PFNGLCREATEPROGRAMPROC, glCreateProgram);
    GL_ENTRY(PFNGLATTACHSHADERPROC, glAttachShader);
    GL_ENTRY(PFNGLLINKPROGRAMPROC, glLinkProgram);
    GL_ENTRY(PFNGLGETPROGRAMIVPROC, glGetProgramiv);
    GL_ENTRY(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog);
    GL_ENTRY(PFNGLDELETEPROGRAMPROC, glDeleteProgram);
    GL_ENTRY(PFNGLUSEPROGRAMPROC, glUseProgram);
    GL_ENTRY(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation);
    GL_ENTRY(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv);
    GL_ENTRY(PFNGLUNIFORM4FVPROC, glUniform4fv);
    GL_ENTRY(PFNGLUNIFORM3FVPROC, glUniform3fv);
    GL_ENTRY(PFNGLUNIFORM1IPROC, glUniform1i);
    GL_ENTRY(PFNGLUNIFORM1FPROC, glUniform1f);
    GL_ENTRY(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays);
    GL_ENTRY(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
    GL_ENTRY(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays);
    GL_ENTRY(PFNGLGENBUFFERSPROC, glGenBuffers);
    GL_ENTRY(PFNGLBUFFERSUBDATAPROC, glBufferSubData);
    GL_ENTRY(PFNGLBINDBUFFERPROC, glBindBuffer);
    GL_ENTRY(PFNGLBUFFERDATAPROC, glBufferData);
    GL_ENTRY(PFNGLDELETEBUFFERSPROC, glDeleteBuffers);
    GL_ENTRY(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);
    GL_ENTRY(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer);
    GL_ENTRY(PFNGLACTIVETEXTUREPROC, glActiveTexture);
    GL_ENTRY(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers);
    GL_ENTRY(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer);
    GL_ENTRY(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D);
    GL_ENTRY(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus);
    GL_ENTRY(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers);
    GL_ENTRY(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers);
    GL_ENTRY(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer);
    GL_ENTRY(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage);
    GL_ENTRY(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer);
    GL_ENTRY(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers);
    GL_ENTRY(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer);
    GL_ENTRY(PFNGLBLENDEQUATIONPROC, glBlendEquation);
    GL_ENTRY(PFNGLCOMPRESSEDTEXIMAGE2DPROC, glCompressedTexImage2D);
    GL_ENTRY(PFNGLUNIFORMMATRIX3FVPROC, glUniformMatrix3fv);
    GL_ENTRY(PFNGLGENQUERIESPROC, glGenQueries);
    GL_ENTRY(PFNGLDELETEQUERIESPROC, glDeleteQueries);
    GL_ENTRY(PFNGLBEGINQUERYPROC, glBeginQuery);
    GL_ENTRY(PFNGLENDQUERYPROC, glEndQuery);
    GL_ENTRY(PFNGLGETQUERYOBJECTIVPROC, glGetQueryObjectiv);
    GL_ENTRY(PFNGLGETQUERYOBJECTUI64VPROC, glGetQueryObjectui64v);
#undef GL_ENTRY
    struct GpuMesh { GLuint vao=0, vertices=0, indices=0, texture=0, stream=0; GLsizei count=0; std::size_t vertex_count=0; std::uint64_t revision=0; std::array<float,4> bounds{}; std::array<float,2> height{}; };
    struct SkinStream { std::array<float,3> position,normal,tangent; }; // skinned attributes (upload_stream)
    std::vector<SkinStream> skin_stream_;
    struct GpuMaterial { GLuint diffuse=0,normal=0,environment=0,lightmap=0,fade=0,height=0; bool referenced=false; };
    struct Draw { const Instance* instance; Matrix4 matrix; float distance; std::uint32_t material; bool fog; std::array<float,4> scroll; bool visible=true; };
    struct ContourDraw { std::uint32_t mesh; Matrix4 matrix; std::array<float,3> color; };
    SDL_Window* window_;
    SDL_GLContext context_ = nullptr;
    RendererOptions options_;
    int width_=0,height_=0;
    GLint max_texture_size_=0;
    bool s3tc_=false; // GL_EXT_texture_compression_s3tc: original DXT blocks stay compressed; otherwise decoded to RGBA8
    GLfloat anisotropy_=1; // min(16, GL_MAX_TEXTURE_MAX_ANISOTROPY); 1 = unsupported
    GLuint program_=0,overlay_program_=0,overlay_vao_=0,overlay_texture_=0;
    GLuint particle_program_=0,particle_vao_=0,particle_buffer_=0;
    GLint particle_vp_location_=-1,particle_bias_location_=-1,particle_reference_location_=-1;
    std::vector<GLuint> particle_textures_;
    std::vector<ParticleBatch> particle_batches_;
    GLuint contour_program_=0;
    GLint contour_vp_location_=-1,contour_object_location_=-1,contour_offset_location_=-1,contour_color_location_=-1;
    std::vector<ContourDraw> contours_;
    GLuint framebuffer_=0,color_=0,depth_=0;
    GLuint glow_program_=0,glow_textures_[2]{},glow_framebuffers_[2]{}; // [0]=bb38, [1]=bb3c
    // Enhanced appearance only: near/far 2048^2 sun depth maps.
    GLuint shadow_program_=0,shadow_textures_[2]{},shadow_framebuffers_[2]{};
    std::array<Matrix4,2> shadow_lights_{};
    float shadow_split_=0;
    GLint light_location_=-1,far_light_location_=-1,shadow_split_location_=-1,shadow_light_location_=-1,shadow_object_location_=-1,shadow_color_location_=-1,shadow_cutoff_location_=-1;
    GLint shadow_texture_location_=-1,shadow_fade_location_=-1,shadow_opaque_location_=-1;
    GLint glow_mode_location_=-1,glow_step_location_=-1;
    GLint vp_location_=-1,object_location_=-1,color_location_=-1,texture_location_=-1,original_location_=-1,exposure_location_=-1;
    GLint normal_location_=-1,cutoff_location_=-1;
    GLint source_light_location_=-1,sun_location_=-1,ambient_location_=-1,diffuse_location_=-1;
    GLint fog_enabled_location_=-1,fog_color_location_=-1,fog_start_location_=-1,fog_end_location_=-1;
    GLint camera_location_=-1,environment_location_=-1,lightmap_location_=-1;
    GLint sky_b_location_=-1,sky_c_location_=-1,lightmap_strength_location_=-1;
    GLint sky_location_=-1,sky_blend_location_=-1;
    GLint fade_location_=-1,height_location_=-1;
    GLint ocean_location_=-1,time_location_=-1,scroll_location_=-1,roofed_location_=-1;
    // NONCANONICAL wave 27: weather uniforms, the integrated wind phase and the IKV_RENDER_WEATHER capture override.
    GLint weather_wind_location_=-1,weather_state_location_=-1,weather_misc_location_=-1,weather_sway_location_=-1,scene_fog_location_=-1;
    GLint shadow_wind_location_=-1,shadow_misc_location_=-1,shadow_sway_location_=-1,shadow_time_location_=-1;
    WeatherUniforms weather_;double wind_phase_=0;std::optional<SceneWeather> weather_override_=parse_weather_override(std::getenv("IKV_RENDER_WEATHER"));
    GLuint water_framebuffer_=0,water_textures_[2]{}; // NONCANONICAL enhanced water scene copy: colour, depth
    bool overlay_present_=false,rendered_=false,grayscale_=false;
    int overlay_width_=0,overlay_height_=0; // overlay_texture_ storage size (sub-rectangle uploads need a matching one)
    bool source_presented_=false;
    // GPU resources of the last render()'s Scene; a different Scene address frees them and uploads in full.
    struct GlSet {
        const Scene* key=nullptr;
        bool planned=false;
        std::uint64_t geometry_revision=0;
        std::vector<std::uint64_t> mesh_keys; // Scene mesh keys as last planned; actor attach/detach remaps meshes by them.
        std::uint64_t mesh_key=0; // Scene::mesh_key as last planned: any actor (re)assembly since then re-plans.
        std::vector<GpuMesh> meshes;
        std::vector<GpuMaterial> materials;
        std::vector<GLuint> textures;
        std::unordered_map<std::string,GLuint> texture_names;
        std::vector<std::uint8_t> material_usage,mesh_plan; // mesh_plan 0: dormant/proxy, 1: referenced no-color, 2: upload
        std::vector<bool> ocean_meshes; // NONCANONICAL wave 28: enhanced uploads these subdivided (renderer.hpp subdivide_ocean_mesh)
        std::size_t next_texture=0,next_material=0,next_mesh=0; // upload cursors
        SourceScrollState scroll;
    };
    struct View { std::vector<Draw> sky,opaque,alpha; };
    GlSet set_;
    View view_;
    std::vector<const Draw*> opaque_order_; // visible non-water opaque draws, nearest first (render())
    // Redundant-state filtering: the last value each cached helper set. Uniform values live in their program object and
    // only these helpers write them, so they stay valid across frames; texture-unit, VAO and cull state is forgotten
    // (state_reset) wherever other code may have changed it. NaN/INT_MIN sentinels force the first write.
    struct UniformInt { GLint location=-1; int value=std::numeric_limits<int>::min(); };
    struct UniformFloat { GLint location=-1; float value=std::numeric_limits<float>::quiet_NaN(); };
    struct UniformVec4 { GLint location=-1; std::array<float,4> value{std::numeric_limits<float>::quiet_NaN()}; };
    struct SceneUniforms { UniformInt sky,ocean,fog,normal,environment,lightmap,fade,height,texture; UniformFloat cutoff; UniformVec4 scroll,sway,color; } draw_uniforms_;
    struct ShadowUniforms { UniformInt texture,fade,opaque; UniformFloat cutoff; UniformVec4 color,sway; } shadow_uniforms_;
    UniformVec4 depth_sway_;
    GLint normal_matrix_location_=-1,grayscale_location_=-1,contour_grayscale_location_=-1;
    GLint particle_grayscale_location_=-1,particle_fog_enabled_location_=-1,particle_fog_start_location_=-1,particle_fog_end_location_=-1;
    std::array<GLuint,10> bound_textures_{};GLenum active_unit_=0;GLuint bound_vao_=0;int cull_=-1;
    // Shadow casters without an alpha cutoff draw with a depth-only program. Static casters (no entity, skin or sway) of a
    // cascade are cached in shadow_static_*: while the light matrix and the static caster list are unchanged, the cached
    // depth is restored only where last frame's dynamic casters drew, then the dynamic casters are drawn again (the depth
    // min over casters is order independent, so the map equals a full redraw).
    GLuint shadow_depth_program_=0,shadow_static_textures_[2]{},shadow_static_framebuffers_[2]{};
    GLint depth_light_location_=-1,depth_object_location_=-1,depth_wind_location_=-1,depth_misc_location_=-1,depth_time_location_=-1;
    struct ShadowKey { const Instance* instance; std::uint32_t mesh,material; std::uint64_t revision; Matrix4 matrix; bool operator==(const ShadowKey&) const=default; };
    struct ShadowCache { bool valid=false,has_light=false; Matrix4 light{}; std::vector<ShadowKey> keys; std::vector<std::array<int,4>> dirty; };
    std::array<ShadowCache,2> shadow_cache_;unsigned shadow_hits_=0,shadow_misses_=0; // cascade passes since the last timing line
    double shadow_static_triangles_=0,shadow_dirty_texels_=0; // summed over those passes (IKV_GPU_TIMING line)
    std::vector<std::array<int,4>> shadow_dirty_;std::vector<ShadowKey> shadow_keys_;std::vector<const Draw*> shadow_static_,shadow_dynamic_;
    // IKV_GPU_TIMING: GL_TIME_ELAPSED per pass in a 4-frame ring, read only once available (never waits); the averages of 30
    // complete frames are logged like Vulkan's "gpu timing" line. scenePassMs = opaque + water + alpha (Vulkan's scene pass).
    enum TimedPass { timed_shadow,timed_opaque,timed_water,timed_alpha,timed_post,timed_passes };
    static constexpr int timing_ring=4;
    bool timing_=false;GLuint timing_queries_[timing_ring][timed_passes]{};unsigned timing_issued_[timing_ring]{};unsigned timing_frame_=0;
    double timing_sum_[timed_passes]{};unsigned timing_frames_=0;
public:
    GlRenderer(SDL_Window* window, const RendererOptions& options) : window_(window), options_(options) {
        if (!window_) throw std::invalid_argument("OpenGL requires an SDL window");
        validate_camera(options_.camera);
        if (options_.ray_tracing==RayTracing::required) throw std::runtime_error("OpenGL provider does not support hardware ray tracing; choose Vulkan with supported hardware");
        if (!std::isfinite(options_.exposure) || options_.exposure<=0) throw std::invalid_argument("invalid graphics exposure");
        try {
            progress();
            if (!SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3) || !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3) ||
                !SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE) ||
                !SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1) || !SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24))
                fail("OpenGL context attributes");
            progress();
            context_=SDL_GL_CreateContext(window_);
            if(!context_) fail("OpenGL 3.3 core context creation");
            progress();
            current();
#define LOAD_GL(name) name##_ = reinterpret_cast<decltype(name##_)>(SDL_GL_GetProcAddress(#name)); if(!name##_) throw std::runtime_error("missing OpenGL entry point " #name)
            LOAD_GL(glCreateShader); LOAD_GL(glShaderSource); LOAD_GL(glCompileShader); LOAD_GL(glGetShaderiv); LOAD_GL(glGetShaderInfoLog); LOAD_GL(glDeleteShader);
            LOAD_GL(glCreateProgram); LOAD_GL(glAttachShader); LOAD_GL(glLinkProgram); LOAD_GL(glGetProgramiv); LOAD_GL(glGetProgramInfoLog); LOAD_GL(glDeleteProgram); LOAD_GL(glUseProgram);
            LOAD_GL(glGetUniformLocation); LOAD_GL(glUniformMatrix4fv); LOAD_GL(glUniform4fv); LOAD_GL(glUniform1i); LOAD_GL(glUniform1f);
            LOAD_GL(glUniform3fv);
            LOAD_GL(glGenVertexArrays); LOAD_GL(glBindVertexArray); LOAD_GL(glDeleteVertexArrays); LOAD_GL(glGenBuffers); LOAD_GL(glBindBuffer); LOAD_GL(glBufferData); LOAD_GL(glDeleteBuffers);
            LOAD_GL(glEnableVertexAttribArray); LOAD_GL(glVertexAttribPointer); LOAD_GL(glActiveTexture);
            LOAD_GL(glBufferSubData);
            LOAD_GL(glGenFramebuffers); LOAD_GL(glBindFramebuffer); LOAD_GL(glFramebufferTexture2D); LOAD_GL(glCheckFramebufferStatus); LOAD_GL(glDeleteFramebuffers);
            LOAD_GL(glGenRenderbuffers); LOAD_GL(glBindRenderbuffer); LOAD_GL(glRenderbufferStorage); LOAD_GL(glFramebufferRenderbuffer); LOAD_GL(glDeleteRenderbuffers); LOAD_GL(glBlitFramebuffer);
            LOAD_GL(glBlendEquation); LOAD_GL(glCompressedTexImage2D); LOAD_GL(glUniformMatrix3fv);
            LOAD_GL(glGenQueries); LOAD_GL(glDeleteQueries); LOAD_GL(glBeginQuery); LOAD_GL(glEndQuery); LOAD_GL(glGetQueryObjectiv); LOAD_GL(glGetQueryObjectui64v);
#undef LOAD_GL
            glGetIntegerv(GL_MAX_TEXTURE_SIZE,&max_texture_size_);
            if(max_texture_size_<1) throw std::runtime_error("OpenGL texture limits unavailable");
            s3tc_=SDL_GL_ExtensionSupported("GL_EXT_texture_compression_s3tc");
            // GL 4.6 core, ARB and EXT texture_filter_anisotropic share these enums. Original 0046b040 sets
            // D3DSAMP_MAXANISOTROPY (default pref 4x); 16 measured no worse in motion (wave-12 Textures report §2).
            if(SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic")||SDL_GL_ExtensionSupported("GL_ARB_texture_filter_anisotropic")) {
                GLfloat limit=1;glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT,&limit);anisotropy_=std::clamp(limit,1.f,16.f);
            }
            std::fprintf(stderr,"OpenGL textures s3tc=%d anisotropy=%.0f\n",s3tc_?1:0,anisotropy_);
            // Vsync is best-effort on headless/compositing drivers; rendering does not depend on it.
            SDL_GL_SetSwapInterval(options_.vsync?1:0);
            progress();
            program_=link(scene_vertex,scene_fragment);
            overlay_program_=link(overlay_vertex,overlay_fragment);
            vp_location_=uniform(program_,"view_projection"); object_location_=uniform(program_,"object_matrix");
            color_location_=uniform(program_,"material_color"); texture_location_=uniform(program_,"has_texture");
            original_location_=uniform(program_,"original_appearance"); exposure_location_=uniform(program_,"exposure");
            normal_location_=uniform(program_,"has_normal_map");cutoff_location_=uniform(program_,"alpha_cutoff");
            source_light_location_=uniform(program_,"source_lighting");sun_location_=uniform(program_,"sun_direction");
            ambient_location_=uniform(program_,"ambient_multiplier");diffuse_location_=uniform(program_,"diffuse_multiplier");
            fog_enabled_location_=uniform(program_,"fog_enabled");fog_color_location_=uniform(program_,"fog_color");
            fog_start_location_=uniform(program_,"fog_start");fog_end_location_=uniform(program_,"fog_end");
            camera_location_=uniform(program_,"camera_position");environment_location_=uniform(program_,"has_environment_map");
            lightmap_location_=uniform(program_,"has_lightmap");lightmap_strength_location_=uniform(program_,"lightmap_strength");
            sky_b_location_=uniform(program_,"sky_b");sky_c_location_=uniform(program_,"sky_c");
            sky_location_=uniform(program_,"sky");sky_blend_location_=uniform(program_,"sky_blend");
            fade_location_=uniform(program_,"has_fade_map");height_location_=uniform(program_,"has_height_map");
            ocean_location_=uniform(program_,"source_ocean");time_location_=uniform(program_,"source_time");
            scroll_location_=uniform(program_,"source_scroll");
            roofed_location_=uniform(program_,"scene_roofed");
            weather_wind_location_=uniform(program_,"weather_wind");weather_state_location_=uniform(program_,"weather_state");
            weather_misc_location_=uniform(program_,"weather_misc");weather_sway_location_=uniform(program_,"weather_sway");
            scene_fog_location_=uniform(program_,"scene_fog");
            glUseProgram_(program_); glUniform1i_(uniform(program_,"diffuse_map"),0);
            glUniform1i_(uniform(program_,"normal_map"),1);
            glUniform1i_(uniform(program_,"environment_map"),2);glUniform1i_(uniform(program_,"lightmap_map"),3);
            glUniform1i_(uniform(program_,"fade_map"),4);glUniform1i_(uniform(program_,"shadow_map"),5);
            glUniform1i_(uniform(program_,"water_color_map"),6);glUniform1i_(uniform(program_,"water_depth_map"),7);glUniform1i_(uniform(program_,"height_map"),8);
            glUniform1i_(uniform(program_,"shadow_far_map"),9);
            light_location_=uniform(program_,"light_view_projection");
            far_light_location_=uniform(program_,"far_light_view_projection");shadow_split_location_=uniform(program_,"shadow_split");
            normal_matrix_location_=uniform(program_,"normal_matrix");grayscale_location_=uniform(program_,"grayscale");
            draw_uniforms_={{sky_location_},{ocean_location_},{fog_enabled_location_},{normal_location_},{environment_location_},{lightmap_location_},
                {fade_location_},{height_location_},{texture_location_},{cutoff_location_},{scroll_location_},{weather_sway_location_},{color_location_}};
            glUseProgram_(overlay_program_); glUniform1i_(uniform(overlay_program_,"overlay_map"),0);
            glGenVertexArrays_(1,&overlay_vao_);
            glGenTextures(1,&overlay_texture_);
            glow_program_=link(glow_vertex,glow_fragment);
            glow_mode_location_=uniform(glow_program_,"mode");glow_step_location_=uniform(glow_program_,"step_gain");
            glUseProgram_(glow_program_);glUniform1i_(uniform(glow_program_,"first"),0);glUniform1i_(uniform(glow_program_,"second"),1);
            glGenTextures(2,glow_textures_);glGenFramebuffers_(2,glow_framebuffers_);
            for(int i=0;i<2;++i) {
                glBindTexture(GL_TEXTURE_2D,glow_textures_[i]);
                glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,128,128,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
                glBindFramebuffer_(GL_FRAMEBUFFER,glow_framebuffers_[i]);
                glFramebufferTexture2D_(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,glow_textures_[i],0);
                if(glCheckFramebufferStatus_(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("OpenGL glow framebuffer incomplete");
                glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
            }
            glBindFramebuffer_(GL_FRAMEBUFFER,0);
            particle_program_=link(particle_vertex,particle_fragment);
            particle_vp_location_=uniform(particle_program_,"view_projection");particle_bias_location_=uniform(particle_program_,"depth_bias");
            particle_reference_location_=uniform(particle_program_,"alpha_reference");
            particle_grayscale_location_=uniform(particle_program_,"grayscale");particle_fog_enabled_location_=uniform(particle_program_,"fog_enabled");
            particle_fog_start_location_=uniform(particle_program_,"fog_start");particle_fog_end_location_=uniform(particle_program_,"fog_end");
            glUseProgram_(particle_program_);glUniform1i_(uniform(particle_program_,"particle_map"),0);
            glGenVertexArrays_(1,&particle_vao_);glGenBuffers_(1,&particle_buffer_);
            glBindVertexArray_(particle_vao_);glBindBuffer_(GL_ARRAY_BUFFER,particle_buffer_);
            glEnableVertexAttribArray_(0);glVertexAttribPointer_(0,3,GL_FLOAT,GL_FALSE,sizeof(ParticleVertex),reinterpret_cast<const void*>(offsetof(ParticleVertex,x)));
            glEnableVertexAttribArray_(1);glVertexAttribPointer_(1,2,GL_FLOAT,GL_FALSE,sizeof(ParticleVertex),reinterpret_cast<const void*>(offsetof(ParticleVertex,u)));
            glEnableVertexAttribArray_(2);glVertexAttribPointer_(2,4,GL_FLOAT,GL_FALSE,sizeof(ParticleVertex),reinterpret_cast<const void*>(offsetof(ParticleVertex,color)));
            glBindVertexArray_(0);
            contour_program_=link(contour_vertex,contour_fragment);
            contour_vp_location_=uniform(contour_program_,"view_projection");contour_object_location_=uniform(contour_program_,"object_matrix");
            contour_offset_location_=uniform(contour_program_,"offset");contour_color_location_=uniform(contour_program_,"contour");
            contour_grayscale_location_=uniform(contour_program_,"grayscale");
            if(!options_.original_appearance) {
                shadow_program_=link(shadow_vertex,shadow_fragment);
                shadow_light_location_=uniform(shadow_program_,"light_view_projection");shadow_object_location_=uniform(shadow_program_,"object_matrix");
                shadow_color_location_=uniform(shadow_program_,"material_color");shadow_cutoff_location_=uniform(shadow_program_,"alpha_cutoff");
                shadow_texture_location_=uniform(shadow_program_,"has_texture");shadow_fade_location_=uniform(shadow_program_,"has_fade_map");
                shadow_opaque_location_=uniform(shadow_program_,"opaque_alpha");
                shadow_wind_location_=uniform(shadow_program_,"weather_wind");shadow_misc_location_=uniform(shadow_program_,"weather_misc");
                shadow_sway_location_=uniform(shadow_program_,"weather_sway");shadow_time_location_=uniform(shadow_program_,"source_time");
                glUseProgram_(shadow_program_);glUniform1i_(uniform(shadow_program_,"diffuse_map"),0);glUniform1i_(uniform(shadow_program_,"fade_map"),4);
                shadow_uniforms_={{shadow_texture_location_},{shadow_fade_location_},{shadow_opaque_location_},{shadow_cutoff_location_},{shadow_color_location_},{shadow_sway_location_}};
                shadow_depth_program_=link(shadow_vertex,shadow_depth_fragment);
                depth_light_location_=uniform(shadow_depth_program_,"light_view_projection");depth_object_location_=uniform(shadow_depth_program_,"object_matrix");
                depth_wind_location_=uniform(shadow_depth_program_,"weather_wind");depth_misc_location_=uniform(shadow_depth_program_,"weather_misc");
                depth_time_location_=uniform(shadow_depth_program_,"source_time");depth_sway_.location=uniform(shadow_depth_program_,"weather_sway");
                glGenTextures(2,shadow_textures_);glGenFramebuffers_(2,shadow_framebuffers_);
                for(int i=0;i<2;++i) {
                    glBindTexture(GL_TEXTURE_2D,shadow_textures_[i]);
                    glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT24,2048,2048,0,GL_DEPTH_COMPONENT,GL_UNSIGNED_INT,nullptr);
                    // Bilinear comparison (hardware PCF): without it the 3x3 kernel's hard texel steps show as jagged
                    // stripes across actors, which GL (no FXAA, unlike Vulkan post.frag) leaves fully aliased.
                    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_COMPARE_MODE,GL_COMPARE_REF_TO_TEXTURE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_COMPARE_FUNC,GL_LEQUAL);
                    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
                    glBindFramebuffer_(GL_FRAMEBUFFER,shadow_framebuffers_[i]);
                    glFramebufferTexture2D_(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,shadow_textures_[i],0);
                    glDrawBuffer(GL_NONE);glReadBuffer(GL_NONE);
                    if(glCheckFramebufferStatus_(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("OpenGL shadow framebuffer incomplete");
                }
                glGenTextures(2,shadow_static_textures_);glGenFramebuffers_(2,shadow_static_framebuffers_);
                for(int i=0;i<2;++i) {
                    glBindTexture(GL_TEXTURE_2D,shadow_static_textures_[i]);
                    glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT24,2048,2048,0,GL_DEPTH_COMPONENT,GL_UNSIGNED_INT,nullptr);
                    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                    glBindFramebuffer_(GL_FRAMEBUFFER,shadow_static_framebuffers_[i]);
                    glFramebufferTexture2D_(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,shadow_static_textures_[i],0);
                    glDrawBuffer(GL_NONE);glReadBuffer(GL_NONE);
                    if(glCheckFramebufferStatus_(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("OpenGL static shadow framebuffer incomplete");
                }
                glBindFramebuffer_(GL_FRAMEBUFFER,0);check("allocating shadow map");
            }
            glGenFramebuffers_(1,&framebuffer_); glGenTextures(1,&color_); glGenRenderbuffers_(1,&depth_);
            glGenFramebuffers_(1,&water_framebuffer_);glGenTextures(2,water_textures_);
            if(SDL_getenv("IKV_GPU_TIMING")) {
                glGenQueries_(timing_ring*timed_passes,&timing_queries_[0][0]);timing_=true;
                std::fprintf(stderr,"OpenGL gpu timing enabled (GL_TIME_ELAPSED, passes shadow/opaque/water/alpha/post)\n");
            }
            int w=0,h=0;
            if(!SDL_GetWindowSizeInPixels(window_,&w,&h)) fail("SDL window extent");
            resize(w,h);
            progress();
        } catch (...) { destroy(); throw; }
    }
    ~GlRenderer() override { destroy(); }
    void set_camera(const Camera& c) override { validate_camera(c); options_.camera=c; }
    const Camera& camera() const override { return options_.camera; }
    void resize(int width,int height) override {
        if(width<=0 || height<=0) return; // SDL minimized extent; preserve last completed frame.
        if(width>max_texture_size_ || height>max_texture_size_) throw std::invalid_argument("render extent exceeds OpenGL texture limit");
        current();
        progress();
        glBindTexture(GL_TEXTURE_2D,color_);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        progress();
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        progress();
        glBindRenderbuffer_(GL_RENDERBUFFER,depth_); glRenderbufferStorage_(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,width,height);
        progress();
        glBindFramebuffer_(GL_FRAMEBUFFER,framebuffer_);
        glFramebufferTexture2D_(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color_,0);
        glFramebufferRenderbuffer_(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth_);
        if(glCheckFramebufferStatus_(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("OpenGL scene framebuffer incomplete");
        // NONCANONICAL enhanced water: colour + depth copy of the opaque scene (copy_water), sampled unfiltered-depth.
        for(int i=0;i<2;++i) {
            glBindTexture(GL_TEXTURE_2D,water_textures_[i]);
            if(i)glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT24,width,height,0,GL_DEPTH_COMPONENT,GL_UNSIGNED_INT,nullptr);
            else glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,i?GL_NEAREST:GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,i?GL_NEAREST:GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        }
        glBindFramebuffer_(GL_FRAMEBUFFER,water_framebuffer_);
        glFramebufferTexture2D_(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,water_textures_[0],0);
        glFramebufferTexture2D_(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,water_textures_[1],0);
        if(glCheckFramebufferStatus_(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("OpenGL water framebuffer incomplete");
        width_=width;height_=height;rendered_=false;
        check("allocating scene framebuffer");
        progress();
    }
    void set_grayscale(bool on) override { grayscale_=on; }
    void set_view_distance(float distance) override {
        if(!std::isfinite(distance)||distance<0)throw std::invalid_argument("view distance requires a finite value >= 0");
        options_.view_distance=distance;
    }
    void set_overlay(const Overlay& overlay,std::span<const std::array<int,4>> changed) override {
        current();
        if(overlay.width==0 && overlay.height==0 && overlay.rgba.empty()){overlay_present_=false;return;}
        validate_texture(overlay.width,overlay.height,overlay.rgba.size());
        progress();
        glActiveTexture_(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,overlay_texture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        if(changed.empty() || overlay.width!=overlay_width_ || overlay.height!=overlay_height_) {
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,overlay.width,overlay.height,0,GL_RGBA,GL_UNSIGNED_BYTE,overlay.rgba.data());
            overlay_width_=overlay.width; overlay_height_=overlay.height;
        } else {
            // Only the changed rectangles; the texture keeps every other texel.
            glPixelStorei(GL_UNPACK_ROW_LENGTH,overlay.width);
            for(const auto& r:changed) {
                const int x0=std::clamp(r[0],0,overlay.width),y0=std::clamp(r[1],0,overlay.height);
                const int x1=std::clamp(r[0]+std::max(r[2],0),x0,overlay.width),y1=std::clamp(r[1]+std::max(r[3],0),y0,overlay.height);
                if(x0<x1 && y0<y1) glTexSubImage2D(GL_TEXTURE_2D,0,x0,y0,x1-x0,y1-y0,GL_RGBA,GL_UNSIGNED_BYTE,overlay.rgba.data()+(static_cast<std::size_t>(y0)*overlay.width+x0)*4);
            }
            glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
        }
        progress();
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        overlay_present_=true;check("uploading HUD");
        progress();
    }
    void set_particles(const ParticleFrame& frame) override {
        current();
        if(frame.textures.size()<particle_textures_.size()){glDeleteTextures(static_cast<GLsizei>(particle_textures_.size()),particle_textures_.data());particle_textures_.clear();}
        while(particle_textures_.size()<frame.textures.size()) {
            particle_textures_.push_back(0);
            upload_texture(frame.textures[particle_textures_.size()-1],particle_textures_.back());
        }
        for(const auto& batch:frame.batches)
            if(batch.texture>=particle_textures_.size() || batch.first>frame.vertices.size() || batch.count>frame.vertices.size()-batch.first || batch.count%6 ||
               !std::isfinite(batch.depth_bias) || !std::isfinite(batch.alpha_reference))
                throw std::invalid_argument("invalid particle batch");
        if(frame.vertices.size()>static_cast<std::size_t>(std::numeric_limits<GLsizeiptr>::max())/sizeof(ParticleVertex))throw std::invalid_argument("particle frame exceeds OpenGL buffer limits");
        glBindBuffer_(GL_ARRAY_BUFFER,particle_buffer_);
        glBufferData_(GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(frame.vertices.size()*sizeof(ParticleVertex)),frame.vertices.empty()?nullptr:frame.vertices.data(),GL_STREAM_DRAW);
        particle_batches_=frame.batches;
        check("uploading particles");
    }
    void render(const Scene& scene,std::span<const RenderEntity> entities,std::uint32_t self,float dt) override {
        (void)self;
        if(!std::isfinite(dt) || dt<0) throw std::invalid_argument("invalid render delta");
        current();
        timing_collect();
        auto& set=set_;
        if(set.key!=&scene){release(set);set.key=&scene;shadow_invalidate();}
        upload(set,scene);
        refresh(set,scene);
        glBindFramebuffer_(GL_FRAMEBUFFER,framebuffer_);glViewport(0,0,width_,height_);
        glDisable(GL_SCISSOR_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glDepthMask(GL_TRUE);
        // Source ADF triangles keep D3D clockwise front faces; Vulkan's signed-area sign and projection Y flip cancel, so both use CW.
        glFrontFace(GL_CW);glCullFace(GL_BACK);
        glUseProgram_(program_);
        const auto vp=camera_view_projection(options_.camera,width_,height_);
        glUniform1i_(original_location_,options_.original_appearance?1:0);glUniform1f_(exposure_location_,options_.exposure);
        glUniform1i_(grayscale_location_,grayscale_?1:0);
        const auto& light=scene.lighting;
        validate_lighting(light);
        {SceneLighting lighting=light;if(weather_override_)lighting.weather=*weather_override_;
         wind_phase_=advance_wind_phase(wind_phase_,lighting,options_.original_appearance,dt);
         weather_=weather_uniforms(lighting,options_.original_appearance,wind_phase_);}
        const auto backdrop=weather_backdrop(weather_,light,{0.045f,0.065f,0.085f}); // wave 27: skyless zones under weather
        glClearColor(backdrop[0],backdrop[1],backdrop[2],1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        const auto ticks=static_cast<std::uint32_t>(SDL_GetTicks());
        const std::array<float,4> source_time{static_cast<float>(ticks),static_cast<float>(ticks)*0.001f,0.0f,0.0f}; // v_ocean c38
        glUniform4fv_(time_location_,1,source_time.data());
        glUniform4fv_(weather_wind_location_,1,weather_.wind.data());glUniform4fv_(weather_state_location_,1,weather_.weather.data());
        glUniform4fv_(weather_misc_location_,1,weather_.misc.data());glUniform1i_(scene_fog_location_,light.fog_enabled?1:0);
        if(shadow_program_) {
            glUseProgram_(shadow_program_);glUniform4fv_(shadow_wind_location_,1,weather_.wind.data());
            glUniform4fv_(shadow_misc_location_,1,weather_.misc.data());glUniform4fv_(shadow_time_location_,1,source_time.data());
            glUseProgram_(shadow_depth_program_);glUniform4fv_(depth_wind_location_,1,weather_.wind.data());
            glUniform4fv_(depth_misc_location_,1,weather_.misc.data());glUniform4fv_(depth_time_location_,1,source_time.data());
            glUseProgram_(program_);
        }
        contours_.clear();
        const auto eye=camera_basis(options_.camera).eye;
        build(view_,set,scene,entities,vp,eye);
        if(shadow_program_) {
            time_begin(timed_shadow);
            const auto cascades=shadow_light_cascades(options_.camera,enhanced_sun(light),options_.view_distance);
            shadow_split_=cascades.split;
            shadow_pass(0,set,scene,view_,cascades.light[0]);shadow_pass(1,set,scene,view_,cascades.light[1]);
            time_end(timed_shadow);
        }
        time_begin(timed_opaque);
        view_uniforms(scene,vp,eye);
        state_reset();
        glDepthMask(GL_FALSE);
        for(const auto& draw:view_.sky) {
            if(scene.materials[draw.material].alpha){glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);}
            else glDisable(GL_BLEND);
            draw_instance(set,scene,draw);
        }
        glDisable(GL_BLEND);glDepthMask(GL_TRUE);
        // NONCANONICAL enhanced water: source ocean draws after the rest of the opaque scene, which is copied first
        // (colour + depth) for its refraction and depth absorption (scene_fragment water_surface).
        const auto water=[&](const Draw& d){return !options_.original_appearance&&scene.materials[d.material].source_ocean;};
        // Enhanced opaque nearest first, as Vulkan build_draws: early depth rejects hidden fragments (parallax, PCF) before
        // shading. Equal distances (parts of one placement) fall back to material, mesh and instance, so coplanar parts keep a
        // stable order between frames and share state. Original appearance keeps instance order: its cheap shading gains
        // nothing from depth order, while instance order keeps consecutive draws on shared state (measured faster).
        opaque_order_.clear();bool any_water=false;
        for(const auto& draw:view_.opaque) if(draw.visible){if(water(draw))any_water=true;else opaque_order_.push_back(&draw);}
        if(!options_.original_appearance)std::sort(opaque_order_.begin(),opaque_order_.end(),[](const Draw* a,const Draw* b) {
            if(a->distance!=b->distance)return a->distance<b->distance;
            if(a->material!=b->material)return a->material<b->material;
            if(a->instance->mesh!=b->instance->mesh)return a->instance->mesh<b->instance->mesh;
            return a->instance<b->instance;
        });
        for(const auto* draw:opaque_order_) draw_instance(set,scene,*draw);
        time_end(timed_opaque);
        if(any_water) {
            time_begin(timed_water);
            glBindFramebuffer_(GL_READ_FRAMEBUFFER,framebuffer_);glBindFramebuffer_(GL_DRAW_FRAMEBUFFER,water_framebuffer_);
            glBlitFramebuffer_(0,0,width_,height_,0,0,width_,height_,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,GL_NEAREST);
            glBindFramebuffer_(GL_FRAMEBUFFER,framebuffer_);
            bind_texture(6,water_textures_[0]);bind_texture(7,water_textures_[1]);
            for(const auto& draw:view_.opaque) if(draw.visible&&water(draw)) draw_instance(set,scene,draw);
            time_end(timed_water);
        }
        time_begin(timed_alpha);
        std::sort(view_.alpha.begin(),view_.alpha.end(),[](const Draw& a,const Draw& b){return a.distance>b.distance;});
        glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_FALSE);
        for(const auto& draw:view_.alpha) if(draw.visible) draw_instance(set,scene,draw);
        // 0046ccf0 draws non-AfterHDR particles with the transparent list (glow input); 0046cbe0 draws AfterHDR ones after
        // the glow composite (0046fdb0 @470aed/@47183f, or straight after 0046ccf0 without glow @471977).
        const auto draw_particles=[&](bool after_hdr) {
            if(std::none_of(particle_batches_.begin(),particle_batches_.end(),[&](const ParticleBatch& b){return b.after_hdr==after_hdr;}))return;
            // Original 0049a780: depth test without depth write, no culling, per-system blend.
            glEnable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glEnable(GL_BLEND);
            glDisable(GL_CULL_FACE);glUseProgram_(particle_program_);glUniformMatrix4fv_(particle_vp_location_,1,GL_FALSE,vp.data());
            glUniform1i_(particle_grayscale_location_,grayscale_?1:0);
            glUniform1i_(particle_fog_enabled_location_,options_.original_appearance&&light.fog_enabled?1:0);
            glUniform1f_(particle_fog_start_location_,light.fog_start);glUniform1f_(particle_fog_end_location_,light.fog_end);
            glBindVertexArray_(particle_vao_);glActiveTexture_(GL_TEXTURE0);
            for(const auto& batch:particle_batches_) {
                if(batch.after_hdr!=after_hdr)continue;
                switch(batch.blend) {
                case ParticleBlend::add: glBlendEquation_(GL_FUNC_ADD);glBlendFunc(GL_SRC_ALPHA,GL_ONE);break;
                case ParticleBlend::alpha: glBlendEquation_(GL_FUNC_ADD);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);break;
                case ParticleBlend::subtract: glBlendEquation_(GL_FUNC_REVERSE_SUBTRACT);glBlendFunc(GL_SRC_ALPHA,GL_ONE);break;
                case ParticleBlend::min: glBlendEquation_(GL_MIN);break;
                case ParticleBlend::max: glBlendEquation_(GL_MAX);break;
                }
                glUniform1f_(particle_bias_location_,batch.depth_bias);glUniform1f_(particle_reference_location_,batch.alpha_reference);
                glBindTexture(GL_TEXTURE_2D,particle_textures_[batch.texture]);
                glDrawArrays(GL_TRIANGLES,static_cast<GLint>(batch.first),static_cast<GLsizei>(batch.count));
            }
            glBlendEquation_(GL_FUNC_ADD);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        };
        draw_particles(false);
        if(!contours_.empty()) {
            // Hilite state: CULLMODE CW (back-face shell), SRCALPHA/INVSRCALPHA, ZWRITE off, Z test on.
            // Contour draw 0047e1xx: shells i=3,2,1 with c8.w = sin(tick*.004)*.015 + i*.02.
            glEnable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_CULL_FACE);glCullFace(GL_FRONT);
            glUseProgram_(contour_program_);glUniformMatrix4fv_(contour_vp_location_,1,GL_FALSE,vp.data());
            glUniform1i_(contour_grayscale_location_,grayscale_?1:0);
            const float pulse=std::sin(static_cast<float>(ticks)*.004f)*.015f;
            for(int shell=3;shell>=1;--shell) {
                glUniform1f_(contour_offset_location_,pulse+static_cast<float>(shell)*.02f);
                for(const auto& contour:contours_) {
                    const auto& gpu=set.meshes[contour.mesh];
                    glUniformMatrix4fv_(contour_object_location_,1,GL_FALSE,contour.matrix.data());
                    glUniform3fv_(contour_color_location_,1,contour.color.data());
                    glBindVertexArray_(gpu.vao);glDrawElements(GL_TRIANGLES,gpu.count,GL_UNSIGNED_INT,nullptr);
                }
            }
            glCullFace(GL_BACK);
        }
        time_end(timed_alpha);
        time_begin(timed_post);
        glDepthMask(GL_TRUE);
        if(options_.original_appearance) {
            glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);
            glUseProgram_(glow_program_);glBindVertexArray_(overlay_vao_);glViewport(0,0,128,128);
            const float gain=scene.lighting.glow*.02f;
            const auto pass=[&](int mode,GLuint source,GLuint target,float dx,float dy) {
                glBindFramebuffer_(GL_FRAMEBUFFER,glow_framebuffers_[target]);
                glActiveTexture_(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,source);
                const std::array<float,4> step{dx,dy,gain,0};
                glUniform1i_(glow_mode_location_,mode);glUniform4fv_(glow_step_location_,1,step.data());
                glDrawArrays(GL_TRIANGLES,0,3);
            };
            glActiveTexture_(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,glow_textures_[0]);
            pass(0,color_,1,0,0);
            glActiveTexture_(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,0);
            // Vertex buffers 0x77c344 (v taps 0046ef60 +0x4a4) then 0x77c358 (u taps +0x5ac), each 2x bb3c->bb38->bb3c.
            for(int axis=0;axis<2;++axis)for(int i=0;i<2;++i){
                pass(1,glow_textures_[1],0,axis?1.f/128:0,axis?0:1.f/128);
                pass(1,glow_textures_[0],1,axis?1.f/128:0,axis?0:1.f/128);
            }
            glBindFramebuffer_(GL_FRAMEBUFFER,framebuffer_);glViewport(0,0,width_,height_);
            glEnable(GL_BLEND);glBlendEquation_(GL_FUNC_ADD);glBlendFunc(GL_SRC_COLOR,GL_ONE);
            glActiveTexture_(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,glow_textures_[1]);
            glUniform1i_(glow_mode_location_,2);glDrawArrays(GL_TRIANGLES,0,3);
            glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        }
        draw_particles(true);
        glDepthMask(GL_TRUE);
        if(overlay_present_) {
            glDisable(GL_CULL_FACE);
            glDisable(GL_DEPTH_TEST);glUseProgram_(overlay_program_);glActiveTexture_(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,overlay_texture_);
            glBindVertexArray_(overlay_vao_);glDrawArrays(GL_TRIANGLES,0,3);
        }
        glDisable(GL_BLEND);glBindVertexArray_(0);
        check("drawing original asset scene");
        progress();
        if(SDL_GetWindowFlags(window_)&SDL_WINDOW_FULLSCREEN) {
            // Original SetGammaRamp (0x467030(1,1,1.2) @0x46711f) is fullscreen-only and covers scene + HUD, like this
            // final pass. GL screenshots read the scene framebuffer (pre-ramp); Vulkan's read output_ (post-ramp).
            glBindFramebuffer_(GL_FRAMEBUFFER,0);glViewport(0,0,width_,height_);glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);
            glUseProgram_(glow_program_);glBindVertexArray_(overlay_vao_);glActiveTexture_(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,color_);
            glUniform1i_(glow_mode_location_,3);glDrawArrays(GL_TRIANGLES,0,3);glBindVertexArray_(0);
            check("drawing fullscreen gamma ramp");
        } else {
            glBindFramebuffer_(GL_READ_FRAMEBUFFER,framebuffer_);glBindFramebuffer_(GL_DRAW_FRAMEBUFFER,0);
            glBlitFramebuffer_(0,0,width_,height_,0,0,width_,height_,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        }
        time_end(timed_post);
        if(timing_)++timing_frame_;
        progress();
        if(!SDL_GL_SwapWindow(window_)) fail("OpenGL presentation");
        rendered_=true;
        if(!source_presented_ && scene.source_registry_id.has_value() && !scene.meshes.empty()) {
            std::fprintf(stdout,"Source presented source_registry_id=%u provider=opengl\n",*scene.source_registry_id);
            std::fflush(stdout);
            source_presented_=true;
        }
        progress();
    }
    void screenshot(const std::filesystem::path& path) override {
        if(!rendered_) throw std::runtime_error("screenshot requires a completed OpenGL frame");
        current();glBindFramebuffer_(GL_READ_FRAMEBUFFER,framebuffer_);glReadBuffer(GL_COLOR_ATTACHMENT0);glPixelStorei(GL_PACK_ALIGNMENT,1);
        const std::size_t row=static_cast<std::size_t>(width_)*3;
        std::vector<std::uint8_t> pixels(row*static_cast<std::size_t>(height_));
        progress();
        glReadPixels(0,0,width_,height_,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());check("reading screenshot framebuffer");
        progress();
        for(int y=0;y<height_/2;++y) std::swap_ranges(pixels.begin()+static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y)*row),pixels.begin()+static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y+1)*row),pixels.begin()+static_cast<std::ptrdiff_t>(static_cast<std::size_t>(height_-1-y)*row)); // bottom-up -> top-down
        write_rgb_image(path,width_,height_,pixels);
    }
private:
    [[noreturn]] static void fail(const char* operation){throw std::runtime_error(std::string(operation)+": "+SDL_GetError());}
    void progress() const {if(options_.progress)options_.progress();}
    void current(){progress();if(!SDL_GL_MakeCurrent(window_,context_))fail("making OpenGL context current");progress();}
    static void check(const char* operation){const auto error=glGetError();if(error!=GL_NO_ERROR)throw std::runtime_error(std::string(operation)+": OpenGL error "+std::to_string(error));}
    GLint uniform(GLuint program,const char* name){const GLint location=glGetUniformLocation_(program,name);if(location<0)throw std::runtime_error(std::string("missing shader uniform ")+name);return location;}
    GLuint compile(GLenum stage,const char* source){
        progress();
        const GLuint shader=glCreateShader_(stage);if(!shader)throw std::runtime_error("OpenGL shader allocation failed");
        try {
            glShaderSource_(shader,1,&source,nullptr);progress();glCompileShader_(shader);progress();
            GLint good=0;glGetShaderiv_(shader,GL_COMPILE_STATUS,&good);
            if(!good){GLint length=0;glGetShaderiv_(shader,GL_INFO_LOG_LENGTH,&length);std::string log(static_cast<std::size_t>(std::max(length,1)), '\0');glGetShaderInfoLog_(shader,length,nullptr,log.data());throw std::runtime_error("OpenGL shader compilation: "+log);}
        }catch(...){glDeleteShader_(shader);throw;}
        return shader;
    }
    GLuint link(const char* vertex,const char* fragment){
        const GLuint v=compile(GL_VERTEX_SHADER,vertex);GLuint f=0,p=0;
        try {f=compile(GL_FRAGMENT_SHADER,fragment);progress();p=glCreateProgram_();if(!p)throw std::runtime_error("OpenGL program allocation failed");glAttachShader_(p,v);glAttachShader_(p,f);progress();glLinkProgram_(p);progress();
            GLint good=0;glGetProgramiv_(p,GL_LINK_STATUS,&good);if(!good){GLint length=0;glGetProgramiv_(p,GL_INFO_LOG_LENGTH,&length);std::string log(static_cast<std::size_t>(std::max(length,1)), '\0');glGetProgramInfoLog_(p,length,nullptr,log.data());throw std::runtime_error("OpenGL program linking: "+log);}
        }catch(...){if(p)glDeleteProgram_(p);if(f)glDeleteShader_(f);glDeleteShader_(v);throw;}
        glDeleteShader_(v);glDeleteShader_(f);return p;
    }
    void validate_texture(int width,int height,std::size_t size) const {
        if(width<=0 || height<=0 || width>max_texture_size_ || height>max_texture_size_ ||
           static_cast<std::size_t>(width)>std::numeric_limits<std::size_t>::max()/static_cast<std::size_t>(height)/4 ||
           size!=static_cast<std::size_t>(width)*static_cast<std::size_t>(height)*4) throw std::invalid_argument("invalid RGBA texture dimensions/payload");
    }
    void validate_vertices(const Mesh& mesh) const {
        if(mesh.vertices.size()>static_cast<std::size_t>(std::numeric_limits<GLsizeiptr>::max())/sizeof(Vertex))
            throw std::invalid_argument("mesh exceeds OpenGL buffer limits");
        for(std::size_t i=0;i<mesh.vertices.size();++i) {
            if((i&1023U)==0)progress();
            const auto& vertex=mesh.vertices[i];
            const std::array<float,8> fields{vertex.x,vertex.y,vertex.z,vertex.nx,vertex.ny,vertex.nz,vertex.u,vertex.v};
            for(float value:fields)if(!std::isfinite(value))throw std::invalid_argument("non-finite mesh vertex");
            for(float value:vertex.ambient)if(!std::isfinite(value))throw std::invalid_argument("non-finite ambient color");
            for(float value:vertex.tangent)if(!std::isfinite(value))throw std::invalid_argument("non-finite tangent");
            for(float value:vertex.uv1)if(!std::isfinite(value))throw std::invalid_argument("non-finite lightmap UV");
            for(float value:vertex.color)if(!std::isfinite(value))throw std::invalid_argument("non-finite diffuse color");
        }
        progress();
    }
    static GLuint find_texture(const GlSet& set,const std::string& name) {
        if(name.empty())return GLuint{0};
        const auto found=set.texture_names.find(name);
        if(found==set.texture_names.end())throw std::runtime_error("missing original material texture: "+name);
        return found->second;
    }
    static void validate_material(const Material& material) {
        for(float value:material.color)if(!std::isfinite(value))throw std::invalid_argument("non-finite material color");
        if(!std::isfinite(material.alpha_cutoff)||material.alpha_cutoff<0||material.alpha_cutoff>1)throw std::invalid_argument("invalid material alpha cutoff");
    }
    void upload_material(GlSet& set,const Scene& scene,std::size_t index) {
        const auto& material=scene.materials[index];
        validate_material(material);
        // height: NONCANONICAL parallax relief (scene_fragment parallax()), enhanced with RendererOptions::parallax only.
        set.materials[index]={find_texture(set,material.diffuse),find_texture(set,material.normal),
            find_texture(set,material.environment),find_texture(set,material.lightmap),find_texture(set,material.luminance),
            options_.parallax&&!options_.original_appearance?find_texture(set,material.height):0,true};
        progress();
    }
    void validate_mesh(const Scene& scene,std::size_t index) const {
        const auto& mesh=scene.meshes[index];
        if(mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size()%3!=0 || mesh.indices.size()>static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()))throw std::invalid_argument("invalid triangle mesh");
        for(std::size_t i=0;i<mesh.indices.size();++i) {
            if((i&1023U)==0)progress();
            if(mesh.indices[i]>=mesh.vertices.size())throw std::invalid_argument("mesh index out of range");
        }
        validate_vertices(mesh);
        if(mesh.material>=scene.materials.size())throw std::invalid_argument("mesh material out of range");
    }
    void upload_mesh(GlSet& set,const Scene& scene,std::size_t index) {
        progress();
        validate_mesh(scene,index);
        const bool wave=index<set.ocean_meshes.size()&&set.ocean_meshes[index];
        const Mesh subdivided=wave?subdivide_ocean_mesh(scene.meshes[index]):Mesh{};
        const auto& source=wave?subdivided:scene.meshes[index];auto& gpu=set.meshes[index];
        gpu.texture=0; // Legacy texture residency is resolved lazily from the actual draw's material.
        gpu.count=static_cast<GLsizei>(source.indices.size());glGenVertexArrays_(1,&gpu.vao);glBindVertexArray_(gpu.vao);
        gpu.vertex_count=scene.meshes[index].vertices.size(); // the scene mesh's (topology check), also for a subdivided ocean
        gpu.revision=source.revision;gpu.bounds=mesh_bounds(source);gpu.height=mesh_height_range(source);
        glGenBuffers_(1,&gpu.vertices);glBindBuffer_(GL_ARRAY_BUFFER,gpu.vertices);
        progress();
        glBufferData_(GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(source.vertices.size()*sizeof(Vertex)),source.vertices.data(),GL_STATIC_DRAW);
        progress();
        glGenBuffers_(1,&gpu.indices);glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER,gpu.indices);glBufferData_(GL_ELEMENT_ARRAY_BUFFER,static_cast<GLsizeiptr>(source.indices.size()*sizeof(std::uint32_t)),source.indices.data(),GL_STATIC_DRAW);
        progress();
        glEnableVertexAttribArray_(0);glVertexAttribPointer_(0,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<const void*>(offsetof(Vertex,x)));
        glEnableVertexAttribArray_(1);glVertexAttribPointer_(1,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<const void*>(offsetof(Vertex,nx)));
        glEnableVertexAttribArray_(2);glVertexAttribPointer_(2,2,GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<const void*>(offsetof(Vertex,u)));
        glEnableVertexAttribArray_(3);glVertexAttribPointer_(3,4,GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<const void*>(offsetof(Vertex,ambient)));
        glEnableVertexAttribArray_(4);glVertexAttribPointer_(4,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<const void*>(offsetof(Vertex,tangent)));
        glEnableVertexAttribArray_(5);glVertexAttribPointer_(5,2,GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<const void*>(offsetof(Vertex,uv1)));
        glEnableVertexAttribArray_(6);glVertexAttribPointer_(6,4,GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<const void*>(offsetof(Vertex,color)));
        // Skinned meshes: skinning rewrites only position, normal and tangent (scene_animation.cpp), so those come from a
        // separate 36-byte stream that refresh() re-specifies; uv, ambient, lightmap uv and colour stay in the static buffer.
        if(!source.influences.empty()) {
            glGenBuffers_(1,&gpu.stream);glBindBuffer_(GL_ARRAY_BUFFER,gpu.stream);
            upload_stream(source);
            glVertexAttribPointer_(0,3,GL_FLOAT,GL_FALSE,sizeof(SkinStream),reinterpret_cast<const void*>(offsetof(SkinStream,position)));
            glVertexAttribPointer_(1,3,GL_FLOAT,GL_FALSE,sizeof(SkinStream),reinterpret_cast<const void*>(offsetof(SkinStream,normal)));
            glVertexAttribPointer_(4,3,GL_FLOAT,GL_FALSE,sizeof(SkinStream),reinterpret_cast<const void*>(offsetof(SkinStream,tangent)));
        }
        progress();
    }
    // Packs the skinned attributes and re-specifies (orphans) the bound GL_ARRAY_BUFFER: the driver neither stalls on nor
    // copies around the previous frame's draws still reading it. GL_DYNAMIC_DRAW, not STREAM: Mesa radeonsi places STREAM
    // buffers in system memory (GTT), which measured slower vertex fetch for the whole opaque pass.
    void upload_stream(const Mesh& mesh) {
        skin_stream_.resize(mesh.vertices.size());
        for(std::size_t i=0;i<mesh.vertices.size();++i) {
            const auto& v=mesh.vertices[i];skin_stream_[i]={{v.x,v.y,v.z},{v.nx,v.ny,v.nz},v.tangent};
        }
        glBufferData_(GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(skin_stream_.size()*sizeof(SkinStream)),skin_stream_.data(),GL_DYNAMIC_DRAW);
    }
    void release(GpuMesh& mesh) noexcept {
        if(mesh.indices)glDeleteBuffers_(1,&mesh.indices);
        if(mesh.vertices)glDeleteBuffers_(1,&mesh.vertices);
        if(mesh.stream)glDeleteBuffers_(1,&mesh.stream);
        if(mesh.vao)glDeleteVertexArrays_(1,&mesh.vao);
        mesh={};
    }
    void release(GlSet& set) noexcept {
        for(auto& mesh:set.meshes)release(mesh);
        if(!set.textures.empty())glDeleteTextures(static_cast<GLsizei>(set.textures.size()),set.textures.data());
        GlSet empty;empty.key=set.key;set=std::move(empty);
    }
    // Upload plan for the Scene's current structure. Actor attach/detach/replacement keeps every other mesh (moved by key)
    // and every texture; only a shrunk texture table reloads the set.
    void plan(GlSet& set,const Scene& scene) {
        progress();
        shadow_invalidate();
        if(set.textures.size()>scene.textures.size())release(set);
        else if(set.planned) {
            const auto remap=scene_mesh_remap(set.mesh_keys,scene);
            std::vector<GpuMesh> kept(scene.meshes.size());
            for(std::size_t i=0;i<remap.size();++i)if(remap[i]!=SIZE_MAX)kept[i]=std::exchange(set.meshes[remap[i]],GpuMesh{});
            for(auto& mesh:set.meshes)release(mesh);
            set.meshes=std::move(kept);
        }
        if(scene.textures.size()>static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()))
            throw std::invalid_argument("too many scene textures");
        set.material_usage=scene_material_usage(scene);
        progress();
        set.textures.resize(scene.textures.size()); // names stay 0 from next_texture on until uploaded
        set.materials.assign(scene.materials.size(),GpuMaterial{});set.next_material=0;
        set.meshes.resize(scene.meshes.size());
        set.mesh_plan.assign(scene.meshes.size(),0);set.next_mesh=0;
        set.ocean_meshes=options_.original_appearance?std::vector<bool>(scene.meshes.size(),false):ocean_wave_meshes(scene);
        if(scene.instances.empty()) {
            for(std::uint32_t i=0;i<scene.meshes.size();++i) {
                if((i&1023U)==0)progress();
                set.mesh_plan[i]=scene_source_color_draw(scene,i)?2:1;
            }
        }
        for(std::size_t i=0;i<scene.instances.size();++i) {
            if((i&1023U)==0)progress();
            const auto& instance=scene.instances[i];
            if(instance.mesh>=scene.meshes.size())throw std::runtime_error("scene instance has invalid mesh index");
            const bool color_draw=scene_source_color_draw(scene,instance.mesh,instance.material,instance.source_ordinary_color_participates);
            if(!instance.occlusion_proxy)set.mesh_plan[instance.mesh]=std::max<std::uint8_t>(set.mesh_plan[instance.mesh],color_draw?2:1);
        }
        // geometry_revision stays: refresh() then updates retained skins whose revision moved since their upload.
        set.mesh_keys.resize(scene.meshes.size());for(std::size_t i=0;i<scene.meshes.size();++i)set.mesh_keys[i]=scene.meshes[i].key;
        set.mesh_key=scene.mesh_key;set.planned=true;
    }
    // Uploads what the Scene still lacks: new textures, then materials, then meshes (retained ones stay).
    void upload(GlSet& set,const Scene& scene) {
        try {
            if(!set.planned||set.mesh_key!=scene.mesh_key||set.meshes.size()!=scene.meshes.size()||
               set.textures.size()!=scene.textures.size()||set.materials.size()!=scene.materials.size())plan(set,scene);
            for(;set.next_texture<set.textures.size();++set.next_texture) {
                const auto i=set.next_texture;
                // Derived parallax relief (Material::height) is never bound unless enhanced parallax is on: not uploaded.
                if(scene.textures[i].name.ends_with("#parallax")&&(options_.original_appearance||!options_.parallax))continue;
                upload_texture(scene.textures[i],set.textures[i]);
                if(!set.texture_names.emplace(scene.textures[i].name,set.textures[i]).second)throw std::invalid_argument("duplicate scene texture name");
                progress();
            }
            for(;set.next_material<set.materials.size();++set.next_material) {
                const auto i=set.next_material;
                if(!set.material_usage[i]){validate_material(scene.materials[i]);continue;}
                upload_material(set,scene,i);
            }
            for(;set.next_mesh<set.meshes.size();++set.next_mesh) {
                const auto i=set.next_mesh;
                if(!set.mesh_plan[i]||set.meshes[i].vao)continue; // Retained meshes were validated on upload.
                if(set.mesh_plan[i]==1)validate_mesh(scene,i);else upload_mesh(set,scene,i);
            }
            glBindVertexArray_(0);check("uploading original asset scene");
        }catch(...){release(set);throw;}
    }
    // Meshes whose vertices moved since their upload (Scene::geometry_revision): skinned ones re-specify only their
    // position/normal/tangent stream, others their whole buffer (both orphaned, never a sync stall). Bounds follow the new
    // pose: skinned culling spheres (colour and shadow) were the upload pose's before wave 27.
    void refresh(GlSet& set,const Scene& scene) {
        if(set.geometry_revision==scene.geometry_revision)return;
        for(std::size_t i=0;i<set.meshes.size();++i) {
            if((i&1023U)==0)progress();
            const auto& mesh=scene.meshes[i];auto& gpu=set.meshes[i];
            if(!gpu.vao)continue;
            if(mesh.vertices.size()!=gpu.vertex_count)throw std::runtime_error("animated mesh topology changed without a new scene");
            if(mesh.revision==gpu.revision||(i<set.ocean_meshes.size()&&set.ocean_meshes[i]))continue; // ocean: static, uploaded subdivided
            validate_vertices(mesh);
            progress();
            if(gpu.stream){glBindBuffer_(GL_ARRAY_BUFFER,gpu.stream);upload_stream(mesh);}
            else {glBindBuffer_(GL_ARRAY_BUFFER,gpu.vertices);glBufferData_(GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(mesh.vertices.size()*sizeof(Vertex)),mesh.vertices.data(),GL_DYNAMIC_DRAW);}
            gpu.revision=mesh.revision;
            gpu.bounds=mesh_bounds(mesh);
            progress();
        }
        check("uploading recovered skeletal animation");set.geometry_revision=scene.geometry_revision;
    }
    static void validate_lighting(const SceneLighting& light) {
        for(const auto* values:{&light.sun_direction,&light.ambient_multiplier,&light.diffuse_multiplier,&light.sky_b,&light.sky_c,&light.fog_color})
            for(float value:*values)if(!std::isfinite(value))throw std::invalid_argument("non-finite scene lighting");
        if(light.fog_enabled&&(!std::isfinite(light.fog_start)||!std::isfinite(light.fog_end)||light.fog_end<=light.fog_start))
            throw std::invalid_argument("invalid source fog interval");
        if(!std::isfinite(light.lightmap_strength))throw std::invalid_argument("non-finite source lightmap multiplier");
        if(!std::isfinite(light.sky_blend)||!std::isfinite(light.sky_rotation))throw std::invalid_argument("non-finite source sky parameters");
    }
    // Scene uniforms (program_ bound): camera and the Scene's own lighting/fog/sky constants.
    void view_uniforms(const Scene& scene,const Matrix4& vp,const std::array<float,3>& eye) {
        const auto& light=scene.lighting;
        // Enhanced lighting needs a sun even without source constants; original paths only read it with them.
        const auto sun=enhanced_sun(light);
        glUniformMatrix4fv_(vp_location_,1,GL_FALSE,vp.data());
        glUniform1i_(source_light_location_,light.source_values?1:0);glUniform3fv_(sun_location_,1,sun.data());
        glUniform3fv_(ambient_location_,1,light.ambient_multiplier.data());glUniform3fv_(diffuse_location_,1,light.diffuse_multiplier.data());
        glUniform3fv_(fog_color_location_,1,light.fog_color.data());
        glUniform1f_(fog_start_location_,light.fog_start);glUniform1f_(fog_end_location_,light.fog_end);
        glUniform3fv_(sky_b_location_,1,light.sky_b.data());glUniform3fv_(sky_c_location_,1,light.sky_c.data());
        glUniform1f_(lightmap_strength_location_,light.lightmap_strength);
        glUniform1f_(sky_blend_location_,light.sky_blend);glUniform1i_(roofed_location_,light.roofed?1:0);
        glUniform3fv_(camera_location_,1,eye.data());
    }
    // Draw lists of the view (and its hover contours). Enhanced keeps off-screen opaque draws for the shadow map.
    void build(View& view,GlSet& set,const Scene& scene,std::span<const RenderEntity> entities,const Matrix4& vp,
               const std::array<float,3>& eye) {
        view.sky.clear();view.opaque.clear();view.alpha.clear();
        view.opaque.reserve(scene.instances.size());view.alpha.reserve(scene.instances.size());
        const auto& light=scene.lighting;
        const auto planes=frustum_planes(vp);
        const float sky_cosine=std::cos(light.sky_rotation),sky_sine=std::sin(light.sky_rotation);
        for(const auto& instance:scene.instances) {
            if(instance.mesh>=set.meshes.size()) throw std::runtime_error("scene instance has invalid mesh index");
            if(instance.occlusion_proxy)continue; // Source class render slot is ret 4, not visible geometry.
            Matrix4 matrix=instance.transform.matrix;
            const RenderEntity* entity=nullptr;
            if(instance.entity_id!=0) {
                const auto p=std::find_if(entities.begin(),entities.end(),[&](const RenderEntity& value){return value.id==instance.entity_id;});
                if(p==entities.end()) continue;
                entity=&*p;
                matrix=entity_instance_transform(scene,instance,entity->x,entity->ground_y,entity->z,entity->yaw).matrix;
            }
            for(float v:matrix) if(!std::isfinite(v)) throw std::runtime_error("non-finite scene instance transform");
            const auto& mesh=scene.meshes[instance.mesh];
            const auto material=instance.material==UINT32_MAX?mesh.material:instance.material;
            if(material>=scene.materials.size())throw std::runtime_error("scene instance has invalid material index");
            std::array<float,4> scroll{};
            if(instance.ocean&&instance.source_ordinary_color_participates)scroll=advance_source_scroll(set.scroll,static_cast<std::uint32_t>(SDL_GetTicks()));
            if(!scene_source_color_draw(scene,instance.mesh,instance.material,instance.source_ordinary_color_participates))continue;
            if(!set.materials[material].referenced)upload_material(set,scene,material);
            if(!set.meshes[instance.mesh].vao)upload_mesh(set,scene,instance.mesh);
            if(scene.materials[material].diffuse.empty()&&!mesh.texture.empty()&&!set.meshes[instance.mesh].texture)
                set.meshes[instance.mesh].texture=find_texture(set,mesh.texture);
            if(instance.sky) {
                if(!light.source_values)throw std::runtime_error("original sky class requires source lighting constants");
                matrix={sky_cosine,0,-sky_sine,0,0,1,0,0,sky_sine,0,sky_cosine,0,eye[0],eye[1],eye[2],1};
                if(!set.materials[material].diffuse&&!set.meshes[instance.mesh].texture)continue;
            }
            // Frustum culling: sky and source ocean (vertex-displaced) always draw. Skinned meshes ignore the view distance
            // (as Vulkan) but are frustum-tested against their current-pose bounds (refresh()).
            // Culled alpha draws stay in alpha so its distance sort sees the unculled list.
            const bool skinned=!mesh.influences.empty();
            const bool visible=instance.sky||instance.ocean||scene.materials[material].source_ocean||
                ((skinned||sphere_within(eye,matrix,set.meshes[instance.mesh].bounds,options_.view_distance))&&
                 sphere_visible(planes,matrix,set.meshes[instance.mesh].bounds));
            // Enhanced keeps off-screen opaque draws too: they still cast into the shadow map.
            if(!visible&&!scene.materials[material].alpha&&options_.original_appearance)continue;
            bool fog=false;
            if(visible&&(options_.original_appearance||scene.materials[material].source_ocean)&&light.fog_enabled&&!instance.sky) {
                if(instance.node>=scene.nodes.size())throw std::runtime_error("source fog requires original model bounds");
                const auto center=entity?
                    entity_source_bounds_center(scene,instance,entity->x,entity->ground_y,entity->z,entity->yaw):
                    scene.nodes[instance.node].source_bounds_center;
                fog=source_fog_enabled(instance.source_half_extents,center,matrix,eye,light.fog_start);
            }
            // NONCANONICAL wave 27: non-ocean draws carry the wind sway vector in the (otherwise unused) scroll slot.
            if(!instance.ocean&&!instance.sky)scroll=weather_sway(weather_,scene.materials[material],matrix,set.meshes[instance.mesh].height,
                instance.entity_id!=0||!mesh.influences.empty());
            const float dx=matrix[12]-eye[0],dy=matrix[13]-eye[1],dz=matrix[14]-eye[2];
            Draw draw{&instance,matrix,dx*dx+dy*dy+dz*dz,material,fog,scroll,visible};
            // NONCANONICAL enhanced water (authored alpha-blended) is drawn opaque with depth over the scene copy.
            const bool alpha=scene.materials[material].alpha&&(options_.original_appearance||!scene.materials[material].source_ocean);
            (instance.sky?view.sky:(alpha?view.alpha:view.opaque)).push_back(draw);
            if(!visible)continue;
            if(entity&&!instance.sky&&entity->contour!=std::array<float,3>{}) {
                for(float v:entity->contour)if(!std::isfinite(v))throw std::invalid_argument("non-finite entity contour colour");
                contours_.push_back({instance.mesh,matrix,entity->contour});
            }
        }
    }
    // Enhanced sun shadow slots 0/1: near/far; leaves both bound for program_.
    void shadow_pass(int slot,const GlSet& set,const Scene& scene,const View& view,const Matrix4& light_vp) {
        const auto s=static_cast<std::size_t>(slot);
        shadow_lights_[s]=light_vp;
        // NONCANONICAL enhanced water: the source ocean casts no shadow. Keep near-clamped casters, cull the rest by light.
        // Static casters (no entity, skin or per-frame sway/scroll vector) go to the cascade's cache; the rest are dynamic.
        const auto caster_planes=shadow_caster_planes(light_vp);
        shadow_static_.clear();shadow_dynamic_.clear();shadow_keys_.clear();
        // Shadow-map texel rectangles {x0,y0,x1,y1} covered by this frame's dynamic casters: light-space bounding square of each
        // world sphere (grown by the sway amplitude) plus 2 texels. A cache hit restores only last frame's rectangles.
        auto& dirty=shadow_dirty_;dirty.clear();
        const float sx=std::sqrt(light_vp[0]*light_vp[0]+light_vp[4]*light_vp[4]+light_vp[8]*light_vp[8]);
        const float sy=std::sqrt(light_vp[1]*light_vp[1]+light_vp[5]*light_vp[5]+light_vp[9]*light_vp[9]);
        for(const auto* list:{&view.opaque,&view.alpha})for(const auto& draw:*list) {
            const auto mesh=draw.instance->mesh;
            if(scene.materials[draw.material].source_ocean||!sphere_visible(caster_planes,draw.matrix,set.meshes[mesh].bounds))continue;
            if(draw.instance->entity_id!=0||!scene.meshes[mesh].influences.empty()||draw.scroll!=std::array<float,4>{}) {
                shadow_dynamic_.push_back(&draw);
                const auto w=world_sphere(draw.matrix,set.meshes[mesh].bounds);
                const float r=w[3]+(draw.instance->ocean?INFINITY:std::abs(draw.scroll[2]));
                const float cx=light_vp[0]*w[0]+light_vp[4]*w[1]+light_vp[8]*w[2]+light_vp[12];
                const float cy=light_vp[1]*w[0]+light_vp[5]*w[1]+light_vp[9]*w[2]+light_vp[13];
                const auto texel=[](float ndc,int bias){return static_cast<int>(std::clamp(std::floor((ndc*.5f+.5f)*2048.f),-4.f,2052.f))+bias;};
                if(!std::isfinite(r*sx+r*sy+cx+cy))dirty.push_back({0,0,2048,2048});
                else dirty.push_back({texel(cx-r*sx,-2),texel(cy-r*sy,-2),texel(cx+r*sx,3),texel(cy+r*sy,3)});
                continue;
            }
            shadow_static_.push_back(&draw);
            shadow_keys_.push_back({draw.instance,mesh,draw.material,scene.meshes[mesh].revision,draw.matrix});
        }
        double dirty_texels=0;
        for(auto& rect:dirty) {
            for(auto& v:rect)v=std::clamp(v,0,2048);
            if(rect[2]>rect[0]&&rect[3]>rect[1])dirty_texels+=double(rect[2]-rect[0])*double(rect[3]-rect[1]);
        }
        // Many or mostly-covering rectangles: one full restore is cheaper than many overlapping ones.
        if(dirty.size()>64||dirty_texels>2048.0*2048.0){dirty.assign(1,{0,0,2048,2048});dirty_texels=2048.0*2048.0;}
        std::size_t static_indices=0;for(const auto* draw:shadow_static_)static_indices+=static_cast<std::size_t>(set.meshes[draw->instance->mesh].count);
        shadow_static_triangles_+=double(static_indices)/3;shadow_dirty_texels_+=dirty_texels;
        auto& cache=shadow_cache_[s];
        // Stable: the same light matrix and static caster list as last frame. The cache is filled on the first stable frame
        // and reused while stability lasts; animated static placements or a moving camera never pay for the copy.
        // Worth caching only while restoring the dirty texels costs less than redrawing the static casters (RX 7900 XTX: a full
        // 2048^2 depth restore measured about as costly as ~200k static triangles; wind-swayed foliage is dynamic and large).
        const bool worth=double(static_indices)/3>0.05*dirty_texels;
        const bool stable=cache.has_light&&cache.light==light_vp&&cache.keys==shadow_keys_&&worth;
        const bool hit=cache.valid&&stable;
        ++(hit?shadow_hits_:shadow_misses_);
        state_reset();
        glViewport(0,0,2048,2048);
        // Vulkan shadow pipeline: back faces culled, depth bias 1.25 constant / 1.75 slope. The light matrix keeps
        // Vulkan's Y flip, which reverses GL winding, so source CW front faces rasterise CCW here.
        glDepthMask(GL_TRUE);glFrontFace(GL_CCW);
        glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(1.75f,1.25f);
        // Casters sunward of the light's near plane (light distance only max(50,3·zoom)) clamp to depth 0 and still cast:
        // clipped, a building's long low-sun shadow came and went with the camera zoom.
        glEnable(GL_DEPTH_CLAMP);
        glUseProgram_(shadow_depth_program_);glUniformMatrix4fv_(depth_light_location_,1,GL_FALSE,light_vp.data());
        glUseProgram_(shadow_program_);glUniformMatrix4fv_(shadow_light_location_,1,GL_FALSE,light_vp.data());
        const auto blit_depth=[&](GLuint from,GLuint to,const std::array<int,4>& r) {
            if(r[0]>=r[2]||r[1]>=r[3])return;
            glBindFramebuffer_(GL_READ_FRAMEBUFFER,from);glBindFramebuffer_(GL_DRAW_FRAMEBUFFER,to);
            glBlitFramebuffer_(r[0],r[1],r[2],r[3],r[0],r[1],r[2],r[3],GL_DEPTH_BUFFER_BIT,GL_NEAREST);
        };
        // Hit: the map holds the static depth plus last frame's dynamic casters inside cache.dirty; restore just those.
        if(hit)for(const auto& rect:cache.dirty)blit_depth(shadow_static_framebuffers_[slot],shadow_framebuffers_[slot],rect);
        else {
            glBindFramebuffer_(GL_FRAMEBUFFER,shadow_framebuffers_[slot]);glClear(GL_DEPTH_BUFFER_BIT);
            draw_casters(set,scene,shadow_static_);
            cache.valid=stable;
            if(stable)blit_depth(shadow_framebuffers_[slot],shadow_static_framebuffers_[slot],{0,0,2048,2048});
        }
        cache.dirty.swap(dirty);
        cache.keys.swap(shadow_keys_);
        cache.light=light_vp;cache.has_light=true;
        glBindFramebuffer_(GL_FRAMEBUFFER,shadow_framebuffers_[slot]);
        draw_casters(set,scene,shadow_dynamic_);
        glDisable(GL_DEPTH_CLAMP);glDisable(GL_POLYGON_OFFSET_FILL);glFrontFace(GL_CW);
        glBindFramebuffer_(GL_FRAMEBUFFER,framebuffer_);glViewport(0,0,width_,height_);
        glUseProgram_(program_);shadow_bind();
        check("drawing shadow map");
    }
    // Casters without an alpha cutoff first (depth-only program), then cutoff casters (alpha-tested shadow program).
    void draw_casters(const GlSet& set,const Scene& scene,const std::vector<const Draw*>& casters) {
        bool any_cutoff=false;
        glUseProgram_(shadow_depth_program_);
        for(const auto* draw:casters) {
            if(scene.materials[draw->material].alpha_cutoff>0){any_cutoff=true;continue;}
            set_cull(!scene.materials[draw->material].double_sided);
            glUniformMatrix4fv_(depth_object_location_,1,GL_FALSE,draw->matrix.data());set4f(depth_sway_,draw->scroll);
            const auto& gpu=set.meshes[draw->instance->mesh];
            bind_vao(gpu.vao);glDrawElements(GL_TRIANGLES,gpu.count,GL_UNSIGNED_INT,nullptr);
        }
        if(!any_cutoff)return;
        glUseProgram_(shadow_program_);
        for(const auto* draw:casters) if(scene.materials[draw->material].alpha_cutoff>0) draw_shadow(set,scene,*draw);
    }
    void shadow_invalidate(){for(auto& cache:shadow_cache_){cache.valid=cache.has_light=false;cache.keys.clear();}}
    void shadow_bind() {
        glUniformMatrix4fv_(light_location_,1,GL_FALSE,shadow_lights_[0].data());
        glUniformMatrix4fv_(far_light_location_,1,GL_FALSE,shadow_lights_[1].data());
        glUniform1f_(shadow_split_location_,shadow_split_);
        bind_texture(5,shadow_textures_[0]);bind_texture(9,shadow_textures_[1]);
    }
    // Cached state helpers (see draw_uniforms_).
    void set1i(UniformInt& u,int v){if(u.value!=v){u.value=v;glUniform1i_(u.location,v);}}
    void set1f(UniformFloat& u,float v){if(!(u.value==v)){u.value=v;glUniform1f_(u.location,v);}}
    void set4f(UniformVec4& u,const std::array<float,4>& v){if(u.value!=v){u.value=v;glUniform4fv_(u.location,1,v.data());}}
    void bind_texture(unsigned unit,GLuint texture) {
        if(bound_textures_[unit]==texture)return;
        if(active_unit_!=GL_TEXTURE0+unit){active_unit_=GL_TEXTURE0+unit;glActiveTexture_(active_unit_);}
        glBindTexture(GL_TEXTURE_2D,texture);bound_textures_[unit]=texture;
    }
    void bind_vao(GLuint vao){if(bound_vao_!=vao){bound_vao_=vao;glBindVertexArray_(vao);}}
    void set_cull(bool on){if(cull_!=int(on)){cull_=on;if(on)glEnable(GL_CULL_FACE);else glDisable(GL_CULL_FACE);}}
    // Forgets texture-unit, active-unit, VAO and cull state (other code changed them directly).
    void state_reset(){bound_textures_.fill(~GLuint{0});active_unit_=0;bound_vao_=~GLuint{0};cull_=-1;}
    void time_begin(TimedPass pass){if(timing_)glBeginQuery_(GL_TIME_ELAPSED,timing_queries_[timing_frame_%timing_ring][pass]);}
    void time_end(TimedPass pass){if(!timing_)return;glEndQuery_(GL_TIME_ELAPSED);timing_issued_[timing_frame_%timing_ring]|=1U<<pass;}
    // Reads the ring slot about to be reused (issued timing_ring frames ago); a frame whose results are not yet available
    // is dropped rather than waited for.
    void timing_collect() {
        if(!timing_)return;
        const unsigned slot=timing_frame_%timing_ring,issued=timing_issued_[slot];
        timing_issued_[slot]=0;
        if(!issued)return;
        double ms[timed_passes]{};
        for(int pass=0;pass<timed_passes;++pass) {
            if(!(issued&(1U<<pass)))continue;
            GLint ready=0;glGetQueryObjectiv_(timing_queries_[slot][pass],GL_QUERY_RESULT_AVAILABLE,&ready);
            if(!ready)return;
            GLuint64 ns=0;glGetQueryObjectui64v_(timing_queries_[slot][pass],GL_QUERY_RESULT,&ns);ms[pass]=static_cast<double>(ns)*1e-6;
        }
        for(int pass=0;pass<timed_passes;++pass)timing_sum_[pass]+=ms[pass];
        if(++timing_frames_<30)return;
        const auto avg=[&](int pass){return timing_sum_[pass]/timing_frames_;};
        std::fprintf(stderr,"OpenGL gpu timing frames=30 scenePassMs=%.4f shadowMs=%.4f opaqueMs=%.4f waterMs=%.4f alphaMs=%.4f postMs=%.4f totalMs=%.4f shadowCacheHits=%u/%u shadowStaticTris=%.0f shadowDirty=%.3f\n",
            avg(timed_opaque)+avg(timed_water)+avg(timed_alpha),avg(timed_shadow),avg(timed_opaque),avg(timed_water),avg(timed_alpha),avg(timed_post),
            avg(timed_shadow)+avg(timed_opaque)+avg(timed_water)+avg(timed_alpha)+avg(timed_post),shadow_hits_,shadow_hits_+shadow_misses_,
            shadow_static_triangles_/std::max(1u,shadow_hits_+shadow_misses_),shadow_dirty_texels_/std::max(1u,shadow_hits_+shadow_misses_)/(2048.0*2048.0));
        shadow_hits_=shadow_misses_=0;shadow_static_triangles_=shadow_dirty_texels_=0;timing_frames_=0;for(auto& sum:timing_sum_)sum=0;
    }
    void upload_texture(const Texture& source,GLuint& name) {
        if(source.width>static_cast<unsigned>(std::numeric_limits<int>::max()) || source.height>static_cast<unsigned>(std::numeric_limits<int>::max()))throw std::invalid_argument("oversized scene texture");
        if(source.mips.empty() || source.mips.size()>32)throw std::invalid_argument("scene texture lacks a bounded original DDS mip chain");
        Texture expanded; // only without S3TC: the BC blocks decoded exactly like the CPU decoder
        if(source.format!=assets::DdsFormat::rgba8&&!s3tc_) {
            auto image=assets::decompress_dds({source.width,source.height,source.data,source.mips,source.format});
            expanded.width=source.width;expanded.height=source.height;expanded.data=std::move(image.rgba);expanded.mips=std::move(image.mips);
        }
        const auto& texture=expanded.data.empty()?source:expanded;
        const auto compressed=texture.format==assets::DdsFormat::bc1?GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
            texture.format==assets::DdsFormat::bc2?GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        glGenTextures(1,&name);glBindTexture(GL_TEXTURE_2D,name);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        std::uint32_t mip_width=texture.width,mip_height=texture.height;
        std::size_t next_offset=0;
        for(std::size_t level=0;level<texture.mips.size();++level) {
            const auto& mip=texture.mips[level];
            if(mip.width!=mip_width || mip.height!=mip_height || mip.offset!=next_offset ||
               mip.offset>texture.data.size() || mip.size>texture.data.size()-mip.offset)
                throw std::invalid_argument("invalid original DDS mip layout");
            progress();
            if(texture.format==assets::DdsFormat::rgba8) {
                validate_texture(static_cast<int>(mip.width),static_cast<int>(mip.height),mip.size);
                glTexImage2D(GL_TEXTURE_2D,static_cast<GLint>(level),GL_RGBA8,static_cast<GLsizei>(mip.width),
                             static_cast<GLsizei>(mip.height),0,GL_RGBA,GL_UNSIGNED_BYTE,texture.data.data()+mip.offset);
            } else {
                if(mip.width>static_cast<unsigned>(max_texture_size_)||mip.height>static_cast<unsigned>(max_texture_size_)||
                   mip.size!=assets::dds_mip_bytes(texture.format,mip.width,mip.height)||mip.size>static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()))
                    throw std::invalid_argument("invalid original DXT texture dimensions/payload");
                glCompressedTexImage2D_(GL_TEXTURE_2D,static_cast<GLint>(level),compressed,static_cast<GLsizei>(mip.width),
                                        static_cast<GLsizei>(mip.height),0,static_cast<GLsizei>(mip.size),texture.data.data()+mip.offset);
            }
            progress();
            next_offset+=mip.size;
            if(mip_width==1 && mip_height==1 && level+1<texture.mips.size())throw std::invalid_argument("DDS mip chain exceeds its final level");
            mip_width=std::max(1U,mip_width/2);mip_height=std::max(1U,mip_height/2);
        }
        if(next_offset!=texture.data.size())throw std::invalid_argument("trailing decoded DDS mip bytes");
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,static_cast<GLint>(texture.mips.size()-1));
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,options_.original_appearance?GL_LINEAR_MIPMAP_NEAREST:GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
        if(anisotropy_>1)glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_MAX_ANISOTROPY_EXT,anisotropy_);
    }
    // Alpha-tested caster (shadow_program_ bound).
    void draw_shadow(const GlSet& set,const Scene& scene,const Draw& draw){
        const auto& material=scene.materials[draw.material];
        set_cull(!material.double_sided);
        const auto& gpu=set.meshes[draw.instance->mesh];const auto& maps=set.materials[draw.material];
        const auto diffuse=maps.diffuse?maps.diffuse:gpu.texture;
        auto& u=shadow_uniforms_;
        glUniformMatrix4fv_(shadow_object_location_,1,GL_FALSE,draw.matrix.data());set4f(u.color,material.color);
        set1f(u.cutoff,material.alpha_cutoff);set1i(u.texture,diffuse?1:0);
        set1i(u.fade,maps.fade?1:0);set1i(u.opaque,maps.normal||maps.environment?1:0);
        set4f(u.sway,draw.scroll); // source ocean draws cast no shadow
        bind_texture(4,maps.fade);bind_texture(0,diffuse);
        bind_vao(gpu.vao);glDrawElements(GL_TRIANGLES,gpu.count,GL_UNSIGNED_INT,nullptr);
    }
    // transpose(inverse(mat3(m))) = cofactor(m)/det(m), column-major for glUniformMatrix3fv.
    static std::array<float,9> normal_matrix(const Matrix4& m) {
        const auto at=[&](int r,int c){return m[static_cast<std::size_t>(c*4+r)];};
        const float c00=at(1,1)*at(2,2)-at(1,2)*at(2,1),c01=at(1,2)*at(2,0)-at(1,0)*at(2,2),c02=at(1,0)*at(2,1)-at(1,1)*at(2,0);
        const float c10=at(0,2)*at(2,1)-at(0,1)*at(2,2),c11=at(0,0)*at(2,2)-at(0,2)*at(2,0),c12=at(0,1)*at(2,0)-at(0,0)*at(2,1);
        const float c20=at(0,1)*at(1,2)-at(0,2)*at(1,1),c21=at(0,2)*at(1,0)-at(0,0)*at(1,2),c22=at(0,0)*at(1,1)-at(0,1)*at(1,0);
        const float inverse=1.f/(at(0,0)*c00+at(0,1)*c01+at(0,2)*c02);
        return {c00*inverse,c10*inverse,c20*inverse,c01*inverse,c11*inverse,c21*inverse,c02*inverse,c12*inverse,c22*inverse};
    }
    void draw_instance(const GlSet& set,const Scene& scene,const Draw& draw){
        const auto& gpu=set.meshes[draw.instance->mesh];const auto& material=scene.materials[draw.material];const auto& maps=set.materials[draw.material];
        auto& u=draw_uniforms_;
        set_cull(!material.double_sided);
        set1i(u.sky,draw.instance->sky?1:0);
        set1i(u.ocean,material.source_ocean?1:0);
        set4f(u.scroll,draw.scroll);
        static constexpr std::array<float,4> rigid{};
        set4f(u.sway,draw.instance->ocean||material.source_ocean?rigid:draw.scroll);
        set1i(u.fog,draw.fog?1:0);
        set1f(u.cutoff,material.alpha_cutoff);set1i(u.normal,maps.normal?1:0);
        const GLuint environment=draw.instance->sky
            ? (maps.environment?maps.environment:(maps.fade?maps.fade:maps.lightmap)):maps.environment;
        set1i(u.environment,environment?1:0);set1i(u.lightmap,maps.lightmap?1:0);
        set1i(u.fade,maps.fade?1:0);set1i(u.height,maps.height?1:0);
        bind_texture(1,maps.normal);bind_texture(2,environment);bind_texture(3,maps.lightmap);bind_texture(4,maps.fade);bind_texture(8,maps.height);
        const auto diffuse=maps.diffuse?maps.diffuse:gpu.texture;
        glUniformMatrix4fv_(object_location_,1,GL_FALSE,draw.matrix.data());set4f(u.color,material.color);set1i(u.texture,diffuse?1:0);
        // scene_vertex reads normal_matrix only on its enhanced, non-sky, non-ocean path.
        if(!options_.original_appearance&&!draw.instance->sky&&!material.source_ocean) {
            const auto n=normal_matrix(draw.matrix);glUniformMatrix3fv_(normal_matrix_location_,1,GL_FALSE,n.data());
        }
        bind_texture(0,diffuse);bind_vao(gpu.vao);glDrawElements(GL_TRIANGLES,gpu.count,GL_UNSIGNED_INT,nullptr);
    }
    void destroy() noexcept {
        if(!context_)return;
        if(SDL_GL_MakeCurrent(window_,context_)){
            // Before entry points are loaded the context itself owns all remaining allocations.
            if(glDeleteBuffers_ && glDeleteVertexArrays_)release(set_);
            if(program_ && glDeleteProgram_)glDeleteProgram_(program_);
            if(overlay_program_ && glDeleteProgram_)glDeleteProgram_(overlay_program_);
            if(particle_program_ && glDeleteProgram_)glDeleteProgram_(particle_program_);
            if(contour_program_ && glDeleteProgram_)glDeleteProgram_(contour_program_);
            if(particle_vao_ && glDeleteVertexArrays_)glDeleteVertexArrays_(1,&particle_vao_);
            if(particle_buffer_ && glDeleteBuffers_)glDeleteBuffers_(1,&particle_buffer_);
            if(!particle_textures_.empty())glDeleteTextures(static_cast<GLsizei>(particle_textures_.size()),particle_textures_.data());
            if(overlay_vao_ && glDeleteVertexArrays_)glDeleteVertexArrays_(1,&overlay_vao_);
            if(overlay_texture_)glDeleteTextures(1,&overlay_texture_);
            if(color_)glDeleteTextures(1,&color_);
            if(depth_ && glDeleteRenderbuffers_)glDeleteRenderbuffers_(1,&depth_);
            if(framebuffer_ && glDeleteFramebuffers_)glDeleteFramebuffers_(1,&framebuffer_);
            if(water_textures_[0])glDeleteTextures(2,water_textures_);
            if(water_framebuffer_ && glDeleteFramebuffers_)glDeleteFramebuffers_(1,&water_framebuffer_);
            if(glow_program_ && glDeleteProgram_)glDeleteProgram_(glow_program_);
            if(glow_textures_[0])glDeleteTextures(2,glow_textures_);
            if(glow_framebuffers_[0] && glDeleteFramebuffers_)glDeleteFramebuffers_(2,glow_framebuffers_);
            if(shadow_program_ && glDeleteProgram_)glDeleteProgram_(shadow_program_);
            if(shadow_depth_program_ && glDeleteProgram_)glDeleteProgram_(shadow_depth_program_);
            if(shadow_textures_[0])glDeleteTextures(2,shadow_textures_);
            if(shadow_framebuffers_[0] && glDeleteFramebuffers_)glDeleteFramebuffers_(2,shadow_framebuffers_);
            if(shadow_static_textures_[0])glDeleteTextures(2,shadow_static_textures_);
            if(shadow_static_framebuffers_[0] && glDeleteFramebuffers_)glDeleteFramebuffers_(2,shadow_static_framebuffers_);
            if(timing_queries_[0][0] && glDeleteQueries_)glDeleteQueries_(timing_ring*timed_passes,&timing_queries_[0][0]);
        }
        SDL_GL_DestroyContext(context_);context_=nullptr;
    }
};
} // namespace
std::unique_ptr<Renderer> make_gl_renderer(SDL_Window* window,const RendererOptions& options){return std::make_unique<GlRenderer>(window,options);}
} // namespace ikv
