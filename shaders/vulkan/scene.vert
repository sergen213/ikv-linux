#version 450
#extension GL_GOOGLE_include_directive : require
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 uv;
layout(location=3) in vec4 ambient;
layout(location=4) in vec3 tangent;
layout(location=5) in vec2 secondaryUV;
layout(location=6) in vec4 sourceColor;
// Binding 1: the previous frame's pose of skinned meshes (the same buffer as binding 0 for static meshes).
layout(location=7) in vec3 previousPosition;
#include "frame.glsl"
layout(push_constant) uniform Draw { mat4 model; vec4 color; vec4 flags; vec4 extra; vec4 scroll; mat4 previousModel; } draw;
layout(location=0) out vec3 worldPosition;
layout(location=1) out vec3 worldNormal;
layout(location=2) out vec2 texcoord;
layout(location=3) out vec4 shadowPosition;
layout(location=4) out vec3 worldTangent;
layout(location=5) out vec4 vertexAmbient;
layout(location=6) out float sourceFog;
layout(location=7) out vec2 lightmapUV;
layout(location=8) out vec3 localNormal;
layout(location=9) out float fadeWeight;
layout(location=10) out vec3 sourceOceanEye;
layout(location=11) out vec2 sourceOceanUV;
// Unjittered clip positions this and last frame: per-pixel motion vectors (FSR 3.1.4, RT history reprojection).
layout(location=12) out vec4 motionCurrent;
layout(location=13) out vec4 motionPrevious;
void main(){
    vec4 world=draw.model*vec4(position,1);
    bool ocean=draw.extra.w>=2;
    if(ocean)world=sourceOceanPosition(world);
    sourceOceanEye=ocean?normalize(frame.eye.xyz-world.xyz):vec3(0);
    sourceOceanUV=ocean?uv+draw.scroll.zw:vec2(0);
    worldPosition=world.xyz;
    worldNormal=normalize(transpose(inverse(mat3(draw.model)))*normal);
    worldTangent=mat3(draw.model)*tangent;
    texcoord=ocean?uv+draw.scroll.xy:uv;
    lightmapUV=secondaryUV;
    localNormal=normal;
    fadeWeight=sourceColor.a;
    vertexAmbient=ambient;
    // D3D vs_1_1 saturates oD0 before the pixel shader consumes it.
    if(frame.sun.w>.5)vertexAmbient=vec4(clamp(ambient.rgb*frame.ambient.rgb+
        clamp(dot(mat3(draw.model)*normal,frame.sun.xyz),0,1)*frame.diffuse.rgb,0,1),1);
    if(draw.extra.y>.5)vertexAmbient=vec4(.80000001*sqrt(max(frame.ambient.rgb,vec3(0))),1);
    if(ocean)vertexAmbient=vec4(frame.ambient.rgb,1);
    shadowPosition=frame.lightVP*world;
    gl_Position=frame.vp*world;
    motionCurrent=vec4(gl_Position.xy-frame.jitter.xy*gl_Position.w,gl_Position.zw);
    // Ocean displacement is evaluated at this frame's time for both: only the transform moves the water.
    vec4 previousWorld=draw.previousModel*vec4(previousPosition,1);
    if(ocean)previousWorld=sourceOceanPosition(previousWorld);
    motionPrevious=frame.previousVP*previousWorld;
    motionPrevious.xy-=frame.jitter.zw*motionPrevious.w;
    sourceFog=1;
    if(frame.fogColor.w>.5&&draw.extra.z>.5&&frame.fog.y>frame.fog.x)
        sourceFog=(frame.fog.y-gl_Position.z)/(frame.fog.y-frame.fog.x);
}
