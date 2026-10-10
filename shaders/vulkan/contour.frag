#version 450
layout(location=0) out vec4 color;
layout(location=5) out float reactive; // FSR 3.1.4 reactive mask (dropped without FSR)
layout(push_constant) uniform Draw { mat4 model; vec4 color; vec4 flags; vec4 extra; vec4 scroll; } draw;
// t_contour.psh outputs v_contour's oD0 = (c8.rgb, .2); no texture, no fog.
void main(){color=vec4(draw.color.rgb,.2);reactive=.2;}
