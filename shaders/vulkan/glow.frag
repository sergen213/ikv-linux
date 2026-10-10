#version 450
// Original fake HDR pass 0046fdb0 (GL renderer glow_fragment): draw.color = (step u, step v, record glow*.02, mode).
// mode 0 t_half_intensity: .45*first + .55*second (bb38, last frame); mode 1 t_pseudo_HDR_blur: taps one 128-texel
// apart weighted (.25+gain, .5, .25+gain); mode 2 composite quad diffuse 0xffcccccc (.8), blended SRCCOLOR/ONE.
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(push_constant) uniform Draw { mat4 model; vec4 color; vec4 flags; vec4 extra; vec4 scroll; } draw;
layout(set=0,binding=1) uniform sampler2D first;
layout(set=0,binding=2) uniform sampler2D second;
void main(){
    vec3 c=texture(first,uv).rgb;
    if(draw.color.w<.5)c=c*.45+texture(second,uv).rgb*.55;
    else if(draw.color.w<1.5)c=(.25+draw.color.z)*(texture(first,uv-draw.color.xy).rgb+texture(first,uv+draw.color.xy).rgb)+.5*c;
    else c*=.8;
    color=vec4(clamp(c,0.,1.),1.);
}
