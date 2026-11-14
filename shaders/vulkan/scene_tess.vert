#version 450
// NONCANONICAL wave 29: tessellated PBR relief (renderer_vk.cpp opaque_tess_/shadow_tess_). The vertex stage only forwards
// the object-space attributes; scene_tess.tese evaluates scene.vert's outputs (scene_vertex.glsl) per generated vertex.
// The draw's mesh is the tessellation copy (pbr.hpp pbr_tessellation_mesh): tangent = welded smoothed normal x weight.
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 uv;
layout(location=3) in vec4 ambient;
layout(location=4) in vec3 tangent;
layout(location=5) in vec2 secondaryUV;
layout(location=6) in vec4 sourceColor;
layout(location=7) in vec3 previousPosition;
layout(push_constant) uniform Draw { mat4 model; vec4 color; vec4 flags; vec4 extra; vec4 scroll; mat4 previousModel; vec4 pbr; } draw;
layout(location=0) out vec3 vPosition;
layout(location=1) out vec3 vNormal;
layout(location=2) out vec2 vUV;
layout(location=3) out vec4 vAmbient;
layout(location=4) out vec3 vSmooth;
layout(location=5) out vec2 vSecondaryUV;
layout(location=6) out vec4 vColor;
layout(location=7) out vec3 vPrevious;
layout(location=8) out vec3 vWorld;
void main(){
    vPosition=position;vNormal=normal;vUV=uv;vAmbient=ambient;vSmooth=tangent;vSecondaryUV=secondaryUV;vColor=sourceColor;
    vPrevious=previousPosition;vWorld=(draw.model*vec4(position,1)).xyz;
}
