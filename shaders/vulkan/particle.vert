#version 450
#extension GL_GOOGLE_include_directive : require
// Original particle billboards (0049b6e0): world-space corners, texture*vertex colour, D3DRS_DEPTHBIAS on NDC depth.
layout(location=0) in vec3 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec4 color;
#include "frame.glsl"
layout(push_constant) uniform Draw { mat4 model; vec4 color; vec4 flags; vec4 extra; vec4 scroll; } draw;
layout(location=0) out vec2 texcoord;
layout(location=1) out vec4 tint;
void main(){
    texcoord=uv;tint=color;
    gl_Position=frame.vp*vec4(position,1);
    // v_particles.vsh fogs alpha only: oD0.w=sat(a*(c20.z-clipZ)/(c20.z-c20.y)), D3D clip Z before DEPTHBIAS.
    if(frame.fogColor.w>.5&&frame.fog.y>frame.fog.x)tint.a=clamp(color.a*(frame.fog.y-gl_Position.z)/(frame.fog.y-frame.fog.x),0,1);
    gl_Position.z+=draw.flags.x*gl_Position.w;
}
