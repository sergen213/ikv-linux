#ifndef IKV_FRAME_GLSL
#define IKV_FRAME_GLSL
layout(set=0,binding=0,std140) uniform Frame {
    mat4 vp;
    mat4 lightVP;
    vec4 eye;
    vec4 sun;
    vec4 params;
    vec4 viewport;
    vec4 ambient;
    vec4 diffuse;
    vec4 fogColor;
    vec4 fog;
    vec4 skyB;
    vec4 skyC;
    mat4 previousVP;
    // xyz: unit camera basis; w: horizontal/vertical tan(half FOV).
    vec4 cameraRight;
    vec4 cameraUp;
    vec4 cameraForward;
    vec4 previousEye;
    vec4 previousForward;
    // Original C38.xy: native low-32 millisecond tick and seconds. z: death-view grayscale; w: fullscreen (gamma ramp).
    vec4 sourceTime;
    // xy: this frame's sub-pixel projection jitter in NDC (vp includes it); zw: the previous frame's (previousVP).
    vec4 jitter;
    // x: NONCANONICAL seamless door transition, scene blended towards black (post.frag); HUD unaffected.
    vec4 fade;
    // NONCANONICAL wave-16 portal view (scene_portal.vert): gl_ClipDistance[i] = dot(world, clip[i]), 4 pyramid sides and
    // the portal plane in this view's (destination) world; toPrimary maps that world into the primary's (guide normals).
    vec4 clip[5];
    mat4 toPrimary;
    // Split-mode source OR plane: gl_ClipDistance[1] = max(dot(world, clip[1]), dot(world, clipOr)); (0,0,0,-1e9) = none.
    vec4 clipOr;
} frame;
// Original fullscreen gamma ramp 0x467030(1,1,1.2) -> SetGammaRamp @0x46711f: ramp[i] = pow(i/255,1/1.2)*1.005, clamped.
// Applied once to display values on every present path (scene in post.frag, HUD in overlay.frag), fullscreen only.
vec3 sourceGammaRamp(vec3 c){return frame.sourceTime.w>.5?min(pow(max(c,vec3(0)),vec3(1./1.2))*1.005,vec3(1)):c;}
// Independently expressed v_ocean.vsh EXPP fraction and authored polynomial.
vec4 sourceOceanPosition(vec4 world) {
    float phase=fract((world.x/32+world.y/13+frame.sourceTime.y)*.15915494-.25);
    vec3 q=vec3(0,.5,1)-phase;
    q*=q;
    vec3 wave=vec3(24.980804,-24.980804,24.980804)*q+
        vec3(-60.145809,60.145809,-60.145809);
    wave=wave*q+vec3(85.453789,-85.453789,85.453789);
    wave=wave*q+vec3(-64.939354,64.939354,-64.939354);
    wave=wave*q+vec3(19.739208,-19.739208,19.739208);
    wave=wave*q+vec3(-1,1,-1);
    float first=phase<.25?1:0,third=phase>=.75?1:0;
    float displacement=-dot(wave,vec3(first,1-first-third,third));
    if(abs(frame.eye.y-world.y)<100)world.y+=displacement*.5;
    return world;
}
#endif
