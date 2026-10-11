#version 450
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
#include "rt_guide.glsl"
// Guided RT resolve. Artwork is never filtered: the deterministic raw colour (artwork x (Sun + indirect) +
// reflection, plus any additive overlays) is kept, and only its two stochastic terms are replaced:
//   display = raw + albedo * (indirect' - indirect) + (reflection' - reflection)
// Pass 0 (draw.color.x 0): motion-reprojected temporal accumulation with a per-pixel history length; its result is
// the next frame's history. Passes 1-3 (draw.color.x = stride 1/2/4): edge-stopped a-trous B3 iterations over that
// accumulation, every frame, so disoccluded/fresh pixels (no history) take their converged same-surface neighbours.
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;             // display HDR
layout(location=1) out vec4 diffuseHistory;    // rgb indirect gain estimate, a history length (0: none)
layout(location=2) out vec4 reflectionHistory; // rgb reflection estimate
layout(push_constant) uniform Draw { mat4 model; vec4 color; vec4 flags; vec4 extra; vec4 scroll; } draw;
layout(set=0,binding=1) uniform sampler2D rawHDR;
layout(set=0,binding=2) uniform sampler2D currentDiffuse;
layout(set=0,binding=3) uniform sampler2D currentReflection;
layout(set=0,binding=4) uniform sampler2D motion;
// Pass 0: the previous frame's history; a-trous passes: the previous iteration.
layout(set=0,binding=6) uniform sampler2D previousDiffuse;
layout(set=0,binding=8) uniform sampler2D previousReflection;
layout(set=0,binding=10) uniform sampler2D currentGuide;
layout(set=0,binding=11) uniform sampler2D previousGuide;

bool inside(ivec2 p, ivec2 size) { return all(greaterThanEqual(p,ivec2(0))) && all(lessThan(p,size)); }
vec3 viewRay(vec2 at) {
    return frame.cameraForward.xyz + frame.cameraRight.xyz*((at.x*2-1)*frame.cameraRight.w) +
        frame.cameraUp.xyz*((1-at.y*2)*frame.cameraUp.w);
}

