#version 450
layout(location=0) in vec2 texcoord;
layout(location=1) in vec4 tint;
layout(location=0) out vec4 color;
// FSR 3.1.4 reactive mask (MAX blend; dropped without FSR): billboards have no motion vectors.
layout(location=5) out float reactive;
layout(set=0,binding=1) uniform sampler2D particleMap;
layout(push_constant) uniform Draw { mat4 model; vec4 color; vec4 flags; vec4 extra; vec4 scroll; } draw;
// flags.y: original ALPHAREF for BlendType "blend" with nonzero mask; D3DCMP_GREATER keeps alpha > ref.
void main(){
    color=texture(particleMap,texcoord)*tint;
    if(draw.flags.y>0&&color.a<=draw.flags.y)discard;
    reactive=min(color.a,.9);
}
