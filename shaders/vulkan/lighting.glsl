layout(location=0) in vec3 worldPosition;
layout(location=1) in vec3 worldNormal;
layout(location=2) in vec2 texcoord;
layout(location=3) in vec4 shadowPosition;
layout(location=4) in vec3 worldTangent;
layout(location=5) in vec4 vertexAmbient;
layout(location=6) in float sourceFog;
layout(location=7) in vec2 lightmapUV;
layout(location=8) in vec3 localNormal;
layout(location=9) in float fadeWeight;
layout(location=10) in vec3 sourceOceanEye;
layout(location=11) in vec2 sourceOceanUV;
// v_dif_lit oD0 terms before the vs_1_1 saturate: lighting_multipliers_1 x ambd colour, saturate(N.Sun) x multipliers_2.
// RT shadows the directional term only (RTLIGHTING faithful RT); raster keeps vertexAmbient, the per-vertex saturate.
layout(location=14) in vec3 sourceAmbientTerm;
layout(location=15) in vec3 sourceSunTerm;
layout(location=0) out vec4 outputColor;
// Unjittered clip positions this/last frame (scene.vert); location 2 = previousUV-UV, 5 = FSR 3.1.4 reactive coverage.
// Unbound locations are dropped: the pipelines without motion/FSR attachments write only colour.
layout(location=12) in vec4 motionCurrent;
layout(location=13) in vec4 motionPrevious;
layout(location=2) out vec2 outputMotion;
layout(location=5) out float outputReactive;
#include "frame.glsl"
#ifdef IKV_RT_GUIDED
#include "rt_guide.glsl"
layout(location=1) out vec4 outputGuide;
// Stochastic RT terms the guided resolve denoises: indirect diffuse gain (multiplies artwork) and additive
// reflection; reflection alpha flags animated draws (history needs their motion vectors).
layout(location=3) out vec4 outputDiffuse;
layout(location=4) out vec4 outputReflection;
#endif
layout(push_constant) uniform Draw { mat4 model; vec4 color; vec4 flags; vec4 extra; vec4 scroll; } draw;
layout(set=0,binding=1) uniform sampler2D diffuseMap;
layout(set=0,binding=2) uniform sampler2D normalMap;
layout(set=0,binding=3) uniform sampler2D environmentMap;
layout(set=0,binding=4) uniform sampler2D fadeMap;
layout(set=0,binding=5) uniform sampler2DShadow shadowMap; // compare sampler (renderer_vk.cpp shadow_compare_): bilinear PCF taps
layout(set=0,binding=9) uniform sampler2D originalLightmap;
const float PI=3.14159265359;
vec3 sky(vec3 direction) { return mix(vec3(.055,.07,.09),vec3(.32,.44,.62),clamp(direction.y*.5+.5,0,1)); }
vec3 environment(vec3 direction) { if(draw.flags.y<.5)return sky(direction);return textureLod(environmentMap,vec2(atan(direction.z,direction.x)/(2*PI)+.5,acos(clamp(direction.y,-1,1))/PI),0).rgb; }
// Shadow atlas (4096x2048): near, then far cascade, in two columns.
layout(location=16) in vec4 farShadowPosition;
float shadowPcf(vec4 clip,float offset,vec3 n){vec3 p=clip.xyz/clip.w;vec2 uv=p.xy*.5+.5;if(any(lessThan(uv,vec2(0)))||any(greaterThan(uv,vec2(1)))||p.z<0||p.z>1)return 1;uv.x=uv.x*.5+offset;float bias=max(.0003,.002*(1-dot(n,normalize(frame.sun.xyz))));vec2 t=1./vec2(textureSize(shadowMap,0));vec2 lo=vec2(offset+t.x*.5,0),hi=vec2(offset+.5-t.x*.5,1);float visible=0;for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++)visible+=texture(shadowMap,vec3(clamp(uv+vec2(x,y)*t,lo,hi),p.z-bias));return visible/9;}
float shadowVisibility(vec3 n){vec3 p=shadowPosition.xyz/shadowPosition.w;vec2 uv=p.xy*.5+.5;bool nearValid=distance(frame.eye.xyz,worldPosition)<=frame.shadowParams.x&&all(greaterThanEqual(uv,vec2(0)))&&all(lessThanEqual(uv,vec2(1)))&&p.z>=0&&p.z<=1;if(nearValid)return shadowPcf(shadowPosition,0,n);return shadowPcf(farShadowPosition,.5,n);}
// NONCANONICAL parallax occlusion (enhanced): static zone materials get a relief texture derived from their diffuse
// (scene.cpp derive_parallax_heights: r height, 1 = surface; g/b central differences + .5); others bind the 1x1 white.
// Zone meshes carry no usable tangents: the frame comes from screen derivatives (exact per planar triangle, linear UVs).
// The depth is 4 relief texels' world size (relief scales with its features; atlases stay inside their regions), capped
// at .35 units; it fades out with distance and toward grazing angles. Offset
// UVs feed diffuse/normal/fade samples, never the lightmap; geometry, depth, motion vectors and RT rays stay on the true
// surface. parallaxGradient: world gradient of the relief, tilting the enhanced lighting normal.
layout(set=0,binding=14) uniform sampler2D heightMap;
vec2 surfaceUV;
vec3 parallaxGradient=vec3(0);
void parallax(){
    surfaceUV=texcoord;
    if(frame.params.x<.5||textureSize(heightMap,0).x<2)return;
    vec3 dp1=dFdx(worldPosition),dp2=dFdy(worldPosition),nf=cross(dp1,dp2),v=frame.eye.xyz-worldPosition;
    vec2 du1=dFdx(texcoord),du2=dFdy(texcoord);float det=du1.x*du2.y-du2.x*du1.y,dist=length(v);
    if(abs(det)<1e-12||dot(nf,nf)<1e-20||dist<1e-4)return;
    nf=normalize(nf);v/=dist;if(dot(nf,v)<0)nf=-nf;
    vec3 pu=(dp1*du2.y-dp2*du1.y)/det,pv=(dp2*du1.x-dp1*du2.x)/det; // dP/du, dP/dv
    vec3 gu=cross(pv,nf),gv=cross(nf,pu);gu/=dot(pu,gu);gv/=dot(pv,gv); // in-plane gradients of u and v
    float c=dot(v,nf),depth=smoothstep(.05,.18,c)*(1-smoothstep(50.,90.,dist))*min(12*min(length(pu),length(pv))/textureSize(heightMap,0).x,.6);
    if(!(depth>1e-3))return;
    vec2 shift=-vec2(dot(v,gu),dot(v,gv))/c*depth; // UV travel down to height 0
    int steps=int(mix(24.,8.,c));float t=0,h=textureGrad(heightMap,texcoord,du1,du2).r,lastT=0,lastF=h-1;
    for(int i=0;i<steps&&t<1-h;++i){lastT=t;lastF=t-(1-h);t+=1./steps;h=textureGrad(heightMap,texcoord+shift*t,du1,du2).r;}
    float f=t-(1-h);if(f>0&&lastF<0)t=mix(lastT,t,lastF/(lastF-f)); // secant refinement
    surfaceUV=texcoord+shift*t;
    vec2 slope=(textureGrad(heightMap,surfaceUV,du1,du2).gb-.5)*.5*vec2(textureSize(heightMap,0)); // dh/du, dh/dv
    parallaxGradient=.7*depth*(slope.x*gu+slope.y*gv);
}
vec3 mappedNormal(){
    vec3 n=normalize(worldNormal);
    if(draw.flags.x<.5)return normalize(n-parallaxGradient);
    // Zero/degenerate-tangent source meshes (e.g. NPC staff tips) skip normal mapping, as GL does: a screen-derivative
    // tangent is per-triangle, so it rotated the map differently on every facet.
    vec3 t=worldTangent-n*dot(n,worldTangent);
    if(dot(t,t)<=1e-12)return n;
    t=normalize(t);
    // Source normal maps carry only xy (b: specular mask, a: illumination, originalSurface): z is rebuilt as the original
    // does. Decoding b as z bent specular-free texels (b~0) inward, so normal-mapped actors went near-black (wave 19).
    vec2 xy=texture(normalMap,surfaceUV).xy*2-1;
    return normalize(mat3(t,cross(n,t),n)*normalize(vec3(xy,1-dot(xy,xy))));
}
// Independently expressed recovered original shader equations, not copied proprietary shader source.
// sunVisibility: 1 in raster; RT sun-ray visibility. Occlusion removes only the source directional terms.
vec4 originalSurface(vec4 diffuse,float sunVisibility) {
    bool hasNormal=draw.flags.x>.5,hasEnvironment=draw.flags.y>.5,hasLightmap=draw.extra.x>.5;
    vec3 eye=normalize(frame.eye.xyz-worldPosition);
    vec3 localN=hasNormal?normalize(localNormal):localNormal;
    vec2 environmentUV=(eye-2*localN*dot(localN,eye)).xy;
    vec3 environmentColor=hasEnvironment?texture(environmentMap,environmentUV).rgb:vec3(0);
    // Source meshes carry zero tangents (e.g. NPC staff tips): like GL, those fragments take the unmapped path,
    // since normalize(0) is NaN and the 9x9 glow taps would spread it as a dot grid.
    vec3 n=worldNormal,t=cross(cross(n,worldTangent),n); // tangent's component orthogonal to n, scaled by |n|^2
    if(hasNormal&&frame.sun.w>.5&&dot(t,t)>1e-12) {
        vec4 normalTexel=texture(normalMap,surfaceUV);
        vec2 xy=normalTexel.xy*2-1;
        vec3 mapped=normalize(vec3(xy,1-dot(xy,xy)));
        n=normalize(n);t=normalize(t);
        vec3 b=cross(n,t);
        vec3 tangentSun=vec3(dot(-frame.sun.xyz,t),dot(-frame.sun.xyz,b),dot(-frame.sun.xyz,n));
        vec3 tangentEye=normalize(vec3(dot(eye,t),dot(eye,b),dot(eye,n)));
        // Fully occluded = facing away from the Sun (factor 1: the dark I_b end of the source sky curve).
        float factor=mix(1.,.5*dot(mapped,tangentSun)+.5,sunVisibility);
        float exponent=hasEnvironment?64:48;
        vec3 halfway=mapped+tangentEye;
        float specular=factor>0&&dot(halfway,halfway)>1e-12?pow(max(dot(mapped,normalize(halfway)),0),exponent):0;
        vec3 illumination=mix(frame.skyC.rgb,frame.skyB.rgb,factor)*normalTexel.a;
        vec3 result=hasEnvironment?
            illumination*(diffuse.rgb+specular*(normalTexel.b+diffuse.a*environmentColor)):
            illumination*(diffuse.rgb+specular*normalTexel.b)+diffuse.rgb*(1-diffuse.a);
        return vec4(result,1);
    }
    vec3 lightmap=hasLightmap?texture(originalLightmap,lightmapUV).rgb:vec3(0);
    vec3 vertexLight=sunVisibility<1&&frame.sun.w>.5?clamp(sourceAmbientTerm+sourceSunTerm*sunVisibility,0,1):vertexAmbient.rgb;
    if(hasEnvironment) {
        vec3 result=hasLightmap?
            mix(diffuse.rgb,environmentColor,diffuse.a)-lightmap*(1-clamp(diffuse.a,0,1))*frame.fog.z:
            diffuse.rgb+diffuse.a*environmentColor;
        return vec4(result*vertexLight,1);
    }
    return vec4((diffuse.rgb-lightmap*frame.fog.z)*vertexLight,diffuse.a);
}
// Enhanced raster and faithful RT albedo keep the original subtractive lightmap darkening (originalSurface; not on the
// normal-mapped path): without it the sun lit lightmap-darkened art 3x brighter than original (NONCANONICAL wave 19).
// Physical RT traces its own occlusion and has no lightmap term.
vec3 enhancedLightmapShadow(float alpha){
#ifdef IKV_RT
    if(frame.params.x<1.5)return vec3(0);
#endif
    if(draw.extra.x<.5||draw.flags.x>.5)return vec3(0);
    vec3 shadow=texture(originalLightmap,lightmapUV).rgb*frame.fog.z;
    return draw.flags.y>.5?shadow*(1-clamp(alpha,0,1)):shadow;
}
vec4 sourceOceanSurface() {
    vec3 normal0=texture(normalMap,sourceOceanUV*.80000001).rgb*2-1;
    vec3 normal1=texture(normalMap,texcoord*.23999999).rgb*2-1;
    vec3 n=normalize(normal0+normal1).xzy;
    float facing=dot(n,sourceOceanEye);
    vec3 reflection=sourceOceanEye-2*n*facing;
    float specular=pow(clamp(dot(reflection,-frame.sun.xyz),0,1),64)*
        (1-clamp(facing,0,1));
    vec3 result=texture(diffuseMap,texcoord).rgb*vertexAmbient.rgb+
        specular*frame.skyC.rgb*.80000001;
    if(frame.fogColor.w>.5&&draw.extra.z>.5)
        result=mix(frame.fogColor.rgb,result,clamp(sourceFog,0,1));
    // Enhanced output is linear, but authored ocean lighting is retained.
    if(frame.params.x>.5)result=pow(max(result,vec3(0)),vec3(2.2));
    return vec4(result,1);
}
// NONCANONICAL: enhanced source ocean, OpenUnderworld's (Tomb Raider: Underworld port) water (water.glsl): the opaque
// scene behind the surface (renderer_vk.cpp copy_water: colour + depth copied before the water draws) traced-refracted
// with caustics on the bed and absorbed, SSR (RT physical: a mirror ray against the scene TLAS), afl_ext sea waves and
// Godot normal layers, GGX glints, edge foam. The v_ocean vertex displacement is kept.
layout(set=0,binding=12) uniform sampler2D waterColor;
layout(set=0,binding=13) uniform sampler2D waterDepth;
vec3 waterSceneColor(vec2 uv){return textureLod(waterColor,uv,0).rgb;}
float waterSceneDepth(vec2 uv){return textureLod(waterDepth,uv,0).r;}
float waterNdcZ(float depth){return depth;}
float waterBedSun(vec3 p){
#ifdef IKV_RT
    return rtSunVisibilityMasked(p+vec3(0,.6,0),vec3(0,1,0),normalize(frame.sun.xyz),3u,0u); // ocean (mask 4) never shadows
#else
    vec4 c=frame.lightVP*vec4(p,1);vec3 q=c.xyz/c.w;vec2 uv=q.xy*.5+.5;
    bool nearValid=distance(frame.eye.xyz,p)<=frame.shadowParams.x&&all(greaterThanEqual(uv,vec2(0)))&&all(lessThanEqual(uv,vec2(1)))&&q.z>=0&&q.z<=1;
    return nearValid?shadowPcf(c,0,vec3(0,1,0)):shadowPcf(frame.farLightVP*vec4(p,1),.5,vec3(0,1,0));
#endif
}
bool waterRayReflection(vec3 origin,vec3 direction,inout vec3 colour){
#ifdef IKV_RT
    if(frame.params.x<1.5){colour=rtIncoming(origin,vec3(0,1,0),normalize(vec3(direction.x,max(direction.y,.02),direction.z)),normalize(frame.sun.xyz),0u);return true;}
#endif
    return false;
}
// NONCANONICAL wave 27: weather fog colour in linear (weather.glsl weatherFogColor); without record fog, a grey following
// the clock's sky light.
vec3 weatherFogLinear(){
    vec3 c=pow(max(weatherFogColor(frame.fogColor.rgb,frame.fogColor.w>.5,frame.weather,frame.weatherMisc),vec3(0)),vec3(2.2));
    return frame.fogColor.w>.5?c:c*min(enhancedAmbientScale(),vec3(1));
}
#include "water.glsl"
vec3 waterSurface(){
    vec3 p=worldPosition,l=normalize(frame.sun.xyz),sunScale=enhancedSunScale();
    vec3 light=frame.sun.w>.5?pow(min(frame.ambient.rgb+frame.diffuse.rgb*max(l.y,0),vec3(1)),vec3(2.2)):vec3(1);
#ifdef IKV_RT
    float sun=frame.params.x<1.5?rtSunVisibility(p+vec3(0,.6,0),vec3(0,1,0),l,0u):
        sceneRoofed()?0:rtSunVisibilityMasked(p+vec3(0,.6,0),vec3(0,1,0),l,3u,0u);
#else
    float sun=sceneRoofed()?0:shadowVisibility(vec3(0,1,0));
#endif
    float caustic=sceneRoofed()?0:.8*min(max(sunScale.r,max(sunScale.g,sunScale.b)),1);
    sun=mix(sun,1.,weatherShadowSoftness(frame.weather,frame.weatherMisc)); // NONCANONICAL wave 27 (0 without weather)
    vec3 c=waterShade(p,frame.eye.xyz,frame.vp,gl_FragCoord.xy/frame.viewport.xy,frame.sourceTime.y,l,light,
        frame.fogColor.rgb,vec3(2.8,2.55,2.2)*sunScale,sun,caustic,frame.wind,frame.weather,frame.weatherMisc);
    if(frame.fogColor.w>.5&&draw.extra.z>.5)c=mix(pow(max(frame.fogColor.rgb,vec3(0)),vec3(2.2)),c,clamp(sourceFog,0,1));
    if(weatherOn(frame.weatherMisc))c=mix(weatherFogLinear(),c,weatherFogTransmit(distance(frame.eye.xyz,p),frame.weather,frame.weatherMisc));
    return max(c,vec3(0));
}
void main(){
outputMotion=(motionPrevious.xy/motionPrevious.w-motionCurrent.xy/motionCurrent.w)*.5;
parallax(); // derivatives: before any discard
// Reactive (FSR): opaque pipelines write 0 whatever this is. Blended draws overwrite it: alpha-tested ones (source
// cutoff: foliage, decals) write motion vectors and are accumulated like opaque surfaces (0); untextured translucent
// draws keep their coverage, how much the surface replaces history.
outputReactive=draw.flags.w>0?0:min(texture(diffuseMap,texcoord).a*draw.color.a,.9);
#ifdef IKV_RT
// Physical facet normal from screen derivatives, before any discard leaves helper lanes undefined.
vec3 rtFacet=cross(dFdx(worldPosition),dFdy(worldPosition));
#endif
#ifdef IKV_RT_GUIDED
// rtUnit: zero/NaN source normals (degenerate meshes) would put NaN in the guide, which history and the a-trous
// passes then spread over the screen as a growing black block.
vec3 guideNormal=rtUnit(worldNormal),rtDiffuse=vec3(0),rtReflection=vec3(0);
float rtArtwork=0;
#endif
if(draw.extra.w>=2) {
    outputColor=frame.params.x>.5?vec4(waterSurface(),1):sourceOceanSurface();
    if(draw.flags.w>0&&outputColor.a<=draw.flags.w)discard;
#ifdef IKV_RT_GUIDED
    // Moving water has no displacement motion vectors: spatial guide only, never history or decomposition.
    float depth=max(dot(worldPosition-frame.eye.xyz,frame.cameraForward.xyz),.0001);
    outputGuide=vec4(rtGuideEncodeNormal(guideNormal),0,-depth);
    outputDiffuse=vec4(0);outputReflection=vec4(0);
#endif
    return;
}
vec4 texel=texture(diffuseMap,surfaceUV)*draw.color;
if(draw.extra.y>.5) {
    // NONCANONICAL wave 27: enhanced clouds drift with the wind (weather.glsl weatherSky*).
    bool weather=weatherOn(frame.weatherMisc);
    vec2 skyUV=weather?texcoord+weatherSkyScroll(frame.wind):texcoord;
    if(weather)texel=texture(diffuseMap,skyUV)*draw.color;
    vec3 secondary=draw.flags.y>.5?texture(environmentMap,skyUV).rgb:vec3(0);
    vec3 clouds=mix(texel.rgb,secondary,clamp(frame.fog.w,0,1));
    float mu=dot(normalize(frame.eye.xyz-worldPosition),-normalize(frame.sun.xyz));
    float scatter=(.001*(1+mu*mu)+(.039999995*.995)/(1.64-1.6*mu))*
        (1-pow(2.7182817,-.001)*pow(2.7182817,-.995))/.99599999;
    vec3 skyColor=mix(frame.fogColor.rgb,clouds*vertexAmbient.rgb+vec3(scatter),texel.a);
    if(weather)skyColor=weatherSky(skyColor,weatherFogColor(frame.fogColor.rgb,frame.fogColor.w>.5,frame.weather,frame.weatherMisc),frame.weather,frame.weatherMisc,frame.sourceTime.y);
    outputColor=vec4(frame.params.x>.5?pow(max(skyColor,vec3(0)),vec3(2.2)):skyColor,1);
#ifdef IKV_RT_GUIDED
    outputGuide=vec4(0);outputDiffuse=vec4(0);outputReflection=vec4(0);
#endif
    return;
}
// Source MAT+14 secondary fade is selected only without higher-priority normal/environment maps.
if(draw.flags.z>.5&&draw.flags.x<.5&&draw.flags.y<.5)
    texel=mix(texture(fadeMap,surfaceUV)*draw.color,texel,fadeWeight);
float sourceAlpha=draw.flags.x>.5||draw.flags.y>.5?1:texel.a;
float sunVisibility=1.;
#ifdef IKV_RT
// The source alpha test (originalSurface's alpha, the discard below) before the Sun ray: cut-out texels trace nothing.
if(frame.params.x<.5&&draw.flags.w>0) {
    vec3 t=cross(cross(worldNormal,worldTangent),worldNormal);
    float originalAlpha=(draw.flags.x>.5&&frame.sun.w>.5&&dot(t,t)>1e-12)||draw.flags.y>.5?1.:texel.a;
    if(originalAlpha<=draw.flags.w)discard;
}
// Faithful RT (original appearance): actors (TLAS mask 2) cast deterministic Sun-ray shadows on static surfaces by
// removing the source directional term. Scenery never occludes: the baked lightmaps hold its shadows, from a light that
// is not the dynamic SunDir (with mask 0xff most of the lightmap-lit courtyard and every actor in it went dark).
if(frame.params.x<.5&&frame.sun.w>.5&&fract(draw.extra.w)<.25&&dot(worldNormal,frame.sun.xyz)>0) {
    vec3 facet=rtUnit(rtFacet);
    if(dot(facet,frame.eye.xyz-worldPosition)<0)facet=-facet;
    sunVisibility=rtSunVisibilityMasked(worldPosition,facet,rtUnit(frame.sun.xyz),2u,0u);
}
#endif
vec4 original=frame.params.x<.5?originalSurface(texel,sunVisibility):vec4(texel.rgb,sourceAlpha);
if(draw.flags.w>0&&original.a<=draw.flags.w)discard;
vec3 result=frame.params.x<.5?original.rgb:texel.rgb;
if(frame.params.x>.5){vec3 n=mappedNormal(),v=normalize(frame.eye.xyz-worldPosition),l=normalize(frame.sun.xyz),h=normalize(v+l);float roughness=.65,metallic=0.;vec3 albedo=pow(max(texel.rgb-enhancedLightmapShadow(texel.a),vec3(0)),vec3(2.2));
// NONCANONICAL wave 27: season tint, rain wetness, running streaks, rippled puddles and snow cover (weather.glsl);
// NONCANONICAL wave 28: footstep rings in puddles and prints in snow (frame.groundMarks).
bool weather=weatherOn(frame.weatherMisc);
vec3 sourceAlbedo=albedo;float directRoughness=roughness;WeatherSurface weathered;
if(weather) {
    vec3 up=dot(worldNormal,worldNormal)>1e-12?normalize(worldNormal):n;
    WeatherSteps steps=weatherStepsNone(); // marks read straight from the UBO (no array argument copies)
    if(weatherStepsHere(worldPosition,up,frame.weather,frame.groundMarkInfo,frame.groundMarkBounds))
        for(int i=0;i<64;++i){if(float(i)>=frame.groundMarkInfo.x)break;weatherStep(steps,frame.groundMarks[i],worldPosition,frame.weather);}
    weathered=weatherSurface(albedo,n,up,worldPosition,roughness,frame.sourceTime.y,frame.weather,frame.weatherMisc,steps);
    albedo=weathered.albedo;n=weathered.normal;roughness=weathered.roughness;directRoughness=weathered.directRoughness;
}
float ndl=max(dot(n,l),0),ndv=max(dot(n,v),.001),ndh=max(dot(n,h),0),vdh=max(dot(v,h),0);float a=directRoughness*directRoughness,a2=a*a;float d=a2/(PI*pow(ndh*ndh*(a2-1)+1,2));float k=pow(directRoughness+1,2)/8;float g=(ndv/(ndv*(1-k)+k))*(ndl/(ndl*(1-k)+k));vec3 f0=mix(vec3(.04),albedo,metallic);vec3 f=f0+(1-f0)*pow(1-vdh,5);vec3 specular=d*g*f/max(4*ndv*ndl,.001);vec3 direct=((1-f)*(1-metallic)*albedo/PI+specular)*ndl*3.2;
#ifdef IKV_RT
// params.x 1: physical RT (traced sun, GI, reflections). 2: faithful RT with enhanced appearance: enhanced direct
// shading and diffuse GI below, with a deterministic Sun ray (scenery + actors, mask 3; ocean mask 4 casts none).
if(frame.params.x<1.5){
// Blended draws (extra.w 1, 1.5): foliage and grass trace their GI and reflection rays opaque (rt_lighting.glsl rtDiffuseLighting).
uint secondaryFlags=draw.extra.w>.75&&draw.extra.w<2.?gl_RayFlagsOpaqueEXT:0u;
RtLight light=rtLighting(worldPosition,dot(rtFacet,v)<0?-rtFacet:rtFacet,worldNormal,n,v,roughness,l,uvec2(gl_FragCoord.xy),uint(frame.viewport.z),1.,secondaryFlags);
result=albedo*(light.sun+light.diffuse)+light.reflection;
#ifdef IKV_RT_GUIDED
rtDiffuse=light.diffuse;rtReflection=light.reflection;rtArtwork=rtGuidePackArtwork(weather?pow(albedo,vec3(1/2.2)):texel.rgb);
#endif
}else
#endif
{
// NONCANONICAL wave 19 (renderer_gl.cpp scene_fragment matches): enhanced direct sun and sky reflection follow the
// clock record. Faithful RT replaces outdoor baked ambient with traced diffuse GI; roofed interiors retain their
// original baked light because the source's artificial lights have no RT emission data. Raster uses original baked
// ambient everywhere. The record fog is applied in linear like the original's (sky included).
bool roofed=sceneRoofed();
vec3 baked=pow(max(originalSurface(texel,roofed?1.:0.).rgb,vec3(0)),vec3(2.2));
// Weather on the baked light: the same tint and wetness; snow takes the baked light level.
if(weather)baked=weatherBaked(baked,sourceAlbedo,weathered,frame.weatherMisc)*weatherAmbientTint(frame.weather,frame.weatherMisc,frame.sourceTime.y);
// x1.5: calibrated against the ACES toe so enhanced is about as bright as original (whose fake-HDR glow is original-only):
// Eminönü noon 108 vs 115, every roofed interior above original (analysis/porting-wave19/LightingFixes/report.md).
vec3 ambient=baked;
#ifdef IKV_RT
vec3 facet=rtUnit(rtFacet);if(dot(facet,v)<0)facet=-facet;
// Fully record-fogged (the fog mix below keeps none of the lit colour; fogWeight 0 for the resolve): no rays (wave 27 perf).
bool fogged=frame.fogColor.w>.5&&draw.extra.z>.5&&sourceFog<=0.;
float sunShadow=!roofed&&!fogged&&dot(worldNormal,l)>0?rtSunVisibilityMasked(worldPosition,facet,l,3u,0u):1.;
if(!roofed) {
    vec3 smoothNormal=rtUnit(dot(worldNormal,facet)<0?-worldNormal:worldNormal);
    vec3 diffuse=fogged?vec3(0):rtDiffuseLighting(worldPosition,facet,smoothNormal,n,l,uvec2(gl_FragCoord.xy),uint(frame.viewport.z),1.,
        draw.extra.w>.75&&draw.extra.w<2.?gl_RayFlagsOpaqueEXT:0u); // foliage: opaque GI rays (rtDiffuseLighting)
    ambient=albedo*diffuse;
#ifdef IKV_RT_GUIDED
    // rawHDR = deterministic + albedo*diffuse; after the fog mix the stochastic share also carries this weight.
    float fogWeight=frame.fogColor.w>.5&&draw.extra.z>.5?clamp(sourceFog,0,1):1.;
    rtDiffuse=diffuse*fogWeight;rtArtwork=rtGuidePackArtwork(weather?pow(albedo,vec3(1/2.2)):max(texel.rgb-enhancedLightmapShadow(texel.a),vec3(0)));
#endif
}
#else
float sunShadow=shadowVisibility(n);
#endif
// Weather: cloud/fog light softens sun shadows; puddles mirror the (weather-tinted) sky strongly.
float reflectionGain=.4;vec3 skyReflection=environment(reflect(-v,n));
if(weather){sunShadow=mix(sunShadow,1.,weatherShadowSoftness(frame.weather,frame.weatherMisc));reflectionGain=mix(.4,2.5,weathered.puddle);
    skyReflection=mix(skyReflection,weatherFogLinear(),max(frame.weather.y,frame.weatherMisc.x));}
#ifdef IKV_RT
// The guided resolve replaces exactly albedo*diffuse: unlike the deterministic enhanced terms, faithful GI has no x1.5.
result=1.5*((roofed?vec3(0):direct*enhancedSunScale()*sunShadow)+skyReflection*f*(1-roughness)*reflectionGain*enhancedAmbientScale())
    +(roofed?1.5*baked:ambient);
#else
result=1.5*((roofed?vec3(0):direct*enhancedSunScale()*sunShadow)+ambient+skyReflection*f*(1-roughness)*reflectionGain*enhancedAmbientScale());
#endif
if(frame.fogColor.w>.5&&draw.extra.z>.5)result=mix(pow(max(frame.fogColor.rgb,vec3(0)),vec3(2.2)),result,clamp(sourceFog,0,1));
}
// NONCANONICAL wave 27: the weather fog (both RT styles and raster); the guided resolve's stochastic terms carry the fog
// weight they are drawn with.
if(weather) {
    float weatherFog=weatherFogTransmit(distance(frame.eye.xyz,worldPosition),frame.weather,frame.weatherMisc);
    result=mix(weatherFogLinear(),result,weatherFog);
#ifdef IKV_RT_GUIDED
    rtDiffuse*=weatherFog;rtReflection*=weatherFog;
#endif
}
}
if(frame.params.x<.5&&frame.fogColor.w>.5&&draw.extra.z>.5)result=mix(frame.fogColor.rgb,result,clamp(sourceFog,0,1));
outputColor=vec4(result,original.a);
#ifdef IKV_RT_GUIDED
// Alpha-tested blended draws (source cutoff: foliage, decals) own the guide: no depth writes and source back-to-front
// order make the last fragment drawn the one the raw colour shows. Their stochastic terms are scaled by coverage, the
// share of the raw colour they produce, so the resolve replaces exactly that share. Untextured translucent draws keep
// their blended raw colour: no decomposition, never history.
float guideDepth=max(dot(worldPosition-frame.eye.xyz,frame.cameraForward.xyz),.0001);
bool translucent=draw.extra.w>.75&&draw.flags.w<=0;
float coverage=draw.extra.w>.75?original.a:1.;
outputGuide=vec4(rtGuideEncodeNormal(guideNormal),translucent?0:rtArtwork,translucent?-guideDepth:guideDepth);
outputDiffuse=vec4(rtDiffuse*coverage,1);
outputReflection=vec4(rtReflection*coverage,fract(draw.extra.w)>.25?1:0);
#endif
}