void main() {
    ivec2 size=textureSize(rawHDR,0);
    ivec2 p=clamp(ivec2(gl_FragCoord.xy),ivec2(0),size-1);
    vec3 raw=texelFetch(rawHDR,p,0).rgb;
    color=vec4(raw,1);
    diffuseHistory=vec4(0);
    reflectionHistory=vec4(0);
    vec4 guide=texelFetch(currentGuide,p,0);
    // Sky, ocean and untextured translucent surfaces have no decomposition: raw colour, no history. Rejects are written
    // !(x>0) so a non-finite guide is rejected too (NaN fails every comparison and would otherwise spread).
    if(!(guide.w>0) || !(guide.z>0))return;
    vec3 normal=rtGuideDecodeNormal(guide.xy);
    float depth=guide.w;
    vec3 diffuse0=texelFetch(currentDiffuse,p,0).rgb;
    vec4 reflection0=texelFetch(currentReflection,p,0);
    vec2 currentUV=(vec2(p)+.5)/vec2(size);
    vec3 ray=viewRay(currentUV);
    float planeScale=1./(.02+depth*.004);
    float stride=draw.color.x;
    vec3 diffuse,reflection;
    if(stride>0) {
        // A-trous iteration: B3 taps at this stride, plane-distance and vertex-normal edge stopping (never across
        // silhouettes/creases). Short history leans on the (converged) neighbours; long history keeps its own value.
        vec4 center=texelFetch(previousDiffuse,p,0);
        float own=max(center.a,1);
        const float taps[3]=float[3](.375,.25,.0625);
        vec3 sumDiffuse=vec3(0),sumReflection=vec3(0);
        float weight=0;
        for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x) {
            ivec2 q=p+ivec2(x,y)*int(stride);
            if(!inside(q,size))continue;
            vec4 neighbor=texelFetch(currentGuide,q,0);
            if(!(neighbor.w>0) || !(neighbor.z>0))continue;
            vec4 d=texelFetch(previousDiffuse,q,0);
            float w=taps[abs(x)]*taps[abs(y)];
            if(x!=0 || y!=0) {
                float plane=abs(dot(normal,neighbor.w*viewRay((vec2(q)+.5)/vec2(size))-depth*ray))*planeScale/stride;
                w*=exp(-plane)*pow(clamp(dot(normal,rtGuideDecodeNormal(neighbor.xy)),0,1),16.)*
                    clamp(max(d.a,1)/own,.25,4.)*clamp(8./own,.125,1.);
            }
            sumDiffuse+=d.rgb*w;
            sumReflection+=texelFetch(previousReflection,q,0).rgb*w;
            weight+=w;
        }
        diffuse=sumDiffuse/weight;reflection=sumReflection/weight;
        diffuseHistory=vec4(diffuse,center.a);
        reflectionHistory=vec4(reflection,1);
        color=vec4(max(raw+rtGuideAlbedo(guide.z)*(diffuse-diffuse0)+(reflection-reflection0.rgb),vec3(0)),1);
        return;
    }
    bool animated=reflection0.a>.5;

    // Temporal: bilinear footprint of the reprojected previous estimate. Every tap must be the same physical surface
    // (previous-camera depth of this point, normal): disocclusions/foreign surfaces drop. This pixel shows the point at
    // unjittered UV currentUV - jitter.xy/2; the previous frame stored it at its jittered position (+ jitter.zw/2).
    vec4 history=vec4(0);
    vec4 historyReflection=vec4(0); // a: fast (4-frame) indirect luminance history
    vec2 jitterUV=frame.jitter.xy*.5,previousJitterUV=frame.jitter.zw*.5;
    vec3 world=frame.eye.xyz+depth*viewRay(currentUV-jitterUV);
    vec2 previousUV=vec2(-1);
    if(animated) {
        vec2 velocity=texelFetch(motion,p,0).xy; // unjittered previous - current
        if(!any(isnan(velocity)) && !any(isinf(velocity)))previousUV=currentUV-jitterUV+velocity+previousJitterUV;
    } else {
        vec4 clip=frame.previousVP*vec4(world,1); // previousVP includes the previous jitter and Vulkan's Y flip
        if(clip.w>0)previousUV=clip.xy/clip.w*.5+.5;
    }
    if(frame.params.z>.5 && previousUV.x>=0) {
        float expected=dot(world-frame.previousEye.xyz,frame.previousForward.xyz);
        // Moving/skinned surfaces only know their previous screen position, not their previous depth.
        float tolerance=max(.03,expected*(animated?.05:.02));
        vec2 position=previousUV*vec2(size)-.5;
        ivec2 base=ivec2(floor(position));
        vec2 f=position-vec2(base);
        float total=0;
        for(int i=0;i<4;++i) {
            ivec2 o=ivec2(i&1,i>>1);
            ivec2 q=base+o;
            if(!inside(q,size))continue;
            vec4 old=texelFetch(previousGuide,q,0);
            if(!(old.w>0) || !(old.z>0) || !(abs(old.w-expected)<=tolerance) || !(dot(normal,rtGuideDecodeNormal(old.xy))>=.8))continue;
            vec4 sampleDiffuse=texelFetch(previousDiffuse,q,0);
            if(!(sampleDiffuse.a>=1))continue;
            float w=(o.x==1?f.x:1-f.x)*(o.y==1?f.y:1-f.y);
            history+=sampleDiffuse*w;
            historyReflection+=texelFetch(previousReflection,q,0)*w;
            total+=w;
        }
        if(total>.05) { history/=total; historyReflection/=total; }
        else history=vec4(0);
    }

    // Spatial prefilter: 5x5 geometry-weighted kernel over the stochastic terms only. Same-surface statistics over the
    // whole 5x5 bound how far history may differ from the current signal (a moving caster's indirect shadow).
    vec3 sumDiffuse=diffuse0,sumReflection=reflection0.rgb;
    float weight=1;
    vec3 mean=diffuse0,square=diffuse0*diffuse0;
    float count=1;
    for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x) {
        if(x==0 && y==0)continue;
        ivec2 q=p+ivec2(x,y);
        if(!inside(q,size))continue;
        vec4 neighbor=texelFetch(currentGuide,q,0);
        if(!(neighbor.w>0) || !(neighbor.z>0))continue;
        float plane=abs(dot(normal,neighbor.w*viewRay((vec2(q)+.5)/vec2(size))-depth*ray))*planeScale;
        float g=exp(-plane)*pow(clamp(dot(normal,rtGuideDecodeNormal(neighbor.xy)),0,1),16.);
        if(!(g>=1e-3))continue;
        float w=g*exp(-.5*float(x*x+y*y)/1.5);
        vec3 d=texelFetch(currentDiffuse,q,0).rgb,r=texelFetch(currentReflection,q,0).rgb;
        sumDiffuse+=d*w;
        sumReflection+=r*w;
        weight+=w;
        mean+=d*g; square+=d*d*g; count+=g;
    }
    diffuse=sumDiffuse/weight;reflection=sumReflection/weight;

    // Variance clip, then a history-length blend: alpha = 1/n until the cap (static 32, moving surfaces 16), independent
    // of screen speed; the clip and the anti-lag below, not a short cap, remove stale diffuse history. Reflections are
    // not clipped: their samples are heavy-tailed, and a neighbourhood clip pulls them toward the median (it darkened
    // actors by ~5%); they forget after 8 frames instead (view-dependent, reprojected with surface motion).
    mean/=count;
    vec3 sigma=sqrt(max(square/count-mean*mean,vec3(0)));
    float gamma=animated?1.25:1.5;
    vec3 previous=clamp(history.rgb,mean-gamma*sigma,mean+gamma*sigma);
    float samples=min(history.a+1,animated?16.:32.);
    // Anti-lag: a 4-frame history of the prefiltered indirect luminance tracks a moving caster's indirect (contact)
    // shadow; when the long history differs from it by more than its noise (raw 5x5 deviation over ~4*count samples),
    // the history length shrinks so the stale shadow clears in a few frames instead of 32.
    const vec3 luma=vec3(.299,.587,.114);
    float fast=mix(historyReflection.a,dot(diffuse,luma),1./min(samples,4.));
    float lag=abs(dot(previous,luma)-fast),noise=2.*dot(sigma,luma)*inversesqrt(count*min(samples,4.))+.02*fast;
    if(lag>noise)samples=max(1.,samples*noise*noise/(lag*lag));
    diffuse=mix(previous,diffuse,1./samples);
    reflection=mix(historyReflection.rgb,reflection,1./min(samples,8.));
    diffuseHistory=vec4(diffuse,samples);
    reflectionHistory=vec4(reflection,fast);
    color=vec4(max(raw+rtGuideAlbedo(guide.z)*(diffuse-diffuse0)+(reflection-reflection0.rgb),vec3(0)),1);
}
