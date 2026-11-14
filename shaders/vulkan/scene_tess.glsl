// Body of scene_tess.tese / shadow_tess.tese (NONCANONICAL wave 29).
// NONCANONICAL wave 29: tessellated PBR relief, evaluation (scene_tess.tesc). The generated vertex moves outward along the
// welded smoothed normal by height x relief depth x weight (0 on open/seam edges, pbr_tessellation_mesh) x distance fade:
// every factor is a function of position and the shared vertex data only, so neighbouring patches agree on shared edges.
// Outward only: raster shadow receivers then never sink under their own caster depth. shadow_tess.tese reuses this
// file with IKV_SHADOW_TESS (light-space position only). Pipelines set LOWER_LEFT domain origin (GL semantics): ccw
// keeps each patch's own winding for the CLOCKWISE front face.
layout(triangles,fractional_odd_spacing,ccw) in;
#ifdef IKV_SHADOW_TESS
#include "frame.glsl"
layout(push_constant) uniform Draw { mat4 model; vec4 color; vec4 flags; vec4 extra; vec4 scroll; mat4 cascadeLight; vec4 pbr; } draw;
#else
#include "scene_vertex.glsl"
#endif
#include "pbr.glsl"
layout(set=0,binding=14) uniform sampler2D heightMap;
layout(location=0) in vec3 tPosition[];
layout(location=1) in vec3 tNormal[];
layout(location=2) in vec2 tUV[];
layout(location=3) in vec4 tAmbient[];
layout(location=4) in vec3 tSmooth[];
layout(location=5) in vec2 tSecondaryUV[];
layout(location=6) in vec4 tColor[];
layout(location=7) in vec3 tPrevious[];
#define LERP(a) (gl_TessCoord.x*a[0]+gl_TessCoord.y*a[1]+gl_TessCoord.z*a[2])
void main(){
    vec3 position=LERP(tPosition),smoothNormal=LERP(tSmooth);
    vec3 world=(draw.model*vec4(position,1)).xyz;
    float weight=min(length(smoothNormal),1.);
    vec3 offset=vec3(0);
    if(weight>1e-4&&draw.pbr.z>0.){
        mat3 m=mat3(draw.model);
        vec3 n=normalize(mat3(cross(m[1],m[2]),cross(m[2],m[0]),cross(m[0],m[1]))*smoothNormal*sign(dot(m[0],cross(m[1],m[2]))));
#ifdef IKV_SHADOW_TESS
        float d=distance(world,frame.eye.xyz),fade=pbrShadowTessFade(d); // <= the receiver's pbrTessFade
#else
        float d=distance(world,frame.eye.xyz),fade=pbrTessFade(d);
#endif
        if(fade>0.){
            vec2 uv=draw.pbr.x>0.?pbrWorldUV(world,n,draw.pbr.xy):LERP(tUV);
            float lod=pbrHeightLod(draw.pbr,d,frame.cameraUp.w,frame.viewport.y,float(textureSize(heightMap,0).x));
            // Terrain fade layers (lighting.glsl pbrMainWeight): the main pack's relief only where its layer shows.
            float layer=draw.flags.z>.5&&draw.flags.x<.5&&draw.flags.y<.5?clamp(LERP(tColor).a,0.,1.):1.;
            offset=n*(textureLod(heightMap,uv,lod).r*draw.pbr.z*weight*fade*layer);
        }
    }
#ifdef IKV_SHADOW_TESS
    gl_Position=draw.cascadeLight*vec4(world+offset,1);
#else
    // The source tangent is not carried (the copy's tangent holds the smoothed normal): source normal maps stay unmapped.
    sceneVertex(position,normalize(LERP(tNormal)),LERP(tUV),LERP(tAmbient),vec3(0),LERP(tSecondaryUV),LERP(tColor),LERP(tPrevious),offset);
#endif
}
