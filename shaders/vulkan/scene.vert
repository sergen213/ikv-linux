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
// The RT depth prepass (renderer_vk.cpp prepass_) draws this shader without a fragment stage, then shades with depth
// writes off against that depth: both pipelines must compute bit-identical positions.
invariant gl_Position;
#include "scene_vertex.glsl"
void main(){sceneVertex(position,normal,uv,ambient,tangent,secondaryUV,sourceColor,previousPosition,vec3(0));}
