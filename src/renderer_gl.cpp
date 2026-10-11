#include "ikv/renderer.hpp"
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
out vec3 fragment_position;
out vec4 shadow_position;
out vec2 texcoord;
out vec3 world_normal;
out vec3 world_tangent;
out vec3 source_vertex_light;
out float fog_blend;
out vec2 environment_uv;
out vec2 lightmap_uv;
out vec3 source_tangent_sun;
out vec3 source_tangent_eye;
out float sky_scatter;
out float fade_blend;
void main() {
    if(source_ocean) {
        vec4 world_position=object_matrix*vec4(position,1.0);
        // v_ocean.vsh: EXPP.y is the fractional phase; retain its authored polynomial.
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
        if(abs(camera_position.y-world_position.y)<100.0)world_position.y+=wave*0.5;
        gl_Position=view_projection*world_position;
        texcoord=uv+source_scroll.xy;
        lightmap_uv=uv+source_scroll.zw;
        source_tangent_eye=normalize(camera_position-world_position.xyz);
        source_vertex_light=ambient_multiplier;
        world_normal=normal;world_tangent=tangent;
        source_tangent_sun=vec3(0.0);environment_uv=vec2(0.0);
        sky_scatter=0.0;fade_blend=1.0;
        float source_clip_z=(gl_Position.z+gl_Position.w)*0.5;
        fog_blend=fog_enabled?(fog_end-source_clip_z)/(fog_end-fog_start):1.0;
        return;
    }
    vec4 world_position=object_matrix*vec4(position,1.0);
    gl_Position=view_projection*world_position;
    fragment_position=world_position.xyz;shadow_position=light_view_projection*world_position;
    texcoord=uv;
    world_normal=normal;world_tangent=tangent;
    if(!original_appearance&&!sky) {
        world_normal=transpose(inverse(mat3(object_matrix)))*normal;
        world_tangent=mat3(object_matrix)*tangent;
    }
    lightmap_uv=uv_lightmap;
    fade_blend=diffuse_color.a;
    vec3 eye_direction=vec3(0.0);environment_uv=vec2(0.0);
    if(sky||(original_appearance&&(has_normal_map||has_environment_map))) {
        eye_direction=camera_position-world_position.xyz;
        if(length(eye_direction)>0.00001)eye_direction=normalize(eye_direction);
    }
    if(original_appearance&&has_environment_map&&!sky) {
        vec3 local_normal=normal;
        if(has_normal_map&&length(local_normal)>0.00001)local_normal=normalize(local_normal);
        environment_uv=(eye_direction-2.0*local_normal*dot(local_normal,eye_direction)).xy;
    }
    source_tangent_sun=vec3(0.0);source_tangent_eye=vec3(0.0);
    if(original_appearance&&has_normal_map&&!sky&&length(normal)>0.00001&&length(tangent)>0.00001) {
        vec3 n=mat3(object_matrix)*normalize(normal);
        vec3 t=mat3(object_matrix)*normalize(tangent);
        vec3 b=cross(n,t);t=cross(b,n);
        source_tangent_sun=vec3(dot(t,-sun_direction),dot(b,-sun_direction),dot(n,-sun_direction));
        vec3 source_eye=vec3(dot(t,eye_direction),dot(b,eye_direction),dot(n,eye_direction));
        if(length(source_eye)>0.00001)source_tangent_eye=normalize(source_eye);
    }
    source_vertex_light=ambient.rgb;
    if(source_lighting&&!sky&&(!original_appearance||!has_normal_map)) {
        float directional=clamp(dot(mat3(object_matrix)*normal,sun_direction),0.0,1.0);
        // D3D vs_1_1 saturates oD0 before the pixel shader consumes it.
        source_vertex_light=clamp(ambient.rgb*ambient_multiplier+directional*diffuse_multiplier,0.0,1.0);
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
in float fog_blend;
uniform vec3 fog_color;
uniform bool sky;
uniform bool source_ocean;
uniform vec3 sun_direction;
uniform float sky_blend;
uniform bool grayscale; // original death view: 3D scene desaturated, HUD untouched
uniform sampler2D shadow_map;
uniform vec3 camera_position;
in vec3 fragment_position;
in vec4 shadow_position;
in float sky_scatter;
// Enhanced raster lighting: Vulkan lighting.glsl (non-RT enhanced) plus its post.frag ACES tonemap.
const float PI=3.14159265359;
vec3 enhanced_environment(vec3 direction) {
    if(!has_environment_map)return mix(vec3(0.055,0.07,0.09),vec3(0.32,0.44,0.62),clamp(direction.y*0.5+0.5,0.0,1.0));
    return textureLod(environment_map,vec2(atan(direction.z,direction.x)/(2.0*PI)+0.5,acos(clamp(direction.y,-1.0,1.0))/PI),0.0).rgb;
}
// shadow_position is in Vulkan light clip space; the GL shadow pass maps z to window depth 0..1 unchanged.
float shadow_visibility(vec3 n,vec3 l) {
    vec3 p=shadow_position.xyz/shadow_position.w;vec2 uv=p.xy*0.5+0.5;
    if(any(lessThan(uv,vec2(0.0)))||any(greaterThan(uv,vec2(1.0)))||p.z<0.0||p.z>1.0)return 1.0;
    float bias=max(0.0003,0.002*(1.0-dot(n,l)));vec2 texel=1.0/vec2(textureSize(shadow_map,0));float visible=0.0;
    for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++)visible+=p.z-bias<=texture(shadow_map,uv+vec2(x,y)*texel).r?1.0:0.0;
    return visible/9.0;
}
out vec4 output_color;
void main() {
    vec4 surface=material_color;
    if(has_texture&&!source_ocean) surface*=texture(diffuse_map,texcoord);
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
        vec3 second=has_environment_map?texture(environment_map,texcoord).rgb:vec3(0.0);
        vec3 clouds=surface.rgb*(1.0-clamp(sky_blend,0.0,1.0))+second*sky_blend;
        surface=vec4(mix(fog_color,clouds*source_vertex_light+vec3(sky_scatter),surface.a),1.0);
    } else if(original_appearance) {
        if(has_normal_map&&source_lighting&&length(source_tangent_eye)>0.00001) {
            vec4 mapped_texel=texture(normal_map,texcoord);
            vec2 xy=mapped_texel.xy*2.0-1.0;
            vec3 mapped=normalize(vec3(xy,1.0-dot(xy,xy)));
            float factor=dot(mapped,source_tangent_sun)*0.5+0.5;
            vec3 half_direction=mapped+source_tangent_eye;
            float specular=length(half_direction)>0.00001
                ? pow(max(dot(mapped,normalize(half_direction)),0.0),has_environment_map?64.0:48.0):0.0;
            if(factor<=0.0)specular=0.0;
            vec3 illumination=mix(sky_c,sky_b,factor)*mapped_texel.a;
            vec3 specular_color=vec3(mapped_texel.b);
            if(has_environment_map)specular_color+=texture(environment_map,environment_uv).rgb*surface.a;
            vec3 result=(surface.rgb+specular*specular_color)*illumination;
            if(!has_environment_map)result+=surface.rgb*(1.0-surface.a);
            surface=vec4(result,1.0);
        } else {
            if(has_environment_map) {
                vec3 environment=texture(environment_map,environment_uv).rgb;
                if(has_lightmap)surface.rgb=mix(surface.rgb,environment,surface.a)
                    -texture(lightmap_map,lightmap_uv).rgb*(1.0-clamp(surface.a,0.0,1.0))*lightmap_strength;
                else surface.rgb+=environment*surface.a;
                surface.a=1.0;
            } else {
                if(has_fade_map)surface=mix(texture(fade_map,texcoord),surface,fade_blend);
                if(has_lightmap)surface.rgb-=texture(lightmap_map,lightmap_uv).rgb*lightmap_strength;
            }
            surface.rgb*=source_vertex_light;
        }
    } else {
        // Source MAT+14 secondary fade applies only without higher-priority normal/environment maps.
        if(has_fade_map&&!has_normal_map&&!has_environment_map)surface=mix(texture(fade_map,texcoord)*material_color,surface,fade_blend);
        if(has_normal_map||has_environment_map)surface.a=1.0;
        float nlength=length(world_normal);
        vec3 n=nlength>0.00001?world_normal/nlength:vec3(0.0,1.0,0.0);
        if(has_normal_map) {
            vec3 t=world_tangent-n*dot(n,world_tangent);
            if(dot(t,t)>1e-12){t=normalize(t);n=normalize(mat3(t,cross(n,t),n)*(texture(normal_map,texcoord).xyz*2.0-1.0));}
        }
        vec3 v=normalize(camera_position-fragment_position),l=normalize(sun_direction),h=normalize(v+l);
        const float roughness=0.65;
        vec3 albedo=pow(max(surface.rgb,vec3(0.0)),vec3(2.2));
        float ndl=max(dot(n,l),0.0),ndv=max(dot(n,v),0.001),ndh=max(dot(n,h),0.0),vdh=max(dot(v,h),0.0);
        float a2=roughness*roughness*roughness*roughness,dd=ndh*ndh*(a2-1.0)+1.0,d=a2/(PI*dd*dd);
        float k=(roughness+1.0)*(roughness+1.0)/8.0,g=(ndv/(ndv*(1.0-k)+k))*(ndl/(ndl*(1.0-k)+k));
        vec3 f=vec3(0.04)+0.96*pow(1.0-vdh,5.0);
        vec3 direct=((1.0-f)*albedo/PI+d*g*f/max(4.0*ndv*ndl,0.001))*ndl*3.2;
        vec3 c=direct*shadow_visibility(n,l)+albedo*enhanced_environment(n)*0.35+enhanced_environment(reflect(-v,n))*f*(1.0-roughness)*0.4;
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
out vec2 texcoord;
out float fade_blend;
void main(){
    texcoord=uv;fade_blend=diffuse_color.a;
    vec4 p=light_view_projection*(object_matrix*vec4(position,1.0));
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
    else if(mode==4){output_color=vec4(0.0,0.0,0.0,step_gain.w);return;} // NONCANONICAL seamless door fade
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
#undef GL_ENTRY
    struct GpuMesh { GLuint vao=0, vertices=0, indices=0, texture=0; GLsizei count=0; std::size_t vertex_count=0; std::uint64_t revision=0; std::array<float,4> bounds{}; };
    struct GpuMaterial { GLuint diffuse=0,normal=0,environment=0,lightmap=0,fade=0; bool referenced=false; };
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
    // Enhanced appearance only: 2048^2 sun depth map (Vulkan shadow pass parity).
    GLuint shadow_program_=0,shadow_texture_=0,shadow_framebuffer_=0;
    GLint light_location_=-1,shadow_light_location_=-1,shadow_object_location_=-1,shadow_color_location_=-1,shadow_cutoff_location_=-1;
    GLint shadow_texture_location_=-1,shadow_fade_location_=-1,shadow_opaque_location_=-1;
    GLint glow_mode_location_=-1,glow_step_location_=-1;
    GLint vp_location_=-1,object_location_=-1,color_location_=-1,texture_location_=-1,original_location_=-1,exposure_location_=-1;
    GLint normal_location_=-1,cutoff_location_=-1;
    GLint source_light_location_=-1,sun_location_=-1,ambient_location_=-1,diffuse_location_=-1;
    GLint fog_enabled_location_=-1,fog_color_location_=-1,fog_start_location_=-1,fog_end_location_=-1;
    GLint camera_location_=-1,environment_location_=-1,lightmap_location_=-1;
    GLint sky_b_location_=-1,sky_c_location_=-1,lightmap_strength_location_=-1;
    GLint sky_location_=-1,sky_blend_location_=-1;
    GLint fade_location_=-1;
    GLint ocean_location_=-1,time_location_=-1,scroll_location_=-1;
    SourceScrollState source_scroll_;
    bool overlay_present_=false,rendered_=false,grayscale_=false;
    float fade_=0;
    int overlay_width_=0,overlay_height_=0; // overlay_texture_ storage size (sub-rectangle uploads need a matching one)
    bool source_presented_=false;
    const Scene* scene_=nullptr;
    std::uint64_t geometry_revision_=0;
    std::vector<std::uint64_t> mesh_keys_; // Scene mesh keys as last uploaded; actor attach/detach remaps meshes_ by them.
    std::uint64_t mesh_key_=0; // Scene::mesh_key as last uploaded: any actor (re)assembly since then re-runs upload().
    std::vector<GpuMesh> meshes_;
    std::vector<GpuMaterial> materials_;
    std::vector<GLuint> textures_;
    std::vector<Draw> sky_,opaque_,alpha_;
    std::unordered_map<std::string,GLuint> texture_names_;
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
            LOAD_GL(glBlendEquation); LOAD_GL(glCompressedTexImage2D);
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
            fade_location_=uniform(program_,"has_fade_map");
            ocean_location_=uniform(program_,"source_ocean");time_location_=uniform(program_,"source_time");
            scroll_location_=uniform(program_,"source_scroll");
            glUseProgram_(program_); glUniform1i_(uniform(program_,"diffuse_map"),0);
            glUniform1i_(uniform(program_,"normal_map"),1);
            glUniform1i_(uniform(program_,"environment_map"),2);glUniform1i_(uniform(program_,"lightmap_map"),3);
            glUniform1i_(uniform(program_,"fade_map"),4);glUniform1i_(uniform(program_,"shadow_map"),5);
            light_location_=uniform(program_,"light_view_projection");
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
            if(!options_.original_appearance) {
                shadow_program_=link(shadow_vertex,shadow_fragment);
                shadow_light_location_=uniform(shadow_program_,"light_view_projection");shadow_object_location_=uniform(shadow_program_,"object_matrix");
                shadow_color_location_=uniform(shadow_program_,"material_color");shadow_cutoff_location_=uniform(shadow_program_,"alpha_cutoff");
                shadow_texture_location_=uniform(shadow_program_,"has_texture");shadow_fade_location_=uniform(shadow_program_,"has_fade_map");
                shadow_opaque_location_=uniform(shadow_program_,"opaque_alpha");
                glUseProgram_(shadow_program_);glUniform1i_(uniform(shadow_program_,"diffuse_map"),0);glUniform1i_(uniform(shadow_program_,"fade_map"),4);
                glGenTextures(1,&shadow_texture_);glBindTexture(GL_TEXTURE_2D,shadow_texture_);
                glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT24,2048,2048,0,GL_DEPTH_COMPONENT,GL_UNSIGNED_INT,nullptr);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
                glGenFramebuffers_(1,&shadow_framebuffer_);glBindFramebuffer_(GL_FRAMEBUFFER,shadow_framebuffer_);
                glFramebufferTexture2D_(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,shadow_texture_,0);
                glDrawBuffer(GL_NONE);glReadBuffer(GL_NONE);
                if(glCheckFramebufferStatus_(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("OpenGL shadow framebuffer incomplete");
                glBindFramebuffer_(GL_FRAMEBUFFER,0);check("allocating shadow map");
            }
            glGenFramebuffers_(1,&framebuffer_); glGenTextures(1,&color_); glGenRenderbuffers_(1,&depth_);
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
        width_=width;height_=height;rendered_=false;
        check("allocating scene framebuffer");
        progress();
    }
    void set_grayscale(bool on) override { grayscale_=on; }
    void set_fade(float black) override { fade_=std::clamp(black,0.f,1.f); }
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
        if(scene_!=&scene || mesh_key_!=scene.mesh_key || meshes_.size()!=scene.meshes.size() || textures_.size()!=scene.textures.size() || materials_.size()!=scene.materials.size())upload(scene);
        if(geometry_revision_!=scene.geometry_revision) {
            for(std::size_t i=0;i<meshes_.size();++i) {
                if((i&1023U)==0)progress();
                const auto& mesh=scene.meshes[i];auto& gpu=meshes_[i];
                if(!gpu.vao)continue;
                if(mesh.vertices.size()!=gpu.vertex_count)throw std::runtime_error("animated mesh topology changed without a new scene");
                if(mesh.revision==gpu.revision)continue;
                validate_vertices(mesh);
                progress();
                glBindBuffer_(GL_ARRAY_BUFFER,gpu.vertices);
                glBufferSubData_(GL_ARRAY_BUFFER,0,static_cast<GLsizeiptr>(mesh.vertices.size()*sizeof(Vertex)),mesh.vertices.data());
                gpu.revision=mesh.revision;
                if(mesh.influences.empty())gpu.bounds=mesh_bounds(mesh);
                progress();
            }
            check("uploading recovered skeletal animation");geometry_revision_=scene.geometry_revision;
        }
        glBindFramebuffer_(GL_FRAMEBUFFER,framebuffer_);glViewport(0,0,width_,height_);
        glDisable(GL_SCISSOR_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glDepthMask(GL_TRUE);
        // Source ADF triangles keep D3D clockwise front faces; Vulkan's signed-area sign and projection Y flip cancel, so both use CW.
        glFrontFace(GL_CW);glCullFace(GL_BACK);
        glClearColor(0.045f,0.065f,0.085f,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glUseProgram_(program_);
        const auto vp=camera_view_projection(options_.camera,width_,height_);
        glUniformMatrix4fv_(vp_location_,1,GL_FALSE,vp.data());
        glUniform1i_(original_location_,options_.original_appearance?1:0);glUniform1f_(exposure_location_,options_.exposure);
        glUniform1i_(uniform(program_,"grayscale"),grayscale_?1:0);
        const auto& light=scene.lighting;
        for(const auto* values:{&light.sun_direction,&light.ambient_multiplier,&light.diffuse_multiplier,&light.sky_b,&light.sky_c,&light.fog_color})
            for(float value:*values)if(!std::isfinite(value))throw std::invalid_argument("non-finite scene lighting");
        if(light.fog_enabled&&(!std::isfinite(light.fog_start)||!std::isfinite(light.fog_end)||light.fog_end<=light.fog_start))
            throw std::invalid_argument("invalid source fog interval");
        if(!std::isfinite(light.lightmap_strength))throw std::invalid_argument("non-finite source lightmap multiplier");
        if(!std::isfinite(light.sky_blend)||!std::isfinite(light.sky_rotation))throw std::invalid_argument("non-finite source sky parameters");
        // Enhanced lighting needs a sun even without source constants; original paths only read it with them.
        const auto sun=enhanced_sun(light);
        glUniform1i_(source_light_location_,light.source_values?1:0);glUniform3fv_(sun_location_,1,sun.data());
        glUniform3fv_(ambient_location_,1,light.ambient_multiplier.data());glUniform3fv_(diffuse_location_,1,light.diffuse_multiplier.data());
        glUniform3fv_(fog_color_location_,1,light.fog_color.data());
        glUniform1f_(fog_start_location_,light.fog_start);glUniform1f_(fog_end_location_,light.fog_end);
        glUniform3fv_(sky_b_location_,1,light.sky_b.data());glUniform3fv_(sky_c_location_,1,light.sky_c.data());
        glUniform1f_(lightmap_strength_location_,light.lightmap_strength);
        glUniform1f_(sky_blend_location_,light.sky_blend);
        const auto ticks=static_cast<std::uint32_t>(SDL_GetTicks());
        const std::array<float,4> source_time{static_cast<float>(ticks),static_cast<float>(ticks)*0.001f,0.0f,0.0f};
        glUniform4fv_(time_location_,1,source_time.data());
        sky_.clear();opaque_.clear();alpha_.clear();contours_.clear();
        const auto planes=frustum_planes(vp);
        const auto eye=camera_basis(options_.camera).eye;
        glUniform3fv_(camera_location_,1,eye.data());
        const float sky_cosine=std::cos(light.sky_rotation),sky_sine=std::sin(light.sky_rotation);
        for(const auto& instance:scene.instances) {
            if(instance.mesh>=meshes_.size()) throw std::runtime_error("scene instance has invalid mesh index");
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
            if(instance.ocean&&instance.source_ordinary_color_participates)scroll=advance_source_scroll(source_scroll_,static_cast<std::uint32_t>(SDL_GetTicks()));
            if(!scene_source_color_draw(scene,instance.mesh,instance.material,instance.source_ordinary_color_participates))continue;
            if(!materials_[material].referenced)upload_material(scene,material);
            if(!meshes_[instance.mesh].vao)upload_mesh(scene,instance.mesh);
            if(scene.materials[material].diffuse.empty()&&!mesh.texture.empty()&&!meshes_[instance.mesh].texture)
                meshes_[instance.mesh].texture=find_texture(mesh.texture);
            if(instance.sky) {
                if(!light.source_values)throw std::runtime_error("original sky class requires source lighting constants");
                matrix={sky_cosine,0,-sky_sine,0,0,1,0,0,sky_sine,0,sky_cosine,0,eye[0],eye[1],eye[2],1};
                if(!materials_[material].diffuse&&!meshes_[instance.mesh].texture)continue;
            }
            // Frustum culling: sky, source ocean (vertex-displaced) and skinned (per-frame world-space) meshes always draw.
            // Culled alpha draws stay in alpha_ so its distance sort sees the unculled list.
            const bool visible=instance.sky||instance.ocean||scene.materials[material].source_ocean||!mesh.influences.empty()||
                sphere_visible(planes,matrix,meshes_[instance.mesh].bounds);
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
            const float dx=matrix[12]-eye[0],dy=matrix[13]-eye[1],dz=matrix[14]-eye[2];
            Draw draw{&instance,matrix,dx*dx+dy*dy+dz*dz,material,fog,scroll,visible};
            (instance.sky?sky_:(scene.materials[material].alpha?alpha_:opaque_)).push_back(draw);
            if(!visible)continue;
            if(entity&&!instance.sky&&entity->contour!=std::array<float,3>{}) {
                for(float v:entity->contour)if(!std::isfinite(v))throw std::invalid_argument("non-finite entity contour colour");
                contours_.push_back({instance.mesh,matrix,entity->contour});
            }
        }
        if(shadow_program_) {
            const auto light_vp=shadow_light_matrix(options_.camera,sun);
            glBindFramebuffer_(GL_FRAMEBUFFER,shadow_framebuffer_);glViewport(0,0,2048,2048);
            // Vulkan shadow pipeline: back faces culled, depth bias 1.25 constant / 1.75 slope. The light matrix keeps
            // Vulkan's Y flip, which reverses GL winding, so source CW front faces rasterise CCW here.
            glDepthMask(GL_TRUE);glClear(GL_DEPTH_BUFFER_BIT);glFrontFace(GL_CCW);
            glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(1.75f,1.25f);
            glUseProgram_(shadow_program_);glUniformMatrix4fv_(shadow_light_location_,1,GL_FALSE,light_vp.data());
            for(const auto* list:{&opaque_,&alpha_})for(const auto& draw:*list)draw_shadow(scene,draw);
            glDisable(GL_POLYGON_OFFSET_FILL);glFrontFace(GL_CW);
            glBindFramebuffer_(GL_FRAMEBUFFER,framebuffer_);glViewport(0,0,width_,height_);
            glUseProgram_(program_);glUniformMatrix4fv_(light_location_,1,GL_FALSE,light_vp.data());
            glActiveTexture_(GL_TEXTURE5);glBindTexture(GL_TEXTURE_2D,shadow_texture_);
            check("drawing shadow map");
        }
        glDepthMask(GL_FALSE);
        for(const auto& draw:sky_) {
            if(scene.materials[draw.material].alpha){glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);}
            else glDisable(GL_BLEND);
            draw_instance(scene,draw);
        }
        glDisable(GL_BLEND);glDepthMask(GL_TRUE);
        for(const auto& draw:opaque_) if(draw.visible) draw_instance(scene,draw);
        std::sort(alpha_.begin(),alpha_.end(),[](const Draw& a,const Draw& b){return a.distance>b.distance;});
        glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_FALSE);
        for(const auto& draw:alpha_) if(draw.visible) draw_instance(scene,draw);
        // 0046ccf0 draws non-AfterHDR particles with the transparent list (glow input); 0046cbe0 draws AfterHDR ones after
        // the glow composite (0046fdb0 @470aed/@47183f, or straight after 0046ccf0 without glow @471977).
        const auto draw_particles=[&](bool after_hdr) {
            if(std::none_of(particle_batches_.begin(),particle_batches_.end(),[&](const ParticleBatch& b){return b.after_hdr==after_hdr;}))return;
            // Original 0049a780: depth test without depth write, no culling, per-system blend.
            glEnable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glEnable(GL_BLEND);
            glDisable(GL_CULL_FACE);glUseProgram_(particle_program_);glUniformMatrix4fv_(particle_vp_location_,1,GL_FALSE,vp.data());
            glUniform1i_(uniform(particle_program_,"grayscale"),grayscale_?1:0);
            glUniform1i_(uniform(particle_program_,"fog_enabled"),options_.original_appearance&&light.fog_enabled?1:0);
            glUniform1f_(uniform(particle_program_,"fog_start"),light.fog_start);glUniform1f_(uniform(particle_program_,"fog_end"),light.fog_end);
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
            glUniform1i_(uniform(contour_program_,"grayscale"),grayscale_?1:0);
            const float pulse=std::sin(static_cast<float>(ticks)*.004f)*.015f;
            for(int shell=3;shell>=1;--shell) {
                glUniform1f_(contour_offset_location_,pulse+static_cast<float>(shell)*.02f);
                for(const auto& contour:contours_) {
                    const auto& gpu=meshes_[contour.mesh];
                    glUniformMatrix4fv_(contour_object_location_,1,GL_FALSE,contour.matrix.data());
                    glUniform3fv_(contour_color_location_,1,contour.color.data());
                    glBindVertexArray_(gpu.vao);glDrawElements(GL_TRIANGLES,gpu.count,GL_UNSIGNED_INT,nullptr);
                }
            }
            glCullFace(GL_BACK);
        }
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
        if(fade_>0) { // below the HUD, like Vulkan's post.frag fade
            glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
            glUseProgram_(glow_program_);glBindVertexArray_(overlay_vao_);
            const std::array<float,4> black{0,0,0,fade_};
            glUniform1i_(glow_mode_location_,4);glUniform4fv_(glow_step_location_,1,black.data());glDrawArrays(GL_TRIANGLES,0,3);
        }
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
    GLuint find_texture(const std::string& name) const {
        if(name.empty())return GLuint{0};
        const auto found=texture_names_.find(name);
        if(found==texture_names_.end())throw std::runtime_error("missing original material texture: "+name);
        return found->second;
    }
    static void validate_material(const Material& material) {
        for(float value:material.color)if(!std::isfinite(value))throw std::invalid_argument("non-finite material color");
        if(!std::isfinite(material.alpha_cutoff)||material.alpha_cutoff<0||material.alpha_cutoff>1)throw std::invalid_argument("invalid material alpha cutoff");
    }
    void upload_material(const Scene& scene,std::size_t index) {
        const auto& material=scene.materials[index];
        validate_material(material);
        materials_[index]={find_texture(material.diffuse),find_texture(material.normal),
            find_texture(material.environment),find_texture(material.lightmap),find_texture(material.luminance),true};
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
    void upload_mesh(const Scene& scene,std::size_t index) {
        progress();
        validate_mesh(scene,index);
        const auto& source=scene.meshes[index];auto& gpu=meshes_[index];
        gpu.texture=0; // Legacy texture residency is resolved lazily from the actual draw's material.
        gpu.count=static_cast<GLsizei>(source.indices.size());glGenVertexArrays_(1,&gpu.vao);glBindVertexArray_(gpu.vao);
        gpu.vertex_count=source.vertices.size();gpu.revision=source.revision;gpu.bounds=mesh_bounds(source);
        glGenBuffers_(1,&gpu.vertices);glBindBuffer_(GL_ARRAY_BUFFER,gpu.vertices);
        progress();
        glBufferData_(GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(source.vertices.size()*sizeof(Vertex)),source.vertices.data(),source.influences.empty()?GL_STATIC_DRAW:GL_DYNAMIC_DRAW);
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
        progress();
    }
    void release(GpuMesh& mesh) noexcept {
        if(mesh.indices)glDeleteBuffers_(1,&mesh.indices);
        if(mesh.vertices)glDeleteBuffers_(1,&mesh.vertices);
        if(mesh.vao)glDeleteVertexArrays_(1,&mesh.vao);
        mesh={};
    }
    void release_scene() noexcept override {
        for(auto& mesh:meshes_)release(mesh);
        if(!textures_.empty())glDeleteTextures(static_cast<GLsizei>(textures_.size()),textures_.data());
        meshes_.clear();materials_.clear();textures_.clear();texture_names_.clear();scene_=nullptr;
    }
    void upload(const Scene& scene){
        progress();
        // Actor attach/detach/replacement keeps every other mesh (moved by key) and every texture; only a new Scene reloads all.
        if(scene_!=&scene || textures_.size()>scene.textures.size())release_scene();
        else {
            const auto remap=scene_mesh_remap(mesh_keys_,scene);
            std::vector<GpuMesh> kept(scene.meshes.size());
            for(std::size_t i=0;i<remap.size();++i)if(remap[i]!=SIZE_MAX)kept[i]=std::exchange(meshes_[remap[i]],GpuMesh{});
            for(auto& mesh:meshes_)release(mesh);
            meshes_=std::move(kept);scene_=nullptr;
        }
        try {
            progress();
            const auto material_usage=scene_material_usage(scene);
            progress();
            if(scene.textures.size()>static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()))
                throw std::invalid_argument("too many scene textures");
            const auto retained_textures=textures_.size();
            textures_.resize(scene.textures.size());
            for(std::size_t i=retained_textures;i<scene.textures.size();++i){
                progress();
                upload_texture(scene.textures[i],textures_[i]);
                if(!texture_names_.emplace(scene.textures[i].name,textures_[i]).second)throw std::invalid_argument("duplicate scene texture name");
                progress();
            }
            materials_.assign(scene.materials.size(),GpuMaterial{});
            for(std::size_t i=0;i<scene.materials.size();++i) {
                progress();
                if(!material_usage[i]){validate_material(scene.materials[i]);continue;}
                upload_material(scene,i);
            }
            meshes_.resize(scene.meshes.size());
            // 0: dormant/proxy, 1: referenced no-color, 2: visible GPU residency.
            std::vector<std::uint8_t> visible_meshes(scene.meshes.size(),0);
            if(scene.instances.empty()) {
                for(std::uint32_t i=0;i<scene.meshes.size();++i) {
                    if((i&1023U)==0)progress();
                    visible_meshes[i]=scene_source_color_draw(scene,i)?2:1;
                }
            }
            std::size_t sky_count=0;
            for(std::size_t i=0;i<scene.instances.size();++i) {
                if((i&1023U)==0)progress();
                const auto& instance=scene.instances[i];
                if(instance.sky)++sky_count;
                const bool color_draw=scene_source_color_draw(scene,instance.mesh,instance.material,instance.source_ordinary_color_participates);
                if(!instance.occlusion_proxy)visible_meshes[instance.mesh]=std::max<std::uint8_t>(visible_meshes[instance.mesh],color_draw?2:1);
            }
            for(std::size_t i=0;i<scene.meshes.size();++i) {
                progress();
                if(!visible_meshes[i]||meshes_[i].vao)continue; // Retained meshes were validated on upload.
                if(visible_meshes[i]==1){validate_mesh(scene,i);continue;}
                upload_mesh(scene,i);
            }
            glBindVertexArray_(0);check("uploading original asset scene");scene_=&scene;
            // geometry_revision_ stays: render() then refreshes retained skins whose revision moved since their upload.
            mesh_keys_.resize(scene.meshes.size());for(std::size_t i=0;i<scene.meshes.size();++i)mesh_keys_[i]=scene.meshes[i].key;
            mesh_key_=scene.mesh_key;
            opaque_.reserve(scene.instances.size());alpha_.reserve(scene.instances.size());
            sky_.reserve(sky_count);
            progress();
        }catch(...){release_scene();throw;}
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
    void draw_shadow(const Scene& scene,const Draw& draw){
        const auto& material=scene.materials[draw.material];
        // ponytail: no wave displacement here, so source ocean casts no shadow; port sourceOceanPosition if water must.
        if(material.source_ocean)return;
        if(material.double_sided)glDisable(GL_CULL_FACE);else glEnable(GL_CULL_FACE);
        const auto& gpu=meshes_[draw.instance->mesh];const auto& maps=materials_[draw.material];
        const auto diffuse=maps.diffuse?maps.diffuse:gpu.texture;
        glUniformMatrix4fv_(shadow_object_location_,1,GL_FALSE,draw.matrix.data());glUniform4fv_(shadow_color_location_,1,material.color.data());
        glUniform1f_(shadow_cutoff_location_,material.alpha_cutoff);glUniform1i_(shadow_texture_location_,diffuse?1:0);
        glUniform1i_(shadow_fade_location_,maps.fade?1:0);glUniform1i_(shadow_opaque_location_,maps.normal||maps.environment?1:0);
        glActiveTexture_(GL_TEXTURE4);glBindTexture(GL_TEXTURE_2D,maps.fade);
        glActiveTexture_(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,diffuse);
        glBindVertexArray_(gpu.vao);glDrawElements(GL_TRIANGLES,gpu.count,GL_UNSIGNED_INT,nullptr);
    }
    void draw_instance(const Scene& scene,const Draw& draw){
        const auto& gpu=meshes_[draw.instance->mesh];const auto& material=scene.materials[draw.material];const auto& maps=materials_[draw.material];
        if(material.double_sided)glDisable(GL_CULL_FACE);else glEnable(GL_CULL_FACE);
        glUniform1i_(sky_location_,draw.instance->sky?1:0);
        glUniform1i_(ocean_location_,material.source_ocean?1:0);
        glUniform4fv_(scroll_location_,1,draw.scroll.data());
        glUniform1i_(fog_enabled_location_,draw.fog?1:0);
        glUniform1f_(cutoff_location_,material.alpha_cutoff);glUniform1i_(normal_location_,maps.normal?1:0);
        const GLuint environment=draw.instance->sky
            ? (maps.environment?maps.environment:(maps.fade?maps.fade:maps.lightmap)):maps.environment;
        glUniform1i_(environment_location_,environment?1:0);glUniform1i_(lightmap_location_,maps.lightmap?1:0);
        glUniform1i_(fade_location_,maps.fade?1:0);
        glActiveTexture_(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,maps.normal);
        glActiveTexture_(GL_TEXTURE2);glBindTexture(GL_TEXTURE_2D,environment);
        glActiveTexture_(GL_TEXTURE3);glBindTexture(GL_TEXTURE_2D,maps.lightmap);
        glActiveTexture_(GL_TEXTURE4);glBindTexture(GL_TEXTURE_2D,maps.fade);
        const auto diffuse=maps.diffuse?maps.diffuse:gpu.texture;
        glUniformMatrix4fv_(object_location_,1,GL_FALSE,draw.matrix.data());glUniform4fv_(color_location_,1,material.color.data());glUniform1i_(texture_location_,diffuse?1:0);
        glActiveTexture_(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,diffuse);glBindVertexArray_(gpu.vao);glDrawElements(GL_TRIANGLES,gpu.count,GL_UNSIGNED_INT,nullptr);
    }
    void destroy() noexcept {
        if(!context_)return;
        if(SDL_GL_MakeCurrent(window_,context_)){
            // Before entry points are loaded the context itself owns all remaining allocations.
            if(glDeleteBuffers_ && glDeleteVertexArrays_)release_scene();
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
            if(glow_program_ && glDeleteProgram_)glDeleteProgram_(glow_program_);
            if(glow_textures_[0])glDeleteTextures(2,glow_textures_);
            if(glow_framebuffers_[0] && glDeleteFramebuffers_)glDeleteFramebuffers_(2,glow_framebuffers_);
            if(shadow_program_ && glDeleteProgram_)glDeleteProgram_(shadow_program_);
            if(shadow_texture_)glDeleteTextures(1,&shadow_texture_);
            if(shadow_framebuffer_ && glDeleteFramebuffers_)glDeleteFramebuffers_(1,&shadow_framebuffer_);
        }
        SDL_GL_DestroyContext(context_);context_=nullptr;
    }
};
} // namespace
std::unique_ptr<Renderer> make_gl_renderer(SDL_Window* window,const RendererOptions& options){return std::make_unique<GlRenderer>(window,options);}
} // namespace ikv
