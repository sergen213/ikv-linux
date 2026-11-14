// NONCANONICAL wave 29: enhanced PBR texture pack (include/ikv/pbr.hpp, assets/pbr/README.md), shared by the Vulkan
// scene fragment/tessellation shaders; renderer_gl.cpp embeds this file (IKV_GL_PBR), so it holds no preprocessor lines.
// The per-draw vec4 `pbr` (Vulkan push constant, GL uniform):
//   xy  pack tiles per scene unit (0.25 m / manifest scale metres); 0 = the original UVs carry the pack maps
//   z   relief depth in scene units (height_scale metres x 4): parallax depth and tessellation displacement
//   w   tint_keep_original + 2 when parallax occlusion is on for the draw; negative = no pack material
// World projection: the dominant axis of the surface normal picks the plane (floors XZ, walls XY or ZY with image-up =
// world +Y), so pack maps tile in metres whatever the original UVs (atlases) are.
vec2 pbrWorldUV(vec3 p,vec3 n,vec2 tiles){
    vec3 a=abs(n);
    vec2 uv=a.y>=a.x&&a.y>=a.z?vec2(p.x,p.z):a.x>=a.z?vec2(p.z,-p.y):vec2(p.x,-p.y);
    return uv*tiles;
}
bool pbrActive(vec4 pbr){return pbr.w>=0.;}
bool pbrParallax(vec4 pbr){return pbr.w>=2.;}
float pbrTintKeep(vec4 pbr){return pbr.w>=2.?pbr.w-2.:pbr.w;}
// Tessellation: segments of about kPbrTessPixels pixels, fading to none (and displacement to zero) between these view
// distances (scene units): beyond them parallax-free flat shading costs nothing extra.
const float kPbrTessPixels=8.;
const float kPbrTessFadeStart=40.,kPbrTessFadeEnd=80.;
float pbrTessFade(float distanceToEye){return 1.-smoothstep(kPbrTessFadeStart,kPbrTessFadeEnd,distanceToEye);}
// Shadow casters (Vulkan near cascade only): coarser segments and half the range. The caster fade never exceeds the
// receiver's (pbrTessFade), so a displaced receiver never sinks under its own caster depth.
const float kPbrShadowTessPixels=24.,kPbrShadowTessFadeStart=20.,kPbrShadowTessFadeEnd=40.;
float pbrShadowTessFade(float distanceToEye){return 1.-smoothstep(kPbrShadowTessFadeStart,kPbrShadowTessFadeEnd,distanceToEye);}
// Height mip for displacement: a function of position only (shared tessellated edges sample identically).
float pbrHeightLod(vec4 pbr,float distanceToEye,float tanHalfFov,float viewportHeight,float heightTexels){
    float unitsPerSegment=distanceToEye*2.*tanHalfFov/max(viewportHeight,1.)*kPbrTessPixels;
    float texelsPerUnit=pbr.x>0.?heightTexels*max(pbr.x,pbr.y):heightTexels/16.;
    return clamp(log2(max(unitsPerSegment*texelsPerUnit,1.)),0.,12.);
}
// Albedo (linear): the pack's detail-normalised albedo (neutral mean, mean linear luminance 0.18) carries the original
// surface's low-frequency colour (the original diffuse blurred, pbrLowLod), then blends back toward the original by
// tint_keep_original.
vec3 pbrAlbedo(vec3 packSrgb,vec3 originalLowSrgb,vec3 originalLinear,float keep){
    vec3 base=pow(max(packSrgb,vec3(0)),vec3(2.2))*pow(max(originalLowSrgb,vec3(0)),vec3(2.2))/.18;
    return mix(base,originalLinear,clamp(keep,0.,1.));
}
// Mip of the original diffuse (texSize texels, `levels` mips) that pbrAlbedo multiplies: at least levels-4 and, for world-
// projected pack maps, coarse enough that one texel spans a whole pack tile, so the original's own pattern never prints
// through the pack (terrain textures repeat many times per pack tile: at levels-4 their pebbles and speckles survived and
// hid the pack). uvPerUnit: original UV change per scene unit along the screen axes (fragment derivatives).
float pbrLowLod(vec4 pbr,float uvPerUnit,float texSize,float levels){
    float lod=max(levels-4.,0.);
    if(pbr.x>0.)lod=max(lod,log2(max(uvPerUnit*texSize/max(min(pbr.x,pbr.y),1e-6),1.)));
    return min(lod,max(levels-1.,0.));
}
// Tangent-space pack normal (OpenGL +Y = image up = -v in these top-down textures) onto n, using the surface's uv frame
// dP/du, dP/dv (screen derivatives in the fragment shaders).
vec3 pbrNormal(vec3 n,vec3 pu,vec3 pv,vec3 texel){
    vec3 t=pu-n*dot(n,pu),b=-(pv-n*dot(n,pv));
    if(dot(t,t)<1e-20||dot(b,b)<1e-20)return n;
    t=normalize(t);b=normalize(b);
    vec3 m=texel*2.-1.;
    return normalize(t*m.x+b*m.y+n*max(m.z,.05));
}
