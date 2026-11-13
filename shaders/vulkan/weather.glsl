// NONCANONICAL wave 27: server-authoritative season, weather and wind (SceneLighting::weather), enhanced appearance only;
// the original game had none of this. Shared by Vulkan (#include after frame.glsl) and OpenGL (renderer_gl.cpp embeds this
// text through CMake as IKV_GL_WEATHER: keep this file free of preprocessor lines and of derivatives, it also feeds vertex
// shaders). Inputs (renderer.hpp weather_uniforms): wind xy unit direction (world X/Z the wind blows toward), z speed m/s,
// w accumulated wind phase in metres (wrapped at 1024 m: every use below is periodic in 1024 m); state x precipitation,
// y cloud cover, z wetness, w snow cover; misc x fog density, y gust, z season 0..3 (spring, summer, autumn, winter),
// w kind+1 (clear, cloudy, rain, storm, snow, fog; 0 = inactive). Callers branch on weatherOn(): inactive, original
// appearance and roofed zones take the exact pre-wave paths. 4 IKV units = 1 m (water.glsl kWaterM).
bool weatherOn(vec4 misc){return misc.w>0.5;}
float weatherHash(vec2 c){
    uvec2 q=uvec2(ivec2(floor(c)));
    uint n=(q.x*1597334677u)^(q.y*3812015801u);
    n=(n^(n>>15))*2246822519u;
    return float(n>>8)*(1.0/16777216.0);
}
float weatherNoise(vec2 p){
    vec2 i=floor(p),f=fract(p),u=f*f*(3.0-2.0*f);
    return mix(mix(weatherHash(i),weatherHash(i+vec2(1.0,0.0)),u.x),mix(weatherHash(i+vec2(0.0,1.0)),weatherHash(i+vec2(1.0,1.0)),u.x),u.y);
}
// Wind strength 0..~1.6: speed over a 25 m/s gale, raised by gusts.
float weatherWindStrength(vec4 wind,vec4 misc){return clamp(wind.z/25.0,0.0,1.0)*(1.0+0.6*misc.y);}
// World-space bend of a swaying vertex. localY: mesh-space height; world: the rigid world position; sway (per draw,
// renderer.hpp weather_sway): x mesh-space base, y 1/mesh height, z bend amplitude (units), w placement seed; t seconds.
// A steady downwind lean, a gust front travelling downwind (16 m wavelength: 64 per phase wrap) and a crosswind flutter
// (seconds-based, so no phase jump when the speed changes); the vertex dips as it leans.
vec3 weatherSway(float localY,vec3 world,vec4 sway,vec4 wind,vec4 misc,float t){
    if(!weatherOn(misc)||sway.z<=0.0||wind.z<=0.0)return vec3(0.0);
    float h=clamp((localY-sway.x)*sway.y,0.0,1.0);
    h*=h;
    float s=weatherWindStrength(wind,misc);
    float front=sin(6.28318530718*((wind.w-dot(world.xz,wind.xy)*0.25)/16.0+sway.w));
    float flutter=sin(t*(1.7+1.1*sway.w)+dot(world,vec3(0.21,0.13,0.17)))*(0.25+0.35*s);
    float bend=sway.z*s*h*(0.6+0.3*front);
    vec2 lateral=vec2(-wind.y,wind.x)*(sway.z*h*min(s,1.0)*0.35*flutter);
    return vec3(wind.x*bend+lateral.x,-0.3*abs(bend)*h,wind.y*bend+lateral.y);
}
// Season tint of linear albedo, subtle and strongest on green (foliage/grass) texels: spring fresher, summer as authored,
// autumn foliage turns ochre, winter desaturates.
vec3 weatherSeason(vec3 c,vec4 misc){
    float g=smoothstep(0.08,0.35,(c.g-max(c.r,c.b))/max(c.g,1e-3));
    float luma=dot(c,vec3(0.2126,0.7152,0.0722));
    if(misc.z<0.5)return c*mix(vec3(1.0),vec3(0.96,1.06,0.92),g);
    if(misc.z<1.5)return c;
    if(misc.z<2.5)return mix(c,luma*vec3(1.75,1.0,0.32),0.6*g);
    return mix(c,vec3(luma),0.15+0.35*g);
}
// Snow on an outdoor surface: upward faces (n world shading normal), broken up by 0.5 m noise. p world position.
float weatherSnow(vec3 p,vec3 n,vec4 state){
    if(state.w<=0.0)return 0.0;
    float breakup=weatherNoise(p.xz*0.5)*0.6+weatherNoise(p.xz*2.1)*0.4-0.5;
    return state.w*smoothstep(0.45,0.85,n.y+0.35*breakup+0.25*(state.w-0.5));
}
// Wetness: rain darkens every outdoor surface, upward faces most; snow covers it.
float weatherWet(vec3 n,float snow,vec4 state){return state.z*(0.35+0.65*smoothstep(0.2,0.8,n.y))*(1.0-snow);}
// Rain-drop rings: the slope (per cell) of expanding rings in 0.5 m (2 unit) cells, three staggered layers; each cell
// drops anew every ~1 s, a drop landing with probability `density`. p world xz (units), t seconds.
vec2 weatherRipple(vec2 p,float t,float density){
    vec2 slope=vec2(0.0);
    for(int layer=0;layer<3;++layer){
        vec2 q=p*0.5+vec2(0.37,0.71)*float(layer)*1.7,cell=floor(q),f=fract(q);
        float h=weatherHash(cell+vec2(31.0+53.0*float(layer),17.0));
        float tt=t*(0.8+0.5*h)+h*7.0,life=fract(tt),drop=floor(tt);
        if(weatherHash(cell+vec2(drop,11.0*float(layer)+3.0))>density)continue;
        vec2 d=f-0.5-0.6*(vec2(weatherHash(cell+vec2(5.0,drop)),weatherHash(cell+vec2(drop,9.0)))-0.5);
        float r=length(d),ring=r-life*0.45;
        float wave=sin(ring*50.0)*exp(-ring*ring*300.0)*(1.0-life)*(1.0-life);
        slope+=r>1e-4?d/r*wave:vec2(0.0);
    }
    return slope;
}
// Puddles: flat upward ground collects water in occasional ~4 m noise hollows, spreading with wetness.
// NONCANONICAL wave 28: thresholds raised so full wetness floods ~12% of flat ground (was ~60%), ~2% at a quarter.
// CPU mirror renderer.hpp weather_puddle (up.y = 1): keep both identical.
float weatherPuddle(vec3 p,vec3 up,vec4 state){
    if(state.z<=0.0)return 0.0;
    float m=weatherNoise(p.xz*0.06)*0.65+weatherNoise(p.xz*0.23)*0.35;
    return state.z*smoothstep(0.88,0.97,up.y)*smoothstep(0.84-0.16*state.z,0.88-0.16*state.z,m);
}
// Rain running down vertical faces (walls, trunks, characters): streaks sliding down at ~1 m/s while it rains.
float weatherStreak(vec3 p,vec3 up,float t,vec4 state){
    float vertical=1.0-smoothstep(0.3,0.6,abs(up.y));
    if(vertical<=0.0||state.z<=0.0||state.x<=0.0)return 0.0;
    float u=dot(p.xz,vec2(-up.z,up.x)/max(length(up.xz),1e-4));
    float streak=smoothstep(0.55,0.85,weatherNoise(vec2(u*3.0,p.y*0.3+t*4.0)))*smoothstep(0.3,0.6,weatherNoise(vec2(u*0.8,7.3)));
    return vertical*streak*min(state.x*1.5,1.0)*state.z;
}
// NONCANONICAL wave 28: recent character footsteps (renderer.hpp ground_mark_uniforms; info.x mark count, 0 = none).
// Callers loop over their own mark array (Vulkan frame.groundMarks, GL weather_marks: never passed as an array, a by-value
// copy spilled 1 KB per fragment) only when weatherStepsHere(), adding each mark with weatherStep(). A mark: x,z world
// (units, the foot itself), y ground height, w packed: sign the foot (+1 right, -1 left), |w|-1 = age (1/64 s steps)*64 +
// heading (64 steps of a turn, travel direction (sin h, cos h)). bounds: xz min and max of every mark's reach. While
// wet (state.z) a step < 1.5 s old sends two expanding rings (ripple xy: their xz slope, z: agitation; weatherSurface
// keeps them to puddles); under snow cover (state.w) it leaves a ~0.28 m foot print fading out over 30..85 s (press:
// depth 0..1, pressSlope its world xz gradient; weatherSurface keeps it to snow-covered texels). Marks more than 2 units
// (0.5 m) off the ground height are ignored (roofs, shoulders above the walker).
struct WeatherSteps{vec3 ripple;vec2 pressSlope;float press;};
WeatherSteps weatherStepsNone(){return WeatherSteps(vec3(0.0),vec2(0.0),0.0);}
bool weatherStepsHere(vec3 p,vec3 up,vec4 state,vec4 info,vec4 bounds){
    return info.x>0.5&&up.y>0.6&&(state.z>0.0||state.w>0.0)&&all(greaterThanEqual(p.xz,bounds.xy))&&all(lessThanEqual(p.xz,bounds.zw));
}
void weatherStep(inout WeatherSteps s,vec4 m,vec3 p,vec4 state){
    vec2 d=p.xz-m.xy;
    if(abs(p.y-m.z)>2.0||max(abs(d.x),abs(d.y))>4.0)return;
    float v=abs(m.w)-1.0,age=floor(v/64.0)/64.0,heading=(v-64.0*floor(v/64.0))*(6.28318530718/64.0),side=m.w<0.0?-1.0:1.0;
    if(state.z>0.0&&age<1.5){
        float r=length(d),fade=(1.0-age/1.5)*(1.0-age/1.5),a=r-age*2.2,b=r-age*1.4;
        float wave=sin(a*10.5)*exp(-a*a*6.0)+0.6*sin(b*10.5)*exp(-b*b*8.0);
        if(r>1e-3)s.ripple+=vec3(d/r*wave,abs(wave))*fade;
    }
    if(state.w>0.0&&dot(d,d)<0.36){
        vec2 f=vec2(sin(heading),cos(heading)),o=vec2(f.y,-f.x)*side; // o: away from the walk line
        float u=dot(d,o),fv=dot(d,f); // u < 0: the instep side, narrowed at the arch
        float hw=mix(0.15,0.21,smoothstep(-0.4,0.3,fv))*(u<0.0?1.0-0.35*exp(-(fv+0.05)*(fv+0.05)*30.0):1.0);
        float e=length(vec2(u/hw,fv/0.56));
        if(e>=1.0)return;
        float fade=1.0-smoothstep(30.0,85.0,age),depth=(1.0-smoothstep(0.7,1.0,e))*fade;
        if(depth>s.press){
            float t=clamp((e-0.7)/0.3,0.0,1.0);
            vec2 g=(-6.0*t*(1.0-t)/0.3)*fade*vec2(u/(hw*hw),fv/0.3136)/max(e,1e-3);
            s.press=depth;s.pressSlope=o*g.x+f*g.y;
        }
    }
}
// Snow colour, pressed (footprint) snow compacted and shadowed bluish grey.
vec3 weatherSnowColor(float press){return mix(vec3(0.78,0.80,0.84),vec3(0.50,0.54,0.61),press);}
// A weathered outdoor surface (enhanced lighting): albedo linear; n shading normal; up interpolated geometric normal;
// p world position; roughness the dry value. Wet/streaked: darker and glossier; puddles: darker still, flat, mirror-like
// and ringed by drops (as are other upward wet faces, faintly: splash flecks); snow: white and rough. directRoughness
// floors the sun glint (glossy wet normal-mapped cobbles otherwise sparkle in single texels), except in puddles.
// steps: footsteps (weatherStep; NONCANONICAL wave 28): rings in puddles (stir), prints in snow (press).
struct WeatherSurface{vec3 albedo;vec3 normal;float roughness;float directRoughness;float snow;float wet;float puddle;float press;float stir;};
WeatherSurface weatherSurface(vec3 albedo,vec3 n,vec3 up,vec3 p,float roughness,float t,vec4 state,vec4 misc,WeatherSteps steps){
    WeatherSurface w;
    w.snow=weatherSnow(p,up,state);
    w.puddle=weatherPuddle(p,up,state)*(1.0-w.snow);
    float cover=smoothstep(0.0,0.2,w.snow); // no print without snow cover
    w.press=steps.press*cover;
    vec3 stepRipple=steps.ripple;vec2 pressSlope=steps.pressSlope*cover;
    w.snow*=1.0-0.4*w.press; // compacted: the ground shows through a little
    w.wet=max(weatherWet(up,w.snow,state),weatherStreak(p,up,t,state)*(1.0-w.snow));
    w.albedo=mix(weatherSeason(albedo,misc)*(1.0-0.45*w.wet)*(1.0-0.35*w.puddle),weatherSnowColor(w.press),w.snow);
    w.roughness=mix(mix(mix(roughness,mix(0.45,0.15,smoothstep(0.5,0.9,up.y)),w.wet),0.04,w.puddle),mix(0.85,0.55,w.press),w.snow);
    w.stir=min(stepRipple.z,1.0)*w.puddle; // footstep rings: agitated, rougher water with lighter crests
    w.roughness=mix(w.roughness,0.6,w.stir);w.albedo*=1.0+1.5*w.stir;
    w.directRoughness=max(w.roughness,mix(0.45,0.08,w.puddle));
    float rippleGain=state.x*smoothstep(0.6,0.9,up.y)*(0.2+0.8*w.puddle)*(1.0-w.snow);
    vec2 ripple=rippleGain>0.0?rippleGain*weatherRipple(p.xz,t,0.3+0.7*state.x):vec2(0.0); // dry: no ring evaluation
    ripple+=2.0*w.puddle*stepRipple.xy;
    w.normal=normalize(mix(n,up,w.puddle)+0.5*vec3(ripple.x,0.0,ripple.y)+0.1*vec3(pressSlope.x,0.0,pressSlope.y));
    w.puddle*=1.0-0.6*w.stir; // ring crests break the puddle's mirror (callers' reflection gain)
    return w;
}
// The baked (original) light of the same surface under weather: tint, wetness and puddles; snow takes its light level
// (footprints: compacted snow colour).
vec3 weatherBaked(vec3 baked,vec3 sourceAlbedo,WeatherSurface w,vec4 misc){
    vec3 luma=vec3(0.2126,0.7152,0.0722);
    float level=clamp(dot(baked,luma)/max(dot(sourceAlbedo,luma),0.02),0.0,4.0);
    return mix(weatherSeason(baked,misc)*(1.0-0.45*w.wet)*(1.0-0.35*w.puddle)*(1.0+1.5*w.stir),weatherSnowColor(w.press)*level,w.snow);
}
// Storm lightning: a flickering flash about every 7 s, deterministic in t (seconds), shared by every backend.
float weatherLightning(vec4 misc,float t){
    if(misc.w<3.5||misc.w>4.5)return 0.0;
    float cell=floor(t*2.0),f=fract(t*2.0);
    return step(0.93,weatherHash(vec2(cell,17.0)))*exp(-f*7.0)*(0.65+0.35*sin(f*70.0));
}
// Weather light, all from the eased continuous values (season index aside), so kinds blend without pops.
// Sun colour x intensity: the clear-sky season (summer warm and strong, autumn golden, spring fresh, winter low and cool),
// turning a cool grey as clouds gather; cloud (to 15%), fog and rain dim it: a storm's sun is nearly gone.
vec3 weatherSunTint(vec4 state,vec4 misc){
    if(!weatherOn(misc))return vec3(1.0);
    vec3 season=misc.z<0.5?vec3(1.0,1.03,0.97):misc.z<1.5?vec3(1.06,1.0,0.9):misc.z<2.5?vec3(1.1,0.96,0.78):vec3(0.75,0.8,0.92);
    return mix(vec3(0.9,0.95,1.05),season,1.0-state.y)*(1.0-0.85*state.y)*(1.0-0.75*misc.x)*(1.0-0.3*state.x);
}
// Sky/ambient light colour x intensity: overcast flattens it cool and desaturated (a bright diffuse dome while dry, dark
// under rain and storms), snow cover bounces bright cool light, the clear season tints it; lightning (t seconds) flashes it.
vec3 weatherAmbientTint(vec4 state,vec4 misc,float t){
    if(!weatherOn(misc))return vec3(1.0);
    vec3 season=misc.z<0.5?vec3(0.98,1.02,0.98):misc.z<1.5?vec3(1.0):misc.z<2.5?vec3(1.04,1.0,0.92):vec3(0.94,0.98,1.06);
    vec3 c=mix(season,vec3(0.9,0.96,1.06),state.y)*(1.0+0.2*state.y*(1.0-state.x)-0.45*state.y*state.x);
    c*=mix(vec3(1.0),vec3(1.25,1.28,1.35),state.w);
    return c*(1.0+4.0*weatherLightning(misc,t));
}
// Share of sun shadow washed out by diffuse cloud/fog light (soft, then near-absent shadows).
float weatherShadowSoftness(vec4 state,vec4 misc){return weatherOn(misc)?clamp(0.75*state.y+0.6*misc.x,0.0,0.95):0.0;}
// Weather fog colour (as authored, gamma): the record fog greyed and darkened by cloud; without record fog a hazy grey.
vec3 weatherFogColor(vec3 recordFog,bool recordFogOn,vec4 state,vec4 misc){
    vec3 base=recordFogOn?recordFog:vec3(0.62,0.66,0.70);
    vec3 grey=vec3(dot(base,vec3(0.299,0.587,0.114)))*vec3(0.96,1.0,1.04);
    return mix(base,grey,clamp(0.5*state.y+0.4*misc.x,0.0,1.0))*(1.0-0.35*state.y);
}
// Transmittance of `distance` units of weather fog: dense fog ~300 units (75 m) visibility, rain/snow haze, overcast haze.
float weatherFogTransmit(float distance,vec4 state,vec4 misc){
    return exp(-distance*(0.01*misc.x*misc.x+0.0015*state.x+0.0004*state.y));
}
// Enhanced sky (as authored, gamma) under weather: cloud texture drift with the wind (u, one turn per 1024 m of wind),
// overcast and fog toward the weather fog colour, storm darkening, lightning.
vec2 weatherSkyScroll(vec4 wind){return vec2(wind.w/1024.0,0.0);}
vec3 weatherSky(vec3 sky,vec3 fogColor,vec4 state,vec4 misc,float t){
    // Overcast converges on the weather fog colour, so the horizon meets fogged distant land; storms are darker still.
    vec3 overcast=fogColor*(misc.w>3.5&&misc.w<4.5?0.75:1.0);
    sky=mix(sky,overcast,clamp(0.9*state.y+0.9*misc.x+0.2*state.x,0.0,1.0));
    return sky+vec3(0.75,0.8,1.0)*weatherLightning(misc,t);
}
