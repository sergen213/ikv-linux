// NONCANONICAL enhanced source ocean, shared by Vulkan (lighting.glsl #include) and OpenGL (renderer_gl.cpp embeds this
// text through CMake: keep this file free of preprocessor lines). Port of OpenUnderworld's (the same author's Tomb
// Raider: Underworld port, 0.23.0 42f7ff0) water.frag / waves.glsl / caustics.glsl, translated to IKV: y up, kM units
// per metre, the open-sea path always (the source ocean is a sea), no interactive wave field, no under-water view.
// Its bases: "Realistic Water with Traced and Simple Reflection and Refraction V2" by smallcableboi (godotshaders.com,
// CC0): Schlick Fresnel, two scrolling normal/height layers (procedural value-noise fbm here), traced refraction and
// SSR, edge foam, soft waterline. afl_ext's "Very fast procedural ocean" (https://www.shadertoy.com/view/MdXyzX;
// "afl_ext 2017-2024, MIT License"; xscreensaver prococean: Copyright (C) 2017 by afl_ext. Permission to use, copy,
// modify, distribute, and sell this software and its documentation for any purpose is hereby granted without fee,
// provided that the above copyright notice appear in all copies and that both that copyright notice and this
// permission notice appear in supporting documentation. No representations are made about the suitability of this
// software for any purpose. It is provided "as is" without express or implied warranty.): the sea waves (waterSea).
// Caustics: OpenUnderworld's own (1 / |det J| of the sun refracted through these waves; Evan Wallace's WebGL Water physics).
// The includer defines: vec3 waterSceneColor(vec2 uv) (linear opaque scene copy), float waterSceneDepth(vec2 uv)
// (hardware depth, 1 = empty), float waterNdcZ(float depth), float waterBedSun(vec3 p) (sun visibility of a bed point),
// bool waterRayReflection(vec3 origin, vec3 direction, inout vec3 colour) (true: a traced reflection replaces SSR),
// includes weather.glsl first (NONCANONICAL wave 27 weatherRipple) and defines the NONCANONICAL wave 28 wind sea
// vec3 oceanWindSwell(vec2 xz, vec3 eye, vec4 wind, vec4 misc, int waves) (Vulkan frame.glsl, OpenGL IKV_GL_OCEAN_WIND),
// which also displaces the ocean vertices.
const float kWaterM=4.0;              // IKV units per metre: AGAH's skinned bounds stand ~7.5 units (~1.8 m)
const float kWaterU=kWaterM/290.0;    // IKV units per OpenUnderworld (TRU) unit, for its raw-unit constants
const float kWaterPi=3.14159265;
const float kWaterIor=1.33,kWaterF0=0.02;
const mat2 kWaterRot=mat2(0.8,0.6,-0.6,0.8);
const float kWaterLambdaA=2.0*kWaterM,kWaterLambdaB=1.4*kWaterM; // Godot layers' wavelengths
const float kWaterNormalAmp=0.12*kWaterM;                         // the height their normals come from
const float kWaterSeaUnit=kWaterM;                                // IKV units per afl unit: longest waves ~6 m
mat4 gWaterVP,gWaterInv;
vec3 gWaterEye;
float waterLattice(vec2 c){
    uvec2 q=uvec2(ivec2(c));
    uint n=(q.x*1597334677u)^(q.y*3812015801u);
    n=(n^(n>>15))*2246822519u;
    return float(n>>8)*(1.0/16777216.0);
}
vec3 waterNoiseD(vec2 p){ // value noise in [0, 1] and its gradient
    vec2 i=floor(p),f=fract(p);
    vec2 u=f*f*(3.0-2.0*f),du=6.0*f*(1.0-f);
    float a=waterLattice(i),b=waterLattice(i+vec2(1.0,0.0)),c=waterLattice(i+vec2(0.0,1.0)),d=waterLattice(i+vec2(1.0,1.0));
    float k=a-b-c+d;
    return vec3(a+(b-a)*u.x+(c-a)*u.y+k*u.x*u.y,du*vec2(b-a+k*u.y,c-a+k*u.x));
}
// 4-octave fbm: x height in [0, 1], yz gradient per lattice unit; octaves finer than ~4 pixels (px: lattice units per
// pixel) fade to their mean and are not evaluated.
vec3 waterFbm(vec2 x,float px){
    vec3 acc=vec3(0.0);
    float a=0.5,f=1.0,norm=0.0;
    for(int i=0;i<4;++i){
        float k=a*(1.0-smoothstep(0.25,0.5,px*f));
        vec3 n=k>0.0?waterNoiseD(x*f+vec2(17.3,5.1)*float(i)):vec3(0.0);
        acc+=vec3(k*n.x+(a-k)*0.5,k*f*n.yz);
        norm+=a;
        a*=0.5;f*=2.03;
    }
    return acc/norm;
}
// Godot layers: x height in [0, 1], yz the slope of kWaterNormalAmp x that height in world xz.
vec3 waterLayerA(vec2 p,float t,float footprint){
    vec3 n=waterFbm(p/kWaterLambdaA+t*vec2(0.10,0.02),footprint/kWaterLambdaA);
    return vec3(n.x,n.yz*(kWaterNormalAmp/kWaterLambdaA));
}
vec3 waterLayerB(vec2 p,float t,float footprint){
    vec3 n=waterFbm(kWaterRot*p/kWaterLambdaB+t*vec2(-0.04,0.12),footprint/kWaterLambdaB);
    return vec3(n.x,transpose(kWaterRot)*n.yz*(kWaterNormalAmp/kWaterLambdaB));
}
// afl_ext wavedx / getwaves (MIT, notice above): position in afl units, height in about [0, 1].
vec2 waterAflWavedx(vec2 position,vec2 direction,float frequency,float timeshift){
    float x=dot(direction,position)*frequency+timeshift;
    float wave=exp(sin(x)-1.0);
    return vec2(wave,-wave*cos(x));
}
float waterSea(vec2 p,float t,int iterations){
    vec2 position=p/kWaterSeaUnit;
    float phaseShift=length(position)*0.1;
    float iter=0.0,frequency=1.0,timeMultiplier=2.0,weight=1.0,sumOfValues=0.0,sumOfWeights=0.0;
    for(int i=0;i<iterations;i++){
        vec2 d=vec2(sin(iter),cos(iter));
        vec2 res=waterAflWavedx(position,d,frequency,t*timeMultiplier+phaseShift);
        position+=d*res.y*weight*0.38; // DRAG_MULT
        sumOfValues+=res.x*weight;
        sumOfWeights+=weight;
        weight=mix(weight,0.0,0.2);
        frequency*=1.18;
        timeMultiplier*=1.07;
        iter+=1232.399963;
    }
    return sumOfValues/sumOfWeights;
}
// ---- caustics ----
// Caustic slope: the chop (Godot layers, halved on seas) x3, the swell (afl, seaIters iterations, step >= blur) x0.3.
vec2 waterCausticSlope(vec2 p,float t,float blur,int seaIters){
    vec2 s=(3.0*0.25)*(waterLayerA(p,t,blur).yz+waterLayerB(p,t,blur).yz);
    float e=max(0.01*kWaterSeaUnit,blur),h=waterSea(p,t,seaIters);
    vec2 g=vec2(waterSea(p+vec2(e,0.0),t,seaIters)-h,waterSea(p+vec2(0.0,e),t,seaIters)-h)*(kWaterSeaUnit/e);
    return s+0.3*g;
}
mat2 waterCausticHessian(vec2 p,vec2 g0,float t,float blur,int seaIters){
    vec2 gx=(waterCausticSlope(p+vec2(blur,0.0),t,blur,seaIters)-g0)/blur;
    vec2 gy=(waterCausticSlope(p+vec2(0.0,blur),t,blur,seaIters)-g0)/blur;
    float xy=0.5*(gx.y+gy.x);
    return mat2(gx.x,xy,xy,gy.y);
}
// 1 / |det(I + c H)| softened by e, soft-clamped toward 4.
float waterFocus(mat2 H,float c,float e){
    float det=(1.0+c*H[0][0])*(1.0+c*H[1][1])-c*c*H[0][1]*H[0][1];
    float i=sqrt((1.0+e*e)/(det*det+e*e));
    return i*(4.0/3.0)/(1.0+i/3.0);
}
float waterFocusLength(float h){return 0.25*min(h,4000.0*kWaterU);}
// Caustic light factor of the opaque bed point p, h under the surface; n its (unnormalised) normal; footprint world
// units per pixel there; l toward the sun; sunColor its hue; strength 0 = none. Multiplies the bed colour.
vec3 waterCausticLight(vec3 p,vec3 n,float h,float footprint,float t,vec3 l,vec3 sunColor,float strength){
    if(strength<=0.0||h<=0.0||l.y<0.05||dot(n,n)<1e-12)return vec3(1.0);
    vec3 lw=-refract(-l,vec3(0.0,1.0,0.0),1.0/1.333); // toward the sun, under water
    n=normalize(n);
    if(dot(n,gWaterEye-p)<0.0)n=-n;
    float path=h/lw.y,deep=1.0-exp(-h/(8000.0*kWaterU));
    float amount=strength*mix(1.0,0.6,deep)*smoothstep(0.15,0.7,dot(n,lw))*(1.0-smoothstep(25.0*kWaterU,60.0*kWaterU,footprint));
    if(amount<=0.002)return vec3(1.0);
    amount*=waterBedSun(p);
    float c=waterFocusLength(path),e=0.025+0.5*deep;
    float blur=max(max(0.005*path,1.5*footprint),4.0*kWaterU);
    vec2 entry=p.xz+lw.xz*path; // where the flat-water refracted sun ray through p entered the surface
    vec2 s=entry-c*waterCausticSlope(entry,t,blur,6);
    mat2 H=waterCausticHessian(s,waterCausticSlope(s,t,blur,6),t,blur,6);
    vec3 k=vec3(waterFocus(H,0.975*c,e),waterFocus(H,c,e),waterFocus(H,1.025*c,e));
    k=max(k,0.5+0.5*k);
    vec3 tint=sunColor/max(max(sunColor.r,sunColor.g),max(sunColor.b,1e-3));
    return 1.0+(0.35*amount)*tint*(k-1.0);
}
// ---- screen-space helpers ----
vec3 waterWorld(vec2 uv,float depth){
    vec4 q=gWaterInv*vec4(uv*2.0-1.0,waterNdcZ(depth),1.0);
    return q.xyz/q.w;
}
vec2 waterScreenUv(vec3 q,out bool inside){
    vec4 clip=gWaterVP*vec4(q,1.0);
    vec2 uv=clip.xy/max(clip.w,1e-4)*0.5+0.5;
    inside=clip.w>0.0&&all(greaterThan(uv,vec2(0.0)))&&all(lessThan(uv,vec2(1.0)));
    return uv;
}
float waterEdgeFade(vec2 uv,float size){vec2 e=clamp(min(uv,1.0-uv)/size,0.0,1.0);return e.x*e.y;}
bool waterBehind(vec3 q,out vec2 uv,out bool inside){ // q lies behind the opaque scene at its pixel
    uv=waterScreenUv(q,inside);
    float sd=waterSceneDepth(uv);
    return sd<0.999999&&distance(waterWorld(uv,sd),gWaterEye)<distance(q,gWaterEye);
}
// Quadratic steps out to maxT, then 5 bisections; fails off screen, at maxT, or `thick` behind the surface found.
bool waterTrace(vec3 p,vec3 d,float maxT,float thick,out vec2 hitUv,out float hitT){
    float prev=0.0;
    bool inside;
    vec2 uv;
    hitUv=vec2(0.0);hitT=0.0;
    for(int i=1;i<=14;++i){
        float s=float(i)/14.0,t=s*s*maxT;
        bool behind=waterBehind(p+d*t,uv,inside);
        if(!inside)return false;
        if(behind){
            float lo=prev,hi=t;
            for(int b=0;b<5;++b){
                float mid=0.5*(lo+hi);
                if(waterBehind(p+d*mid,uv,inside))hi=mid;else lo=mid;
            }
            vec3 h=p+d*hi;
            hitUv=waterScreenUv(h,inside);
            if(distance(h,gWaterEye)-distance(waterWorld(hitUv,waterSceneDepth(hitUv)),gWaterEye)>thick+0.05*hi)return false;
            hitT=hi;
            return inside;
        }
        prev=t;
    }
    return false;
}
float waterSchlick(float c){return kWaterF0+(1.0-kWaterF0)*pow(1.0-clamp(c,0.0,1.0),5.0);}
float waterGgxSpec(vec3 n,vec3 v,vec3 l,float a){
    vec3 h=normalize(l+v);
    float nl=max(dot(n,l),0.0),nv=max(dot(n,v),1e-3),nh=max(dot(n,h),0.0),a2=a*a,d=nh*nh*(a2-1.0)+1.0;
    float vis=0.5/max(nl*sqrt(nv*nv*(1.0-a2)+a2)+nv*sqrt(nl*nl*(1.0-a2)+a2),1e-5);
    return a2/(kWaterPi*d*d)*vis*nl*waterSchlick(dot(v,h));
}
// The surface colour (linear, before the scene fog) at p. eye, vp: camera; uv: screen uv of this pixel; t seconds;
// l toward the sun; light: ambient + sun on a level surface (linear); fogGamma: the fog (sky) colour as authored;
// sunColor: sun radiance; sunVisibility at the surface; caustic strength. wind, weather, weatherMisc: the NONCANONICAL
// wave 27 weather inputs (weather.glsl; weatherMisc.w 0 = inactive, the pre-wave water exactly): the waves turn to run
// downwind and steepen with wind and gusts, rain roughens the glint and rings the surface, gales raise whitecaps. Wave 28:
// still air calms the sea to a near-glassy surface (chop x0.2, sea waves x0.25, a smoother glint, a thinner shore lace);
// wind lengthens the waves and stretches them across the wind into crests, the wind sea (oceanWindSwell, all three
// crests; the vertices carry the two longest) tilts the surface, whitecaps spread over its crests, foam streaks run
// downwind and the surf widens.
vec3 waterShade(vec3 p,vec3 eye,mat4 vp,vec2 uv,float t,vec3 l,vec3 light,vec3 fogGamma,vec3 sunColor,float sunVisibility,float caustic,
                vec4 wind,vec4 weather,vec4 weatherMisc){
    gWaterVP=vp;gWaterInv=inverse(vp);gWaterEye=eye;
    float dist=distance(eye,p),footprint=length(fwidth(p.xz));
    bool weatherActive=weatherMisc.w>0.5;
    float ws=weatherActive?clamp(wind.z/25.0,0.0,1.0)*(1.0+0.5*weatherMisc.y):0.0;
    // NONCANONICAL wave 28: breeze 0 in still air .. 1 from ~9 m/s; swell: the wind sea's height and world xz gradient.
    float breeze=weatherActive?smoothstep(0.0,0.35,ws):1.0;
    vec3 swell=weatherActive?oceanWindSwell(p.xz,eye,wind,weatherMisc,3):vec3(0.0);
    // Sea sample scale (wave frame): features grow x1.4 in a gale and stretch x2.2 across the wind (wind runs along z).
    vec2 sk=weatherActive?vec2(1.0/(1.0+1.2*ws),1.0)/(1.0+0.4*ws):vec2(1.0);
    // Wave frame: afl's first (heaviest) wave runs toward -z there, so -z is turned onto the wind; calm wind keeps the old frame.
    float turn=weatherActive?atan(-wind.x,-wind.y)*smoothstep(0.0,2.0,wind.z):0.0;
    mat2 frameR=mat2(cos(turn),sin(turn),-sin(turn),cos(turn));
    vec2 wp=weatherActive?frameR*p.xz:p.xz;
    // OpenUnderworld's default look (Coastal Thailand): surface / deep colour, full fog at 100000 TRU units.
    vec3 deepColor=pow(vec3(0.216,0.439,0.427),vec3(2.2));
    vec3 sigma=(1.0-0.85*pow(vec3(0.447,0.780,0.792),vec3(2.2)))*(30.0/(100000.0*kWaterU));
    // Normals: the Godot layers averaged and halved (a sea), faded with distance; afl's sea, 36/24/12 iterations.
    vec3 la=waterLayerA(wp,t,footprint),lb=waterLayerB(wp,t,footprint);
    float nfade=1.0/(1.0+dist/(60.0*kWaterM));
    vec2 s=0.25*(la.yz+lb.yz)*nfade;
    if(weatherActive)s*=mix(0.2,1.0,breeze)*(1.0+1.5*ws);
    int iters=dist<25.0*kWaterM?36:dist<80.0*kWaterM?24:12;
    float eps=max(0.01*kWaterSeaUnit,footprint);
    float seaH=waterSea(wp*sk,t,iters); // sk is 1 without weather: the pre-wave samples exactly
    vec2 g=vec2(waterSea((wp+vec2(eps,0.0))*sk,t,iters)-seaH,waterSea((wp+vec2(0.0,eps))*sk,t,iters)-seaH)/eps;
    s+=g*kWaterSeaUnit*(1.0-0.8*min(1.0,sqrt(dist/kWaterSeaUnit*0.01)*1.1))*(weatherActive?mix(0.25,1.0,breeze)*(1.0+2.4*ws):1.0);
    if(weatherActive){
        s=transpose(frameR)*s+swell.yz; // back to world xz, the wind sea's slope added
        // Rain-drop rings (weather.glsl weatherRipple), denser and stronger with the rain.
        float rippleGain=weather.x*0.9*(1.0-smoothstep(12.0*kWaterM,40.0*kWaterM,dist));
        if(rippleGain>0.0)s+=rippleGain*weatherRipple(p.xz,t,0.35+0.65*weather.x);
    }
    vec3 n=normalize(vec3(-s.x,1.0,-s.y));
    vec3 v=normalize(eye-p);
    float cosI=max(dot(n,v),1e-3);
    // Contact depth: depth plus a metre where the bed is no shore (a hull or an outline over a deeper bed).
    float sd0=waterSceneDepth(uv);
    bool open=sd0>=0.999999;
    vec3 bed0=waterWorld(uv,sd0);
    float depth=open?1e5:max(p.y-bed0.y,0.0);
    vec3 bx=dFdx(bed0),by=dFdy(bed0),bn=cross(bx,by);
    float run=depth*abs(bn.y)/max(length(bn.xz),1e-3*abs(bn.y)+1e-6);
    bool jump=max(length(bx),length(by))>16.0*footprint;
    float cdepth=open?depth:depth+kWaterM*(jump?smoothstep(0.02*kWaterM,0.1*kWaterM,depth):smoothstep(0.5*kWaterM,1.5*kWaterM,run));
    // Sky for reflection misses: the fog colour brightening toward the horizon, a soft sun lobe.
    vec3 r=reflect(-v,n),rs=normalize(vec3(r.x,abs(r.y),r.z));
    vec3 sunGamma=pow(min(sunColor,vec3(1.0)),vec3(1.0/2.2));
    vec3 skyColor=pow(max(fogGamma*(0.6+0.6*pow(1.0-rs.y,3.0))+sunGamma*(0.12*pow(max(dot(rs,l),0.0),12.0)),vec3(0.0)),vec3(2.2));
    // Refraction: traced, else the depth-scaled normal offset; caustics light the bed, then the water absorbs it.
    vec2 huv;float ht,path;
    bool traced=false;
    vec2 ruv=uv;
    vec3 rd=refract(-v,n,1.0/kWaterIor);
    if(!open&&waterTrace(p,rd,30.0*kWaterM,1.5*kWaterM,huv,ht)&&waterWorld(huv,waterSceneDepth(huv)).y<p.y+2.0*kWaterU){traced=true;ruv=huv;path=ht;}
    if(!traced){
        float ds=open?1e5:distance(bed0,eye);
        ruv=uv+s*(0.6*clamp((ds-dist)/max(ds,kWaterU),0.0,0.25));
        float sd=waterSceneDepth(ruv);
        vec3 bed=waterWorld(ruv,sd);
        if(sd<0.999999&&bed.y>p.y){ruv=uv;sd=sd0;bed=bed0;} // the offset reached the shore
        path=sd>=0.999999?1e5:distance(p,bed);
    }
    float rsd=waterSceneDepth(ruv);
    vec3 rbed=waterWorld(ruv,rsd);
    float down=rsd>=0.999999?1e5:max(p.y-rbed.y,0.0);
    vec3 seen=waterSceneColor(ruv);
    if(rsd<0.999999)seen*=waterCausticLight(rbed,bn,down,length(abs(bx.xz)+abs(by.xz)),t,l,sunColor,caustic);
    vec3 tr=exp(-sigma*min(path,1e5));
    vec3 c=seen*tr*exp(-sigma*min(down,1e5))+deepColor*light*(1.0-tr);
    c+=deepColor*light*(1.6*seaH*(0.4+0.6*pow(1.0-cosI,2.0))); // afl's subsurface glow: crests lit through
    // Reflection: a traced ray (RT physical), else SSR, the sky where it misses.
    float fresnel=waterSchlick(cosI);
    vec3 refl=skyColor;
    vec3 ro=p+vec3(0.0,2.0*kWaterU,0.0);
    if(!waterRayReflection(ro,rs,refl)&&waterTrace(ro,rs,40.0*kWaterM,2.0*kWaterM,huv,ht)){
        float conf=waterEdgeFade(huv,0.08)*(1.0-smoothstep(0.6,1.0,ht/(40.0*kWaterM)));
        refl=mix(skyColor,waterSceneColor(huv),conf);
    }
    float shore=smoothstep(0.0,0.2*kWaterM,cdepth);
    c=mix(c,refl,clamp(fresnel,0.0,1.0)*shore);
    // Sun glint (GGX, rougher far away) and afl's sharp sun disc in the reflection.
    float spec=weatherActive?waterGgxSpec(n,v,l,mix(0.10,0.04,nfade)*(0.6+0.4*breeze)+0.14*weather.x+0.06*ws)+pow(max(dot(rs,l),0.0),720.0)*210.0*fresnel*(1.0-0.8*weather.x):
        waterGgxSpec(n,v,l,mix(0.10,0.04,nfade))+pow(max(dot(rs,l),0.0),720.0)*210.0*fresnel;
    c+=min(spec,64.0)*sunColor*sunVisibility*shore;
    // Edge foam: lace where the squared edge noise exceeds the contact depth / 0.15 m (wave 28: 0.09 m in still air).
    float e=0.5*(waterNoiseD(p.xz/(0.30*kWaterM)+t*vec2(0.30,0.10)).x+waterNoiseD(p.xz/(0.22*kWaterM)-t*vec2(0.10,0.25)).x);
    float foam=smoothstep(-0.05,0.05,e*e-cdepth/(weatherActive?0.15*kWaterM*(0.6+0.4*breeze):0.15*kWaterM));
    // Wave 28 surf: in the wind the lace spreads over the shallows, ~0.5 m deep in a gale. It follows the plain water
    // depth (continuous; the contact depth's shore run steps at the bed's triangle edges), the contact depth at outlines,
    // and fades over most of its depth: a sharp depth threshold traced the bed triangles' straight contour lines.
    if(weatherActive){
        float surfDepth=0.15*kWaterM*(0.1+2.5*ws);
        foam=max(foam,smoothstep(0.15,0.6,ws)*(1.0-smoothstep(0.2*surfDepth,surfDepth,jump?cdepth:depth))*smoothstep(0.3,0.7,e));
    }
    c=mix(c,light*0.9,foam*0.65);
    if(weatherActive){
        // Whitecaps (wave 27/28): crests, the wind sea's highest, break into foam from ~6 m/s, spreading as the wind rises;
        // foam streaks lie along the wind (the wave frame's z) from ~12 m/s.
        float crest=seaH+0.12*swell.x,wsc=min(ws,1.0);
        c=mix(c,light*0.95,0.75*smoothstep(0.62-0.22*wsc,0.88-0.18*wsc,crest)*smoothstep(0.22,0.7,ws)*smoothstep(0.3,0.65,e));
        float streak=waterNoiseD(vec2(wp.x/(0.7*kWaterM),(wp.y+t*1.5*kWaterM)/(7.0*kWaterM))).x;
        c=mix(c,light*0.9,0.35*smoothstep(0.62,0.85,streak)*smoothstep(0.45,0.95,ws)*(1.0-smoothstep(40.0*kWaterM,120.0*kWaterM,dist)));
    }
    // Soft waterline: the bed shows through the last few units.
    c=mix(waterSceneColor(uv),c,smoothstep(0.0,6.0*kWaterU,cdepth));
    return max(c,vec3(0.0));
}
