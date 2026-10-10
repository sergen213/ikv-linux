#version 450
#extension GL_GOOGLE_include_directive : require
layout(location=0) in vec3 position;
layout(location=2) in vec2 uv;
layout(location=6) in vec4 sourceColor;
#include "frame.glsl"
layout(push_constant) uniform Draw {mat4 model;vec4 color;vec4 flags;vec4 extra;vec4 scroll;} draw;
layout(location=0) out vec2 texcoord;
layout(location=1) out float fadeWeight;
void main(){
    texcoord=draw.extra.w>=2?uv+draw.scroll.xy:uv;fadeWeight=sourceColor.a;
    vec4 world=draw.model*vec4(position,1);
    if(draw.extra.w>=2)world=sourceOceanPosition(world);
    gl_Position=frame.lightVP*world;
}
