#version 450
#extension GL_GOOGLE_include_directive : require
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
#include "frame.glsl"
layout(set=0,binding=1) uniform sampler2D hdr;
float luminance(vec3 c){return dot(c,vec3(.299,.587,.114));}
vec3 sampleColor(vec2 p){return texture(hdr,p).rgb;}
void main(){vec2 pixel=1./vec2(textureSize(hdr,0));vec3 c=sampleColor(uv);
// FXAA directional filter at the source extent, bilinear upscale at output extent. FXAA 3.11 local-contrast early exit
// (edgeThresholdMin .0312, edgeThreshold .125): flat texture interiors stay unfiltered.
if(frame.params.w>.5){vec3 nw=sampleColor(uv+vec2(-1,-1)*pixel),ne=sampleColor(uv+vec2(1,-1)*pixel),sw=sampleColor(uv+vec2(-1,1)*pixel),se=sampleColor(uv+vec2(1,1)*pixel);float lnw=luminance(nw),lne=luminance(ne),lsw=luminance(sw),lse=luminance(se),lc=luminance(c);float lo=min(lc,min(min(lnw,lne),min(lsw,lse))),hi=max(lc,max(max(lnw,lne),max(lsw,lse)));
if(hi-lo>=max(.0312,hi*.125)){vec2 direction=vec2(-((lnw+lne)-(lsw+lse)),((lnw+lsw)-(lne+lse)));float reduce=max((lnw+lne+lsw+lse)*(.25*.125),.0078125);float inverse=1./(min(abs(direction.x),abs(direction.y))+reduce);direction=clamp(direction*inverse,vec2(-8),vec2(8))*pixel;vec3 a=.5*(sampleColor(uv+direction*(1./3.-.5))+sampleColor(uv+direction*(2./3.-.5)));vec3 b=a*.5+.25*(sampleColor(uv+direction*-.5)+sampleColor(uv+direction*.5));float lb=luminance(b);c=(lb<lo||lb>hi)?a:b;}}
if(frame.params.x>.5){c*=frame.params.y;c=clamp((c*(2.51*c+.03))/(c*(2.43*c+.59)+.14),0,1);c=pow(c,vec3(1./2.2));}
// sourceTime.z: original death view grayscale (HUD overlay is drawn afterwards in colour).
if(frame.sourceTime.z>.5)c=vec3(luminance(c));
color=vec4(sourceGammaRamp(c),1);}
