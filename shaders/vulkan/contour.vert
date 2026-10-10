#version 450
#extension GL_GOOGLE_include_directive : require
// Original hover hilite v_contour.vsh (0046ccf0 tail, node draw 0047e1xx): vertex pushed along its normal by c8.w.
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
#include "frame.glsl"
layout(push_constant) uniform Draw { mat4 model; vec4 color; vec4 flags; vec4 extra; vec4 scroll; } draw;
void main(){
    // ponytail: v_contour adds the unrotated object normal in world space; the world normal keeps rotated shells outward.
    vec3 n=mat3(draw.model)*normal;
    n=length(n)>.00001?normalize(n)*draw.color.w:vec3(0);
    gl_Position=frame.vp*(draw.model*vec4(position,1)+vec4(n,0));
}
