#include "ikv/renderer.hpp"
#include <cassert>
#include <cmath>
#include <limits>

int main() {
    ikv::Camera camera;
    camera.target={3,0,-2};
    const auto center=ikv::camera_ground_point(camera,400,300,800,600);
    assert(center && std::abs((*center)[0]-3)<0.0001f && std::abs((*center)[2]+2)<0.0001f);
    const auto gl=ikv::camera_view_projection(camera,800,600);
    const auto vk=ikv::camera_view_projection(camera,800,600,true);
    auto project=[](const ikv::Matrix4& matrix,const std::array<float,3>& p){
        std::array<float,4> clip{};
        for(unsigned row=0;row<4;++row) clip[row]=matrix[row]*p[0]+matrix[4+row]*p[1]+matrix[8+row]*p[2]+matrix[12+row];
        return std::array<float,3>{clip[0]/clip[3],clip[1]/clip[3],clip[2]/clip[3]};
    };
    const auto gl_center=project(gl,camera.target), vk_center=project(vk,camera.target);
    assert(std::abs(gl_center[0])<0.0001f && std::abs(gl_center[1])<0.0001f);
    assert(gl_center[2]>-1 && gl_center[2]<1 && vk_center[2]>0 && vk_center[2]<1);
    assert(!ikv::camera_ground_point(camera,0,0,0,600));
    camera.pitch=0;
    assert(!ikv::camera_ground_point(camera,400,300,800,600));
    camera.distance=std::numeric_limits<float>::quiet_NaN();
    bool rejected=false;
    try { ikv::camera_basis(camera); } catch(const std::invalid_argument&) { rejected=true; }
    assert(rejected);
    ikv::Matrix4 basis{1,0,0,0,0,1,0,0,0,0,1,0,1000,2000,3000,1};
    const std::array<float,3> origin{},point{10,0,0},half{0.5f,0.5f,0.5f};
    assert(!ikv::source_fog_enabled(origin,point,basis,origin,10)); // Strict >, not >=.
    assert(ikv::source_fog_enabled(origin,{10.001f,0,0},basis,origin,10));
    assert(!ikv::source_fog_enabled(half,origin,basis,origin,1)); // Matrix translation is ignored.
    assert(ikv::source_fog_enabled(half,{1,0,0},basis,origin,1)); // Test every corner.
    basis[0]=4;
    assert(ikv::source_fog_enabled(half,origin,basis,origin,1)); // Source basis scale is retained.
    rejected=false;
    try { ikv::source_fog_enabled(origin,origin,basis,origin,std::numeric_limits<float>::quiet_NaN()); }
    catch(const std::invalid_argument&) { rejected=true; }
    assert(rejected);
    const float nan=std::numeric_limits<float>::quiet_NaN();
    assert(!ikv::source_fog_enabled({nan,.5f,.5f},point,basis,origin,1)); // Unordered source corners stay false.
    for(float start:{1.f,10.f,13.f}) {
        assert(ikv::source_fog_enabled({-.5f,.5f,-.5f},point,basis,origin,start)==
               ikv::source_fog_enabled(half,point,basis,origin,start)); // Signed extents give the same eight corners.
    }
    rejected=false;
    try { ikv::source_fog_enabled(half,origin,basis,{nan,0,0},1); }
    catch(const std::invalid_argument&) { rejected=true; }
    assert(rejected);
    ikv::Scene scene;
    scene.nodes.emplace_back();
    scene.nodes[0].source_bounds_center={4,5,6};
    ikv::Instance actor;
    actor.entity_id=17;
    actor.transform.matrix={2,0,0,0,0,3,0,0,0,0,4,0,1,2,3,1};
    const ikv::RenderEntity entity{17,10,7,20,1.5707963267948966f};
    const auto placed=ikv::entity_instance_transform(scene,actor,entity.x,entity.ground_y,entity.z,entity.yaw).matrix;
    assert(std::abs(placed[12]-13)<.0001f && std::abs(placed[13]-9)<.0001f && std::abs(placed[14]-19)<.0001f);
    assert(std::abs(placed[2]+2)<.0001f && placed[5]==3 && std::abs(placed[8]-4)<.0001f); // Keep source scale; rotate all bases.
    const auto placed_center=ikv::entity_source_bounds_center(scene,actor,entity.x,entity.ground_y,entity.z,entity.yaw);
    assert(std::abs(placed_center[0]-16)<.0001f && placed_center[1]==12 && std::abs(placed_center[2]-16)<.0001f);
    assert(ikv::source_fog_enabled(origin,placed_center,placed,origin,20)); // Separate source center, not matrix translation.
    actor.entity_id=0;
    assert(ikv::entity_instance_transform(scene,actor,entity.x,entity.ground_y,entity.z,entity.yaw).matrix==actor.transform.matrix);
    assert(ikv::entity_source_bounds_center(scene,actor,entity.x,entity.ground_y,entity.z,entity.yaw)==scene.nodes[0].source_bounds_center);
    scene.meshes.emplace_back();
    scene.materials.resize(2);
    assert(ikv::scene_source_color_draw(scene,0)); // Default-true manual scene.
    scene.materials[1].source_color_draw=false;
    assert(!ikv::scene_source_color_draw(scene,0,1)); // Override, not the mesh's default material.
    scene.meshes[0].material=1;
    assert(!ikv::scene_source_color_draw(scene,0));
    assert(ikv::scene_source_color_draw(scene,0,0)); // Drawable override exposes a hidden default.
    scene.instances.resize(2);
    for(auto& instance:scene.instances){instance.mesh=0;instance.material=0;}
    scene.instances[1].source_ordinary_color_participates=false;
    const auto& visible=scene.instances[0];
    const auto& excluded=scene.instances[1];
    assert(ikv::scene_source_color_draw(scene,visible.mesh,visible.material,visible.source_ordinary_color_participates));
    assert(!ikv::scene_source_color_draw(scene,excluded.mesh,excluded.material,excluded.source_ordinary_color_participates));
    scene.materials[0].source_color_draw=false;
    assert(!ikv::scene_source_color_draw(scene,visible.mesh,visible.material,visible.source_ordinary_color_participates));
    assert(!ikv::scene_source_color_draw(scene,excluded.mesh,excluded.material,excluded.source_ordinary_color_participates));
    scene.materials[0].source_color_draw=true;
    rejected=false;
    try { ikv::scene_source_color_draw(scene,0,2,false); }
    catch(const std::invalid_argument&) { rejected=true; }
    assert(rejected);
    rejected=false;
    try { ikv::scene_source_color_draw(scene,1,0,false); }
    catch(const std::invalid_argument&) { rejected=true; }
    assert(rejected);
    rejected=false;
    try { ikv::scene_source_color_draw(scene,0,2); }
    catch(const std::invalid_argument&) { rejected=true; }
    assert(rejected); // A hidden mesh never suppresses invalid material references.
    scene.materials.clear();
    assert(ikv::scene_source_color_draw(scene,0)); // Preserve material-free Vulkan previews.
    assert(!ikv::scene_source_color_draw(scene,0,UINT32_MAX,false));
    rejected=false;
    try { ikv::scene_source_color_draw(scene,1); }
    catch(const std::invalid_argument&) { rejected=true; }
    assert(rejected); // Material-free previews still reject invalid mesh indices.
    const auto close_uv=[](const auto& actual,const std::array<float,4>& expected) {
        for(unsigned i=0;i<4;++i)assert(std::abs(actual[i]-expected[i])<1e-8f);
    };
    ikv::SourceScrollState scroll;
    close_uv(ikv::advance_source_scroll(scroll,1234),{0,0,0,0});
    assert(scroll.random_state==0xe7847115U); // First invocation consumes four CRT draws even with zero elapsed.
    close_uv(scroll.velocity,{-.02295f,-.00165f,-.0083f,0});
    close_uv(ikv::advance_source_scroll(scroll,1234),{0,0,0,0});
    assert(scroll.random_state==0x72ae7869U); // Another instance at the same tick still consumes four draws.
    close_uv(ikv::advance_source_scroll(scroll,2234),{-.00164f,.000775f,.000085f,-.002485f});
    ikv::SourceScrollState wrapped;
    wrapped.initialized=true;wrapped.previous_tick=UINT32_MAX-4;
    close_uv(ikv::advance_source_scroll(wrapped,5),{-.00002295f,-.00000165f,-.0000083f,0});
    ikv::SourceScrollState boundary;
    boundary.initialized=true;boundary.previous_tick=100;
    boundary.velocity={.5f,-.5f,.50001f,-.50001f};
    close_uv(ikv::advance_source_scroll(boundary,1100),{.047705f,-.050165f,.048331f,-.047501f});
    // Frustum culling may only drop meshes whose every vertex lies outside one clip plane (GL and Vulkan depth).
    ikv::Mesh cube;
    for(int i=0;i<8;++i)cube.vertices.push_back({float(i&1)-.5f,float((i>>1)&1)-.5f,float((i>>2)&1)-.5f,0,0,1,0,0});
    const auto bounds=ikv::mesh_bounds(cube);
    assert(std::abs(bounds[3]-std::sqrt(3.f)/2)<1e-6f);
    ikv::Mesh broken=cube;broken.vertices[3].y=std::numeric_limits<float>::quiet_NaN();
    assert(std::isinf(ikv::mesh_bounds(broken)[3]));
    int culled=0,kept=0;
    for(const auto* vp:{&gl,&vk}) {
        const auto planes=ikv::frustum_planes(*vp);
        for(int x=-40;x<=40;x+=4)for(int z=-40;z<=40;z+=4)for(float scale:{.5f,3.f}) {
            const float c=std::cos(float(x+z)*.1f),s=std::sin(float(x+z)*.1f);
            const ikv::Matrix4 model{c*scale,0,-s*scale,0,0,scale,0,0,s*scale,0,c*scale,0,float(x),float(z%7),float(z),1};
            if(ikv::sphere_visible(planes,model,bounds)){++kept;continue;}
            ++culled;
            bool outside_one=false;
            for(unsigned plane=0;plane<6&&!outside_one;++plane) {
                bool all=true;
                for(const auto& v:cube.vertices) {
                    std::array<float,3> w{};
                    for(unsigned r=0;r<3;++r)w[r]=model[r]*v.x+model[4+r]*v.y+model[8+r]*v.z+model[12+r];
                    std::array<float,4> p{};
                    for(unsigned r=0;r<4;++r)p[r]=(*vp)[r]*w[0]+(*vp)[4+r]*w[1]+(*vp)[8+r]*w[2]+(*vp)[12+r];
                    // z+w<0 separates both the GL volume and Vulkan's 0<=z<=w volume.
                    const float d=plane==0?p[3]+p[0]:plane==1?p[3]-p[0]:plane==2?p[3]+p[1]:plane==3?p[3]-p[1]:plane==4?p[3]+p[2]:p[3]-p[2];
                    all=all&&d<0;
                }
                outside_one=all;
            }
            assert(outside_one);
        }
    }
    assert(culled>100&&kept>100);
    // View distance drops a sphere only when it lies wholly beyond the limit; 0 and infinite bounds never drop.
    const ikv::Matrix4 far_cube{2,0,0,0,0,2,0,0,0,0,2,0,100,0,0,1}; // radius sqrt(3) at x=100
    assert(ikv::sphere_within({0,0,0},far_cube,bounds,100-std::sqrt(3.f)+.01f));
    assert(!ikv::sphere_within({0,0,0},far_cube,bounds,100-std::sqrt(3.f)-.01f));
    assert(ikv::sphere_within({0,0,0},far_cube,bounds,0));
    assert(ikv::sphere_within({0,0,0},far_cube,ikv::mesh_bounds(broken),1));
    const auto identity=ikv::Transform{}.matrix;
    const auto caster_planes=ikv::shadow_caster_planes(identity),full_planes=ikv::frustum_planes(identity);
    const ikv::Matrix4 near_clamped{1,0,0,0,0,1,0,0,0,0,1,0,0,0,-2,1};
    const ikv::Matrix4 beside_light{1,0,0,0,0,1,0,0,0,0,1,0,2,0,0,1};
    assert(!ikv::sphere_visible(full_planes,near_clamped,bounds));
    assert(ikv::sphere_visible(caster_planes,near_clamped,bounds)); // depth-clamped Sun casters must not be near-culled
    assert(!ikv::sphere_visible(caster_planes,beside_light,bounds));
    const ikv::Camera shadow_camera;
    const auto shadow_sun=ikv::enhanced_sun(ikv::SceneLighting{});
    const auto shadow_radius=[](const ikv::Matrix4& light) {
        return 1/std::sqrt(light[0]*light[0]+light[4]*light[4]+light[8]*light[8]);
    };
    assert(ikv::original_blob_shadows_enabled(true));
    assert(!ikv::original_blob_shadows_enabled(false));
    const auto unlimited=ikv::shadow_light_cascades(shadow_camera,shadow_sun,0);
    assert(unlimited.light[0]==ikv::shadow_light_matrix(shadow_camera,shadow_sun));
    assert(std::abs(unlimited.split-44)<1e-6f);
    assert(std::abs(shadow_radius(unlimited.light[0])-44)<1e-4f);
    assert(std::abs(shadow_radius(unlimited.light[1])-600)<1e-4f);
    const auto small=ikv::shadow_light_cascades(shadow_camera,shadow_sun,10);
    assert(std::abs(shadow_radius(small.light[1])-176)<1e-4f);
    const auto configured=ikv::shadow_light_cascades(shadow_camera,shadow_sun,2000);
    assert(std::abs(shadow_radius(configured.light[1])-2000)<1e-2f);
    for(const auto& cascades:{unlimited,small,configured}) {
        for(const auto& light:cascades.light) {
            for(float value:light) assert(std::isfinite(value));
            const float radius=shadow_radius(light);
            assert(std::abs(light[12]*1024-std::round(light[12]*1024))<1e-3f);
            assert(std::abs(light[13]*1024-std::round(light[13]*1024))<1e-3f);
            for(float direction:{-1.f,1.f}) {
                std::array<float,3> edge{};
                for(unsigned i=0;i<3;++i) edge[i]=shadow_camera.target[i]+shadow_sun[i]*radius*direction;
                const auto projected=project(light,edge);
                assert(projected[2]>=0&&projected[2]<=1);
            }
        }
    }
    { // NONCANONICAL wave 27 weather inputs
        ikv::SceneLighting light;
        auto zero=[](const ikv::WeatherUniforms& u){for(const auto* v:{&u.wind,&u.weather,&u.misc})for(float f:*v)if(f!=0)return false;return true;};
        assert(zero(ikv::weather_uniforms(light,false,5))); // inactive
        light.weather=*ikv::parse_weather_override("season=winter,kind=storm,wind=0:-2:30,gust=.5,precip=1.5,cloud=.7,fog=.2,wet=.3,snow=1,temp=-4");
        assert(light.weather.active&&light.weather.season==ikv::Season::winter&&light.weather.kind==ikv::WeatherKind::storm&&light.weather.temperature==-4);
        assert(zero(ikv::weather_uniforms(light,true,5))); // original appearance
        auto u=ikv::weather_uniforms(light,false,1030);
        assert(u.active()&&u.wind[0]==0&&u.wind[1]==-1&&u.wind[2]==25&&std::abs(u.wind[3]-6)<1e-4f&&u.weather[0]==1&&u.misc[2]==3&&u.misc[3]==4);
        assert(std::abs(ikv::advance_wind_phase(1020,light,false,.5f)-(1020+12.5-1024))<1e-9);
        assert(ikv::advance_wind_phase(7,light,true,.5f)==7);
        light.roofed=true;assert(zero(ikv::weather_uniforms(light,false,5))&&ikv::advance_wind_phase(7,light,false,.5f)==7);
        light.roofed=false;
        { // wave 28 fog parity: GL clears to Vulkan's linear backdrop after the enhanced tonemap
            const auto quiet=ikv::enhanced_backdrop_linear(ikv::weather_uniforms(ikv::SceneLighting{},false,5),light);
            assert(quiet==(std::array<float,3>{.04f,.05f,.065f}));
            const auto storm=ikv::enhanced_backdrop_linear(u,light),expected=ikv::weather_backdrop(u,light,{std::pow(.04f,1/2.2f),std::pow(.05f,1/2.2f),std::pow(.065f,1/2.2f)});
            for(unsigned i=0;i<3;++i)assert(std::abs(std::pow(storm[i],1/2.2f)-expected[i])<1e-5f);
            const auto shown=ikv::enhanced_display({0,.18f,4},1);
            assert(shown[0]==0&&shown[1]>.5f&&shown[1]<.6f&&shown[2]>.95f&&shown[2]<=1); // ACES(.18)^(1/2.2) ~ .55
        }
        ikv::Material leaf;leaf.alpha_cutoff=.3f;leaf.diffuse="textures/AgacYeni.dds";
        const ikv::Matrix4 unit{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        const auto sway=ikv::weather_sway(u,leaf,unit,{2,22},false);
        assert(sway[0]==2&&std::abs(sway[1]-.05f)<1e-6f&&std::abs(sway[2]-.7f)<1e-5f);
        assert(ikv::weather_sway(u,leaf,unit,{2,22},true)[2]==0&&ikv::weather_sway(u,leaf,unit,{0,200},false)[2]==0);
        assert(ikv::weather_sway(u,ikv::Material{},unit,{2,22},false)[2]==0);
        ikv::Material wall;wall.alpha_cutoff=.3f;wall.diffuse="textures/yenicami_duvar.dds";
        assert(ikv::weather_sway(u,wall,unit,{2,22},false)[2]==0); // alpha-tested but not foliage/cloth
        bool override_rejected=false;
        try{ikv::parse_weather_override("kind=hail");}catch(const std::invalid_argument&){override_rejected=true;}
        assert(override_rejected&&!ikv::parse_weather_override(nullptr)&&!ikv::parse_weather_override(""));
    }
    { // NONCANONICAL wave 28: puddle mirror (weather.glsl weatherPuddle) coverage and ground-mark packing
        auto coverage=[](float wet){int covered=0;for(int i=0;i<400;++i)for(int j=0;j<400;++j)covered+=ikv::weather_puddle(float(i)*2.3f-410,float(j)*1.9f+57,wet)>.05f*wet;return float(covered)/160000.f;};
        const float full=coverage(1),quarter=coverage(.25f),halfway=coverage(.5f);
        assert(full>.08f&&full<.16f&&quarter<.04f&&quarter<halfway&&halfway<full); // occasional puddles, growing with wetness
        assert(ikv::weather_puddle(3,4,0)==0&&ikv::weather_puddle(3,4,-1)==0&&ikv::weather_puddle(3,4,2)==ikv::weather_puddle(3,4,1));
        // Regression lock of the shader formula (change both sides together): sampled values at known points.
        assert(std::abs(ikv::weather_hash(3,-7)-.414270818f)<1e-7f);
        assert(std::abs(ikv::weather_puddle(-23.04f,146.88f,1)-.52662307f)<1e-4f);
        ikv::WeatherUniforms active;active.misc[3]=3;
        const std::array<ikv::GroundMark,3> marks{{{10,-4,.5f,-1.f,-1},{12,-2,200,0,1},{11,-6,89,7.f,1}}};
        const std::array<float,3> heights{2,2,3};
        assert(ikv::ground_mark_uniforms({},marks,heights).info[0]==0); // inactive weather: nothing
        const auto g=ikv::ground_mark_uniforms(active,marks,heights);
        assert(g.info[0]==2&&g.bounds[0]==10-ikv::kGroundMarkReach&&g.bounds[1]==-6-ikv::kGroundMarkReach&&g.bounds[2]==11+ikv::kGroundMarkReach&&g.bounds[3]==-4+ikv::kGroundMarkReach);
        auto decode=[](float w){const float v=std::abs(w)-1;return std::array<float,3>{std::floor(v/64)/64,(v-64*std::floor(v/64))*(6.2831853f/64),w<0?-1.f:1.f};};
        const auto a=decode(g.marks[0][3]),b=decode(g.marks[1][3]);
        assert(g.marks[0][0]==10&&g.marks[0][1]==-4&&g.marks[0][2]==2&&a[0]==.5f&&std::abs(a[1]-(6.2831853f-1))<.06f&&a[2]==-1);
        assert(g.marks[1][2]==3&&std::abs(b[0]-89)<1.f/64&&std::abs(b[1]-(7-6.2831853f))<.06f&&b[2]==1);
        ikv::GroundMarkTracker tracker(nullptr);
        tracker.set(marks);
        const std::array<ikv::RenderEntity,2> walkers{{{1,10.5f,7,-4,0,{}},{2,40,1,40,0,{}}}};
        assert(tracker.uniforms(active,walkers,ikv::Camera{},0).marks[0][2]==7); // the nearest walker's ground
        const std::array<ikv::RenderEntity,1> moved{{{1,40,9,40,0,{}}}};
        assert(tracker.uniforms(active,moved,ikv::Camera{},0).marks[0][2]==7); // kept while the mark lives
    }
    return 0;
}
