#version 450
#extension GL_GOOGLE_include_directive : require
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
#include "frame.glsl"
layout(set=0,binding=8) uniform sampler2D overlay;
// ponytail: the ramp is applied to the HUD texel before straight-alpha blending, exact for opaque/clear texels; a
// final display pass after the HUD would be exact for translucent edges too.
void main(){color=texture(overlay,uv);color.rgb=sourceGammaRamp(color.rgb);}
