#version 450
#extension GL_GOOGLE_include_directive : require
layout(location=0) in vec2 texcoord;
layout(location=1) in vec4 tint;
layout(location=0) out vec4 color;
// FSR 3.1.4 reactive mask (MAX blend; dropped without FSR): billboards have no motion vectors.
layout(location=5) out float reactive;
#include "frame.glsl"
layout(set=0,binding=1) uniform sampler2D particleMap;
layout(push_constant) uniform Draw { mat4 model; vec4 color; vec4 flags; vec4 extra; vec4 scroll; } draw;
// flags.y: original ALPHAREF for BlendType "blend" with nonzero mask; D3DCMP_GREATER keeps alpha > ref.
void main(){
    color=texture(particleMap,texcoord)*tint;
    if(draw.flags.y>0&&color.a<=draw.flags.y)discard;
    // Enhanced/physical blend into the linear HDR target: authored gamma-space colour is linearised like albedo
    // (unconverted, additive fire washed out white and subtractive smoke went far negative: post.frag inverted it).
    // NONCANONICAL wave 27 (GL parity): an alpha blend (flags.z) in linear HDR shows far more coverage than OpenGL's blend of
    // the same colours into its display-referred target (rain streaks ~2x as opaque). Its alpha is remapped to the coverage
    // that reproduces the display-space blend: a^2.2 for particles brighter than what they cover (rain, snow, sparks), and
    // 1-(1-a)^2.2 for darker ones (smoke), chosen by the authored luminance.
    if(frame.params.x>.5&&draw.flags.z>.5){
        float a=clamp(color.a,0,1),bright=smoothstep(.25,.6,dot(color.rgb,vec3(.299,.587,.114)));
        color.a=mix(1-pow(1-a,2.2),pow(a,2.2),bright);
    }
    if(frame.params.x>.5)color.rgb=pow(max(color.rgb,vec3(0)),vec3(2.2));
    reactive=min(color.a,.9);
}
