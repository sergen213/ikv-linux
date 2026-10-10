#version 450
layout(location=0) in vec2 texcoord;
layout(location=1) in float fadeWeight;
layout(set=0,binding=1) uniform sampler2D diffuseMap;
layout(set=0,binding=4) uniform sampler2D fadeMap;
layout(push_constant) uniform Draw {mat4 model;vec4 color;vec4 flags;vec4 extra;vec4 scroll;} draw;
void main(){
    if(draw.extra.w>=2) {
        if(draw.flags.w>0&&1<=draw.flags.w)discard;
        return;
    }
    float alpha=texture(diffuseMap,texcoord).a;
    if(draw.flags.x>.5||draw.flags.y>.5)alpha=1;
    else if(draw.flags.z>.5)alpha=mix(texture(fadeMap,texcoord).a,alpha,fadeWeight);
    if(draw.flags.w>0&&alpha*draw.color.a<=draw.flags.w)discard;
}
