#include "ikv/scene.hpp"
#include "ikv/renderer.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>


namespace ikv {
namespace {
AnimationKey interpolate_key(const AnimationKey& a,const AnimationKey& b,float t) {
    float first=1, second=0, sign=1;
    if (t!=0) {
        float dot=0;
        for (unsigned i=0;i<4;++i) dot += a.rotation[i]*b.rotation[i];
        sign = dot<0 ? -1.0f : 1.0f;
        dot = std::clamp(dot*sign,0.0f,1.0f);
        first=1-t; second=t;
        if (1-dot>=0.000001f) {
            const float angle = std::acos(dot);
            const float inverse_sine = 1/std::sin(angle);
            first = std::sin((1-t)*angle)*inverse_sine;
            second = std::sin(t*angle)*inverse_sine;
        }
    }
    AnimationKey result;
    auto& q=result.rotation;
    float length_squared = 0;
    for (unsigned i=0;i<4;++i) {
        q[i] = first*a.rotation[i]+second*sign*b.rotation[i];
        length_squared += q[i]*q[i];
    }
    if (!(length_squared>0)) throw std::runtime_error("invalid animation quaternion");
    const float inverse_length = 1/std::sqrt(length_squared);
    for(auto& value:q)value*=inverse_length;
    for(unsigned i=0;i<3;++i)result.position[i]=a.position[i]+t*(b.position[i]-a.position[i]);
    return result;
}
AnimationKey sample_key(const AnimationTrack& track,float seconds) {
    const auto next=std::upper_bound(track.keys.begin(),track.keys.end(),seconds,
        [](float time,const AnimationKey& key){return time<key.time;});
    const auto previous=next==track.keys.begin()?0:std::size_t(next-track.keys.begin()-1);
    const auto& a=track.keys[previous];
    const auto& b=track.keys.size()==1?a:track.keys[previous==0?1:std::size_t(next-track.keys.begin())];
    const float delta=b.time-a.time;
    return interpolate_key(a,b,track.keys.size()==1?0:(seconds-a.time)/(delta==0?1:delta));
}
Transform key_transform(const AnimationKey& key) {
    // Recovered 004444a0 maps raw quaternion axes to the renderer's coordinates.
    const float x=key.rotation[0],y=-key.rotation[2],z=-key.rotation[1],w=key.rotation[3];
    Transform result;
    auto& m = result.matrix;
    m[0]=1-2*(y*y+z*z); m[1]=2*(x*y+z*w); m[2]=2*(x*z-y*w);
    m[4]=2*(x*y-z*w); m[5]=1-2*(x*x+z*z); m[6]=2*(y*z+x*w);
    m[8]=2*(x*z+y*w); m[9]=2*(y*z-x*w); m[10]=1-2*(x*x+y*y);
    for (unsigned i=0;i<3;++i) m[12+i]=key.position[i];
    return result;
}

Vertex& source_corner(Scene& scene, const SourceSurface& surface, std::uint32_t index) {
    if (index>=surface.corners.size()) throw std::runtime_error("invalid source surface corner");
    const auto& corner=surface.corners[index];
    if (corner.mesh>=scene.meshes.size() || corner.vertex>=scene.meshes[corner.mesh].vertices.size())
        throw std::runtime_error("invalid source surface vertex");
    return scene.meshes[corner.mesh].vertices[corner.vertex];
}

// Per-mesh views of scene.source_surfaces, built in O(corners) per skinning pass. Scanning every surface's corners for
// every mesh was O(meshes x corners) and dominated frames once several PCs were in view.
struct SurfaceIndex {
    std::vector<std::uint8_t> recompute;             // mesh owns a corner of a recompute_normals surface
    std::vector<std::uint32_t> touch_begin, touch;   // CSR: surfaces owning a corner of the mesh, in surface order
    std::vector<std::uint32_t> clear_begin;          // CSR: root-clear {surface, corner} of the mesh, in source order
    std::vector<std::array<std::uint32_t,2>> clear;
    std::vector<std::uint32_t> last, touch_at, clear_at;
};
void index_surfaces(Scene& scene, SurfaceIndex& x) {
    const auto meshes=scene.meshes.size();
    x.recompute.assign(meshes,0);
    x.touch_begin.assign(meshes+1,0); x.clear_begin.assign(meshes+1,0);
    // Pass 0 counts (and validates root-clear corners like the per-mesh loop did); pass 1 fills in the same order.
    for (unsigned pass=0; pass<2; ++pass) {
        x.last.assign(meshes,UINT32_MAX);
        for (std::uint32_t s=0; s<scene.source_surfaces.size(); ++s) {
            const auto& surface=scene.source_surfaces[s];
            for (const auto& corner : surface.corners) {
                if (corner.mesh>=meshes || x.last[corner.mesh]==s) continue;
                x.last[corner.mesh]=s;
                if (pass) x.touch[x.touch_at[corner.mesh]++]=s;
                else { ++x.touch_begin[corner.mesh+1]; if (surface.recompute_normals) x.recompute[corner.mesh]=1; }
            }
            if (!surface.root_clears || !meshes) continue;
            for (const auto corner : surface.unweighted_corners) {
                if (pass) { x.clear[x.clear_at[surface.corners[corner].mesh]++]={s,corner}; continue; }
                source_corner(scene,surface,corner);
                ++x.clear_begin[surface.corners[corner].mesh+1];
            }
        }
        if (pass) break;
        for (std::size_t m=0; m<meshes; ++m) { x.touch_begin[m+1]+=x.touch_begin[m]; x.clear_begin[m+1]+=x.clear_begin[m]; }
        x.touch.resize(x.touch_begin[meshes]); x.clear.resize(x.clear_begin[meshes]);
        x.touch_at.assign(x.touch_begin.begin(),x.touch_begin.end()-1);
        x.clear_at.assign(x.clear_begin.begin(),x.clear_begin.end()-1);
    }
}

void update_source_normals(Scene& scene, SourceSurface& surface, bool& changed) {
    if (surface.normals.size()!=surface.heads.size() || surface.aliases.size()!=surface.heads.size() ||
        surface.indices.size()%3!=0)
        throw std::runtime_error("invalid source surface normal metadata");
    for (const auto index : surface.indices) {
        if (index>=surface.heads.size() || surface.aliases[index][0]>=surface.heads.size() ||
            surface.aliases[index][1]>=surface.heads.size())
            throw std::runtime_error("invalid source surface index or alias");
        source_corner(scene,surface,surface.heads[index]);
    }
    if (surface.root_clears)
        for (auto& normal : surface.normals) normal={0,0,0};
    for (std::size_t triangle=0; triangle<surface.indices.size(); triangle+=3) {
        // 0047cf50 reads the CPU index array that 00469580 stores already reversed (src2,src1,src0), exactly like
        // surface.indices: normal = (I0-I1)x(I2-I1). Un-reversing here pointed every recomputed actor normal inward.
        const auto ia=surface.indices[triangle], ib=surface.indices[triangle+1],
                   ic=surface.indices[triangle+2];
        const auto& a=source_corner(scene,surface,surface.heads[ia]);
        const auto& b=source_corner(scene,surface,surface.heads[ib]);
        const auto& c=source_corner(scene,surface,surface.heads[ic]);
        const float az=a.z-b.z, cz=c.z-b.z, cy=c.y-b.y, ay=a.y-b.y,
                    cx=c.x-b.x, ax=a.x-b.x;
        const std::array<float,3> face{cz*ay-cy*az, cx*az-cz*ax, cy*ax-cx*ay};
        for (const auto index : {ia,ib,ic})
            for (unsigned axis=0; axis<3; ++axis) surface.normals[index][axis]+=face[axis];
    }
    // Native loops visit every index occurrence, not every distinct alias vertex.
    for (std::size_t triangle=0; triangle<surface.indices.size(); triangle+=3)
        for (unsigned corner=3; corner!=0; --corner) {
            const auto index=surface.indices[triangle+corner-1];
            const auto& alias=surface.aliases[index];
            if (index!=alias[0] && index==alias[1])
                for (unsigned axis=0; axis<3; ++axis)
                    surface.normals[alias[0]][axis]=surface.normals[index][axis]+surface.normals[alias[0]][axis];
        }
    for (std::size_t triangle=0; triangle<surface.indices.size(); triangle+=3)
        for (unsigned corner=3; corner!=0; --corner) {
            const auto index=surface.indices[triangle+corner-1];
            const auto& alias=surface.aliases[index];
            if (index!=alias[0] && index==alias[1]) surface.normals[index]=surface.normals[alias[0]];
        }
    for (std::size_t index=0; index<surface.heads.size(); ++index) {
        // 0047dab0 uploads the raw face/alias sums; it does not normalize.
        const auto& normal=surface.normals[index];
        std::size_t visited=0;
        for (auto corner=surface.heads[index]; corner!=UINT32_MAX; corner=surface.corners[corner].next) {
            if (visited++>=surface.corners.size()) throw std::runtime_error("cyclic source surface corners");
            auto& vertex=source_corner(scene,surface,corner);
            if (vertex.nx!=normal[0] || vertex.ny!=normal[1] || vertex.nz!=normal[2]) {
                vertex.nx=normal[0]; vertex.ny=normal[1]; vertex.nz=normal[2];
                ++scene.meshes[surface.corners[corner].mesh].revision;
                changed=true;
            }
        }
    }
}

void advance_actor(Scene& scene, ActorAnimation& actor, float seconds) {
    if (actor.stopped || actor.library==UINT32_MAX) return;
    if (actor.library>=scene.actor_tables.libraries.size())
        throw std::runtime_error("invalid actor animation library");
    const auto& library=scene.actor_tables.libraries[actor.library];
    const auto& clips=library.clips;
    if (clips.empty()) {
        actor.stopped=true;
        return;
    }
    const auto stop=[&](const char* reason, std::uint32_t id) {
        actor.stopped=true;
        scene.diagnostics.push_back("Actor animation stopped: "+std::string(reason)+" "+
            std::to_string(id)+" (model "+library.model+", class "+library.class_id+")");
    };
    if (!actor.initialized) {
        actor.initialized=true;
        actor.previous_seconds=seconds;
        const auto clip=std::find_if(clips.begin(),clips.end(),
            [](const ActorClip& candidate) { return candidate.id==2; });
        if (clip==clips.end()) {
            stop("missing source generic startup clip",2);
            return;
        }
        actor.current_clip=static_cast<std::uint32_t>(clip-clips.begin());
        actor.time=clip->start;
        actor.action_elapsed=0;
        return;
    }
    const float delta=(seconds-actor.previous_seconds)*actor.rate;
    actor.previous_seconds=seconds;
    if (delta<0) return;
    if (actor.current_clip>=clips.size())
        throw std::runtime_error("invalid actor animation clip cursor");
    const auto& clip=clips[actor.current_clip];
    const bool blending=actor.blend_duration>0;
    actor.time+=delta;
    actor.action_elapsed+=delta;
    if(actor.blend_duration>0){
        actor.blend_elapsed+=delta;actor.blend_from_time+=delta;
        if(actor.blend_from_clip<clips.size()){
            const auto& old=clips[actor.blend_from_clip];
            if(old.loops&&actor.blend_from_time>=old.loop_end){const float period=old.loop_end-old.loop_start;actor.blend_from_time=old.loop_start+std::fmod(actor.blend_from_time-old.loop_end,period==0?1:period);}
            else if(!old.loops)actor.blend_from_time=std::min(actor.blend_from_time,old.loop_end);
        }
        if(actor.blend_elapsed>actor.blend_duration||actor.time>=clip.loop_end)actor.blend_duration=0;
    }
    if (!blending && clip.loops) {
        if (actor.time>=clip.loop_end) {
            const float period=clip.loop_end-clip.loop_start;
            // 0045c9e0 subtracts loop end; an equal loop interval substitutes one.
            actor.time=clip.loop_start+std::fmod(actor.time-clip.loop_end,period==0 ? 1 : period);
            ++actor.audio_cue; // 0045cb9e replays clip audio on each loop wrap.
        }
    } else if(!blending) {
        actor.time=std::min(actor.time,clip.loop_end);
    }
    // 0047bea0 arms a looping action with deadline 0x70000000: it loops until another action replaces it.
    if (!actor.action_held && actor.action_elapsed>=std::max(clip.end-clip.start,1.0f)) {
        const auto next=std::find_if(clips.begin(),clips.end(),
            [&clip](const ActorClip& candidate) { return candidate.id==clip.next_id; });
        if (next==clips.end()) {
            stop("missing source jump target",clip.next_id);
            return;
        }
        actor.current_clip=static_cast<std::uint32_t>(next-clips.begin());
        // 0047aa80 uses 00594210 (float bits 3d088889), resets once, and discards overshoot.
        actor.time=float(clip.next_frame)*0.033333335f;
        actor.action_elapsed=0;
        // 0047e070 -> 0047cec0 -> 0045c980 plays non-looping clip audio on jump entry. A jump to the same clip
        // (Death/Fare_Die hold their last frame) is not re-voiced [INFERENCE: the original deadline re-arm path
        // through controller +0xc is unresolved; a dead actor screaming every 1.5 s is not original behaviour].
        if (!next->loops && &*next != &clip) ++actor.audio_cue;
    }
}
} // namespace

void set_actor_action(Scene& scene,std::uint32_t entity_id,std::uint32_t source_clip_id,float blend_seconds,bool restart,bool terminal) {
    if(!entity_id||!std::isfinite(blend_seconds)||blend_seconds<0)throw std::invalid_argument("invalid source actor action");
    const auto assembly=std::find_if(scene.actor_assemblies.begin(),scene.actor_assemblies.end(),
        [entity_id](const SceneActorAssembly& value){return value.entity_id==entity_id;});
    if(assembly==scene.actor_assemblies.end()||assembly->controller>=scene.actors.size())throw std::invalid_argument("source actor entity has no assembly");
    auto& actor=scene.actors[assembly->controller];
    if(actor.library>=scene.actor_tables.libraries.size())throw std::runtime_error("source actor has no animation library");
    const auto& clips=scene.actor_tables.libraries[actor.library].clips;
    const auto clip=std::find_if(clips.begin(),clips.end(),[source_clip_id](const ActorClip& value){return value.id==source_clip_id;});
    if(clip==clips.end())throw std::invalid_argument("source numeric action absent from actor library");
    const auto next=std::uint32_t(clip-clips.begin());
    if(terminal){
        actor.blend_duration=0;actor.blend_elapsed=0;actor.blend_from_time=0;actor.blend_from_clip=UINT32_MAX;
        actor.current_clip=next;actor.time=clip->end;actor.action_elapsed=0;actor.action_held=false;
        actor.previous_seconds=scene.animation_seconds;actor.initialized=true;actor.stopped=false;
        for(auto& track:scene.animation){
            if(track.controller!=assembly->controller||track.keys.empty()||track.instance>=scene.nodes.size())continue;
            float sample_seconds=clip->end;
            if(track.keys.size()>1){
                // Restore the last legal source sample without changing ordinary sparse-track holding.
                sample_seconds=std::min(sample_seconds,std::nextafter(track.keys.back().time,-std::numeric_limits<float>::infinity()));
                if(!(sample_seconds>=clip->start))continue;
            }
            track.cached_pose=sample_key(track,sample_seconds);track.pose_initialized=true;
            scene.nodes[track.instance].local=key_transform(track.cached_pose);
        }
        scene.force_pose_update=true;
        return;
    }
    if(!restart&&actor.initialized&&actor.current_clip==next)return; // 0047bea0 does not restart the current numeric action.
    const bool was_blending=actor.blend_duration>0;
    if(!was_blending){actor.blend_from_time=actor.time;actor.blend_from_clip=actor.current_clip;}
    actor.blend_elapsed=0;actor.blend_duration=actor.initialized?blend_seconds:0;
    if(!was_blending)for(auto& track:scene.animation)if(track.controller==assembly->controller)track.blend_pose=track.cached_pose;
    actor.current_clip=next;actor.time=clip->start;actor.action_elapsed=0;actor.stopped=false;actor.action_held=clip->loops;
    if(!clip->loops)++actor.audio_cue; // 0047ced4 -> 0045c980 plays non-looping clip audio on entry.
    if(!actor.initialized)actor.previous_seconds=scene.animation_seconds;
    actor.initialized=true;scene.force_pose_update=true;
}
Transform entity_instance_transform(const Scene&,const Instance& instance,float x,float ground,float z,float orientation) {
    if(!std::isfinite(x)||!std::isfinite(ground)||!std::isfinite(z)||!std::isfinite(orientation))throw std::invalid_argument("invalid entity placement");
    if(!instance.entity_id)return instance.transform;
    Transform placement;const float c=std::cos(orientation),s=std::sin(orientation);
    placement.matrix[0]=c;placement.matrix[2]=-s;placement.matrix[8]=s;placement.matrix[10]=c;
    placement.matrix[12]=x;placement.matrix[13]=ground;placement.matrix[14]=z;
    Transform result;result.matrix=multiply_matrix(placement.matrix,instance.transform.matrix);return result;
}
std::array<float,3> entity_source_bounds_center(const Scene& scene,const Instance& instance,float x,float ground,float z,float orientation) {
    if(instance.node>=scene.nodes.size())throw std::invalid_argument("invalid source bounds node");
    const auto& center=scene.nodes[instance.node].source_bounds_center;
    if(!instance.entity_id)return center;
    if(!std::isfinite(x)||!std::isfinite(ground)||!std::isfinite(z)||!std::isfinite(orientation))throw std::invalid_argument("invalid entity bounds placement");
    const float c=std::cos(orientation),s=std::sin(orientation);
    return {c*center[0]+s*center[2]+x,center[1]+ground,-s*center[0]+c*center[2]+z};
}

void update_scene(Scene& scene, float seconds) {
    if (!std::isfinite(seconds)) throw std::invalid_argument("non-finite animation time");
    scene.animation_seconds=seconds;
    // 0046d680 publishes the unsigned native millisecond tick used by the sky class.
    double sky_tick=std::fmod(double(seconds)*1000.0,4294967296.0);
    if(sky_tick<0)sky_tick+=4294967296.0;
    scene.lighting.sky_rotation=float(static_cast<std::uint32_t>(sky_tick))*5e-6f;
    for (auto& actor : scene.actors) advance_actor(scene,actor,seconds);
    bool pose_changed = scene.force_pose_update;
    scene.force_pose_update=false;
    bool bounds_changed = false;
    for (auto& track : scene.animation) {
        float sample_seconds=seconds;
        if (track.controller!=UINT32_MAX) {
            if (track.controller>=scene.actors.size())
                throw std::runtime_error("invalid animation track controller");
            const auto& actor=scene.actors[track.controller];
            if (actor.stopped || actor.library==UINT32_MAX ||
                scene.actor_tables.libraries[actor.library].clips.empty()) continue;
            sample_seconds=actor.time;
        }
        if (track.keys.empty()) continue;
        // Recovered 0045c9e0 retains its cached pose when no strictly later key exists.
        if (track.keys.size()>1 && sample_seconds>=track.keys.back().time) continue;
        auto key=sample_key(track,sample_seconds);
        if(track.controller!=UINT32_MAX){
            const auto& actor=scene.actors[track.controller];
            if(actor.blend_duration>0&&track.keys.size()>1){
                if(actor.blend_from_time<track.keys.back().time)track.blend_pose=sample_key(track,actor.blend_from_time);
                key=interpolate_key(track.blend_pose,key,std::clamp(actor.blend_elapsed/actor.blend_duration,0.f,1.f));
            }
        }
        track.cached_pose=key;track.pose_initialized=true;
        auto& local = scene.nodes[track.instance].local;
        const auto sampled = key_transform(key);
        if (local.matrix!=sampled.matrix) {
            local=sampled;
            pose_changed=true;
        }
    }
    for (const auto index : scene.node_order) {
        auto& node = scene.nodes[index];
        const auto world = node.parent<0 ? node.local.matrix :
            multiply_matrix(scene.nodes[static_cast<std::size_t>(node.parent)].world.matrix,node.local.matrix);
        if (node.world.matrix!=world) {
            node.world.matrix=world;
            pose_changed=true;
        }
        // Recovered 00410800 uses source-space position addition, not world rotation.
        std::array<float,3> center{};
        if (node.source_reference>=0) {
            center=scene.nodes[static_cast<std::size_t>(node.source_reference)].source_bounds_center;
        } else {
            for (unsigned axis=0; axis<3; ++axis) {
                center[axis]=node.local.matrix[12+axis];
                if (node.parent>=0)
                    center[axis]+=scene.nodes[static_cast<std::size_t>(node.parent)].source_bounds_center[axis];
            }
        }
        if (node.source_bounds_center!=center) {
            node.source_bounds_center=center;
            bounds_changed=true;
        }
    }
    bool changed = pose_changed || bounds_changed || scene.geometry_revision==0;
    const Transform identity;
    for (auto& instance : scene.instances) {
        // 00476eb0 already produces skinned vertices in world space.
        const auto& transform = scene.meshes[instance.mesh].influences.empty() ?
            scene.nodes[instance.node].world : identity;
        if (instance.transform.matrix!=transform.matrix) {
            instance.transform=transform;
            changed=true;
        }
    }
    // Held actors (skin_held) skip skinning. One released after skipping a pose change is re-skinned; when that is the
    // only reason for this pass (unchanged bones), every other mesh is skipped too.
    bool resumed = false;
    for (const auto& assembly : scene.actor_assemblies) resumed |= assembly.skin_stale && !assembly.skin_held;
    if (pose_changed || resumed || scene.geometry_revision==0) {
        for (auto& surface : scene.source_surfaces) surface.positions_changed=false;
        static thread_local SurfaceIndex by_mesh; // reused buffers
        index_surfaces(scene,by_mesh);
        const bool all = pose_changed || scene.geometry_revision==0;
        static thread_local std::vector<std::uint8_t> held; // per mesh: 1 = not skinned in this pass
        held.assign(scene.meshes.size(),all ? 0 : 1);
        const bool initial = scene.geometry_revision==0;
        for (auto& assembly : scene.actor_assemblies) {
            if (assembly.end[0]>held.size() || assembly.begin[0]>assembly.end[0])
                throw std::runtime_error("invalid actor assembly mesh range");
            const bool hold = !initial && assembly.skin_held;
            const bool skip = hold || (!all && !assembly.skin_stale);
            if (all || !hold) assembly.skin_stale = hold;
            std::fill(held.begin()+static_cast<std::ptrdiff_t>(assembly.begin[0]),held.begin()+static_cast<std::ptrdiff_t>(assembly.end[0]),std::uint8_t{skip});
        }
        for (std::size_t mesh_index=0; mesh_index<scene.meshes.size(); ++mesh_index) {
            if (held[mesh_index]) continue;
            auto& mesh=scene.meshes[mesh_index];
            bool surface_changed=false;
            bool positions_changed=false;
            const bool source_normals=!mesh.influences.empty() && by_mesh.recompute[mesh_index];
            // Loader groups by vertex, retaining original bone accumulation order.
            for (std::size_t first=0; first<mesh.influences.size();) {
                const auto vertex_index = mesh.influences[first].vertex;
                float x=0,y=0,z=0,nx=0,ny=0,nz=0;
                do {
                    const auto& influence = mesh.influences[first++];
                    const auto& m = scene.nodes[influence.node].world.matrix;
                    const auto& p = influence.position;
                    const auto& n = influence.normal;
                    const float weight = influence.weight;
                    x += weight*(m[0]*p[0]+m[4]*p[1]+m[8]*p[2]+m[12]);
                    y += weight*(m[1]*p[0]+m[5]*p[1]+m[9]*p[2]+m[13]);
                    z += weight*(m[2]*p[0]+m[6]*p[1]+m[10]*p[2]+m[14]);
                    if (!source_normals) {
                        nx += weight*(m[0]*n[0]+m[4]*n[1]+m[8]*n[2]);
                        ny += weight*(m[1]*n[0]+m[5]*n[1]+m[9]*n[2]);
                        nz += weight*(m[2]*n[0]+m[6]*n[1]+m[10]*n[2]);
                    }
                } while (first<mesh.influences.size() && mesh.influences[first].vertex==vertex_index);
                const float length_squared = nx*nx+ny*ny+nz*nz;
                if (length_squared>0) {
                    const float inverse_length = 1/std::sqrt(length_squared);
                    nx*=inverse_length; ny*=inverse_length; nz*=inverse_length;
                }
                auto& vertex = mesh.vertices[vertex_index];
                if (vertex.x!=x || vertex.y!=y || vertex.z!=z ||
                    (!source_normals && (vertex.nx!=nx || vertex.ny!=ny || vertex.nz!=nz))) {
                    positions_changed |= vertex.x!=x || vertex.y!=y || vertex.z!=z;
                    vertex.x=x; vertex.y=y; vertex.z=z;
                    if (!source_normals) { vertex.nx=nx; vertex.ny=ny; vertex.nz=nz; }
                    changed=true;
                    surface_changed=true;
                }
            }
            // 00476eb0 clears root positions and normals; no weight means no added position.
            for (auto k=by_mesh.clear_begin[mesh_index]; k<by_mesh.clear_begin[mesh_index+1]; ++k) {
                const auto& surface=scene.source_surfaces[by_mesh.clear[k][0]];
                auto& vertex=source_corner(scene,surface,by_mesh.clear[k][1]);
                const bool clear_normal=!surface.recompute_normals;
                if (vertex.x!=0 || vertex.y!=0 || vertex.z!=0 ||
                    (clear_normal && (vertex.nx!=0 || vertex.ny!=0 || vertex.nz!=0))) {
                    positions_changed |= vertex.x!=0 || vertex.y!=0 || vertex.z!=0;
                    vertex.x=0; vertex.y=0; vertex.z=0;
                    // Regenerated normals clear their source scratch, avoiding intermediate GPU dirtiness.
                    if (clear_normal) { vertex.nx=0; vertex.ny=0; vertex.nz=0; }
                    changed=true;
                    surface_changed=true;
                }
            }
            if (surface_changed && !mesh.indices.empty()) {
                for (auto& vertex : mesh.vertices) vertex.tangent={0,0,0};
                for (std::size_t triangle=0; triangle+2<mesh.indices.size(); triangle+=3) {
                    const auto ia=mesh.indices[triangle], ib=mesh.indices[triangle+1], ic=mesh.indices[triangle+2];
                    const auto& a=mesh.vertices[ia];
                    const auto& b=mesh.vertices[ib];
                    const auto& c=mesh.vertices[ic];
                    const float du1=b.u-a.u, dv1=b.v-a.v, du2=c.u-a.u, dv2=c.v-a.v;
                    const float determinant=du1*dv2-du2*dv1;
                    if (std::abs(determinant)<=0.000001f) continue;
                    const float inverse=1/determinant;
                    const std::array<float,3> tangent{
                        ((b.x-a.x)*dv2-(c.x-a.x)*dv1)*inverse,
                        ((b.y-a.y)*dv2-(c.y-a.y)*dv1)*inverse,
                        ((b.z-a.z)*dv2-(c.z-a.z)*dv1)*inverse};
                    for (const auto index : {ia,ib,ic})
                        for (unsigned axis=0; axis<3; ++axis) mesh.vertices[index].tangent[axis]+=tangent[axis];
                }
                for (auto& vertex : mesh.vertices) {
                    auto& tangent=vertex.tangent;
                    const float length_squared=tangent[0]*tangent[0]+tangent[1]*tangent[1]+tangent[2]*tangent[2];
                    if (length_squared>0) {
                        const float inverse_length=1/std::sqrt(length_squared);
                        for (auto& component : tangent) component*=inverse_length;
                    }
                }
            }
            if (surface_changed) ++mesh.revision;
            if (positions_changed)
                for (auto k=by_mesh.touch_begin[mesh_index]; k<by_mesh.touch_begin[mesh_index+1]; ++k)
                    scene.source_surfaces[by_mesh.touch[k]].positions_changed=true;
        }
        for (auto& surface : scene.source_surfaces)
            if (surface.recompute_normals &&
                (!surface.normals_initialized || scene.geometry_revision==0 || surface.positions_changed)) {
                update_source_normals(scene,surface,changed);
                surface.normals_initialized=true;
            }
    }
    if (changed) ++scene.geometry_revision;
}
} // namespace ikv
namespace ikv {
void scene_animation_self_test() {
    const auto require = [](bool condition) {
        if (!condition) throw std::runtime_error("scene animation self-test failed");
    };
    const auto near = [](float a, float b) { return std::abs(a-b) < 0.0001f; };
    {
        Scene actions;actions.nodes.resize(1);actions.node_order={0};actions.actor_tables.libraries.resize(1);
        actions.actor_tables.libraries[0].clips={{2,2,0,"Rest",0,20,0,20,true},{1,2,0,"Move",4,6,4,6,true},{27,2,0,"Entry",10,11,10,11,false}};
        actions.actors.push_back({0});SceneActorAssembly assembly;assembly.entity_id=91;assembly.controller=0;actions.actor_assemblies.push_back(assembly);
        actions.animation.push_back({0,{{0,{0,0,0,1},{}},{20,{0,0,0,1},{20,0,0}}},0});
        set_actor_action(actions,91,27,0);update_scene(actions,0);require(near(actions.nodes[0].local.matrix[12],10));
        update_scene(actions,1);require(actions.actors[0].current_clip==0&&actions.actors[0].time==0);
        update_scene(actions,1.1f);set_actor_action(actions,91,1,.25f);update_scene(actions,1.225f);
        require(near(actions.actors[0].blend_from_time,.225f)&&near(actions.actors[0].time,4.125f)&&near(actions.nodes[0].local.matrix[12],2.175f));
        const auto elapsed=actions.actors[0].action_elapsed;set_actor_action(actions,91,1,.25f);require(actions.actors[0].action_elapsed==elapsed);
        update_scene(actions,1.4f);require(actions.actors[0].blend_duration==0&&near(actions.nodes[0].local.matrix[12],4.3f)&&actions.actors.size()==1);
        const auto live_time=actions.actors[0].time;set_actor_action(actions,91,1,.25f);require(actions.actors[0].time==live_time);
        set_actor_action(actions,91,1,.25f,true);require(actions.actors[0].time==4&&actions.actors[0].action_elapsed==0&&actions.actors[0].blend_from_time==live_time&&actions.actors.size()==1);
        update_scene(actions,9.4f);require(actions.actors[0].current_clip==1&&actions.actors[0].action_held); // Action-requested loop never takes its jump.
        Instance instance;instance.entity_id=91;instance.node=0;instance.transform.matrix[12]=2;actions.nodes[0].source_bounds_center={2,3,4};
        const auto placed=entity_instance_transform(actions,instance,5,7,11,0);
        const auto center=entity_source_bounds_center(actions,instance,5,7,11,0);
        require(placed.matrix[12]==7&&placed.matrix[13]==7&&placed.matrix[14]==11&&center==std::array<float,3>{7,10,15});
    }
    {
        // Terminal admission restores the owned sparse pose, then uses the ordinary real clock.
        constexpr float frame_seconds=0.033333335f, real_seconds=80;
        const float start=1155*frame_seconds, end=1300*frame_seconds, duration=end-start;
        Scene corpse;corpse.nodes.resize(4);corpse.node_order={0,1,2,3};
        corpse.actor_tables.libraries.resize(2);
        corpse.actor_tables.libraries[0].clips={
            {2,2,0,"Startup",0,10,0,10,true},
            {28,28,1300,"Death",start,end,start,end,false},
            {62,11,490,"Resurrection",440*frame_seconds,490*frame_seconds,440*frame_seconds,490*frame_seconds,false},
            {11,11,490,"Rest",490*frame_seconds,start,490*frame_seconds,start,true}};
        corpse.actor_tables.libraries[1].clips={{2,2,0,"Unrelated",0,200,0,200,true}};
        corpse.actors={{0},{1}};
        SceneActorAssembly assembly;assembly.entity_id=91;assembly.controller=0;
        corpse.actor_assemblies.push_back(assembly);
        assembly.entity_id=92;assembly.controller=1;corpse.actor_assemblies.push_back(assembly);
        corpse.animation={
            {0,{{0,{0,0,0,1},{100,0,0}},{start,{0,0,0,1},{}},{end,{0,0,0,1},{8,0,0}}},0},
            {1,{{0,{0,0,0,1},{100,0,0}},{start,{0,0,0,1},{}},{(start+end)/2,{0,0,0,1},{4,0,0}}},0},
            {2,{{0,{0,0,0,1},{37,0,0}},{1,{0,0,0,1},{41,0,0}}},0},
            {3,{{0,{0,0,0,1},{}},{200,{0,0,0,1},{200,0,0}}},1}};
        corpse.meshes.resize(1);corpse.meshes[0].vertices={{99,0,0,0,1,0,0,0}};
        corpse.meshes[0].influences={{0,0,.5f,{},{0,1,0}},{0,1,.5f,{},{0,1,0}}};
        update_scene(corpse,real_seconds);require(near(corpse.meshes[0].vertices[0].x,100));
        Scene ordinary=corpse;set_actor_action(ordinary,91,28);
        require(ordinary.actors[0].time==start&&ordinary.actors[0].blend_duration==.25f);
        update_scene(ordinary,real_seconds+.125f);
        require(near(ordinary.actors[0].time,start+.125f)&&ordinary.actors[0].action_elapsed==.125f);
        const auto held_pose=corpse.animation[2].cached_pose;
        const auto held_local=corpse.nodes[2].local.matrix;
        const auto other=corpse.actors[1];
        set_actor_action(corpse,91,28); // Also exercise terminal seeking the already-current numeric action.
        set_actor_action(corpse,91,28,.25f,false,true);
        const auto& actor=corpse.actors[0];
        require(actor.time==end&&actor.action_elapsed==0&&actor.blend_duration==0&&
                actor.blend_elapsed==0&&actor.blend_from_time==0&&actor.blend_from_clip==UINT32_MAX&&
                actor.previous_seconds==real_seconds&&actor.initialized&&!actor.stopped);
        require(corpse.animation_seconds==real_seconds&&corpse.actors[1].time==other.time&&
                corpse.actors[1].action_elapsed==other.action_elapsed&&corpse.actors[1].previous_seconds==other.previous_seconds);
        require(corpse.animation[2].cached_pose.time==held_pose.time&&
                corpse.animation[2].cached_pose.position==held_pose.position&&
                corpse.animation[2].cached_pose.rotation==held_pose.rotation&&corpse.nodes[2].local.matrix==held_local);
        update_scene(corpse,real_seconds);
        require(near(corpse.meshes[0].vertices[0].x,6)&&actor.time==end&&actor.action_elapsed==0&&
                corpse.actors[1].time==other.time&&corpse.nodes[3].local.matrix[12]==0);
        float seconds=real_seconds+.125f;update_scene(corpse,seconds);
        require(actor.current_clip==1&&near(corpse.meshes[0].vertices[0].x,6)&&!actor.stopped);
        seconds=real_seconds+2*duration;update_scene(corpse,seconds);
        require(actor.current_clip==1&&actor.time==end&&near(corpse.meshes[0].vertices[0].x,6)&&!actor.stopped);
        const auto from_a=corpse.animation[0].cached_pose, from_b=corpse.animation[1].cached_pose;
        set_actor_action(corpse,91,62);
        require(actor.blend_duration==.25f&&actor.blend_elapsed==0&&actor.previous_seconds==seconds);
        seconds+=.125f;update_scene(corpse,seconds);
        const float expected=.5f*(interpolate_key(from_a,sample_key(corpse.animation[0],actor.time),.5f).position[0]+
                                  interpolate_key(from_b,sample_key(corpse.animation[1],actor.time),.5f).position[0]);
        require(near(actor.time,440*frame_seconds+.125f)&&actor.action_elapsed==.125f&&
                actor.blend_elapsed==.125f&&actor.blend_duration==.25f&&near(corpse.meshes[0].vertices[0].x,expected));
        seconds+=50*frame_seconds;update_scene(corpse,seconds);
        require(actor.current_clip==3&&actor.time==490*frame_seconds&&actor.action_elapsed==0&&!actor.stopped);
    }
    Scene scene;
    scene.nodes.resize(3);
    scene.nodes[2].local.matrix[12] = 10;
    scene.nodes[0].parent = 2;
    scene.nodes[1].parent = 2;
    scene.nodes[1].local.matrix[12] = 3;
    scene.node_order = {2,0,1};
    AnimationTrack track;
    track.instance = 0;
    track.keys = {{0,{0,0,0,1},{0,0,0}}, {1,{0,-1,0,0},{2,0,0}}};
    scene.animation.push_back(track);
    Mesh mesh;
    mesh.vertices = {{99,99,99,1,0,0,0,0}, {99,99,99,1,0,0,0,0}, {7,8,9,0,0,1,0,0}};
    mesh.influences = {{0,0,0.75f,{2,0,0},{1,0,0}}, {0,1,0.25f,{0,2,0},{1,0,0}},
                       {1,1,0.5f,{0,2,0},{1,0,0}}};
    scene.meshes.push_back(mesh);
    scene.meshes.emplace_back();
    scene.instances.push_back({0,{},"skin",0,2});
    scene.instances.push_back({1,{},"rigid",0,0});

    update_scene(scene,0.5f);
    const auto& local = scene.nodes[0].local.matrix;
    require(near(local[12],1) && near(local[0],0) && near(local[1],1));
    require(near(scene.nodes[0].world.matrix[12],11));
    require(near(scene.instances[0].transform.matrix[12],0));
    require(near(scene.instances[1].transform.matrix[12],11));
    const auto& vertex = scene.meshes[0].vertices[0];
    require(near(vertex.x,11.5f) && near(vertex.y,2) && near(vertex.z,0));
    require(near(vertex.nx,1/std::sqrt(10.0f)) && near(vertex.ny,3/std::sqrt(10.0f)) && near(vertex.nz,0));
    require(near(scene.meshes[0].vertices[1].x,6.5f) && near(scene.meshes[0].vertices[1].y,1));
    require(scene.meshes[0].vertices[2].x==7 && scene.meshes[0].vertices[2].y==8 && scene.meshes[0].vertices[2].z==9);
    const auto revision = scene.geometry_revision;
    require(revision>0);
    const auto mesh_revision=scene.meshes[0].revision;
    require(mesh_revision>0 && scene.meshes[1].revision==0);
    update_scene(scene,0.5f);
    require(scene.geometry_revision==revision);
    require(scene.meshes[0].revision==mesh_revision && scene.meshes[1].revision==0);
    update_scene(scene,0.25f);
    require(near(scene.nodes[0].local.matrix[0],std::sqrt(0.5f)) &&
            near(scene.nodes[0].local.matrix[1],std::sqrt(0.5f)));
    const auto cached_revision=scene.geometry_revision;
    update_scene(scene,2);
    require(near(scene.nodes[0].local.matrix[12],0.5f) &&
            near(scene.nodes[0].local.matrix[0],std::sqrt(0.5f)));
    require(scene.geometry_revision==cached_revision);
    update_scene(scene,-1);
    require(near(scene.nodes[0].local.matrix[12],-2));
    update_scene(scene,2);
    require(near(scene.nodes[0].local.matrix[12],-2) && near(scene.nodes[0].local.matrix[0],-1));
    require(scene.geometry_revision>revision);
    require(scene.meshes[0].revision>mesh_revision && scene.meshes[1].revision==0);
    // skin_held (original 00476eb0 skips roots whose +0xf4 frustum flag is clear): bones advance, the skin waits.
    scene.actor_assemblies.emplace_back();
    scene.actor_assemblies[0].entity_id=1; scene.actor_assemblies[0].end[0]=1; scene.actor_assemblies[0].skin_held=true;
    const auto held_revision=scene.meshes[0].revision;
    update_scene(scene,0.5f);
    require(near(scene.nodes[0].world.matrix[12],11) && !near(vertex.x,11.5f) &&
            scene.meshes[0].revision==held_revision && scene.actor_assemblies[0].skin_stale);
    scene.actor_assemblies[0].skin_held=false;
    update_scene(scene,0.5f); // no pose change: the release alone re-skins
    require(near(vertex.x,11.5f) && scene.meshes[0].revision>held_revision && !scene.actor_assemblies[0].skin_stale);
    scene.actor_assemblies.clear();

    Scene zero_time;
    zero_time.nodes.resize(1);
    zero_time.node_order={0};
    zero_time.animation.push_back({0,{{0,{0,0,0,1},{3,4,5}}, {0,{0,-1,0,0},{6,7,8}}}});
    update_scene(zero_time,0);
    require(zero_time.nodes[0].local.matrix==Transform{}.matrix);
    const auto zero_revision=zero_time.geometry_revision;
    update_scene(zero_time,10);
    require(zero_time.nodes[0].local.matrix==Transform{}.matrix && zero_time.geometry_revision==zero_revision);

    Scene before_first;
    before_first.nodes.resize(1);
    before_first.node_order={0};
    before_first.animation.push_back({0,{{1,{0,0,0,1},{3,4,5}},
                                        {2,{0,-std::sqrt(0.5f),0,std::sqrt(0.5f)},{5,8,11}}}});
    update_scene(before_first,0);
    require(near(before_first.nodes[0].local.matrix[12],1) &&
            near(before_first.nodes[0].local.matrix[13],0) &&
            near(before_first.nodes[0].local.matrix[14],-1));
    require(near(before_first.nodes[0].local.matrix[1],-1));

    Scene shortest_arc;
    shortest_arc.nodes.resize(1);
    shortest_arc.node_order = {0};
    AnimationTrack equivalent;
    equivalent.keys = {{0,{0,0,0,1},{}}, {1,{0,0,0,-1},{2,0,0}}};
    shortest_arc.animation.push_back(equivalent);
    update_scene(shortest_arc,0.5f);
    require(near(shortest_arc.nodes[0].local.matrix[0],1) && near(shortest_arc.nodes[0].local.matrix[12],1));
    shortest_arc.animation[0].keys = {{0,{0,0,std::sqrt(0.5f),std::sqrt(0.5f)},{}}};
    update_scene(shortest_arc,0);
    require(near(shortest_arc.nodes[0].local.matrix[2],1) &&
            near(shortest_arc.nodes[0].local.matrix[8],-1));

    Scene initial_skin;
    initial_skin.nodes.resize(1);
    initial_skin.node_order = {0};
    initial_skin.meshes.emplace_back();
    initial_skin.meshes[0].vertices = {{99,99,99,0,0,1,0,0}, {99,99,99,0,0,1,1,0}, {99,99,99,0,0,1,0,1}};
    initial_skin.meshes[0].indices = {0,1,2};
    initial_skin.meshes[0].influences = {{0,0,1,{1,2,3},{0,0,1}}, {1,0,1,{3,2,3},{0,0,1}}, {2,0,1,{1,4,3},{0,0,1}}};
    update_scene(initial_skin,0);
    require(initial_skin.meshes[0].vertices[0].x==1 &&
            initial_skin.meshes[0].vertices[0].y==2 &&
            initial_skin.meshes[0].vertices[0].z==3 && initial_skin.geometry_revision>0);
    require(near(initial_skin.meshes[0].vertices[0].tangent[0],1) &&
            near(initial_skin.meshes[0].vertices[0].tangent[1],0) &&
            near(initial_skin.meshes[0].vertices[0].tangent[2],0));
    initial_skin.animation.push_back({0,{{0,{0,0,0,1},{}}, {1,{0,-1,0,0},{}}}});
    update_scene(initial_skin,0.5f);
    require(near(initial_skin.meshes[0].vertices[0].tangent[0],0) &&
            near(initial_skin.meshes[0].vertices[0].tangent[1],1));

    // 0047cf50 accumulates aliases once per index occurrence, before copying.
    Scene source;
    source.nodes.resize(2);
    source.node_order={0,1};
    source.meshes.resize(2);
    source.meshes[0].vertices={{9,8,7,1,0,0,0,0}, {0,1,0,0,0,0,0,1},
                              {0,0,1,0,0,0,1,1}, {0,1,0,0,0,0,1,0}};
    source.meshes[1].vertices={{1,0,0,0,0,0,1,0}, {9,8,7,1,0,0,0,0}};
    source.meshes[0].vertices[0].nx=std::nanf("");
    source.meshes[1].vertices[1].ambient={0.25f,0.5f,0.75f,1};
    source.meshes[0].influences={{1,0,1,{0,1,0},{}}, {2,1,1,{0,0,1},{}},
                                {3,0,1,{0,1,0},{}}};
    source.meshes[1].influences={{0,0,1,{1,0,0},{}}};
    SourceSurface surface;
    surface.heads={0,1,2,3,UINT32_MAX};
    surface.corners={{0,0,5}, {1,0,UINT32_MAX}, {0,1,4}, {0,2,UINT32_MAX},
                     {0,3,UINT32_MAX}, {1,1,UINT32_MAX}};
    surface.unweighted_corners={0,5};
    surface.indices={2,1,0,2,3,0}; // Reversed source triangles {0,1,2}, {0,3,2}.
    surface.aliases={{{0,0}},{{1,1}},{{1,2}},{{0,2}},{{4,4}}};
    surface.normals.resize(surface.heads.size());
    surface.recompute_normals=true;
    surface.root_clears=true;
    source.source_surfaces.push_back(std::move(surface));
    update_scene(source,0);
    require(source.meshes[0].vertices[0].x==0 && source.meshes[0].vertices[0].y==0 &&
            source.meshes[0].vertices[0].z==0 && source.meshes[1].vertices[1].x==0);
    const auto& shared=source.meshes[1].vertices[0];
    require(shared.nx==-2 && shared.ny==0 && shared.nz==3);
    require(near(source.meshes[0].vertices[1].nx,shared.nx) &&
            near(source.meshes[0].vertices[3].nz,shared.nz));
    require(source.meshes[0].vertices[2].nx==-1 && source.meshes[0].vertices[2].nz==0);
    require(source.meshes[0].vertices[0].nx==source.meshes[1].vertices[1].nx &&
            source.meshes[0].vertices[0].nz==source.meshes[1].vertices[1].nz);
    require(source.meshes[1].vertices[1].ambient==std::array<float,4>{0.25f,0.5f,0.75f,1});
    require(source.source_surfaces[0].normals[4]==std::array<float,3>{});
    const auto source_revision=source.geometry_revision;
    const auto normal_only_revision=source.meshes[1].revision;
    update_scene(source,0);
    require(source.geometry_revision==source_revision &&
            source.meshes[1].revision==normal_only_revision);
    source.nodes[1].local.matrix[14]=1;
    update_scene(source,0);
    require(source.meshes[0].vertices[2].z==2 && source.meshes[1].vertices[0].x==1);
    require(shared.nx==-4 && shared.nz==3 && source.meshes[1].revision>normal_only_revision);
    // A pose change outside this surface must not dirty unchanged final normals.
    source.nodes.emplace_back();
    source.node_order.push_back(2);
    source.nodes[2].local.matrix[12]=1;
    const auto unchanged_mesh_revision=source.meshes[1].revision;
    update_scene(source,0);
    require(source.meshes[1].revision==unchanged_mesh_revision);
    const auto rejects_source=[&] {
        source.geometry_revision=0;
        bool rejected=false;
        try { update_scene(source,0); } catch (const std::runtime_error&) { rejected=true; }
        require(rejected);
    };
    source.source_surfaces[0].aliases[2][0]=5;
    rejects_source();
    source.source_surfaces[0].aliases[2][0]=1;
    source.source_surfaces[0].indices[0]=5;
    rejects_source();
    source.source_surfaces[0].indices[0]=2;
    source.source_surfaces[0].corners[4].next=2;
    rejects_source();
    source.source_surfaces[0].corners[4].next=UINT32_MAX;
    source.source_surfaces[0].corners[5].mesh=2;
    rejects_source();

    Scene cleared;
    cleared.meshes.emplace_back();
    cleared.meshes[0].vertices={{4,5,6,1,2,3,0,0}};
    SourceSurface clear_surface;
    clear_surface.corners={{0,0,UINT32_MAX}};
    clear_surface.unweighted_corners={0};
    clear_surface.root_clears=true;
    cleared.source_surfaces.push_back(std::move(clear_surface));
    update_scene(cleared,0);
    const auto& cleared_vertex=cleared.meshes[0].vertices[0];
    require(cleared_vertex.x==0 && cleared_vertex.y==0 && cleared_vertex.z==0 &&
            cleared_vertex.nx==0 && cleared_vertex.ny==0 && cleared_vertex.nz==0);

    // Type 4 does not clear authored normals without an actual root-bone clear.
    Scene no_root;
    no_root.nodes.resize(1);
    no_root.node_order={0};
    no_root.meshes.emplace_back();
    no_root.meshes[0].vertices={{0,0,0,1,0,0,0,0}, {1,0,0,1,0,0,1,0},
                               {0,1,0,1,0,0,0,1}};
    SourceSurface authored;
    authored.heads={0,1,2};
    authored.corners={{0,0,UINT32_MAX}, {0,1,UINT32_MAX}, {0,2,UINT32_MAX}};
    authored.indices={2,1,0};
    authored.aliases={{{0,0}},{{1,1}},{{2,2}}};
    authored.normals={{1,0,0},{1,0,0},{1,0,0}};
    authored.recompute_normals=true;
    no_root.source_surfaces.push_back(std::move(authored));
    update_scene(no_root,0);
    const auto& authored_vertex=no_root.meshes[0].vertices[0];
    require(authored_vertex.nx==1 && authored_vertex.nz==1);
    const auto static_normal_revision=no_root.meshes[0].revision;
    no_root.nodes.emplace_back();
    no_root.node_order.push_back(1);
    no_root.nodes[1].local.matrix[12]=1;
    update_scene(no_root,0);
    require(authored_vertex.nx==1 && authored_vertex.nz==1 &&
            no_root.meshes[0].revision==static_normal_revision);
    require(no_root.source_surfaces[0].normals[0]==std::array<float,3>{1,0,1} &&
            !no_root.source_surfaces[0].positions_changed);
    no_root.meshes[0].influences={{0,0,1,{0,0,0},{}}, {1,0,1,{1,0,0},{}}, {2,0,1,{0,1,0},{}}};
    no_root.nodes[0].local.matrix[12]=1;
    update_scene(no_root,0);
    require(no_root.source_surfaces[0].normals[0]==std::array<float,3>{1,0,2} &&
            no_root.source_surfaces[0].positions_changed);
    require(authored_vertex.nx==1 && authored_vertex.nz==2);

    Scene bounds;
    bounds.nodes.resize(4);
    bounds.node_order={0,1,2,3};
    auto& root=bounds.nodes[0].local.matrix;
    root[0]=0; root[1]=1; root[4]=-1; root[5]=0;
    root[12]=10; root[13]=20;
    bounds.nodes[1].parent=0;
    bounds.nodes[1].local.matrix[12]=2;
    bounds.nodes[1].local.matrix[13]=3;
    bounds.nodes[2].parent=0;
    bounds.nodes[2].source_reference=1;
    bounds.nodes[2].local.matrix[12]=99;
    bounds.nodes[2].local.matrix[13]=99;
    bounds.nodes[3].parent=2;
    bounds.nodes[3].local.matrix[12]=1;
    bounds.nodes[3].local.matrix[13]=2;
    update_scene(bounds,0);
    require(bounds.nodes[1].source_bounds_center==std::array<float,3>{12,23,0});
    require(near(bounds.nodes[1].world.matrix[12],7) && near(bounds.nodes[1].world.matrix[13],22));
    require(bounds.nodes[2].source_bounds_center==bounds.nodes[1].source_bounds_center);
    require(bounds.nodes[3].source_bounds_center==std::array<float,3>{13,25,0});
    const auto bounds_revision=bounds.geometry_revision;
    update_scene(bounds,0);
    require(bounds.geometry_revision==bounds_revision);
    bounds.nodes[2].source_reference=0;
    update_scene(bounds,0);
    require(bounds.nodes[2].source_bounds_center==std::array<float,3>{10,20,0} &&
            bounds.nodes[3].source_bounds_center==std::array<float,3>{11,22,0});
    require(bounds.geometry_revision>bounds_revision);

    {
        // Generic metadata clocks: attach-local startup, one jump, and source loop modulo.
        constexpr float frame_seconds=0.033333335f;
        Scene clocks;
        clocks.nodes.resize(6);
        clocks.node_order={0,1,2,3,4,5};
        clocks.actor_tables.libraries.resize(3);
        clocks.actor_tables.libraries[0].clips={
            {0,0,0,"Unused default",0,1,0,1,false},
            {1,1,10,"Loop",10*frame_seconds,10*frame_seconds+10,1,2,true},
            {2,1,10,"Startup",2.5f,4.5f,0,4.5f,false}};
        clocks.actor_tables.libraries[1].clips={{1,1,0,"No startup",0,1,0,1,false}};
        clocks.actors.push_back({0});
        clocks.actors.push_back({1});
        clocks.actors.push_back({});
        clocks.actors.push_back({2});
        clocks.actors.push_back({0});
        clocks.actors.back().stopped=true;
        for (std::uint32_t i=0; i<5; ++i) {
            clocks.nodes[i].local.matrix[12]=42;
            clocks.animation.push_back({i,{{0,{0,0,0,1},{}}, {20,{0,0,0,1},{20,0,0}}},i});
        }
        clocks.animation.push_back({5,{{0,{0,0,0,1},{}}, {200,{0,0,0,1},{200,0,0}}}});
        update_scene(clocks,100);
        require(clocks.actors[0].current_clip==2 && clocks.actors[0].time==2.5f &&
                clocks.actors[0].action_elapsed==0 && near(clocks.nodes[0].local.matrix[12],2.5f));
        require(clocks.actors[1].stopped && !clocks.diagnostics.empty());
        for (unsigned i=1; i<5; ++i) require(clocks.nodes[i].local.matrix[12]==42);
        require(near(clocks.nodes[5].local.matrix[12],100));
        update_scene(clocks,101);
        require(clocks.actors[0].time==3.5f && clocks.actors[0].action_elapsed==1);
        clocks.nodes.emplace_back();
        clocks.node_order.push_back(6);
        clocks.actors.push_back({0});
        clocks.animation.push_back({6,clocks.animation[0].keys,5});
        update_scene(clocks,101);
        require(clocks.actors[0].time==3.5f && clocks.actors[5].time==2.5f);
        update_scene(clocks,102);
        require(clocks.actors[0].current_clip==1 && clocks.actors[0].time==10*frame_seconds &&
                clocks.actors[0].action_elapsed==0 && clocks.actors[5].time==3.5f);
        update_scene(clocks,103);
        require(near(clocks.actors[0].time,10*frame_seconds+1) &&
                clocks.actors[5].time==10*frame_seconds && clocks.actors[5].action_elapsed==0);
        update_scene(clocks,104);
        require(near(clocks.actors[0].time,10*frame_seconds+1) && clocks.actors[0].action_elapsed==2);
        const auto held=clocks.nodes[0].local.matrix;
        update_scene(clocks,103.5f);
        require(clocks.actors[0].action_elapsed==2 && clocks.actors[0].previous_seconds==103.5f &&
                clocks.nodes[0].local.matrix==held);
        update_scene(clocks,104);
        require(near(clocks.actors[0].time,10*frame_seconds+1.5f) &&
                clocks.actors[0].action_elapsed==2.5f);
        update_scene(clocks,124);
        require(clocks.actors[0].time==10*frame_seconds && clocks.actors[0].action_elapsed==0 &&
                clocks.actors[5].time==10*frame_seconds && clocks.actors[5].action_elapsed==0);

        Scene clamped;
        clamped.nodes.resize(1);
        clamped.node_order={0};
        clamped.actor_tables.libraries.resize(1);
        clamped.actor_tables.libraries[0].clips={
            {2,3,270,"Startup",0,1,0,1,false},
            {3,99,0,"Nonloop",0,10,0,10,false}};
        clamped.actors.push_back({0});
        clamped.animation.push_back({0,{{0,{0,0,0,1},{}}, {10,{0,0,0,1},{10,0,0}}},0});
        update_scene(clamped,200);
        update_scene(clamped,201);
        require(clamped.actors[0].current_clip==1 && clamped.actors[0].time==270*frame_seconds);
        const auto before_end=clamped.nodes[0].local.matrix;
        update_scene(clamped,203);
        require(clamped.actors[0].time==10 && clamped.actors[0].action_elapsed==2 &&
                clamped.nodes[0].local.matrix==before_end);
        update_scene(clamped,211);
        require(clamped.actors[0].stopped && !clamped.diagnostics.empty() &&
                clamped.nodes[0].local.matrix==before_end);
        const auto diagnostics=clamped.diagnostics.size();
        update_scene(clamped,220);
        require(clamped.nodes[0].local.matrix==before_end && clamped.diagnostics.size()==diagnostics);

        Scene equal_loop;
        equal_loop.actor_tables.libraries.resize(1);
        equal_loop.actor_tables.libraries[0].clips={{2,2,0,"Equal loop",0,10,1,1,true}};
        equal_loop.actors.push_back({0});
        update_scene(equal_loop,0);
        update_scene(equal_loop,1.5f);
        require(equal_loop.actors[0].time==1.5f && equal_loop.actors[0].action_elapsed==1.5f);
        equal_loop.actors[0].rate=2;update_scene(equal_loop,2.f); // Locomotion stride rate scales the clip clock only.
        require(equal_loop.actors[0].action_elapsed==2.5f && equal_loop.actors[0].previous_seconds==2.f);

        // 0045c860 floors generic action duration at one second, independently of the cursor cap.
        Scene durations;
        durations.nodes.resize(3);
        durations.node_order={0,1,2};
        durations.actor_tables.libraries.resize(3);
        durations.actor_tables.libraries[0].clips={
            {2,1,0,"Short startup",0,frame_seconds,0,frame_seconds,false},
            {1,1,0,"Next",0,10,0,10,false}};
        durations.actor_tables.libraries[1].clips={
            {2,1,0,"Zero span",0.5f,0.5f,0,0.5f,false},
            {1,1,0,"Next",0,10,0,10,false}};
        durations.actor_tables.libraries[2].clips={
            {2,1,0,"Separate cursor cap",0,2,0,1,false},
            {1,1,0,"Next",0,10,0,10,false}};
        for (std::uint32_t i=0; i<3; ++i) {
            durations.actors.push_back({i});
            durations.animation.push_back({i,{{0,{0,0,0,1},{}}, {3,{0,0,0,1},{3,0,0}}},i});
        }
        update_scene(durations,0);
        update_scene(durations,0.5f);
        require(durations.actors[0].current_clip==0 && durations.actors[0].time==frame_seconds &&
                near(durations.nodes[0].local.matrix[12],frame_seconds));
        require(durations.actors[1].current_clip==0 && durations.actors[1].time==0.5f &&
                near(durations.nodes[1].local.matrix[12],0.5f));
        update_scene(durations,1);
        require(durations.actors[0].current_clip==1 && durations.actors[0].time==0 &&
                durations.actors[0].action_elapsed==0);
        require(durations.actors[1].current_clip==1 && durations.actors[1].time==0 &&
                durations.actors[1].action_elapsed==0);
        require(durations.actors[2].current_clip==0 && durations.actors[2].time==1 &&
                durations.actors[2].action_elapsed==1);
        update_scene(durations,1.5f);
        require(durations.actors[2].current_clip==0 && durations.actors[2].time==1 &&
                durations.actors[2].action_elapsed==1.5f && near(durations.nodes[2].local.matrix[12],1));
        update_scene(durations,2);
        require(durations.actors[2].current_clip==1 && durations.actors[2].time==0 &&
                durations.actors[2].action_elapsed==0);
    }
}
} // namespace ikv
