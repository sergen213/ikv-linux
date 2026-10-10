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
layout(set=0,binding=5) uniform sampler2D shadowMap;
layout(set=0,binding=9) uniform sampler2D originalLightmap;
const float PI=3.14159265359;
vec3 sky(vec3 direction) { return mix(vec3(.055,.07,.09),vec3(.32,.44,.62),clamp(direction.y*.5+.5,0,1)); }
vec3 environment(vec3 direction) { if(draw.flags.y<.5)return sky(direction);return textureLod(environmentMap,vec2(atan(direction.z,direction.x)/(2*PI)+.5,acos(clamp(direction.y,-1,1))/PI),0).rgb; }
float shadowVisibility(vec3 n){vec3 p=shadowPosition.xyz/shadowPosition.w;vec2 uv=p.xy*.5+.5;if(any(lessThan(uv,vec2(0)))||any(greaterThan(uv,vec2(1)))||p.z<0||p.z>1)return 1;float bias=max(.0003,.002*(1-dot(n,normalize(frame.sun.xyz))));vec2 t=1./vec2(textureSize(shadowMap,0));float visible=0;for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++)visible+=p.z-bias<=texture(shadowMap,uv+vec2(x,y)*t).r?1:0;return visible/9;}
vec3 mappedNormal(){
    vec3 n=normalize(worldNormal);
    if(draw.flags.x<.5)return n;
    // Zero/degenerate-tangent source meshes (e.g. NPC staff tips) skip normal mapping, as GL does: a screen-derivative
    // tangent is per-triangle, so it rotated the map differently on every facet.
    vec3 t=worldTangent-n*dot(n,worldTangent);
    if(dot(t,t)<=1e-12)return n;
    t=normalize(t);
    return normalize(mat3(t,cross(n,t),n)*(texture(normalMap,texcoord).xyz*2-1));
}
// Independently expressed recovered original shader equations, not copied proprietary shader source.
vec4 originalSurface(vec4 diffuse) {
    bool hasNormal=draw.flags.x>.5,hasEnvironment=draw.flags.y>.5,hasLightmap=draw.extra.x>.5;
    vec3 eye=normalize(frame.eye.xyz-worldPosition);
    vec3 localN=hasNormal?normalize(localNormal):localNormal;
    vec2 environmentUV=(eye-2*localN*dot(localN,eye)).xy;
    vec3 environmentColor=hasEnvironment?texture(environmentMap,environmentUV).rgb:vec3(0);
    // Source meshes carry zero tangents (e.g. NPC staff tips): like GL, those fragments take the unmapped path,
    // since normalize(0) is NaN and the 9x9 glow taps would spread it as a dot grid.
    vec3 n=worldNormal,t=cross(cross(n,worldTangent),n); // tangent's component orthogonal to n, scaled by |n|^2
    if(hasNormal&&frame.sun.w>.5&&dot(t,t)>1e-12) {
        vec4 normalTexel=texture(normalMap,texcoord);
        vec2 xy=normalTexel.xy*2-1;
        vec3 mapped=normalize(vec3(xy,1-dot(xy,xy)));
        n=normalize(n);t=normalize(t);
        vec3 b=cross(n,t);
        vec3 tangentSun=vec3(dot(-frame.sun.xyz,t),dot(-frame.sun.xyz,b),dot(-frame.sun.xyz,n));
        vec3 tangentEye=normalize(vec3(dot(eye,t),dot(eye,b),dot(eye,n)));
        float factor=.5*dot(mapped,tangentSun)+.5;
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
    if(hasEnvironment) {
        vec3 result=hasLightmap?
            mix(diffuse.rgb,environmentColor,diffuse.a)-lightmap*(1-clamp(diffuse.a,0,1))*frame.fog.z:
            diffuse.rgb+diffuse.a*environmentColor;
        return vec4(result*vertexAmbient.rgb,1);
    }
    return vec4((diffuse.rgb-lightmap*frame.fog.z)*vertexAmbient.rgb,diffuse.a);
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
void main(){
outputMotion=(motionPrevious.xy/motionPrevious.w-motionCurrent.xy/motionCurrent.w)*.5;
// Only translucent pipelines write reactive (MAX blend); their texel alpha is how much the surface replaces history.
outputReactive=min(texture(diffuseMap,texcoord).a*draw.color.a,.9);
#ifdef IKV_RT
// Physical facet normal from screen derivatives, before any discard leaves helper lanes undefined.
vec3 rtFacet=cross(dFdx(worldPosition),dFdy(worldPosition));
#endif
#ifdef IKV_RT_GUIDED
vec3 guideNormal=normalize(worldNormal),rtDiffuse=vec3(0),rtReflection=vec3(0);
float rtArtwork=0;
#endif
if(draw.extra.w>=2) {
    outputColor=sourceOceanSurface();
    if(draw.flags.w>0&&outputColor.a<=draw.flags.w)discard;
#ifdef IKV_RT_GUIDED
    // Moving water has no displacement motion vectors: spatial guide only, never history or decomposition.
    float depth=max(dot(worldPosition-frame.eye.xyz,frame.cameraForward.xyz),.0001);
    outputGuide=vec4(rtGuideEncodeNormal(guideNormal),0,-depth);
    outputDiffuse=vec4(0);outputReflection=vec4(0);
#endif
    return;
}
vec4 texel=texture(diffuseMap,texcoord)*draw.color;
if(draw.extra.y>.5) {
    vec3 secondary=draw.flags.y>.5?texture(environmentMap,texcoord).rgb:vec3(0);
    vec3 clouds=mix(texel.rgb,secondary,clamp(frame.fog.w,0,1));
    float mu=dot(normalize(frame.eye.xyz-worldPosition),-normalize(frame.sun.xyz));
    float scatter=(.001*(1+mu*mu)+(.039999995*.995)/(1.64-1.6*mu))*
        (1-pow(2.7182817,-.001)*pow(2.7182817,-.995))/.99599999;
    vec3 skyColor=mix(frame.fogColor.rgb,clouds*vertexAmbient.rgb+vec3(scatter),texel.a);
    outputColor=vec4(frame.params.x>.5?pow(max(skyColor,vec3(0)),vec3(2.2)):skyColor,1);
#ifdef IKV_RT_GUIDED
    outputGuide=vec4(0);outputDiffuse=vec4(0);outputReflection=vec4(0);
#endif
    return;
}
// Source MAT+14 secondary fade is selected only without higher-priority normal/environment maps.
if(draw.flags.z>.5&&draw.flags.x<.5&&draw.flags.y<.5)
    texel=mix(texture(fadeMap,texcoord)*draw.color,texel,fadeWeight);
float sourceAlpha=draw.flags.x>.5||draw.flags.y>.5?1:texel.a;
vec4 original=frame.params.x<.5?originalSurface(texel):vec4(texel.rgb,sourceAlpha);
if(draw.flags.w>0&&original.a<=draw.flags.w)discard;
vec3 result=frame.params.x<.5?original.rgb:texel.rgb;
if(frame.params.x>.5){vec3 n=mappedNormal(),v=normalize(frame.eye.xyz-worldPosition),l=normalize(frame.sun.xyz),h=normalize(v+l);float roughness=.65,metallic=0.;vec3 albedo=pow(max(texel.rgb,vec3(0)),vec3(2.2));float ndl=max(dot(n,l),0),ndv=max(dot(n,v),.001),ndh=max(dot(n,h),0),vdh=max(dot(v,h),0);float a=roughness*roughness,a2=a*a;float d=a2/(PI*pow(ndh*ndh*(a2-1)+1,2));float k=pow(roughness+1,2)/8;float g=(ndv/(ndv*(1-k)+k))*(ndl/(ndl*(1-k)+k));vec3 f0=mix(vec3(.04),albedo,metallic);vec3 f=f0+(1-f0)*pow(1-vdh,5);vec3 specular=d*g*f/max(4*ndv*ndl,.001);vec3 direct=((1-f)*(1-metallic)*albedo/PI+specular)*ndl*3.2;
#ifdef IKV_RT
RtLight light=rtLighting(worldPosition,dot(rtFacet,v)<0?-rtFacet:rtFacet,worldNormal,n,v,roughness,l,uvec2(gl_FragCoord.xy),uint(frame.viewport.z),1.);
result=albedo*(light.sun+light.diffuse)+light.reflection;
#ifdef IKV_RT_GUIDED
rtDiffuse=light.diffuse;rtReflection=light.reflection;rtArtwork=rtGuidePackArtwork(texel.rgb);
#endif
#else
result=direct*shadowVisibility(n)+albedo*environment(n)*.35+environment(reflect(-v,n))*f*(1-roughness)*.4;
#endif
}
if(frame.params.x<.5&&frame.fogColor.w>.5&&draw.extra.z>.5)result=mix(frame.fogColor.rgb,result,clamp(sourceFog,0,1));
outputColor=vec4(result,original.a);
#ifdef IKV_RT_GUIDED
// Alpha-blended draws (extra.w 1) keep their blended raw colour: no decomposition, never history.
float guideDepth=max(dot(worldPosition-frame.eye.xyz,frame.cameraForward.xyz),.0001);
bool blended=draw.extra.w>.75;
outputGuide=vec4(rtGuideEncodeNormal(guideNormal),blended?0:rtArtwork,blended?-guideDepth:guideDepth);
outputDiffuse=vec4(rtDiffuse,1);
outputReflection=vec4(rtReflection,fract(draw.extra.w)>.25?1:0);
#endif
}
