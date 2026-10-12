#version 450
// NONCANONICAL wave-16 portal seal: the doorway quad (Draw.model columns = its corners in clip space, already pulled
// towards the eye) writes only depth (compare ALWAYS), so source geometry behind the doorway fails against it.
// Columns are selected, not indexed: a dynamically indexed push-constant matrix column never rasterized on RADV 26.
layout(push_constant) uniform Draw { mat4 model; } draw;
void main(){
    const int corner[6]=int[6](0,1,2,0,2,3);
    int i=corner[gl_VertexIndex];
    gl_Position=i==0?draw.model[0]:i==1?draw.model[1]:i==2?draw.model[2]:draw.model[3];
}
