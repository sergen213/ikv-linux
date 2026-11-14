// NONCANONICAL wave 29: OpenGL 4.x tessellated PBR relief (renderer_gl.cpp tess_program_), the GL twin of Vulkan
// scene_tess.vert/.tesc and scene_tess.glsl. The evaluation stage reproduces renderer_gl.cpp scene_vertex's enhanced
// static path (no ocean, sky or sway: pack materials are opaque static zone materials) for each generated vertex, displaced
// outward along the welded smoothed normal (pbr.hpp pbr_tessellation_mesh; tangent attribute = normal x weight).
#pragma once
#include "ikv_gl_pbr.h"

namespace ikv {
namespace {
constexpr const char* gl_tess_vertex = R"GLSL(#version 400 core
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 uv;
layout(location=3) in vec4 ambient;
layout(location=4) in vec3 tangent;
layout(location=5) in vec2 uv_lightmap;
layout(location=6) in vec4 diffuse_color;
uniform mat4 object_matrix;
out vec3 v_position;out vec3 v_normal;out vec2 v_uv;out vec4 v_ambient;out vec3 v_smooth;out vec2 v_lightmap;out vec4 v_color;out vec3 v_world;
void main(){
    v_position=position;v_normal=normal;v_uv=uv;v_ambient=ambient;v_smooth=tangent;v_lightmap=uv_lightmap;v_color=diffuse_color;
    v_world=(object_matrix*vec4(position,1.0)).xyz;
}
)GLSL";
// tess_view: x viewport height in pixels, y tan(half vertical FOV) (Vulkan frame.viewport.y / frame.cameraUp.w).
constexpr const char* gl_tess_control = R"GLSL(#version 400 core
layout(vertices=3) out;
in vec3 v_position[];in vec3 v_normal[];in vec2 v_uv[];in vec4 v_ambient[];in vec3 v_smooth[];in vec2 v_lightmap[];in vec4 v_color[];in vec3 v_world[];
out vec3 t_position[];out vec3 t_normal[];out vec2 t_uv[];out vec4 t_ambient[];out vec3 t_smooth[];out vec2 t_lightmap[];out vec4 t_color[];
uniform vec3 camera_position;
uniform vec4 pbr;
uniform vec2 tess_view;
uniform mat4 view_projection;
uniform mat4 object_matrix;
)GLSL" IKV_GL_PBR R"GLSL(
float edge_level(vec3 a,vec3 b){
    float d=max(distance((a+b)*0.5,camera_position),0.5);
    float pixels=distance(a,b)/d*tess_view.x*0.5/max(tess_view.y,1e-3);
    return clamp(pixels/kPbrTessPixels*pbrTessFade(d),1.0,64.0);
}
// Vulkan scene_tess.tesc patchOutside: the patch, displaced by up to pbr.z along its smoothed normals, outside one plane.
bool patch_outside(){
    mat3 m=mat3(object_matrix);vec4 out4=vec4(1.0);bool behind=true;
    for(int i=0;i<6;++i){
        vec3 n=m*v_smooth[i%3];float l=length(n);
        vec4 p=view_projection*vec4(v_world[i%3]+(i<3||l<=1e-6?vec3(0.0):n/l*pbr.z),1.0);
        out4*=vec4(bvec4(p.x<-p.w,p.x>p.w,p.y<-p.w,p.y>p.w));behind=behind&&p.z<-p.w;
    }
    return dot(out4,vec4(1.0))>0.0||behind;
}
void main(){
    t_position[gl_InvocationID]=v_position[gl_InvocationID];t_normal[gl_InvocationID]=v_normal[gl_InvocationID];
    t_uv[gl_InvocationID]=v_uv[gl_InvocationID];t_ambient[gl_InvocationID]=v_ambient[gl_InvocationID];
    t_smooth[gl_InvocationID]=v_smooth[gl_InvocationID];t_lightmap[gl_InvocationID]=v_lightmap[gl_InvocationID];
    t_color[gl_InvocationID]=v_color[gl_InvocationID];
    if(gl_InvocationID==0){
        bool rigid=pbr.z<=0.0||(dot(v_smooth[0],v_smooth[0])+dot(v_smooth[1],v_smooth[1])+dot(v_smooth[2],v_smooth[2]))<1e-12;
        if(!rigid&&patch_outside()){gl_TessLevelOuter[0]=gl_TessLevelOuter[1]=gl_TessLevelOuter[2]=gl_TessLevelInner[0]=0.0;return;}
        float e0=rigid?1.0:edge_level(v_world[1],v_world[2]),e1=rigid?1.0:edge_level(v_world[2],v_world[0]),e2=rigid?1.0:edge_level(v_world[0],v_world[1]);
        gl_TessLevelOuter[0]=e0;gl_TessLevelOuter[1]=e1;gl_TessLevelOuter[2]=e2;gl_TessLevelInner[0]=max(e0,max(e1,e2));
    }
}
)GLSL";
constexpr const char* gl_tess_evaluation = R"GLSL(#version 400 core
layout(triangles,fractional_odd_spacing,ccw) in;
in vec3 t_position[];in vec3 t_normal[];in vec2 t_uv[];in vec4 t_ambient[];in vec3 t_smooth[];in vec2 t_lightmap[];in vec4 t_color[];
uniform mat4 view_projection;
uniform mat4 object_matrix;
uniform mat3 normal_matrix;
uniform mat4 light_view_projection;
uniform mat4 far_light_view_projection;
uniform vec3 sun_direction;
uniform vec3 camera_position;
uniform bool source_lighting;
uniform vec3 ambient_multiplier;
uniform vec3 diffuse_multiplier;
uniform bool fog_enabled;
uniform float fog_start;
uniform float fog_end;
uniform vec4 pbr;
uniform vec2 tess_view;
uniform bool has_fade_map;uniform bool has_normal_map;uniform bool has_environment_map;
uniform sampler2D height_map;
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
out vec3 source_ambient_light;
out float sky_scatter;
out float fade_blend;
)GLSL" IKV_GL_PBR R"GLSL(
#define LERP(a) (gl_TessCoord.x*a[0]+gl_TessCoord.y*a[1]+gl_TessCoord.z*a[2])
void main(){
    vec3 position=LERP(t_position),normal=LERP(t_normal),smooth_normal=LERP(t_smooth);
    vec2 uv=LERP(t_uv);vec4 ambient=LERP(t_ambient);
    vec4 world_position=object_matrix*vec4(position,1.0);
    float weight=min(length(smooth_normal),1.0);
    if(weight>1e-4&&pbr.z>0.0){
        vec3 n=normalize(normal_matrix*smooth_normal);
        float d=distance(world_position.xyz,camera_position),fade=pbrTessFade(d);
        if(fade>0.0){
            vec2 huv=pbr.x>0.0?pbrWorldUV(world_position.xyz,n,pbr.xy):uv;
            float lod=pbrHeightLod(pbr,d,tess_view.y,tess_view.x,float(textureSize(height_map,0).x));
            // Terrain fade layers (scene_fragment pbr_main_weight): the main pack's relief only where its layer shows.
            float layer=has_fade_map&&!has_normal_map&&!has_environment_map?clamp(LERP(t_color).a,0.0,1.0):1.0;
            world_position.xyz+=n*(textureLod(height_map,huv,lod).r*pbr.z*weight*fade*layer);
        }
    }
    gl_Position=view_projection*world_position;
    fragment_position=world_position.xyz;shadow_position=light_view_projection*world_position;far_shadow_position=far_light_view_projection*world_position;
    vec3 n=normal_matrix*normal;float nl=length(n);
    float texel=2.0/(2048.0*max(length(vec3(light_view_projection[0][0],light_view_projection[1][0],light_view_projection[2][0])),1e-6));
    float far_texel=2.0/(2048.0*max(length(vec3(far_light_view_projection[0][0],far_light_view_projection[1][0],far_light_view_projection[2][0])),1e-6));
    if(nl>0.00001){n/=nl;shadow_position+=light_view_projection*vec4(n*texel*2.0*(1.0-max(dot(n,normalize(sun_direction)),0.0)),0.0);
        far_shadow_position+=far_light_view_projection*vec4(n*far_texel*2.0*(1.0-max(dot(n,normalize(sun_direction)),0.0)),0.0);}
    texcoord=uv;world_normal=normal_matrix*normal;world_tangent=vec3(0.0); // the copy's tangent holds the smoothed normal
    lightmap_uv=LERP(t_lightmap);fade_blend=LERP(t_color).a;environment_uv=vec2(0.0);
    source_tangent_sun=vec3(0.0);source_tangent_eye=vec3(0.0);
    source_vertex_light=source_ambient_light=ambient.rgb;
    if(source_lighting){
        float directional=clamp(dot(mat3(object_matrix)*normal,sun_direction),0.0,1.0);
        source_vertex_light=clamp(ambient.rgb*ambient_multiplier+directional*diffuse_multiplier,0.0,1.0);
        source_ambient_light=clamp(ambient.rgb*ambient_multiplier,0.0,1.0);
    }
    sky_scatter=0.0;
    float source_clip_z=(gl_Position.z+gl_Position.w)*0.5;
    fog_blend=fog_enabled?clamp((fog_end-source_clip_z)/(fog_end-fog_start),0.0,1.0):1.0;
}
)GLSL";
} // namespace
} // namespace ikv
