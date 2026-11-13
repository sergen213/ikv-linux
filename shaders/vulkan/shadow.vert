#version 450
#extension GL_GOOGLE_include_directive : require
layout(location=0) in vec3 position;
layout(location=2) in vec2 uv;
layout(location=6) in vec4 sourceColor;
#include "frame.glsl"
layout(push_constant) uniform Draw {mat4 model;vec4 color;vec4 flags;vec4 extra;vec4 scroll;mat4 cascadeLight;} draw;
layout(location=0) out vec2 texcoord;
layout(location=1) out float fadeWeight;
void main(){
    texcoord=uv;fadeWeight=sourceColor.a;
    // the source ocean casts no shadow (renderer_vk.cpp draw_scene). NONCANONICAL wave 27: swaying draws bend as in scene.vert.
    vec3 sway=weatherSway(position.y,(draw.model*vec4(position,1)).xyz,draw.scroll,frame.wind,frame.weatherMisc,frame.sourceTime.y);
    gl_Position=sway==vec3(0)?draw.cascadeLight*draw.model*vec4(position,1):draw.cascadeLight*(draw.model*vec4(position,1)+vec4(sway,0));
}
