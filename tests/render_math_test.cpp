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
    return 0;
}
