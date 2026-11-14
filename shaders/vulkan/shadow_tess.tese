#version 450
#extension GL_GOOGLE_include_directive : require
// NONCANONICAL wave 29: shadow-map pass of tessellated PBR relief (scene_tess.glsl with IKV_SHADOW_TESS).
#define IKV_SHADOW_TESS
#include "scene_tess.glsl"
