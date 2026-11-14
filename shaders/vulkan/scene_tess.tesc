#version 450
#extension GL_GOOGLE_include_directive : require
// NONCANONICAL wave 29: distance-adaptive tessellation of PBR relief (pbr.glsl). Each outer level depends only on its
// edge's two world end points (length projected at the edge midpoint's distance), so the two patches sharing an edge
// always split it identically: no T-junction cracks. Patches without any displaceable vertex stay untessellated.
layout(vertices=3) out;
#include "frame.glsl"
#include "pbr.glsl"
layout(push_constant) uniform Draw { mat4 model; vec4 color; vec4 flags; vec4 extra; vec4 scroll; mat4 previousModel; vec4 pbr; } draw;
layout(location=0) in vec3 vPosition[];
layout(location=1) in vec3 vNormal[];
layout(location=2) in vec2 vUV[];
layout(location=3) in vec4 vAmbient[];
layout(location=4) in vec3 vSmooth[];
layout(location=5) in vec2 vSecondaryUV[];
layout(location=6) in vec4 vColor[];
layout(location=7) in vec3 vPrevious[];
layout(location=8) in vec3 vWorld[];
layout(location=0) out vec3 tPosition[];
layout(location=1) out vec3 tNormal[];
layout(location=2) out vec2 tUV[];
layout(location=3) out vec4 tAmbient[];
layout(location=4) out vec3 tSmooth[];
layout(location=5) out vec2 tSecondaryUV[];
layout(location=6) out vec4 tColor[];
layout(location=7) out vec3 tPrevious[];
float edgeLevel(vec3 a,vec3 b){
    float d=max(distance((a+b)*.5,frame.eye.xyz),.5);
    float pixels=distance(a,b)/d*frame.viewport.y*.5/max(frame.cameraUp.w,1e-3);
    return clamp(pixels/kPbrTessPixels*pbrTessFade(d),1.,64.);
}
void main(){
    tPosition[gl_InvocationID]=vPosition[gl_InvocationID];tNormal[gl_InvocationID]=vNormal[gl_InvocationID];
    tUV[gl_InvocationID]=vUV[gl_InvocationID];tAmbient[gl_InvocationID]=vAmbient[gl_InvocationID];
    tSmooth[gl_InvocationID]=vSmooth[gl_InvocationID];tSecondaryUV[gl_InvocationID]=vSecondaryUV[gl_InvocationID];
    tColor[gl_InvocationID]=vColor[gl_InvocationID];tPrevious[gl_InvocationID]=vPrevious[gl_InvocationID];
    if(gl_InvocationID==0){
        bool rigid=draw.pbr.z<=0.||(dot(vSmooth[0],vSmooth[0])+dot(vSmooth[1],vSmooth[1])+dot(vSmooth[2],vSmooth[2]))<1e-12;
        float e0=rigid?1.:edgeLevel(vWorld[1],vWorld[2]),e1=rigid?1.:edgeLevel(vWorld[2],vWorld[0]),e2=rigid?1.:edgeLevel(vWorld[0],vWorld[1]);
        gl_TessLevelOuter[0]=e0;gl_TessLevelOuter[1]=e1;gl_TessLevelOuter[2]=e2;
        gl_TessLevelInner[0]=max(e0,max(e1,e2));
    }
}
