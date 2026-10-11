#include "ikv/scene.hpp"
#include "scene_source_sort.hpp"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <bit>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace ikv {
namespace {
using Bytes=std::span<const std::uint8_t>;
void need(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
std::uint32_t word(Bytes b,std::size_t p) { need(p<=b.size()&&b.size()-p>=4,"ADF truncated word");return b[p]|(std::uint32_t(b[p+1])<<8)|(std::uint32_t(b[p+2])<<16)|(std::uint32_t(b[p+3])<<24); }
float scalar(Bytes b,std::size_t p) { auto f=std::bit_cast<float>(word(b,p));need(std::isfinite(f),"ADF nonfinite scalar");return f; }
std::string text(Bytes b,std::size_t p,std::size_t n) { need(p<=b.size()&&n<=b.size()-p,"ADF truncated string");auto first=b.begin()+p;auto end=std::find(first,first+n,0);need(end!=first+n,"ADF unterminated string");return {first,end}; }
std::string lower(std::string s) { for(auto& c:s) { if(c=='\\')c='/';if(c>='A'&&c<='Z')c+=32; }return s; }
struct MaterialResources { std::set<std::string> textures;bool lightmaps=false; };
MaterialResources material_resources(const Scene& scene,const std::vector<std::uint8_t>& usage) {
 MaterialResources wanted;
 for(std::size_t i=0;i<scene.materials.size();++i)if(usage[i]){
  const auto& material=scene.materials[i];
  for(const auto* name:{&material.diffuse,&material.normal,&material.environment,&material.luminance})if(!name->empty())wanted.textures.insert(lower(*name));
  wanted.lightmaps|=!material.lightmap.empty();
 }
 return wanted;
}
void normalize_loaded_bindings(Scene& scene,const std::vector<std::uint8_t>& usage,const Scene* resident=nullptr) {
 std::vector<std::string_view> loaded;loaded.reserve(scene.textures.size()+(resident?resident->textures.size():0));for(const auto& texture:scene.textures)loaded.push_back(texture.name);if(resident)for(const auto& texture:resident->textures)loaded.push_back(texture.name);std::sort(loaded.begin(),loaded.end());
 const auto bound=[&](std::string& name){if(name.empty())return;name=lower(std::move(name));need(std::binary_search(loaded.begin(),loaded.end(),std::string_view(name)),"referenced decoded texture binding missing");};
 for(std::size_t i=0;i<scene.materials.size();++i)if(usage[i]){
  auto& material=scene.materials[i];for(auto* name:{&material.diffuse,&material.normal,&material.environment,&material.luminance})bound(*name);
  need(material.lightmap.empty()||std::binary_search(loaded.begin(),loaded.end(),std::string_view(material.lightmap)),"referenced decoded lightmap binding missing");
 }
 std::vector<std::uint8_t> active_meshes(scene.meshes.size());
 if(scene.instances.empty()){for(std::uint32_t i=0;i<scene.meshes.size();++i){need(scene.meshes[i].material<scene.materials.size(),"scene referenced material out of range");if(scene_source_color_draw(scene,i)&&usage[scene.meshes[i].material])active_meshes[i]=1;}}
 else for(const auto& instance:scene.instances){need(instance.mesh<scene.meshes.size(),"scene referenced mesh out of range");const auto material=instance.material==UINT32_MAX?scene.meshes[instance.mesh].material:instance.material;need(material<scene.materials.size(),"scene referenced material out of range");if(scene_source_color_draw(scene,instance.mesh,instance.material,instance.source_ordinary_color_participates&&!instance.occlusion_proxy)&&usage[material]&&(scene.materials[material].diffuse.empty()||lower(scene.meshes[instance.mesh].texture)==scene.materials[material].diffuse))active_meshes[instance.mesh]=1;}
 for(std::size_t i=0;i<scene.meshes.size();++i)if(active_meshes[i])bound(scene.meshes[i].texture);
}
Texture decoded_texture(std::string name,const std::string& archive_name,const assets::Entry& entry,assets::DdsImage image) {
 return {std::move(name),image.width,image.height,std::move(image.rgba),std::move(image.mips),archive_name+":"+entry.path,image.format};
}
std::filesystem::path base_path(const std::filesystem::path& root) {
 for(const auto& p:std::array<std::filesystem::path,3>{root,root/"base",root/"app"/"base"})if(std::filesystem::is_regular_file(p/"objects.dat"))return p;
 throw std::runtime_error("asset root has no original objects.dat (base contents, app or installation required)");
}
std::vector<std::uint8_t> file_bytes(const std::filesystem::path& path,std::uint64_t maximum=assets::max_entry_bytes) {
 std::ifstream f(path,std::ios::binary);need(bool(f),"cannot open original asset read-only");auto n=std::filesystem::file_size(path);need(n<=maximum&&n<=UINT32_MAX,"asset file exceeds budget");return assets::read_entry(f,{path.filename().string(),0,std::uint32_t(n)},maximum);
}
struct Pack {
 std::ifstream stream;assets::Archive archive;std::unordered_map<std::string,std::size_t> entries;std::string source_name;
 explicit Pack(const std::filesystem::path& p):stream(p,std::ios::binary),source_name(p.filename().string()) { need(bool(stream),"cannot open SKYF read-only");archive=assets::read_archive(stream,std::filesystem::file_size(p));entries.reserve(archive.entries.size());for(std::size_t i=0;i<archive.entries.size();++i)need(entries.emplace(lower(archive.entries[i].path),i).second,"case-insensitive SKYF ambiguity"); }
 std::vector<std::uint8_t> read(const std::string& name) {auto i=entries.find(lower(name));if(i==entries.end())throw std::runtime_error("referenced original resource missing: "+source_name+":"+name);return assets::read_entry(stream,archive.entries[i->second]);}
 Texture read_texture(const std::string& name) {auto i=entries.find(name);if(i==entries.end())throw std::runtime_error("referenced original resource missing: "+source_name+":"+name);const auto& entry=archive.entries[i->second];auto bytes=assets::read_entry(stream,entry);return decoded_texture(name,source_name,entry,assets::decode_dds(bytes,true));}
};
void normalize(float& x,float& y,float& z) { const auto length=std::sqrt(x*x+y*y+z*z);if(length>0){x/=length;y/=length;z/=length;} }
struct Object { std::uint32_t id,parent,type,reference,owner;std::string name,reuse;bool bone=false,occlusion_proxy=false,root_bone=false;std::array<float,3> half_extents{}; };
bool source_color_seed(std::string_view name) {
 // 004737a0 classifies each queued object independently and never walks children.
 return !name.starts_with("walkset")&&!name.starts_with("nocamera")&&(!name.starts_with("sky")||name.starts_with("skydome"));
}
bool source_particle_provider_name(std::string_view name) {
 // 004a0890(name,"particle_") is an ASCII case-folded prefix test: it returns true once the prefix is exhausted
 // (@4a08fb je 4a0951 with ZF set), so "particle_bocek_01" qualifies (004a2e70 type-11 particle node).
 return name.size()>=9&&std::equal(name.begin(),name.begin()+9,"particle_",[](char a,char b){if(a>='A'&&a<='Z')a+=32;return a==b;});
}
void source_color_participation(Scene& scene,std::vector<std::uint8_t> state) {
 // Bits: direct registration, recursive virtual, reachable, selective (+221==0) virtual, +221, constructed class. References are not child edges.
 need(state.size()==scene.nodes.size(),"source color enrollment node count");
 for(auto node:scene.node_order){need(node<state.size(),"source color enrollment node index");if(!(state[node]&32))continue;const auto parent=scene.nodes[node].parent;need(parent<0||std::size_t(parent)<state.size(),"source color enrollment parent index");if((state[node]&1)||(parent>=0&&(state[std::size_t(parent)]&4)&&((state[std::size_t(parent)]&2)||((state[std::size_t(parent)]&8)&&!(state[node]&16)))))state[node]|=4;}
 for(auto& instance:scene.instances){need(instance.node<state.size(),"source color enrollment placement node");instance.source_ordinary_color_participates=(state[instance.node]&4)!=0&&scene.nodes[instance.node].source_has_geometry;}
}
Scene decode_scene(const std::vector<assets::AdfChunk>& chunks) {
 Scene scene;std::unordered_map<std::uint32_t,std::uint32_t> materials,nodes;std::unordered_map<std::uint32_t,const assets::AdfChunk*> verts,tris,ambient,weights;
 std::vector<Object> objects;std::unordered_map<std::string,std::uint32_t> names;std::map<std::string,std::size_t> unsupported;
 std::uint64_t geometry_bytes=0;
 std::uint32_t source_null_material=UINT32_MAX;std::uint64_t ignored_qkeys=0,ignored_lightmaps=0;
 std::uint64_t resized_ambient_streams=0,unused_ambient_tail_bytes=0;std::string first_ambient_resize;
 std::map<std::uint32_t,std::uint64_t> source_registry_misses;
 std::unordered_map<std::uint32_t,std::string> lightmaps;
 std::unordered_map<std::uint32_t,std::uint32_t> source_nodes;std::unordered_map<const assets::AdfChunk*,std::uint32_t> track_nodes;
 std::uint32_t current_model=0,current_model_source=0,current_bone=0,current_bone_source=0;
 const auto resolve=[&](std::uint32_t id){auto it=source_nodes.find(id);return it==source_nodes.end()?0U:it->second;};
 const auto charge=[&](std::uint64_t bytes){need(bytes<=assets::max_entry_bytes-geometry_bytes,"ADF expanded geometry/metadata budget");geometry_bytes+=bytes;};
 const auto retain_word=[&](const assets::AdfChunk& c,std::size_t offset,SourceScalarUse use){
  const auto bits=word(c.data,offset);if((bits&0x7f800000U)!=0x7f800000U)return;
  charge(sizeof(SourceScalarWord));if(scene.source_resources.empty())scene.source_resources.emplace_back();
  scene.source_scalar_words.push_back({0,std::uint32_t(&c-chunks.data()),std::uint32_t(offset),bits,use});
 };
 const auto read_scalar=[&](const assets::AdfChunk& c,std::size_t offset){
  const auto bits=word(c.data,offset);const auto value=std::bit_cast<float>(bits);
  if(!std::isfinite(value))throw std::runtime_error("ADF nonfinite active scalar: chunk="+std::to_string(&c-chunks.data())+" tag="+c.tag+" id="+std::to_string(c.id)+" decoded_offset="+std::to_string(offset)+" bits="+std::to_string(bits));
  return value;
 };
 for(const auto& c:chunks) {
  Bytes b=c.data;
  if(c.tag=="mate") {
   need(b.size()%468==0,"ADF material stride");for(std::size_t p=0;p<b.size();p+=468) {
    auto id=word(b,p);if(materials.contains(id))continue; // Original 004a2320 registers only the first material with this source ID.
    Material m;m.flags=word(b,p+4);m.double_sided=word(b,p+8)!=0;m.alpha=read_scalar(c,p+12)>0;m.alpha_cutoff=(m.flags&1)&&m.alpha?79.f/255.f:0;m.source_color_draw=(m.flags&1)!=0; // Original CMP DWORD+8, not float; loader ignores name+404.
    if(m.flags&1)m.diffuse="textures/"+text(b,p+20,64);
    if(m.flags&4)m.normal="textures/"+text(b,p+212,64);
    if(m.flags&8)m.environment="textures/"+text(b,p+276,64);
    if(m.flags&32)m.luminance="textures/"+text(b,p+340,64);
    materials.emplace(id,scene.materials.size());scene.materials.push_back(std::move(m));
   }
  } else if(c.tag=="item"||c.tag=="bone") {
   need(b.size()==256,"ADF item/bone record size");auto source_id=word(b,8);bool bone=c.tag=="bone";if(!source_id){if(!bone)current_model=current_model_source=0;++unsupported["zero-ID-item/bone"];continue;}
   need(scene.nodes.size()<UINT32_MAX-1,"ADF node handle budget");auto id=std::uint32_t(scene.nodes.size()+1);
   Object o{id,resolve(word(b,16)),bone?0U:word(b,20),0,resolve(word(b,12)),text(b,192,64),{},bone};
   o.root_bone=bone&&o.parent==0;source_nodes[source_id]=id; // Original registers after looking up the bone parent, including self-root misses.
   if(!bone&&(o.type==1||o.type==4)){o.reference=word(b,28);if(o.reference)o.reuse=text(b,128,64);o.occlusion_proxy=o.name.starts_with("occ_");for(auto offset:{108U,116U,112U})retain_word(c,offset,SourceScalarUse::UnorderedBounds);o.half_extents={std::bit_cast<float>(word(b,108))*.05f,std::bit_cast<float>(word(b,116))*.05f,std::bit_cast<float>(word(b,112))*.05f};}
   nodes.emplace(id,scene.nodes.size());SceneNode node;node.id=source_id;node.name=o.name;node.source_bone=bone;node.source_has_geometry=!bone&&(o.type==1||o.type==4);if(!bone&&o.type==11&&source_particle_provider_name(o.name))node.source_particle_system=text(b,128,64);scene.nodes.push_back(std::move(node));objects.push_back(std::move(o)); // Type 11 "particle_*" (004a2e70): zone *_particles.adf emitter; +128 names its particle_systems.xml system.
   if(bone){current_bone=id;current_bone_source=source_id;}else{current_model=id;current_model_source=source_id;names.emplace(lower(objects.back().name),id);}
  } else if(c.tag=="vert"||c.tag=="mver"||c.tag=="pack") {need(current_model!=0,"ADF vertices without current model");verts[current_model]=&c;}
  else if(c.tag=="tris"||c.tag=="ptri") {need(current_model!=0,"ADF triangles without current model");tris[current_model]=&c;}
  else if(c.tag=="ambd") {if(b.size()<4)throw std::runtime_error("ADF ambient payload header: chunk="+std::to_string(&c-chunks.data())+" directory_id="+std::to_string(c.id)+" decoded_bytes="+std::to_string(b.size()));auto raw_id=word(b,0);auto id=raw_id==current_model_source?current_model:resolve(raw_id);need(id!=0,"ADF ambient target unavailable");ambient[id]=&c;} // Original 004a1870 consumes floor((bytes-4)/4) words, not a modulo-4 record contract.
  else if(c.tag=="wmap") {auto id=c.id==current_bone_source?current_bone:resolve(c.id);need(id!=0,"ADF skin target unavailable");weights[id]=&c;} // Original 004a0590 replaces previous maps for this bone.
  else if(c.tag=="lmpd") {need(b.size()==76,"ADF lightmap record size");auto raw_id=word(b,0);auto id=raw_id==current_model_source?current_model:resolve(raw_id);if(!id){++ignored_lightmaps;continue;}lightmaps[id]=text(b,4,64);} // Original 004a1930 returns before reading filename on a registry miss.
  else if(c.tag=="qkey") {track_nodes[&c]=c.id==current_model_source?current_model:resolve(c.id);}
  else ++unsupported[c.tag];
 }
 for(const auto& o:objects) {auto parent=nodes.find(o.parent);if(o.bone&&parent==nodes.end())parent=nodes.find(o.owner);if(parent!=nodes.end())scene.nodes[nodes.at(o.id)].parent=parent->second;else need(o.parent==0,"ADF missing item parent node");}
 for(const auto& o:objects)if(!o.bone&&o.reference){auto ref=names.find(lower(o.reuse));need(ref!=names.end(),"ADF source bounds reference unavailable");scene.nodes[nodes.at(o.id)].source_reference=nodes.at(ref->second);}
 std::vector<std::uint8_t> mark(scene.nodes.size());std::function<void(std::uint32_t,unsigned)> visit=[&](std::uint32_t n,unsigned depth){need(depth<=256&&mark[n]!=1,"ADF cyclic/deep hierarchy");if(mark[n]==2)return;mark[n]=1;const auto& node=scene.nodes[n];if(node.parent>=0)visit(node.parent,depth+1);if(node.source_reference>=0)visit(node.source_reference,depth+1);mark[n]=2;scene.node_order.push_back(n);};
 for(std::uint32_t i=0;i<scene.nodes.size();++i)visit(i,0);
 std::vector<std::uint32_t> track_index(scene.nodes.size(),UINT32_MAX);
 for(const auto& c:chunks) if(c.tag=="qkey") {
  // One track per node keeps this index within the checked node-handle budget.
  auto handle=track_nodes.at(&c);if(!handle){++ignored_qkeys;continue;}auto it=nodes.find(handle);need(it!=nodes.end(),"ADF transform handle unavailable");Bytes b=c.data;need(!b.empty()&&b.size()%64==0,"ADF transform key stride");auto& index=track_index[it->second];if(index==UINT32_MAX){index=std::uint32_t(scene.animation.size());scene.animation.push_back({it->second,{}});}auto& track=scene.animation[index];track.keys.reserve(track.keys.size()+b.size()/64);
  for(std::size_t p=0;p<b.size();p+=64) {AnimationKey k;k.time=read_scalar(c,p+48);need(k.time>=0,"ADF negative key time");for(unsigned j=0;j<3;++j)k.position[j]=read_scalar(c,p+j*4)*.1f;float norm=0;for(unsigned j=0;j<4;++j){k.rotation[j]=read_scalar(c,p+32+j*4);norm+=k.rotation[j]*k.rotation[j];}need(norm>0,"ADF zero quaternion");need(track.keys.empty()||k.time>=track.keys.back().time,"ADF key ordering");track.keys.push_back(k);}
 }
 std::set<std::uint32_t> cleared_owners,lightmap_geometry;
 for(const auto& o:objects)if(o.root_bone)cleared_owners.insert(o.owner);
 for(const auto& o:objects)if(!o.bone&&lightmaps.contains(o.id)){
  auto geometry_id=o.id;if(o.reference){auto ref=names.find(lower(o.reuse));need(ref!=names.end(),"ADF lightmap clone source unavailable");geometry_id=ref->second;}
  lightmap_geometry.insert(geometry_id);
 }
 struct Geometry {std::vector<std::uint32_t> meshes;SourceSurface surface;bool colored_ambient=false;};
 std::unordered_map<std::uint32_t,Geometry> geometry;
 using CornerKey=std::array<std::uint32_t,7>; // Source identity, material mesh, emitted UV bits, and quantized RGBA.
 struct CornerHash {std::size_t operator()(const CornerKey& key)const noexcept{std::size_t hash=2166136261U;for(auto value:key){hash^=value;hash*=16777619U;}return hash;}};
 for(const auto& [id,vc]:verts) {
  auto ti=tris.find(id);need(ti!=tris.end(),"ADF vertices missing triangles");const auto& tc=*ti->second;Bytes v=vc->data,t=tc.data;
  const auto stride=vc->tag=="vert"?48U:vc->tag=="pack"?32U:16U;const bool packed=tc.tag=="ptri";const auto triangle_stride=packed?116U:160U;
  need(v.size()%stride==0&&t.size()%triangle_stride==0,"ADF geometry stride");const auto count=v.size()/stride;need(count>0&&count<=1000000,"ADF vertex budget");
  Geometry g;auto& surface=g.surface;surface.heads.assign(count,UINT32_MAX);surface.recompute_normals=objects[nodes.at(id)].type==4;surface.root_clears=cleared_owners.contains(id);
  const bool persistent=surface.recompute_normals||surface.root_clears;
  if(persistent)charge(count*sizeof(std::uint32_t));
  if(surface.recompute_normals){charge(count*(sizeof(std::array<std::uint16_t,2>)+sizeof(std::array<float,3>)));surface.aliases.resize(count);surface.normals.resize(count);}
  const auto reserve=std::min({std::uint64_t(count),std::uint64_t(t.size()/triangle_stride)*3,(assets::max_entry_bytes-geometry_bytes)/(sizeof(Vertex)+sizeof(std::uint32_t))});
  surface.corners.reserve(reserve);std::unordered_map<CornerKey,std::uint32_t,CornerHash> emitted;emitted.reserve(reserve);std::vector<Vertex> source;source.reserve(count);
  std::vector<std::uint32_t> nonfinite_vertices;
  for(std::size_t p=0;p<v.size();p+=stride) {
   Vertex x{};const auto offset=stride==48?12U:stride==32?16U:4U;x.x=read_scalar(*vc,p+offset)*.1f;x.y=read_scalar(*vc,p+offset+4)*.1f;x.z=read_scalar(*vc,p+offset+8)*.1f;
   if(surface.root_clears){if(stride==48)for(auto normal_offset:{24U,28U,32U})retain_word(*vc,p+normal_offset,SourceScalarUse::ReplacedNormal);}
   else if(stride==48){
    const std::array<std::uint32_t,3> bits{word(v,p+24),word(v,p+28),word(v,p+32)};
    if(std::any_of(bits.begin(),bits.end(),[](auto value){return (value&0x7f800000U)==0x7f800000U;}))nonfinite_vertices.push_back(std::uint32_t(p/stride));
    else {x.nx=std::bit_cast<float>(bits[0]);x.ny=std::bit_cast<float>(bits[1]);x.nz=std::bit_cast<float>(bits[2]);normalize(x.nx,x.ny,x.nz);}
   } else {const auto n=word(v,p+(stride==32?8:0));x.nx=float(2.0*(double(n&2047)*0x1.00401p-10-1));x.ny=float(2.0*(double((n>>11)&2047)*0x1.00401p-10-1));x.nz=float(2.0*(double(n>>22)*0x1.00804p-9-1));normalize(x.nx,x.ny,x.nz);}
   source.push_back(x);
   if(surface.recompute_normals){
    const auto vi=p/stride;
    if(stride==16)surface.aliases[vi]={std::uint16_t(vi),std::uint16_t(vi)};
    else {const auto flags=word(v,p+(stride==48?8:28));const auto first=std::uint16_t(word(v,p)),second=std::uint16_t(word(v,p+4));surface.aliases[vi]={flags&12?second:first,flags&2?second:first};}
    if(!surface.root_clears)surface.normals[vi]={x.nx,x.ny,x.nz};
   }
  }
  std::vector<std::uint32_t> triangle_order(t.size()/triangle_stride);std::iota(triangle_order.begin(),triangle_order.end(),0U);
  const auto material_at=[&](std::uint32_t triangle){return word(t,std::size_t(triangle)*triangle_stride+(packed?112:156));};
  detail::source_sort_order(std::span<std::uint32_t>(triangle_order),[&](std::uint32_t a,std::uint32_t b){const auto left=material_at(a),right=material_at(b);return left>right?-1:left<right?1:0;});
  const auto index_at=[&](std::size_t p,unsigned corner){return packed?(std::uint32_t(t[p+corner*2])|(std::uint32_t(t[p+corner*2+1])<<8)):word(t,p+corner*4);};
  std::vector<std::uint8_t> active_normals;std::vector<SourceScalarUse> normal_use;
  if(!nonfinite_vertices.empty()&&!persistent){active_normals.resize(count);normal_use.assign(count,SourceScalarUse::UnreferencedNormal);}
  if(!active_normals.empty())for(auto triangle:triangle_order){
   const auto p=std::size_t(triangle)*triangle_stride;std::array<std::uint32_t,3> indices{index_at(p,0),index_at(p,1),index_at(p,2)};for(auto vi:indices)need(vi<count,"ADF triangle index range");
   const auto mi=materials.find(material_at(triangle));const bool color=mi!=materials.end()&&scene.materials[mi->second].source_color_draw;
   const auto& a=source[indices[0]];const auto& b=source[indices[1]];const auto& c=source[indices[2]];
   const std::array<double,3> ab{double(b.x)-a.x,double(b.y)-a.y,double(b.z)-a.z},ac{double(c.x)-a.x,double(c.y)-a.y,double(c.z)-a.z};
   const bool coverage=ab[1]*ac[2]!=ab[2]*ac[1]||ab[2]*ac[0]!=ab[0]*ac[2]||ab[0]*ac[1]!=ab[1]*ac[0];
   for(auto vi:indices){if(color)normal_use[vi]=SourceScalarUse::DegenerateNormal;else if(normal_use[vi]==SourceScalarUse::UnreferencedNormal)normal_use[vi]=SourceScalarUse::NoColorNormal;if(color&&coverage)active_normals[vi]=1;}
  }
  for(auto vi:nonfinite_vertices){
   const auto p=std::size_t(vi)*stride;
   if(surface.recompute_normals||active_normals[vi]){for(auto offset:{24U,28U,32U})(void)read_scalar(*vc,p+offset);}
   else for(auto offset:{24U,28U,32U})retain_word(*vc,p+offset,normal_use[vi]);
  }
  std::unordered_map<std::uint32_t,std::uint32_t> parts;
  for(auto triangle:triangle_order) {
   const auto p=std::size_t(triangle)*triangle_stride;charge(3*sizeof(std::uint32_t));if(surface.recompute_normals)charge(3*sizeof(std::uint32_t));
   const auto material=material_at(triangle);auto mi=materials.find(material);std::uint32_t material_index;
   if(mi!=materials.end())material_index=mi->second;
   else {
    ++source_registry_misses[material];
    if(source_null_material==UINT32_MAX){need(scene.materials.size()<UINT32_MAX,"ADF material index budget");Material m;m.name="DAT_0077bf6c";m.color={};m.source_color_draw=false;source_null_material=std::uint32_t(scene.materials.size());scene.materials.push_back(std::move(m));} // Original 004a0540 unconditional BSS-null copy; 0047ee50 skips its color draw.
    material_index=source_null_material;
   }
   const bool color_draw=scene.materials[material_index].source_color_draw;
   auto [part,inserted]=parts.emplace(material,scene.meshes.size());if(inserted){Mesh m;m.material=material_index;m.source_material_id=material;m.texture=scene.materials[material_index].diffuse;scene.meshes.push_back(std::move(m));g.meshes.push_back(part->second);}
   const auto mesh_id=part->second;auto& mesh=scene.meshes[mesh_id];need(mesh.indices.size()+3<=10000000,"ADF expanded triangle budget");
   for(int j=2;j>=0;--j){
    const auto vi=index_at(p,j);need(vi<count,"ADF triangle index range");Vertex x=source[vi];const auto uv=p+(packed?8:12)+j*32;x.u=read_scalar(tc,uv);x.v=1-read_scalar(tc,uv+4);
    if(color_draw&&lightmap_geometry.contains(id))x.uv1={read_scalar(tc,uv+8),1-read_scalar(tc,uv+12)};
    else {retain_word(tc,uv+8,SourceScalarUse::InactiveLightmapUv);retain_word(tc,uv+12,SourceScalarUse::InactiveLightmapUv);}
    std::uint32_t color=0;
    if(packed){const auto argb=std::uint32_t(t[p+104+j*2])|(std::uint32_t(t[p+105+j*2])<<8);color=((argb>>8&15)<<4)|((argb>>4&15)<<12)|((argb&15)<<20)|((argb>>12)<<28);}
    else for(unsigned k=0;k<4;++k){const auto value=read_scalar(tc,p+108+j*16+k*4);need(value>=0&&value<=1,"ADF triangle color range");color|=std::uint32_t(std::floor(value*255))<<(k*8);}
    for(unsigned k=0;k<4;++k)x.color[k]=float((color>>(k*8))&255)/255;
    const CornerKey key{vi,mesh_id,std::bit_cast<std::uint32_t>(x.u),std::bit_cast<std::uint32_t>(x.v),std::bit_cast<std::uint32_t>(x.uv1[0]),std::bit_cast<std::uint32_t>(x.uv1[1]),color};
    auto found=emitted.find(key);std::uint32_t corner;
    if(found!=emitted.end())corner=found->second;
    else {
     charge(sizeof(Vertex)+(persistent?sizeof(SourceSurfaceCorner):0));need(mesh.vertices.size()<10000000,"ADF expanded vertex budget");
     corner=std::uint32_t(mesh.vertices.size());mesh.vertices.push_back(x);surface.corners.push_back({mesh_id,corner,surface.heads[vi]});surface.heads[vi]=std::uint32_t(surface.corners.size()-1);emitted.emplace(key,corner);
    }
    mesh.indices.push_back(corner);if(surface.recompute_normals)surface.indices.push_back(vi);
   }
  }
  if(auto ac=ambient.find(id);ac!=ambient.end()){Bytes colors=ac->second->data;need(colors.size()>=4,"ADF ambient payload header");const auto used=std::min(surface.heads.size(),(colors.size()-4)/4);for(std::size_t vi=0;vi<used;++vi)if(surface.heads[vi]!=UINT32_MAX&&word(colors,4+vi*4)!=UINT32_MAX){g.colored_ambient=true;break;}}
  geometry.emplace(id,std::move(g));
 }
 std::unordered_map<std::uint32_t,std::uint32_t> default_ambient_meshes;
 for(const auto& o:objects) if(!o.bone) {
  auto geom=geometry.find(o.id);if(o.reference){auto name=names.find(lower(o.reuse));need(name!=names.end(),"ADF instance references missing geometry name");geom=geometry.find(name->second);}
  if(geom==geometry.end()) { if(o.type==1||o.type==4)++unsupported["item-without-render-geometry"];continue; }
  std::unordered_map<std::uint32_t,std::uint32_t> render_meshes;
  for(auto mesh:geom->second.meshes) {
   auto selected=mesh;
   const bool own_ambient=ambient.contains(o.id);const auto shared_default=default_ambient_meshes.find(mesh);
   if(o.reference&&!own_ambient&&shared_default!=default_ambient_meshes.end())selected=shared_default->second;
   else if(o.reference&&(own_ambient||geom->second.colored_ambient)) {
    // Interleaved native vertices fold original independent COLOR1 streams; default streams share one copy.
    charge(scene.meshes[mesh].vertices.size()*sizeof(Vertex)+scene.meshes[mesh].indices.size()*sizeof(std::uint32_t));
    need(scene.meshes.size()<UINT32_MAX,"ADF mesh index budget");selected=std::uint32_t(scene.meshes.size());Mesh copy=scene.meshes[mesh];if(geom->second.colored_ambient)for(auto& vertex:copy.vertices)vertex.ambient={1,1,1,1};scene.meshes.push_back(std::move(copy));if(!own_ambient)default_ambient_meshes.emplace(mesh,selected);
    auto& surface=geom->second.surface;
    if(surface.recompute_normals||surface.root_clears)for(std::size_t vi=0;vi<surface.heads.size();++vi)for(auto ci=surface.heads[vi];ci!=UINT32_MAX;){
     const auto original=surface.corners[ci];ci=original.next;if(original.mesh!=mesh)continue;
     charge(sizeof(SourceSurfaceCorner));surface.corners.push_back({selected,original.vertex,surface.heads[vi]});surface.heads[vi]=std::uint32_t(surface.corners.size()-1);
    }
   }
   auto material=UINT32_MAX;const auto lm=lightmaps.find(o.id);const bool source_ocean=o.name.starts_with("ocean");
   if(lm!=lightmaps.end()||source_ocean){
    need(scene.materials.size()<UINT32_MAX,"ADF material index budget");Material copy=scene.materials[scene.meshes[mesh].material];
    if(lm!=lightmaps.end())copy.lightmap=lm->second;
    // 004737a0 mutates normal+1c/environment+18, never the first diffuse handle+14.
    if(source_ocean){copy.source_ocean=true;copy.normal="textures/ocean_bump.dds";copy.environment.clear();}
    material=std::uint32_t(scene.materials.size());scene.materials.push_back(std::move(copy));
   }
   const bool special=o.type==1||o.type==4;render_meshes.emplace(mesh,selected);scene.instances.push_back({selected,{},o.name,0,nodes.at(o.id),material,o.occlusion_proxy,special&&o.name.starts_with("sky"),special&&(o.name.starts_with("ocean")||o.name.starts_with("isik_huzme")),o.half_extents,special&&o.name.starts_with("walkset"),special&&o.name.starts_with("nocamera")});
  }
  if(auto ac=ambient.find(o.id);ac!=ambient.end()) {
   const auto& chunk=*ac->second;Bytes colors=chunk.data;const auto& surface=geom->second.surface;need(colors.size()>=4,"ADF ambient payload header");const auto authored=(colors.size()-4)/4,used=std::min(authored,surface.heads.size());unused_ambient_tail_bytes+=(colors.size()-4)%4;
   if(authored!=surface.heads.size()){++resized_ambient_streams;if(first_ambient_resize.empty())first_ambient_resize=" chunk="+std::to_string(&chunk-chunks.data())+" source_id="+std::to_string(scene.nodes[nodes.at(o.id)].id)+" authored="+std::to_string(authored)+" geometry="+std::to_string(surface.heads.size());}
   // 004a1870 copies authored DWORDs; 0047e990/0047c690 preserve the bounded prefix on resize.
   // 0047c3d0 constructs newly allocated COLOR1 words as FFFFFFFF, independently of prototype colors.
   for(std::size_t vi=0;vi<used;++vi){auto color=word(colors,4+vi*4);for(auto ci=surface.heads[vi];ci!=UINT32_MAX;ci=surface.corners[ci].next){const auto& corner=surface.corners[ci];auto selected=render_meshes.find(corner.mesh);if(selected==render_meshes.end())continue;scene.meshes[selected->second].vertices[corner.vertex].ambient={float((color>>16)&255)/255,float((color>>8)&255)/255,float(color&255)/255,float(color>>24)/255};}}
  }
 }
 for(const auto& o:objects) if(o.bone) {
  auto w=weights.find(o.id);if(w==weights.end()||o.root_bone)continue;auto geom=geometry.find(o.owner);need(geom!=geometry.end(),"ADF bone owner has no geometry");const auto& wc=*w->second;Bytes b=wc.data;need(b.size()%32==0,"ADF weight map stride");
  for(std::size_t p=0;p<b.size();p+=32) {auto vi=word(b,p);const auto& surface=geom->second.surface;need(vi<surface.heads.size(),"ADF skin vertex range");auto weight=read_scalar(wc,p+4);need(weight>=0&&weight<=1,"ADF skin weight range");std::array<float,3> pos{};if(weight!=0)pos={-read_scalar(wc,p+8)*.1f,read_scalar(wc,p+16)*.1f,read_scalar(wc,p+12)*.1f};else for(auto offset:{8U,12U,16U})retain_word(wc,p+offset,SourceScalarUse::ZeroWeightPosition);for(auto offset:{20U,24U,28U})retain_word(wc,p+offset,SourceScalarUse::InactiveSkinNormal);for(auto ci=surface.heads[vi];ci!=UINT32_MAX;ci=surface.corners[ci].next){const auto& corner=surface.corners[ci];scene.meshes[corner.mesh].influences.push_back({corner.vertex,nodes.at(o.id),weight,pos,{}});}} // Original 00476eb0 consumes weighted positions only, never serialized skin normals.
 }
 for(auto& mesh:scene.meshes) {
  std::stable_sort(mesh.influences.begin(),mesh.influences.end(),[](const auto& a,const auto& b){return a.vertex<b.vertex;});
  // Shared exact-attribute vertices use a normalized sum instead of the last incident face's tangent.
  for(std::size_t p=0;p<mesh.indices.size();p+=3) {auto& a=mesh.vertices[mesh.indices[p]];auto& b=mesh.vertices[mesh.indices[p+1]];auto& c=mesh.vertices[mesh.indices[p+2]];float du=b.u-a.u,dv=b.v-a.v,eu=c.u-a.u,ev=c.v-a.v,det=du*ev-eu*dv;if(std::abs(det)>1e-6f){std::array<float,3>tangent{((b.x-a.x)*ev-(c.x-a.x)*dv)/det,((b.y-a.y)*ev-(c.y-a.y)*dv)/det,((b.z-a.z)*ev-(c.z-a.z)*dv)/det};for(auto index:{mesh.indices[p],mesh.indices[p+1],mesh.indices[p+2]})for(unsigned j=0;j<3;++j)mesh.vertices[index].tangent[j]+=tangent[j];}}
  for(auto& vertex:mesh.vertices)normalize(vertex.tangent[0],vertex.tangent[1],vertex.tangent[2]);
 }
 for(auto& [id,g]:geometry){auto& surface=g.surface;if(!surface.recompute_normals&&!surface.root_clears)continue;
  if(surface.root_clears)for(std::uint32_t ci=0;ci<surface.corners.size();++ci){const auto& corner=surface.corners[ci];const auto& influences=scene.meshes[corner.mesh].influences;auto it=std::lower_bound(influences.begin(),influences.end(),corner.vertex,[](const auto& influence,std::uint32_t vertex){return influence.vertex<vertex;});if(it==influences.end()||it->vertex!=corner.vertex){charge(sizeof(std::uint32_t));surface.unweighted_corners.push_back(ci);}}
  scene.source_surfaces.push_back(std::move(surface));
 }
 for(const auto& [tag,count]:unsupported)scene.diagnostics.push_back("Unrendered ADF semantics: "+tag+" chunks/records="+std::to_string(count));
 if(ignored_qkeys)scene.diagnostics.push_back("Original source no-op: 004a2890 ignores qkey registry misses="+std::to_string(ignored_qkeys));
 if(resized_ambient_streams)scene.diagnostics.push_back("Original source COLOR1 preserving resize: streams="+std::to_string(resized_ambient_streams)+first_ambient_resize);
 if(unused_ambient_tail_bytes)scene.diagnostics.push_back("Original source 004a1870 unread partial DWORD tail bytes="+std::to_string(unused_ambient_tail_bytes));
 if(ignored_lightmaps)scene.diagnostics.push_back("Original source no-op: 004a1930 ignores lightmap registry misses="+std::to_string(ignored_lightmaps));
 for(const auto& [id,count]:source_registry_misses)scene.diagnostics.push_back("Original BSS-null material registry miss: source_id="+std::to_string(id)+" retained triangles="+std::to_string(count)+"; no source color draw");
 if(!scene.source_scalar_words.empty()){const auto& first=scene.source_scalar_words.front();scene.diagnostics.push_back("Original inactive/IEEE scalar words retained="+std::to_string(scene.source_scalar_words.size())+"; first chunk="+std::to_string(first.chunk)+" decoded_offset="+std::to_string(first.offset)+" raw_bits="+std::to_string(first.bits));}
 if(scene.instances.empty())scene.diagnostics.push_back("Standalone nonrenderable ADF structure; owning original animation/particle context is not reconstructed");
 std::vector<std::uint8_t> color_nodes(scene.nodes.size());
 for(const auto& o:objects){
  auto& state=color_nodes[nodes.at(o.id)];
  if(o.bone)state=std::uint8_t(source_color_seed(o.name)|2);
  else if(o.type==1||o.type==4)state=std::uint8_t(source_color_seed(o.name)|((o.name.starts_with("sky")||o.occlusion_proxy)?0:2)|(o.type==4?16:0));
  else if(o.type==11)state=source_particle_provider_name(o.name)?std::uint8_t(source_color_seed(o.name)|16):std::uint8_t(1|8);
  if(o.bone||o.type==1||o.type==4||o.type==11)state|=32;
 }
 source_color_participation(scene,std::move(color_nodes));
 update_scene(scene,0);return scene;
}
}
std::vector<std::uint8_t> scene_material_usage(const Scene& scene) {
 std::vector<std::uint8_t> usage(scene.materials.size());if(usage.empty())return usage;
 const auto mark=[&](std::uint32_t mesh,std::uint32_t override_material=UINT32_MAX,bool participates=true){need(mesh<scene.meshes.size(),"scene referenced mesh out of range");const auto material=override_material==UINT32_MAX?scene.meshes[mesh].material:override_material;need(material<usage.size(),"scene referenced material out of range");if(scene_source_color_draw(scene,mesh,override_material,participates))usage[material]=1;};
 if(scene.instances.empty()){for(std::uint32_t i=0;i<scene.meshes.size();++i)mark(i);}
 else for(const auto& instance:scene.instances)mark(instance.mesh,instance.material,instance.source_ordinary_color_participates&&!instance.occlusion_proxy);
 return usage;
}
std::vector<std::size_t> scene_mesh_remap(const std::vector<std::uint64_t>& previous_keys,const Scene& scene) {
 std::unordered_map<std::uint64_t,std::size_t> keyed;
 for(std::size_t i=0;i<previous_keys.size();++i)if(previous_keys[i])keyed.emplace(previous_keys[i],i);
 std::vector<std::size_t> remap(scene.meshes.size(),SIZE_MAX);
 for(std::size_t i=0;i<remap.size();++i){
  const auto key=scene.meshes[i].key;
  if(!key){if(i<previous_keys.size()&&!previous_keys[i])remap[i]=i;}
  else if(const auto found=keyed.find(key);found!=keyed.end())remap[i]=found->second;
 }
 return remap;
}
const SourceSceneDescriptor& source_scene_descriptor(unsigned registry_id) {
 static const std::array<SourceSceneDescriptor,71> rows=[]{
  std::array<SourceSceneDescriptor,71> out;
  const auto add=[&](unsigned id,unsigned support,std::string key,std::string label,std::string environment,float minute,std::vector<std::string> members,const char* lightmap){
   out[id]={id,support,std::move(key),std::move(label),std::move(environment),minute,lightmap,std::move(members)};
  };
  const auto members=[](const char* stem,bool sky=false){std::vector<std::string> names{std::string("objects/maps/")+stem+".adf"};if(sky)names.emplace_back("objects/maps/sky_box.adf");names.emplace_back(std::string("objects/maps/")+stem+"_particles.adf");return names;};
  add(0,0,"eminonu","Eminönü","eminonu",749,members("eminonu",true),"textures/clouds.dds");
  add(1,1,"misir","Mısır Çarşısı","misir",749,members("misir"),"textures/clouds.dds");
  add(2,2,"antrepo","Antrepo","antrepo",749,members("antrepo"),"textures/clouds.dds");
  add(3,3,"labirent","Labirent","labirent",749,members("labirent"),"textures/clouds.dds");
  add(4,4,"meteor","Meteor Bölgesi","meteor",590,members("meteor",true),"textures/clouds.dds");
  add(5,5,"ada","Sivri Ada","ada",590,members("ada"),"textures/clouds.dds");
  for(unsigned i=0;i<20;++i)add(6+i,6,"savas"+std::to_string(i+1),"Er Meydanı "+std::to_string(i+1),"savas",749,{"objects/maps/savas.adf","objects/maps/sky_box.ADF"},"textures/clouds.dds");
  add(26,7,"yeralti","Yeraltı","yeralti",749,members("yeralti"),"textures/pureblack.dds");
  for(unsigned i=0;i<9;++i)add(27+i,8,"migrat"+std::to_string(i+1),"Migrat "+std::to_string(i+1),"migrat",749,members("migrat"),"textures/pureblack.dds");
  add(36,14,"migratzone","Migrat","migrat",749,members("migrat"),"textures/pureblack.dds");
  for(unsigned i=0;i<9;++i)add(37+i,9,"siginak"+std::to_string(i+1),"Sığınaklar "+std::to_string(i+1),"siginak",749,members("siginak"),"textures/pureblack.dds");
  add(46,15,"siginakzone","Sığınaklar","siginak",749,members("siginak"),"textures/pureblack.dds");
  for(unsigned i=0;i<19;++i)add(47+i,10,"cemberli"+std::to_string(i+1),"Çemberlitaş "+std::to_string(i+1),"cemberli",749,members("cemberli"),"textures/cemberli_sky.dds");
  add(66,16,"cemberlizone","Çemberlitaş","cemberli",749,members("cemberli"),"textures/pureblack.dds");
  add(67,11,"topkapi","Topkapı","topkapi",749,members("topkapi",true),"textures/clouds.dds");
  add(68,12,"buyukhol","Büyük Hol","buyukhol",749,members("buyukhol"),"textures/pureblack.dds");
  add(69,13,"karakoy","Karakoy","karakoy",749,members("karakoy",true),"textures/clouds.dds");
  add(70,17,"karakoyic","Karakoy İç Mekan","karakoyic",749,members("karakoyic",true),"textures/clouds.dds");
  return out;
 }();
 need(registry_id<rows.size(),"original scene registry ID outside supplied corpus");return rows[registry_id];
}
namespace {
std::atomic<unsigned> texture_detail_{2},lightmap_detail_{1}; // original >=1700 MB launcher defaults (0x483330)
// Resident (GPU-native BC or RGBA8) bytes of one scene's textures and lightmaps.
constexpr std::uint64_t scene_texture_budget=assets::max_entry_bytes;
}
void set_texture_detail(unsigned textures,unsigned lightmaps) {
 need(textures<=2&&lightmaps<=2,"texture/lightmap detail requires 0..2");texture_detail_=textures;lightmap_detail_=lightmaps;
}
std::filesystem::path texture_archive(const std::filesystem::path& base,const char* kind) {
 // 004619d0 opens exactly <kind>_4/_2/_1.dat for detail 0/1/2. A missing pack (partial install) falls back to the nearest
 // detail, sharper first; the original had no fallback.
 static constexpr const char* order[3][3]={{"_4","_2","_1"},{"_2","_1","_4"},{"_1","_2","_4"}};
 const unsigned detail=std::string_view(kind)=="lightmaps"?lightmap_detail_.load():texture_detail_.load();
 for(const auto* suffix:order[detail])for(const auto* extension:{".DAT",".dat"}){auto path=base/(std::string(kind)+suffix+extension);if(std::filesystem::is_regular_file(path))return path;}
 return {};
}
namespace {
std::array<std::size_t,8> scene_sizes(const Scene& scene) {return {scene.meshes.size(),scene.materials.size(),scene.instances.size(),scene.nodes.size(),scene.animation.size(),scene.source_surfaces.size(),scene.source_resources.size(),scene.source_scalar_words.size()};}
void append_model(Scene& scene,Scene model,std::uint32_t entity_id=0,std::uint32_t controller=UINT32_MAX) {
 const auto node_offset=std::uint32_t(scene.nodes.size()),mesh_offset=std::uint32_t(scene.meshes.size()),material_offset=std::uint32_t(scene.materials.size()),resource_offset=std::uint32_t(scene.source_resources.size());
 need(model.nodes.size()<=INT32_MAX-node_offset&&model.meshes.size()<=UINT32_MAX-mesh_offset&&model.materials.size()<=UINT32_MAX-material_offset&&model.source_resources.size()<=UINT32_MAX-resource_offset,"scene appended model handle budget");
 for(auto& n:model.nodes){if(n.parent>=0)n.parent+=node_offset;if(n.source_reference>=0)n.source_reference+=node_offset;scene.nodes.push_back(std::move(n));}for(auto n:model.node_order)scene.node_order.push_back(n+node_offset);
 for(auto& track:model.animation){track.instance+=node_offset;track.controller=controller;scene.animation.push_back(std::move(track));}
 for(auto& mesh:model.meshes){mesh.material+=material_offset;for(auto& i:mesh.influences)i.node+=node_offset;scene.meshes.push_back(std::move(mesh));}
 for(auto& i:model.instances){i.mesh+=mesh_offset;i.node+=node_offset;if(i.material!=UINT32_MAX)i.material+=material_offset;i.entity_id=entity_id;scene.instances.push_back(std::move(i));}
 for(auto& m:model.materials)scene.materials.push_back(std::move(m));
 for(auto& t:model.textures)if(std::none_of(scene.textures.begin(),scene.textures.end(),[&](const auto& old){return old.name==t.name;}))scene.textures.push_back(std::move(t));
 for(auto& diagnostic:model.diagnostics)scene.diagnostics.push_back(std::move(diagnostic));
 for(auto& resource:model.source_resources)scene.source_resources.push_back(std::move(resource));
 for(auto word:model.source_scalar_words){word.resource+=resource_offset;scene.source_scalar_words.push_back(word);}
 for(auto& surface:model.source_surfaces){for(auto& corner:surface.corners)corner.mesh+=mesh_offset;scene.source_surfaces.push_back(std::move(surface));}
}
void load_model_resources(Scene& scene,const std::filesystem::path& base,const Scene* resident=nullptr) {
 const auto usage=scene_material_usage(scene);const auto wanted=material_resources(scene,usage);std::uint64_t total=0;
 for(const auto& texture:scene.textures)total+=texture.data.size();
 if(resident)for(const auto& texture:resident->textures)total+=texture.data.size();
 const auto loaded=[&](const std::string& name){const auto contains=[&](const Scene& value){return std::any_of(value.textures.begin(),value.textures.end(),[&](const Texture& texture){return texture.name==name;});};return contains(scene)||(resident&&contains(*resident));};
 if(std::any_of(wanted.textures.begin(),wanted.textures.end(),[&](const auto& name){return !loaded(name);})){
  auto path=texture_archive(base,"textures");need(!path.empty(),"original texture SKYF archive missing");Pack textures(path);
  // A texture the shipped archive lacks (Migrat's textures/tap_sonradan_silinecek_bayrak.dds) leaves that channel unbound, like an
  // absent lightmap below. [INFERENCE] the original's failed texture create left the stage empty instead of refusing the zone.
  std::set<std::string> absent;
  for(const auto& name:wanted.textures){if(loaded(name))continue;if(!textures.entries.contains(name)){absent.insert(name);scene.diagnostics.push_back("Original texture absent in shipped archive, channel unbound: "+name);continue;}auto texture=textures.read_texture(name);total+=texture.data.size();need(total<=scene_texture_budget,"scene resident texture budget");scene.textures.push_back(std::move(texture));}
  const auto unbind=[&](std::string& name){if(!name.empty()&&absent.contains(lower(name)))name.clear();};
  if(!absent.empty()){for(std::size_t i=0;i<scene.materials.size();++i)if(usage[i])for(auto* name:{&scene.materials[i].diffuse,&scene.materials[i].normal,&scene.materials[i].environment,&scene.materials[i].luminance})unbind(*name);for(auto& mesh:scene.meshes)unbind(mesh.texture);}
 }
 if(wanted.lightmaps){
  auto path=texture_archive(base,"lightmaps");need(!path.empty(),"referenced original lightmap SKYF archive absent");Pack lightmaps(path);
  for(std::size_t i=0;i<scene.materials.size();++i)if(usage[i]&&!scene.materials[i].lightmap.empty()){
   auto& material=scene.materials[i];auto name=lower(material.lightmap);
   if(!lightmaps.entries.contains(name)){scene.diagnostics.push_back("Original lightmap absent in selected source variant: "+material.lightmap);material.lightmap.clear();continue;}
   material.lightmap=name;if(loaded(name))continue;
   auto texture=lightmaps.read_texture(name);total+=texture.data.size();need(total<=scene_texture_budget,"scene resident texture/lightmap budget");scene.textures.push_back(std::move(texture));
  }
 }
 normalize_loaded_bindings(scene,usage,resident);
}
void resolve_model_lightmaps(Scene& model,const std::string& member,const std::filesystem::path& base,const std::vector<std::uint8_t>& usage) {
 std::optional<Pack> lightmaps;
 for(std::size_t i=0;i<model.materials.size();++i)if(usage[i]&&!model.materials[i].lightmap.empty()){
  if(!lightmaps){auto path=texture_archive(base,"lightmaps");need(!path.empty(),"referenced original lightmap SKYF archive absent");lightmaps.emplace(path);}
  auto& material=model.materials[i];auto path=lower("lightmaps/"+std::filesystem::path(member).stem().string()+"/"+material.lightmap);
  if(!lightmaps->entries.contains(path))path=lower("lightmaps/"+material.lightmap);
  material.lightmap=path;
 }
}
Scene read_model(Pack& objects,const std::string& member,const std::filesystem::path& base,const std::filesystem::path* loose=nullptr) {
 auto model=decode_scene(assets::decode_adf(loose?file_bytes(*loose):objects.read(member)));
 if(model.source_resources.empty())model.source_resources.push_back(member);else model.source_resources[0]=member;
 resolve_model_lightmaps(model,member,base,scene_material_usage(model));return model;
}
void load_scene_help(Scene& scene,const std::filesystem::path& base) {
 // Sobee's Latin letter slots are source icons, not the native human text face.
 for(const auto* name:{"fonts/verdana.ttf","fonts/verdanab.ttf"})if(std::filesystem::is_regular_file(base/name)){scene.font=base/name;break;}
 scene.help=load_help(base);if(scene.help.empty())scene.diagnostics.push_back("Original helps.xml absent");
}
void bind_source_skydome(Scene& scene,const SourceSceneDescriptor& descriptor) {
 for(auto& instance:scene.instances)if(instance.sky&&instance.name.starts_with("skydome")){
  need(instance.mesh<scene.meshes.size(),"source skydome mesh index");const auto material=instance.material==UINT32_MAX?scene.meshes[instance.mesh].material:instance.material;
  need(material<scene.materials.size()&&scene.materials.size()<UINT32_MAX,"source skydome material index");
  auto selected=scene.materials[material];selected.diffuse=descriptor.lightmap;selected.luminance="textures/clouds_night.dds";selected.environment.clear();
  instance.material=std::uint32_t(scene.materials.size());scene.materials.push_back(std::move(selected));
 }
}
}
Scene load_scene(const std::filesystem::path& installation,const std::string& member) {
 const auto base=base_path(installation);Pack objects(base/"objects.dat");auto scene=read_model(objects,member,base);
 need(!scene.instances.empty(),"ADF standalone nonrenderable resource needs its original owning context, not a blank scene");
 load_model_resources(scene,base);load_scene_help(scene,base);return scene;
}
Scene load_source_scene(const std::filesystem::path& installation,unsigned registry_id) {
 const auto& descriptor=source_scene_descriptor(registry_id);const auto base=base_path(installation);Pack objects(base/"objects.dat");Scene scene;
 // 004a3770 diagnoses an absent packed/loose open; 0041c9a0 continues the same ordered vector.
 for(std::size_t index=0;index<descriptor.members.size();++index){
  const auto& member=descriptor.members[index];
  if(objects.entries.contains(lower(member))){append_model(scene,read_model(objects,member,base));continue;}
  const auto loose=base/member;
  if(std::filesystem::is_regular_file(loose)){append_model(scene,read_model(objects,member,base,&loose));continue;}
  scene.diagnostics.push_back("Original registered scene component absent and unloaded: registry="+std::to_string(registry_id)+" member["+std::to_string(index)+"] "+objects.source_name+":"+member);
 }
 scene.source_registry_id=registry_id;scene.source_support=descriptor.support_id;scene.source_environment_key=descriptor.environment_key;scene.source_lightmap=descriptor.lightmap;
 scene.zone=load_zone(installation,descriptor.support_id);set_scene_environment(scene,descriptor.support_id,descriptor.minute);
 bind_source_skydome(scene,descriptor);
 load_model_resources(scene,base);load_scene_help(scene,base);return scene;
}
Scene load_charsel_scene(const std::filesystem::path& installation) {
 // cModuleCharsel 004b1950 loads charsel.adf, charsel_particles.adf and sky_box.adf; no zone grid or registry row.
 const auto base=base_path(installation);Pack objects(base/"objects.dat");Scene scene;
 for(const auto* member:{"objects/maps/charsel.adf","objects/maps/charsel_particles.adf","objects/maps/sky_box.adf"})append_model(scene,read_model(objects,member,base));
 // [INFERENCE] the module performs no zone load; Karaköy's sky with the Eminönü day environment matches VOD 00:05.
 const auto& karakoy=source_scene_descriptor(69);scene.source_environment_key=karakoy.environment_key;scene.source_lightmap=karakoy.lightmap;
 set_scene_environment(scene,0,karakoy.minute);bind_source_skydome(scene,karakoy);
 // Fog start 240 from renderer init 00474590 (@474a01 0x592f48), end 610 from cModuleCharsel 004b17f0 (@4b1865 0x59b190);
 // the charsel fog colour is not set by either (NONCANONICAL, the former zone default).
 scene.lighting.fog_record={240,610};scene.lighting.fog_color={184.f/255,199.f/255,217.f/255};set_scene_fog(scene.lighting,1);
 load_model_resources(scene,base);load_scene_help(scene,base);return scene;
}
namespace {
const SourceActorRecord& source_record(const Scene& scene,unsigned uid,bool object=true) {
 const SourceActorRecord* selected=nullptr;
 // Non-object entities: npc/monster actors and <loc> world objects (gathering nodes, anvils) share uid space without collisions.
 for(const auto& record:scene.actor_tables.source_records)if(record.uid==uid&&(object?record.tag=="object":record.tag=="npc"||record.tag=="monster"||record.tag=="loc")){
  need(!selected,"ambiguous selected source UID");selected=&record;
 }
 need(selected,"selected source UID absent from classes.dat");return *selected;
}
const SourceActorRecord& source_piece(const Scene& scene,const std::string& model) {
 const SourceActorRecord* selected=nullptr;
 for(const auto& record:scene.actor_tables.source_records)if(record.tag=="object"&&record.model==model&&!record.path.empty()){
  need(!selected,"ambiguous source default bodypiece");selected=&record;
 }
 need(selected,"source default bodypiece identifier unresolved");return *selected;
}
std::vector<std::string> bodypiece_list(const std::string& list) {
 std::vector<std::string> pieces;std::size_t first=0;
 while(first<list.size()){auto end=list.find(';',first);if(end==std::string::npos)end=list.size();pieces.push_back(list.substr(first,end-first));first=end+1;}
 return pieces;
}
const Scene& cached_model(Scene& scene,std::optional<Pack>& objects,const std::filesystem::path& base,const std::string& member) {
 const auto key=source_object_member(member);
 for(const auto& resource:scene.model_resources)if(resource.member==key)return *resource.decoded;
 need(scene.model_resources.size()<4096,"original model cache resource budget");if(!objects)objects.emplace(base/"objects.dat");
 auto model=std::make_shared<const Scene>(read_model(*objects,key,base));scene.model_resources.push_back({key,std::move(model)});return *scene.model_resources.back().decoded;
}
std::uint32_t biped_root(const Scene& model,const std::string& biped) {
 std::uint32_t found=UINT32_MAX;
 for(std::uint32_t i=0;i<model.nodes.size();++i)if(model.nodes[i].name==biped){need(found==UINT32_MAX,"ambiguous original BIP root");found=i;}
 need(found!=UINT32_MAX,"selected original BIP absent from model hierarchy");return found;
}
std::uint32_t source_bip(const Scene& model,std::uint32_t root) {
 // 004a5120 takes the cached model root's first child as the BIP; classes.dat biped names are never looked up
 // (BIP_DEM vs BIP_DEMIRCI, monsters' "BIP_X BoneNN" roots). No child: original warns "hata" and builds no skeleton.
 for(std::uint32_t i=0;i<model.nodes.size();++i)if(model.nodes[i].parent==std::int32_t(root)&&!model.nodes[i].source_attachment)return i;
 return UINT32_MAX;
}
void initialize_source_actor_root(Scene& model,const SourceActorRecord& record) {
 const auto index=biped_root(model,record.model);auto& root=model.nodes[index];
 // 00473e50/00479de0 clone BIP under a fresh identity entity node, not the archived BODY ITEM pose.
 root.parent=-1;root.local={};root.world={};
 std::erase_if(model.animation,[index](const auto& track){return track.instance==index;});
}
void source_actor_geometry(Scene& model,std::uint32_t root,std::vector<std::size_t>& projection,std::size_t offset=0) {
 // 00479fd0 contributes selected cached ROOT subsets; 00479de0 clones descendants as geometry-free bones.
 for(std::size_t i=0;i<model.instances.size();++i){auto& instance=model.instances[i];instance.source_ordinary_color_participates=instance.node==root;if(instance.source_ordinary_color_participates)projection.push_back(offset+i);}
}
void prepare_source_actor_geometry(Scene& model,std::uint32_t root,std::vector<std::size_t>& projection,std::size_t offset,const std::filesystem::path& base,const std::string& member) {
 const auto previous=scene_material_usage(model);source_actor_geometry(model,root,projection,offset);auto activated=scene_material_usage(model);
 for(std::size_t i=0;i<activated.size();++i)activated[i]&=!previous[i];
 resolve_model_lightmaps(model,member,base,activated);
}
void source_actor_color_participation(Scene& model,const std::vector<std::size_t>& projection) {
 for(auto& instance:model.instances)instance.source_ordinary_color_participates=false;
 for(auto index:projection){need(index<model.instances.size(),"source actor color projection index");model.instances[index].source_ordinary_color_participates=true;}
}
void remap_bodypart(Scene& assembly,Scene part,std::uint32_t root,std::uint32_t source) {
 need(source<part.nodes.size(),"source part first-child index");std::vector<std::uint32_t> map(part.nodes.size(),UINT32_MAX);
 // 00479d70 appends geometry-free nodes in preorder and stops at geometry.
 const auto collect=[](const Scene& model,std::uint32_t first){
  std::vector<std::uint32_t> result,heads(model.nodes.size(),UINT32_MAX),next(model.nodes.size(),UINT32_MAX);result.reserve(model.nodes.size());
  for(std::size_t i=model.nodes.size();i>0;){--i;const auto& value=model.nodes[i];if(value.parent>=0&&!value.source_attachment){need(std::size_t(value.parent)<heads.size(),"source child parent index");next[i]=heads[std::size_t(value.parent)];heads[std::size_t(value.parent)]=std::uint32_t(i);}}
  const auto visit=[&](const auto& self,std::uint32_t node,unsigned depth)->void{
   need(depth<=256&&node<model.nodes.size(),"original dressing hierarchy depth/index");if(model.nodes[node].source_has_geometry)return;
   result.push_back(node);for(auto child=heads[node];child!=UINT32_MAX;child=next[child])self(self,child,depth+1);
  };
  visit(visit,first,0);return result;
 };
 const auto target_nodes=collect(assembly,root),source_nodes=collect(part,source);
 need(target_nodes.size()==source_nodes.size(),"original dressing BIP remap vector length mismatch");
 for(std::size_t i=0;i<source_nodes.size();++i)map[source_nodes[i]]=target_nodes[i];
 const auto node_offset=std::uint32_t(assembly.nodes.size());
 std::uint32_t added=0;for(std::uint32_t i=0;i<part.nodes.size();++i)if(map[i]==UINT32_MAX)map[i]=node_offset+added++;
 std::vector<SceneNode> nodes;nodes.reserve(added);
 for(std::uint32_t i=0;i<part.nodes.size();++i)if(map[i]>=node_offset){
  auto node=std::move(part.nodes[i]);if(node.parent>=0)node.parent=std::int32_t(map[std::size_t(node.parent)]);if(node.source_reference>=0)node.source_reference=std::int32_t(map[std::size_t(node.source_reference)]);
  nodes.push_back(std::move(node));
 }
 for(auto& influence_mesh:part.meshes)for(auto& influence:influence_mesh.influences)influence.node=map[influence.node];
 for(auto& instance:part.instances)instance.node=map[instance.node];
 // Only the BODY owns animation; the part's duplicate controllers are not appended.
 part.animation.clear();part.nodes.clear();part.node_order.clear();
 const auto mesh_offset=std::uint32_t(assembly.meshes.size()),material_offset=std::uint32_t(assembly.materials.size()),resource_offset=std::uint32_t(assembly.source_resources.size());
 for(auto& node:nodes)assembly.nodes.push_back(std::move(node));
 for(auto& mesh:part.meshes){mesh.material+=material_offset;assembly.meshes.push_back(std::move(mesh));}
 for(auto& instance:part.instances){instance.mesh+=mesh_offset;if(instance.material!=UINT32_MAX)instance.material+=material_offset;assembly.instances.push_back(std::move(instance));}
 for(auto& material:part.materials)assembly.materials.push_back(std::move(material));
 for(auto& surface:part.source_surfaces){for(auto& corner:surface.corners)corner.mesh+=mesh_offset;assembly.source_surfaces.push_back(std::move(surface));}
 for(auto& resource:part.source_resources)assembly.source_resources.push_back(std::move(resource));
 for(auto word:part.source_scalar_words){word.resource+=resource_offset;assembly.source_scalar_words.push_back(word);}
 for(auto& diagnostic:part.diagnostics)assembly.diagnostics.push_back(std::move(diagnostic));
}
std::uint32_t rigid_anchor(const Scene& assembly,std::uint32_t biped,unsigned source_slot) {
 // 0047a2d0 follows source child vectors, not semantic head/hand name aliases.
 const auto child=[&](std::uint32_t node,unsigned index){
  unsigned cursor=0;for(std::uint32_t i=0;i<assembly.nodes.size();++i)if(assembly.nodes[i].parent==std::int32_t(node)&&!assembly.nodes[i].source_attachment){if(cursor++==index)return i;}
  throw std::runtime_error("original rigid anchor child path absent");
 };
 const auto parent=assembly.nodes[biped].parent;need(parent>=0&&child(std::uint32_t(parent),0)==biped,"selected BODY BIP is not original render-root first child");
 auto anchor=biped;for(auto index:{1U,0U,0U})anchor=child(anchor,index);
 if(source_slot==2)return anchor;
 anchor=child(anchor,0);
 if(source_slot==7)return child(anchor,0);
 need(source_slot==11||source_slot==12,"original rigid item has no source slot dispatch");
 anchor=child(anchor,source_slot==11?2:1);for(auto index:{0U,0U,0U})anchor=child(anchor,index);return anchor;
}
void dress_part(Scene& assembly,Scene part,std::uint32_t root,const SourceActorRecord& record,std::vector<std::size_t>& projection,const std::filesystem::path& base) {
 // 0047a850 dispatches on the cached model root's child vector, not a bone/type heuristic.
 const auto model_root=biped_root(part,record.model);auto first=UINT32_MAX;
 prepare_source_actor_geometry(part,model_root,projection,assembly.instances.size(),base,record.path);
 for(std::uint32_t i=0;i<part.nodes.size();++i)if(part.nodes[i].parent==std::int32_t(model_root)){first=i;break;}
 if(first!=UINT32_MAX){remap_bodypart(assembly,std::move(part),root,first);return;}
 unsigned source_slot=0;const auto parsed=std::from_chars(record.suitable_slot.data(),record.suitable_slot.data()+record.suitable_slot.size(),source_slot);
 need(parsed.ec==std::errc{}&&parsed.ptr==record.suitable_slot.data()+record.suitable_slot.size(),"source rigid suitable-slot attribute invalid");
 const auto anchor=rigid_anchor(assembly,root,source_slot),offset=std::uint32_t(assembly.nodes.size());
 for(auto& node:part.nodes)if(node.parent<0)node.source_attachment=true;
 append_model(assembly,std::move(part));
 // 0047a2d0 parents the item's identity render root to the bone, like the PC constructor's fresh root: the archived
 // item placement/track (a bind-space offset of several units) is not its live transform. Keeping it floated weapons away from the hand.
 for(std::size_t i=offset;i<assembly.nodes.size();++i)if(assembly.nodes[i].source_attachment&&assembly.nodes[i].parent<0){assembly.nodes[i].parent=std::int32_t(anchor);assembly.nodes[i].local=Transform{};}
 for(auto& track:assembly.animation)if(track.instance>=offset&&track.instance<assembly.nodes.size()&&assembly.nodes[track.instance].parent==std::int32_t(anchor))track.keys.assign(1,AnimationKey{});
}
void rebuild_node_order(Scene& scene) {
 scene.node_order.clear();std::vector<std::uint8_t> mark(scene.nodes.size());
 const auto visit=[&](const auto& self,std::uint32_t node,unsigned depth)->void{
  need(node<scene.nodes.size()&&depth<=256&&mark[node]!=1,"dressed hierarchy cyclic or invalid");if(mark[node]==2)return;mark[node]=1;
  const auto& value=scene.nodes[node];if(value.parent>=0)self(self,std::uint32_t(value.parent),depth+1);if(value.source_reference>=0)self(self,std::uint32_t(value.source_reference),depth+1);
  mark[node]=2;scene.node_order.push_back(node);
 };
 for(std::uint32_t i=0;i<scene.nodes.size();++i)visit(visit,i,0);
}
void remove_assembly_geometry(Scene& scene,const SceneActorAssembly& assembly) {
 const auto& first=assembly.begin;const auto& last=assembly.end;
 const auto erase=[&](auto& values,unsigned index){values.erase(values.begin()+first[index],values.begin()+last[index]);};
 erase(scene.meshes,0);erase(scene.materials,1);erase(scene.instances,2);erase(scene.nodes,3);erase(scene.animation,4);erase(scene.source_surfaces,5);erase(scene.source_resources,6);erase(scene.source_scalar_words,7);
 const auto rebase=[&](auto& value,unsigned index){if(std::size_t(value)>=last[index])value-=static_cast<decltype(+value)>(last[index]-first[index]);else need(std::size_t(value)<first[index],"foreign reference into removed actor assembly");};
 for(auto& mesh:scene.meshes){rebase(mesh.material,1);for(auto& influence:mesh.influences)rebase(influence.node,3);}
 for(auto& instance:scene.instances){rebase(instance.mesh,0);rebase(instance.node,3);if(instance.material!=UINT32_MAX)rebase(instance.material,1);}
 for(auto& node:scene.nodes){if(node.parent>=0)rebase(node.parent,3);if(node.source_reference>=0)rebase(node.source_reference,3);}
 for(auto& track:scene.animation)rebase(track.instance,3);
 for(auto& surface:scene.source_surfaces)for(auto& corner:surface.corners)rebase(corner.mesh,0);
 for(auto& word:scene.source_scalar_words)rebase(word.resource,6);
 for(auto& other:scene.actor_assemblies)if(other.entity_id!=assembly.entity_id)for(unsigned i=0;i<first.size();++i)if(other.begin[i]>=last[i]){other.begin[i]-=last[i]-first[i];other.end[i]-=last[i]-first[i];}
 rebuild_node_order(scene);
}
void install_assembly(Scene& scene,Scene model,ActorAnimation actor,SceneActorAssembly descriptor,bool entry) {
 need(descriptor.entity_id!=0,"source actor requires nonzero authority ID");
 auto existing=std::find_if(scene.actor_assemblies.begin(),scene.actor_assemblies.end(),[&](const auto& value){return value.entity_id==descriptor.entity_id;});
 if(existing!=scene.actor_assemblies.end()){
  descriptor.controller=existing->controller;auto& previous=scene.actors[descriptor.controller];
  if(actor.library!=previous.library){
   if(previous.initialized&&previous.library<scene.actor_tables.libraries.size()&&actor.library<scene.actor_tables.libraries.size()){
    const auto& old_clips=scene.actor_tables.libraries[previous.library].clips;const auto& new_clips=scene.actor_tables.libraries[actor.library].clips;
    need(previous.current_clip<old_clips.size(),"replaced source actor cursor invalid");const auto id=old_clips[previous.current_clip].id;
    auto clip=std::find_if(new_clips.begin(),new_clips.end(),[id](const auto& value){return value.id==id;});need(clip!=new_clips.end(),"replacement source library cannot preserve current action");
    previous.library=actor.library;previous.current_clip=std::uint32_t(clip-new_clips.begin());previous.blend_duration=0;
   }else{previous=actor;previous.previous_seconds=scene.animation_seconds;}
  }
  else if(previous.library!=UINT32_MAX){
   // Sparse source tracks hold their last sampled pose; dressing must not replace that cache with time0.
   std::vector<const AnimationTrack*> held(existing->end[3]-existing->begin[3]);
   for(auto i=existing->begin[4];i<existing->end[4];++i){const auto& old=scene.animation[i];need(old.instance>=existing->begin[3]&&old.instance<existing->end[3],"owned source track node index");held[old.instance-existing->begin[3]]=&old;}
   for(auto& track:model.animation)if(track.instance<held.size()&&held[track.instance]){
    const auto& old=*held[track.instance];const auto& before=scene.nodes[old.instance];const auto& after=model.nodes[track.instance];
    if(before.id!=after.id||before.name!=after.name)continue;
    track.cached_pose=old.cached_pose;track.blend_pose=old.blend_pose;track.pose_initialized=old.pose_initialized;model.nodes[track.instance].local=before.local;
   }
  }
  SceneActorAssembly removed;removed.entity_id=existing->entity_id;removed.begin=existing->begin;removed.end=existing->end;
  remove_assembly_geometry(scene,removed);*existing=descriptor;entry=false;
 }else{
  need(scene.actors.size()<UINT32_MAX,"source actor controller budget");descriptor.controller=std::uint32_t(scene.actors.size());actor.previous_seconds=scene.animation_seconds;scene.actors.push_back(actor);scene.actor_assemblies.push_back(descriptor);existing=scene.actor_assemblies.end()-1;
 }
 for(auto& mesh:model.meshes)mesh.key=++scene.mesh_key; // Renderers retain every other mesh's GPU resources by key.
 existing->begin=scene_sizes(scene);append_model(scene,std::move(model),descriptor.entity_id,descriptor.controller);existing->end=scene_sizes(scene);
 scene.force_pose_update=true;
 if(entry)set_actor_action(scene,descriptor.entity_id,27,0);
 update_scene(scene,scene.animation_seconds);
}
}
const SourceActorRecord& source_actor_record(const Scene& scene,unsigned source_uid) {
 const auto& record=source_record(scene,source_uid,true);if(!record.error.empty())throw std::runtime_error("invalid selected source item metadata: "+record.error);return record;
}
void install_actor(Scene& scene,PreparedActor prepared) {
 install_assembly(scene,std::move(prepared.model),prepared.animation,std::move(prepared.descriptor),prepared.entry);
}
void attach_source_actor(Scene& scene,const std::filesystem::path& installation,const SourceAppearance& appearance,std::uint32_t entity_id) {
 for(const auto& assembly:scene.actor_assemblies)if(assembly.entity_id==entity_id&&!assembly.npc_uid&&assembly.member.empty()&&assembly.appearance==appearance)return;
 install_actor(scene,prepare_source_actor(scene,installation,appearance,entity_id,&scene));
}
PreparedActor prepare_source_actor(Scene& scene,const std::filesystem::path& installation,const SourceAppearance& appearance,std::uint32_t entity_id,const Scene* resident) {
 need(appearance.source_class>=2&&appearance.source_class<=7,"original PC class outside 2..7");
 const unsigned sex=appearance.source_class&1,career=appearance.source_class/2-1;
 need(appearance.face<10&&appearance.hair<(sex?18U:21U)&&appearance.hair_color<(sex?5U:10U)&&appearance.beard<(sex?15U:25U)&&appearance.beard_color<(sex?5U:2U)&&appearance.equipment.size()<=12,"original creator appearance selection outside source tables");
 // All425 original B/C words were selected; these constexpr runs encode their exact
 // consecutive UID rows and -1 sentinels, not an inferred archive filename sequence.
 static constexpr auto hair=[]{
  std::array<std::array<int,210>,2> rows{};
  for(unsigned color=0;color<10;++color)for(unsigned style=0;style<21;++style)rows[0][color*21+style]=style==20?-1:int(3201+color*20+style);
  for(unsigned color=0;color<5;++color)for(unsigned style=0;style<18;++style)rows[1][color*18+style]=style==17?-1:int(3001+color*17+style);
  return rows;
 }();
 static constexpr auto beard=[]{
  std::array<std::array<int,75>,2> rows{};
  for(unsigned color=0;color<2;++color)for(unsigned style=0;style<25;++style)rows[0][color*25+style]=style==0?-1:int(6300+color*24+style);
  for(unsigned color=0;color<5;++color)for(unsigned style=0;style<15;++style)rows[1][color*15+style]=style==0?-1:int(6200+color*14+style);
  return rows;
 }();
 static constexpr unsigned face[2][10]={{5002,5008,5009,5006,5012,5013,5202,5203,4015,4017},{4002,4007,4008,4009,4012,4013,5200,5201,4014,4016}};
 static constexpr unsigned roots[6]={5999,4999,7999,6999,9999,8999},legs[2][3]={{5101,7104,9104},{4101,6104,8104}};
 std::array<unsigned,13> selected{};selected[1]=face[sex][appearance.face];selected[3]=legs[sex][career];
 const auto hair_uid=hair[sex][appearance.hair_color*(sex?18:21)+appearance.hair],beard_uid=beard[sex][appearance.beard_color*(sex?15:25)+appearance.beard];
 if(hair_uid>=0)selected[6]=unsigned(hair_uid);
 if(beard_uid>=0)selected[7]=unsigned(beard_uid);
 std::array<bool,13> occupied{};
 for(const auto& equipment:appearance.equipment){need(equipment.slot>=1&&equipment.slot<=12&&equipment.uid!=0&&!occupied[equipment.slot],"invalid or duplicate source visual equipment slot");occupied[equipment.slot]=true;selected[equipment.slot]=equipment.uid;}
 std::array<unsigned,12> dress_order{};unsigned dress_count=0;
 for(auto slot:{1U,6U,7U,3U})if(selected[slot]&&!occupied[slot])dress_order[dress_count++]=slot;
 for(const auto& equipment:appearance.equipment)dress_order[dress_count++]=equipment.slot;
 const auto base=base_path(installation);(void)load_actor_animation(scene,base,"objects/pc/e_efe/E_BODY_EFE.ADF");
 const auto& root_record=source_record(scene,roots[appearance.source_class-2]);need(root_record.object_type=="bodyroot"&&root_record.class_id==std::to_string(appearance.source_class),"selected source PC BODY class mismatch");
 const auto& defaults_record=source_record(scene,900+appearance.source_class,false);const auto defaults=bodypiece_list(defaults_record.bodypieces);need(defaults.size()>=6,"source PC default bodypiece list incomplete");
 // 0043fe40 dresses the equipped vector first, then fills unoccupied generic slots1..5.
 for(unsigned slot=1;slot<=5;++slot)if(!selected[slot]&&!defaults[slot].empty()&&defaults[slot][0]!='*'){selected[slot]=source_piece(scene,defaults[slot]).uid;dress_order[dress_count++]=slot;}
 auto actor=load_actor_animation(scene,base,root_record.path,root_record);need(actor.library!=UINT32_MAX&&!actor.stopped,"source PC exact animation binding unavailable");
 std::optional<Pack> objects;Scene model=cached_model(scene,objects,base,root_record.path);const auto model_root=biped_root(model,root_record.model);
 const auto root=source_bip(model,model_root);need(root!=UINT32_MAX,"source PC BODY model root has no BIP child");
 initialize_source_actor_root(model,root_record);
 std::vector<std::size_t> projection;prepare_source_actor_geometry(model,model_root,projection,0,base,root_record.path);
 SceneActorAssembly descriptor;descriptor.entity_id=entity_id;descriptor.appearance=appearance;descriptor.source_parts.reserve(dress_count+1);descriptor.source_parts.push_back({0,root_record.uid});
 for(unsigned item=0;item<dress_count;++item){const auto slot=dress_order[item];
  const auto& piece=source_record(scene,selected[slot]);need(!piece.path.empty(),"selected source visual equipment has no model member");
  descriptor.source_parts.push_back({slot,piece.uid});const auto first=model.instances.size();dress_part(model,cached_model(scene,objects,base,piece.path),root,piece,projection,base);
  if(slot==11)descriptor.weapon={first,model.instances.size()};
 }
 rebuild_node_order(model);source_actor_color_participation(model,projection);load_model_resources(model,base,resident);
 return {std::move(model),actor,std::move(descriptor),true};
}
const Instance* source_actor_weapon(const Scene& scene,std::uint32_t entity_id) {
 for(const auto& a:scene.actor_assemblies)if(a.entity_id==entity_id){
  // The rigid item's own root (parented to the hand bone by dress_part) carries its render mesh.
  for(auto i=a.begin[2]+a.weapon[0];i<a.begin[2]+a.weapon[1]&&i<scene.instances.size();++i)
   if(scene.nodes[scene.instances[i].node].source_attachment)return &scene.instances[i];
  return a.weapon[0]<a.weapon[1]&&a.begin[2]+a.weapon[0]<scene.instances.size()?&scene.instances[a.begin[2]+a.weapon[0]]:nullptr;
 }
 return nullptr;
}
void attach_entity_model(Scene& scene,const std::filesystem::path& installation,const std::string& member,std::uint32_t entity_id) {
 for(const auto& assembly:scene.actor_assemblies)if(assembly.entity_id==entity_id&&assembly.member==lower(member))return;
 install_actor(scene,prepare_entity_model(scene,installation,member,entity_id,&scene));
}
PreparedActor prepare_entity_model(Scene& scene,const std::filesystem::path& installation,const std::string& member,std::uint32_t entity_id,const Scene* resident) {
 const auto base=base_path(installation);auto actor=load_actor_animation(scene,base,member);std::optional<Pack> objects;Scene model=cached_model(scene,objects,base,member);need(!model.instances.empty(),"operator model member lacks original renderable owning context");
 load_model_resources(model,base,resident);SceneActorAssembly descriptor;descriptor.entity_id=entity_id;descriptor.member=lower(member);return {std::move(model),actor,std::move(descriptor),false};
}
void attach_source_npc(Scene& scene,const std::filesystem::path& installation,unsigned source_uid,std::uint32_t entity_id,const std::vector<SourceEquipment>& equipment) {
 for(const auto& assembly:scene.actor_assemblies)if(assembly.entity_id==entity_id&&assembly.npc_uid==source_uid&&assembly.appearance.equipment==equipment)return;
 install_actor(scene,prepare_source_npc(scene,installation,source_uid,entity_id,equipment,&scene));
}
PreparedActor prepare_source_npc(Scene& scene,const std::filesystem::path& installation,unsigned source_uid,std::uint32_t entity_id,const std::vector<SourceEquipment>& equipment,const Scene* resident) {
 const auto base=base_path(installation);(void)load_actor_animation(scene,base,"objects/pc/e_efe/E_BODY_EFE.ADF");
 const auto& record=source_record(scene,source_uid,false);const auto defaults=bodypiece_list(record.bodypieces);
 const auto& root_record=record.path.empty()?source_piece(scene,defaults.empty()?record.model:defaults.front()):record;
 auto actor=load_actor_animation(scene,base,root_record.path,record);need(!actor.stopped,"source NPC exact animation binding unavailable"); // library UINT32_MAX: no cliplist, holds its pose (0047bea0)
 std::optional<Pack> objects;Scene model=cached_model(scene,objects,base,root_record.path);const auto model_root=biped_root(model,root_record.model);
 initialize_source_actor_root(model,root_record);
 std::vector<std::size_t> projection;prepare_source_actor_geometry(model,model_root,projection,0,base,root_record.path);
 SceneActorAssembly descriptor;descriptor.entity_id=entity_id;descriptor.npc_uid=source_uid;descriptor.member=root_record.path;descriptor.appearance.equipment=equipment;descriptor.source_parts.reserve(6+equipment.size());descriptor.source_parts.push_back({0,root_record.uid});
 // 00473e50 clones/dresses only when 004a5120 found a BIP (first model-root child).
 if(const auto root=source_bip(model,model_root);root!=UINT32_MAX){
  for(unsigned slot=1;slot<=5&&slot<defaults.size();++slot)if(!defaults[slot].empty()&&defaults[slot][0]!='*'){
   const auto& piece=source_piece(scene,defaults[slot]);descriptor.source_parts.push_back({slot,piece.uid});dress_part(model,cached_model(scene,objects,base,piece.path),root,piece,projection,base);
  }
  // Server-sent visible equipment (classes.dat rhweaponid/lhweaponid) after the bodypieces, as the PC dressing 0043fe40 does.
  for(const auto& item:equipment){
   const auto& piece=source_record(scene,item.uid);need(!piece.path.empty(),"selected NPC visual equipment has no model member");
   descriptor.source_parts.push_back({item.slot,piece.uid});const auto first=model.instances.size();dress_part(model,cached_model(scene,objects,base,piece.path),root,piece,projection,base);
   if(item.slot==11)descriptor.weapon={first,model.instances.size()};
  }
 }
 // 00434cd0 (on entity construction 004a53a8 and every re-dress 004a9639/004a9661) looks the class record's diffuse_texture up
 // (0043a90b) and sets it on every material of the entity's models: classes.dat colour variants (Mor/Yeşil/Lağım Faresi on one
 // zıçan mesh, Örümcek/Akrep tints) and Tangriss/Salon Tuzak's textures\none.dds, absent from the archives, so that channel stays
 // unbound like any absent texture (load_model_resources).
 if(!record.diffuse.empty()){for(auto& material:model.materials)material.diffuse=record.diffuse;for(auto& mesh:model.meshes)mesh.texture=record.diffuse;}
 rebuild_node_order(model);source_actor_color_participation(model,projection);load_model_resources(model,base,resident);
 return {std::move(model),actor,std::move(descriptor),false};
}
void detach_source_actor(Scene& scene,std::uint32_t entity_id) {
 const auto found=std::find_if(scene.actor_assemblies.begin(),scene.actor_assemblies.end(),[entity_id](const auto& assembly){return assembly.entity_id==entity_id;});
 if(found==scene.actor_assemblies.end())return;
 const auto controller=found->controller;SceneActorAssembly removed;removed.entity_id=entity_id;removed.begin=found->begin;removed.end=found->end;
 remove_assembly_geometry(scene,removed);scene.actor_assemblies.erase(found);
 need(controller<scene.actors.size(),"retired actor controller invalid");scene.actors.erase(scene.actors.begin()+controller);
 for(auto& track:scene.animation)if(track.controller!=UINT32_MAX&&track.controller>controller)--track.controller;
 for(auto& assembly:scene.actor_assemblies)if(assembly.controller>controller)--assembly.controller;
 ++scene.geometry_revision;
}
Zone load_zone(const std::filesystem::path& installation,unsigned zone) {
 need(zone<18,"original zone number outside supplied corpus");auto base=base_path(installation);char name[32];std::snprintf(name,sizeof(name),"zone%02u.bin",zone);auto cells=file_bytes(base/name,4194304);need(cells.size()==4194304,"original zone cell size");std::snprintf(name,sizeof(name),"zone%02u_height.bin",zone);auto bytes=file_bytes(base/name,16777216);need(bytes.size()==16777216,"original zone height size");Zone out;out.width=out.height=2048;out.cells=std::move(cells);out.source=name;out.heights.reserve(4194304);for(std::size_t p=0;p<bytes.size();p+=4){auto h=scalar(bytes,p);need(out.cells[p/4]==0||out.cells[p/4]==255,"unknown original zone cell");out.heights.push_back(h);}return out;
}
std::optional<float> zone_height(const Zone& zone,float x,float z) {
 if(!std::isfinite(x)||!std::isfinite(z)||zone.width!=2048||zone.height!=2048||zone.cells.size()!=4194304||zone.heights.size()!=4194304)return {};
 const auto index=[](float c)->double {volatile float scaled=c*0x1.555556p-1f;return std::round(double(scaled))+1024;};auto ix=index(x),iz=index(z);if(ix<0||ix>=2048||iz<0||iz>=2048)return {};auto i=std::size_t(iz)*2048+std::size_t(ix);if(zone.cells[i]!=255)return {};return zone.heights[i];
}
void set_scene_environment(Scene& scene,unsigned source_zone,std::optional<float> source_minute) {
 need(source_zone<18,"invalid original environment selection");
 float minute=source_minute.value_or(source_zone==4||source_zone==5?590.f:749.f);need(std::isfinite(minute),"invalid original environment minute");
 const std::array<float,8> times{0,360,375,555,885,1065,1080,1440};
 const std::array<std::array<float,3>,8> directions{{{0,-.1f,.2f},{-.5f,0,.2f},{-1,0,.2f},{0,1,.2f},{1,0,.2f},{.5f,0,.2f},{0,-.25f,.2f},{0,-.1f,.2f}}};
 const std::array<std::array<float,3>,8> sky_b{{{3,3,10},{15,15,25},{40,40,50},{50,50,60},{30,30,40},{15,15,25},{3,3,10},{3,3,10}}};
 const std::array<std::array<float,3>,8> sky_c{{{20,20,42},{60,72,60},{179,154,102},{235,235,235},{225,210,197},{60,72,60},{20,20,42},{20,20,42}}};
 minute=std::fmod(minute,1440.f);if(minute<0)minute+=1440;
 auto upper=std::upper_bound(times.begin(),times.end(),minute);auto i=std::size_t(upper-times.begin()-1);auto blend=(minute-times[i])/(times[i+1]-times[i]);
 // Zone-load record 0041c9a0: Eminönü (zone 0) and every other zone get fixed lighting_multipliers_1 (ambient,
 // .a = lightmap shadow strength c2d4) and lighting_multipliers_2 (sun) until the server environment record arrives
 // (set_scene_clock). Fog: the game-state ctor 004b43a0 initialises the record fog distances to 10/100 (@4b4502/@4b4522,
 // 0x58d7d0/0x58d7a4) and the fog/clear colour bytes +0xe2..e4 to 0; 004b6970 applies them every frame until thgf.
 auto& light=scene.lighting;light.source_values=true;light.source_zone=source_zone;light.source_minute=minute;light.fog_enabled=true;light.fog_color={0,0,0};light.fog_record={10,100};set_scene_fog(light,1);
 for(unsigned j=0;j<3;++j){light.sun_direction[j]=directions[i][j]+(directions[i+1][j]-directions[i][j])*blend;light.sky_b[j]=(sky_b[i][j]+(sky_b[i+1][j]-sky_b[i][j])*blend)*.0039216f;light.sky_c[j]=(sky_c[i][j]+(sky_c[i+1][j]-sky_c[i][j])*blend)*.0039216f;}
 normalize(light.sun_direction[0],light.sun_direction[1],light.sun_direction[2]);
 if(source_zone==0){light.ambient_multiplier={.784f,.616f,.556f};light.diffuse_multiplier={.496f,.412f,.432f};light.lightmap_strength=.304f;}
 else{light.ambient_multiplier={.8f,.74f,.6f};light.diffuse_multiplier={0,0,0};light.lightmap_strength=.5f;}
 light.sky_blend=light.sky_rotation=light.glow=0; // c308 is BSS zero until the server record arrives
}
void set_scene_clock(Scene& scene,float hour,const std::array<std::uint8_t,11>& record) {
 need(hour>=0&&hour<24,"invalid server hour");
 auto& light=scene.lighting;
 // 004a4c90: night sky blend c2d0 (sky pixel shader c7.x mixes clouds_night); 3..4h and 23..24h ramps are original.
 light.sky_blend=hour<=3?1.f:hour>=4&&hour<=23?0.f:hour<4?1-(hour-3)*.5f:(hour-23)*.5f;
 light.source_hour=hour;
 // 004b6970: record bytes /255 -> multipliers_1 rgb, multipliers_2 rgb, clear/horizon rgb, multipliers_1.a.
 for(unsigned j=0;j<3;++j){light.ambient_multiplier[j]=record[j]/255.f;light.diffuse_multiplier[j]=record[3+j]/255.f;light.fog_color[j]=record[6+j]/255.f;}
 light.lightmap_strength=record[9]/255.f;
 light.glow=record[10]/255.f; // c308 (record +0xe9); fake-HDR blur c3.x = glow*.02 (0046fdb0 @471012)
}
void set_scene_fog(SceneLighting& light,float view) {
 need(std::isfinite(view)&&view>0&&std::isfinite(light.fog_record[0])&&std::isfinite(light.fog_record[1]),"invalid fog distances");
 light.fog_start=light.fog_record[0]*view;light.fog_far=light.fog_record[1]*view;light.fog_end=light.fog_far+110; // 0x595d74 = 110
}
namespace {
void scene_material_resources_self_test() {
 Scene scene;scene.materials.resize(3);scene.meshes.resize(2);scene.meshes[0].material=0;scene.meshes[1].material=UINT32_MAX;
 auto& base=scene.materials[0];base.diffuse="textures/base.dds";base.lightmap="base.dds";
 auto& bound=scene.materials[1];bound.diffuse="Textures\\Bound.dds";bound.normal="textures/bound_n.dds";bound.environment="textures/bound_e.dds";bound.luminance="textures/bound_l.dds";
 auto& dormant=scene.materials[2];dormant.flags=13;dormant.diffuse="textures/merannormal R1.dds";dormant.normal="textures/merannormal1.dds";dormant.lightmap="dormant.dds";
 Instance instance;instance.material=1;scene.instances.push_back(instance);
 auto usage=scene_material_usage(scene);need(usage==std::vector<std::uint8_t>{0,1,0},"scene override excludes base and dormant materials self-check");
 auto wanted=material_resources(scene,usage);
 need(wanted.textures==std::set<std::string>{"textures/bound.dds","textures/bound_n.dds","textures/bound_e.dds","textures/bound_l.dds"}&&!wanted.lightmaps,"scene loads only resolved material channels self-check");
 need(base.diffuse=="textures/base.dds"&&base.lightmap=="base.dds"&&bound.diffuse=="Textures\\Bound.dds"&&dormant.flags==13&&dormant.diffuse=="textures/merannormal R1.dds"&&dormant.normal=="textures/merannormal1.dds"&&dormant.lightmap=="dormant.dds","scene resource gathering retains source flags and paths self-check");
 scene.instances[0].material=2;const auto active_missing=material_resources(scene,scene_material_usage(scene));
 need(active_missing.textures==std::set<std::string>{"textures/merannormal r1.dds","textures/merannormal1.dds"}&&active_missing.lightmaps,"scene missing source paths remain requested when bound self-check");scene.instances[0].material=1;
 bound.lightmap="original/proxy.dds";bound.source_color_draw=false;
 auto disabled=material_resources(scene,scene_material_usage(scene));need(disabled.textures.empty()&&!disabled.lightmaps&&bound.diffuse=="Textures\\Bound.dds"&&bound.lightmap=="original/proxy.dds","scene no-color override retains authored bindings without residency self-check");
 scene.instances[0].material=UINT32_MAX;auto fallback=material_resources(scene,scene_material_usage(scene));need(fallback.textures==std::set<std::string>{"textures/base.dds"}&&fallback.lightmaps,"scene effective mesh fallback restores only its color resources self-check");
 bound.source_color_draw=true;scene.instances[0].material=1;scene.instances[0].occlusion_proxy=true;auto proxy_resources=material_resources(scene,scene_material_usage(scene));need(proxy_resources.textures.empty()&&!proxy_resources.lightmaps&&bound.lightmap=="original/proxy.dds","scene proxy exclusion retains authored lightmap without residency self-check");scene.instances[0].occlusion_proxy=false;
 const auto reject=[](const Scene& input,const char* message){bool failed=false;try{(void)scene_material_usage(input);}catch(const std::runtime_error&){failed=true;}need(failed,message);};
 scene.instances[0].material=3;reject(scene,"scene out-of-range material override accepted");
 scene.instances[0].material=1;scene.instances[0].mesh=2;reject(scene,"scene out-of-range instance mesh accepted");
 scene.instances[0].mesh=1;scene.instances[0].material=UINT32_MAX;reject(scene,"scene out-of-range resolved mesh material accepted");
 scene.instances[0].material=1;need(scene_material_usage(scene)==std::vector<std::uint8_t>{0,1,0},"scene override does not resolve unused mesh material self-check");
 scene.instances.clear();reject(scene,"scene standalone out-of-range mesh material accepted");
 scene.meshes[1].material=1;usage=scene_material_usage(scene);need(usage==std::vector<std::uint8_t>{1,1,0},"scene standalone preview uses mesh materials self-check");
 wanted=material_resources(scene,usage);need(wanted.textures.contains("textures/base.dds")&&wanted.textures.size()==5&&wanted.lightmaps,"scene standalone preview loads base and bound resources self-check");
 scene.meshes.clear();need(scene_material_usage(scene)==std::vector<std::uint8_t>{0,0,0},"scene declarations without geometry remain dormant self-check");
 scene.materials.clear();scene.meshes.resize(1);need(scene_material_usage(scene).empty(),"scene material-less programmatic default remains supported self-check");
 Scene sky;sky.meshes.resize(1);sky.materials.resize(1);auto& authored=sky.materials[0];authored.diffuse="textures/authored.dds";authored.normal="textures/authored_n.dds";authored.environment="textures/authored_e.dds";authored.luminance="textures/authored_l.dds";authored.lightmap="lightmaps/authored.dds";authored.flags=13;authored.color={.1f,.2f,.3f,.4f};sky.meshes[0].texture=authored.diffuse;
 Instance dome;dome.sky=true;dome.name="skydome_source";sky.instances.push_back(dome);sky.instances.push_back({});bind_source_skydome(sky,source_scene_descriptor(0));
 const auto& resolved=sky.materials.at(sky.instances[0].material);
 need(sky.materials.size()==2&&sky.instances[1].material==UINT32_MAX&&sky.materials[0].diffuse=="textures/authored.dds"&&resolved.diffuse=="textures/clouds.dds"&&resolved.luminance=="textures/clouds_night.dds"&&resolved.environment.empty()&&resolved.normal=="textures/authored_n.dds"&&resolved.lightmap=="lightmaps/authored.dds"&&resolved.flags==13&&resolved.color==sky.materials[0].color,"source skydome isolates exact primary/fade channels without mutating shared nonsky self-check");
}
void scene_loaded_bindings_self_test() {
 // Synthetic decoded DDS exercises binding preparation; original archive/provider proof remains in render_integration.py.
 std::vector<std::uint8_t> dds(132);const auto store=[&](std::size_t p,std::uint32_t value){for(unsigned i=0;i<4;++i)dds[p+i]=std::uint8_t(value>>(8*i));};
 store(0,0x20534444);store(4,124);store(8,0x100f);store(12,1);store(16,1);store(20,4);store(28,1);store(76,32);store(80,0x41);store(88,32);store(92,0xff);store(96,0xff00);store(100,0xff0000);store(104,0xff000000);store(108,0x1000);store(128,0xff21160b);
 Scene scene;scene.materials.resize(4);scene.meshes.resize(3);
 auto& base=scene.materials[0];base.diffuse="Textures\\Bound.dds";base.normal="Dormant\\Base_N.dds";
 auto& dormant=scene.materials[1];dormant.flags=13;dormant.diffuse="Textures\\Missing.dds";dormant.normal="Missing_N.dds";dormant.environment="Missing_E.dds";dormant.luminance="Missing_L.dds";dormant.lightmap="Missing_LM.dds";
 auto& bound=scene.materials[2];bound.diffuse="Textures\\Bound.dds";bound.normal="TEXTURES/Bound_N.DDS";bound.environment="Textures\\Bound_E.dds";bound.luminance="TEXTURES/Bound_L.DDS";bound.lightmap="lightmaps/eminonu/bound_lm.dds";
 scene.materials[3].diffuse="TEXTURES/BOUND.DDS";scene.meshes[0].texture=base.diffuse;scene.meshes[1].material=3;scene.meshes[1].texture="textures\\BOUND.dds";scene.meshes[2].material=1;scene.meshes[2].texture="Textures\\Unused.dds";
 Instance instance;instance.material=2;scene.instances.push_back(instance);instance.mesh=1;instance.material=UINT32_MAX;scene.instances.push_back(instance);
 for(const auto* member:{"textures/Bound.dds","textures/Bound_N.dds","textures/Bound_E.dds","textures/Bound_L.dds","Lightmaps/Eminonu/Bound_LM.dds"}){
  assets::Entry entry{member,0,std::uint32_t(dds.size())};auto image=assets::decode_dds(dds);
  auto texture=decoded_texture(lower(entry.path),entry.path.starts_with("Lightmaps/")?"lightmaps_4.DAT":"textures_4.DAT",entry,std::move(image));
  scene.textures.push_back(std::move(texture));
 }
 normalize_loaded_bindings(scene,scene_material_usage(scene));
 need(bound.diffuse=="textures/bound.dds"&&bound.normal=="textures/bound_n.dds"&&bound.environment=="textures/bound_e.dds"&&bound.luminance=="textures/bound_l.dds"&&bound.lightmap=="lightmaps/eminonu/bound_lm.dds","loaded material channels use exact decoded keys self-check");
 need(scene.meshes[0].texture==bound.diffuse&&scene.meshes[1].texture==bound.diffuse&&scene.materials[3].diffuse==bound.diffuse&&scene.textures.size()==5,"loaded override and mesh fallback share one canonical texture self-check");
 need(base.diffuse=="Textures\\Bound.dds"&&base.normal=="Dormant\\Base_N.dds"&&dormant.flags==13&&dormant.diffuse=="Textures\\Missing.dds"&&dormant.normal=="Missing_N.dds"&&dormant.environment=="Missing_E.dds"&&dormant.luminance=="Missing_L.dds"&&dormant.lightmap=="Missing_LM.dds"&&scene.meshes[2].texture=="Textures\\Unused.dds","loaded normalization leaves dormant declarations and meshes untouched self-check");
 Scene shared;shared.materials.resize(1);shared.materials[0].normal="textures/bound_n.dds";shared.meshes.resize(2);shared.meshes[0].texture="Dormant\\Legacy.dds";shared.meshes[1].texture.clear();Instance hidden;hidden.source_ordinary_color_participates=false;shared.instances.push_back(hidden);Instance visible;visible.mesh=1;shared.instances.push_back(visible);shared.textures=scene.textures;
 const auto shared_usage=scene_material_usage(shared);need(shared_usage==std::vector<std::uint8_t>{1}&&material_resources(shared,shared_usage).textures==std::set<std::string>{"textures/bound_n.dds"},"shared eligible material demands normal without hidden legacy diffuse");
 normalize_loaded_bindings(shared,shared_usage);need(shared.meshes[0].texture=="Dormant\\Legacy.dds"&&!scene_source_color_draw(shared,0,UINT32_MAX,false)&&scene_source_color_draw(shared,1),"hidden shared-material placement never normalizes dormant legacy binding");
 const auto invalid_hidden=[&](const char* message){for(bool normalize_binding:{false,true}){bool rejected=false;try{if(normalize_binding)normalize_loaded_bindings(shared,shared_usage);else(void)scene_material_usage(shared);}catch(const std::runtime_error&){rejected=true;}need(rejected,message);}};
 shared.instances[0].mesh=2;invalid_hidden("hidden placement still validates mesh");shared.instances[0].mesh=0;shared.instances[0].material=1;invalid_hidden("hidden placement still validates material");
 const auto rejects=[&](const char* message){bool failed=false;try{normalize_loaded_bindings(scene,scene_material_usage(scene));}catch(const std::runtime_error&){failed=true;}need(failed,message);};
 bound.normal="Textures\\Missing.dds";rejects("loaded unknown material binding accepted");bound.normal="textures/bound_n.dds";
 scene.instances.clear();scene.meshes.resize(1);scene.meshes[0].material=2;scene.meshes[0].texture="TEXTURES\\BOUND.DDS";normalize_loaded_bindings(scene,scene_material_usage(scene));need(scene.meshes[0].texture==bound.diffuse,"loaded standalone preview fallback normalizes once self-check");
}
}
void scene_decoder_self_test() {
 assets::adf_decoder_self_test();scene_animation_self_test();Zone z;z.width=z.height=2048;z.cells.resize(4194304);z.heights.resize(4194304);z.cells[1024*2048+1024]=255;z.heights[1024*2048+1024]=12.5f;need(zone_height(z,0,0)==12.5f,"zone original center sample");need(!zone_height(z,1.5f,0)&&!zone_height(z,10000,0),"zone support/bounds rejection");
 z.cells[1024*2048+1023]=255;z.heights[1024*2048+1023]=7.25f;
 z.cells[1023*2048+1024]=255;z.heights[1023*2048+1024]=8.5f;
 need(zone_height(z,-.75f,0)==7.25f&&zone_height(z,0,-.75f)==8.5f&&zone_height(z,-.749f,0)==12.5f,"zone rounds scaled negative half-cell before adding center");
 need(!zone_height(z,-1536.75f,1.5f)&&!zone_height(z,1535.25f,-1536.75f),"zone rejects each out-of-bounds axis without row wrapping");
 scene_resources_self_test();
 scene_material_resources_self_test();
 scene_loaded_bindings_self_test();
 need(detail::source_sort_self_test(),"original triangle material ordering self-check");
 Scene daylight;set_scene_environment(daylight,0,555);need(daylight.lighting.sun_direction[1]>.98f&&daylight.lighting.ambient_multiplier[0]==.784f&&daylight.lighting.diffuse_multiplier[2]==.432f&&daylight.lighting.lightmap_strength==.304f&&daylight.lighting.sky_b[0]==50*.0039216f,"source light curve and 0041c9a0 zone constants self-check");
 set_scene_clock(daylight,3.5f,{255,0,0,0,0,0,0,0,0,51,255});need(daylight.lighting.glow==1&&daylight.lighting.sky_blend==.75f&&daylight.lighting.ambient_multiplier[0]==1&&daylight.lighting.lightmap_strength==.2f,"004a4c90 night ramp and 004b6970 record self-check");
 set_scene_clock(daylight,23.5f,{});need(daylight.lighting.sky_blend==.25f,"004a4c90 evening ramp self-check");set_scene_clock(daylight,12,{});need(daylight.lighting.sky_blend==0,"004a4c90 day self-check");
 set_scene_environment(daylight,4);need(daylight.lighting.source_minute==590,"source zone clock initializer self-check");
 const auto store=[](std::vector<std::uint8_t>& b,std::size_t p,std::uint32_t w){for(unsigned i=0;i<4;++i)b.at(p+i)=std::uint8_t(w>>(8*i));};
 const auto real=[&](std::vector<std::uint8_t>& b,std::size_t p,float f){store(b,p,std::bit_cast<std::uint32_t>(f));};
 std::vector<assets::AdfChunk> chunks{{"mate",0,std::vector<std::uint8_t>(468)},{"item",1,std::vector<std::uint8_t>(256)},{"vert",1,std::vector<std::uint8_t>(144)},{"tris",1,std::vector<std::uint8_t>(160)},{"qkey",1,std::vector<std::uint8_t>(64)}};
 store(chunks[0].data,0,7);store(chunks[0].data,4,1);std::copy_n("self.dds",9,chunks[0].data.begin()+20);store(chunks[1].data,8,1);store(chunks[1].data,20,1);
 for(unsigned i=0;i<3;++i){store(chunks[2].data,i*48,i);real(chunks[2].data,i*48+12,i==1?10.f:0.f);real(chunks[2].data,i*48+20,i==2?10.f:0.f);real(chunks[2].data,i*48+28,1);store(chunks[3].data,i*4,i);real(chunks[3].data,12+i*32,float(i)/2);real(chunks[3].data,16+i*32,.25f);}
 store(chunks[3].data,156,7);real(chunks[4].data,44,1);auto s=decode_scene(chunks);
 need(s.meshes.size()==1&&s.instances.size()==1&&s.meshes[0].vertices[0].z==1&&s.meshes[0].vertices[0].v==.75f,"ADF geometry scale/winding/UV self-check");
 {
  Scene owned;owned.actor_tables.libraries.resize(1);owned.actor_tables.libraries[0].clips={{2,2,0,"Rest",0,10,0,10,true}};
  ActorAnimation clock;clock.library=0;SceneActorAssembly first,second;first.entity_id=11;second.entity_id=22;
  auto animated=s;animated.animation={{0,{{0,{0,0,0,1},{0,0,0}},{1,{0,0,0,1},{9,0,0}}}}};
  install_assembly(owned,animated,clock,first,false);install_assembly(owned,animated,clock,second,false);update_scene(owned,.5f);update_scene(owned,1.1f);
  const auto before=scene_sizes(owned);const auto time=owned.actors[0].time;
  auto replacement=animated;replacement.meshes[0].vertices[0].x=77;install_assembly(owned,std::move(replacement),clock,first,false);
  need(scene_sizes(owned)==before&&owned.actors.size()==2&&owned.actors[0].time==time&&owned.actors[1].time==time&&owned.meshes[0].key==2&&owned.meshes[1].key==3,"actor replacement preserves one clock and rekeys only the replaced geometry");
  need(scene_mesh_remap({1,2},owned)==std::vector<std::size_t>{1,SIZE_MAX},"actor replacement retains the surviving actor's uploaded mesh");
  need(owned.instances[0].entity_id==22&&owned.instances[0].mesh==0&&owned.instances[0].node==0&&owned.instances[1].entity_id==11&&owned.meshes[1].vertices[0].x==77,"actor replacement deletes obsolete geometry and rebases surviving entity");
  need(owned.nodes[owned.instances[1].node].local.matrix[12]==4.5f,"actor replacement preserves sparse held pose rather than rewinding its track cache");
  detach_source_actor(owned,22);need(owned.actors.size()==1&&owned.actor_assemblies[0].controller==0&&owned.instances[0].entity_id==11&&owned.instances[0].node==0&&scene_mesh_remap({2,3},owned)==std::vector<std::size_t>{1},"actor AOI retirement deletes/rebases geometry/controller and moves survivor resources");
  detach_source_actor(owned,11);need(owned.nodes.empty()&&owned.meshes.empty()&&owned.animation.empty()&&owned.actors.empty()&&owned.actor_assemblies.empty(),"actor AOI retirement retains no obsolete parts");
  // AOI churn over a keyless zone prefix: a renderer that remaps its last-uploaded keys never re-uploads a zone mesh.
  Scene zone;zone.actor_tables=owned.actor_tables;append_model(zone,s);const auto base=zone.meshes.size();std::vector<std::uint64_t> uploaded;
  const auto upload=[&]{
   const auto remap=scene_mesh_remap(uploaded,zone);
   for(std::size_t i=0;i<base;++i)need(remap[i]==(uploaded.empty()?SIZE_MAX:i)&&!zone.meshes[i].key,"actor attach/detach retains every zone mesh at its index");
   uploaded.clear();for(const auto& mesh:zone.meshes)uploaded.push_back(mesh.key);return remap;
  };
  upload();
  for(unsigned cycle=0;cycle<8;++cycle){
   for(std::uint32_t id:{31U,32U,33U}){SceneActorAssembly actor;actor.entity_id=id;install_assembly(zone,animated,clock,actor,false);}
   need(upload()==std::vector<std::size_t>{0,SIZE_MAX,SIZE_MAX,SIZE_MAX},"actor entry uploads only its own meshes");
   detach_source_actor(zone,32);need(upload()==std::vector<std::size_t>{0,1,3},"actor exit moves later actor meshes without upload");
   SceneActorAssembly actor;actor.entity_id=31;auto changed=animated;changed.meshes[0].vertices[0].x=5;install_assembly(zone,std::move(changed),clock,actor,false);
   need(upload()==std::vector<std::size_t>{0,2,SIZE_MAX},"actor replacement uploads only the replaced actor");
   detach_source_actor(zone,31);detach_source_actor(zone,33);need(upload()==std::vector<std::size_t>{0}&&zone.meshes.size()==base&&zone.actor_assemblies.empty()&&zone.actors.empty(),"AOI churn leaves only the zone");
  }
 }
 const auto consumer_fixture=chunks;
 const auto decode_consumer=[](const std::vector<assets::AdfChunk>& input,const char* label){try{return decode_scene(input);}catch(const std::exception& error){throw std::runtime_error(std::string("ADF consumer fixture ")+label+": "+error.what());}};
 {
  const auto pair=[&](std::string_view parent_name,std::string_view child_name,bool attached=true){
   auto fixture=consumer_fixture;std::fill_n(fixture[1].data.begin()+192,64,0);std::copy(parent_name.begin(),parent_name.end(),fixture[1].data.begin()+192);
   auto child=fixture[1];store(child.data,8,2);store(child.data,16,attached?1:0);store(child.data,28,1);std::copy(parent_name.begin(),parent_name.end(),child.data.begin()+128);std::fill_n(child.data.begin()+192,64,0);std::copy(child_name.begin(),child_name.end(),child.data.begin()+192);fixture.push_back(std::move(child));
   return decode_consumer(fixture,"ordinary color enrollment hierarchy");
  };
  for(auto excluded:{"walkset_root","nocamera_root","sky_root","occ_root"}){auto value=pair(excluded,"shop457");need(value.instances.size()==2&&value.instances[1].source_ordinary_color_participates&&value.meshes[0].indices.size()==3,"ordinary child independently registered beneath excluded or no-op root");}
  for(auto recursive:{"ordinary_root","ocean_root","isik_huzme_root"}){auto value=pair(recursive,"walkset_child");need(value.instances[1].source_ordinary_color_participates&&value.instances[1].source_support,"generic and delegated color virtual reaches retained helper child");}
  for(auto terminal:{"skydome_root","occ_root"}){auto value=pair(terminal,"nocamera_child");need(!value.instances[1].source_ordinary_color_participates&&value.instances[1].source_nocamera,"sky and occlusion color virtual do not recurse into helper child");}
  const auto structural=[&](std::string_view name,unsigned parent_type,unsigned child_type){
   auto fixture=consumer_fixture;store(fixture[1].data,20,parent_type);std::copy(name.begin(),name.end(),fixture[1].data.begin()+192);
   auto child=consumer_fixture[1];store(child.data,8,2);store(child.data,16,1);store(child.data,20,child_type);std::copy_n("walkset_child",14,child.data.begin()+192);fixture.insert(fixture.begin()+2,std::move(child));
   return decode_consumer(fixture,"structural source color virtual");
  };
  need(structural("walkset_container",11,1).instances[0].source_ordinary_color_participates&&!structural("walkset_container",11,4).instances[0].source_ordinary_color_participates,"empty particle fallback c_ seed visits only child with source +221 zero");
  need(!structural("particle_",11,1).instances[0].source_ordinary_color_participates&&!structural("Particle_",11,1).instances[0].source_ordinary_color_participates&&!structural("particle_container",11,1).instances[0].source_ordinary_color_participates&&structural("particle",11,1).instances[0].source_ordinary_color_participates&&!structural("ordinary_container",5,1).instances[0].source_ordinary_color_participates,"particle provider uses the case-folded \"particle_\" prefix (004a0890) and has no child color recursion; unsupported records are not generic seeds");
  auto detached=pair("ordinary_root","walkset_child",false);need(!detached.instances[1].source_ordinary_color_participates&&detached.nodes[1].source_reference==0,"clone source reference is not ordinary color parent");
  auto value=pair("ordinary_root","walkset_child");value.materials[0].source_color_draw=false;value.materials.emplace_back();value.instances[1].material=1;need(scene_material_usage(value)==std::vector<std::uint8_t>{0,1},"no-color parent material does not block reachable child");
  auto duplicate=pair("skydome_root","skydome_root");need(duplicate.nodes[0].id!=duplicate.nodes[1].id&&duplicate.instances[0].source_ordinary_color_participates&&duplicate.instances[1].source_ordinary_color_participates,"duplicate names retain independently classified placements");
  auto upper=pair("Walkset_root","Nocamera_child");need(upper.instances[0].source_ordinary_color_participates&&upper.instances[1].source_ordinary_color_participates,"source ordinary color prefix classifier is case sensitive");
  Scene combined;append_model(combined,std::move(detached));append_model(combined,std::move(duplicate));need(!combined.instances[1].source_ordinary_color_participates&&combined.instances[3].source_ordinary_color_participates&&combined.nodes[3].source_reference==2,"member append preserves color enrollment and scoped clone handles");
  auto bone_fixture=consumer_fixture;std::copy_n("walkset_root",13,bone_fixture[1].data.begin()+192);auto color_bone=consumer_fixture[1];color_bone.tag="bone";store(color_bone.data,8,2);store(color_bone.data,12,1);store(color_bone.data,16,1);std::copy_n("ordinary_bone",14,color_bone.data.begin()+192);bone_fixture.push_back(std::move(color_bone));
  auto bone_child=consumer_fixture[1];store(bone_child.data,8,3);store(bone_child.data,16,2);store(bone_child.data,28,1);std::copy_n("walkset_root",13,bone_child.data.begin()+128);std::copy_n("walkset_child",14,bone_child.data.begin()+192);bone_fixture.push_back(std::move(bone_child));const auto bone_color=decode_consumer(bone_fixture,"bone unfiltered child color virtual");
  need(bone_color.nodes[1].source_bone&&bone_color.instances[1].source_ordinary_color_participates&&bone_color.instances[1].source_support,"independently registered bone color virtual visits helper child without classifier recursion");
  const auto cached_actor=pair("walkset_BODY","ordinary_child");Scene assembled=cached_actor;std::vector<std::size_t> projection;source_actor_geometry(assembled,0,projection);
  auto composite=pair("nocamera_part","ordinary_child");source_actor_geometry(composite,0,projection,assembled.instances.size());remap_bodypart(assembled,std::move(composite),0,1);
  auto rigid=pair("sky_item","ordinary_unrelated",false);source_actor_geometry(rigid,0,projection,assembled.instances.size());append_model(assembled,std::move(rigid));source_actor_color_participation(assembled,projection);
  for(std::size_t i=0;i<assembled.instances.size();++i)need(assembled.instances[i].source_ordinary_color_participates==(i%2==0),"actor projects only selected own-root aggregate or rigid leaf subsets");
  need(assembled.instances.size()==6&&assembled.nodes.size()==6&&assembled.meshes.size()==3&&assembled.meshes[0].indices.size()==3&&assembled.instances[0].source_support&&assembled.instances[2].source_nocamera&&assembled.instances[4].sky&&!cached_actor.instances[0].source_ordinary_color_participates&&cached_actor.instances[1].source_ordinary_color_participates,"owned actor projection preserves all CPU geometry/helper metadata and immutable cached enrollment");
 }
 auto resource_chunks=chunks;store(resource_chunks[0].data,4,45);std::copy_n("bound.dds",10,resource_chunks[0].data.begin()+20);std::copy_n("bound_n.dds",12,resource_chunks[0].data.begin()+212);std::copy_n("bound_e.dds",12,resource_chunks[0].data.begin()+276);std::copy_n("bound_l.dds",12,resource_chunks[0].data.begin()+340);
 assets::AdfChunk dormant_material{"mate",0,std::vector<std::uint8_t>(468)};store(dormant_material.data,0,0x2ae1e957);store(dormant_material.data,4,13);std::copy_n("merannormal R1.dds",18,dormant_material.data.begin()+20);std::copy_n("merannormal1.dds",16,dormant_material.data.begin()+212);std::copy_n("missing_e.dds",14,dormant_material.data.begin()+276);resource_chunks.push_back(dormant_material);
 dormant_material.data.assign(468,0);store(dormant_material.data,0,0x1bd3aa54);store(dormant_material.data,4,1);std::copy_n("zemin_ingiliztasi.dds",21,dormant_material.data.begin()+20);resource_chunks.push_back(dormant_material);
 assets::AdfChunk bound_lightmap{"lmpd",1,std::vector<std::uint8_t>(76)};store(bound_lightmap.data,0,1);std::copy_n("bound_lm.dds",13,bound_lightmap.data.begin()+4);resource_chunks.push_back(bound_lightmap);
 auto resource_scene=decode_scene(resource_chunks);const auto resource_usage=scene_material_usage(resource_scene);const auto requested=material_resources(resource_scene,resource_usage);
 need(resource_usage==std::vector<std::uint8_t>{0,0,0,1}&&resource_scene.meshes[0].material==0&&resource_scene.meshes[0].texture=="textures/bound.dds"&&resource_scene.instances[0].material==3,"ADF bound lightmap copy preserves declared slots and legacy mesh texture self-check");
 need(requested.textures==std::set<std::string>{"textures/bound.dds","textures/bound_n.dds","textures/bound_e.dds","textures/bound_l.dds"}&&requested.lightmaps,"ADF actual resource gathering uses referenced copy channels self-check");
 need(resource_scene.materials[0].lightmap.empty()&&resource_scene.materials[1].flags==13&&resource_scene.materials[1].diffuse=="textures/merannormal R1.dds"&&resource_scene.materials[1].normal=="textures/merannormal1.dds"&&resource_scene.materials[2].flags==1&&resource_scene.materials[2].diffuse=="textures/zemin_ingiliztasi.dds"&&resource_scene.materials[3].lightmap=="bound_lm.dds","ADF dormant declared missing paths and authored override lightmap remain intact self-check");
 auto ocean_chunks=resource_chunks;store(ocean_chunks[0].data,8,1);real(ocean_chunks[0].data,12,1);std::copy_n("shared_water",13,ocean_chunks[1].data.begin()+192);
 for(const auto& [id,name]:std::array<std::pair<unsigned,const char*>,3>{{{2,"ocean_test"},{3,"isik_huzme_test"},{4,"Ocean_test"}}}) {
  auto placement=ocean_chunks[1];store(placement.data,8,id);store(placement.data,28,1);std::copy_n("shared_water",13,placement.data.begin()+128);std::fill_n(placement.data.begin()+192,64,0);std::copy_n(name,std::char_traits<char>::length(name)+1,placement.data.begin()+192);ocean_chunks.push_back(std::move(placement));
 }
 auto ocean_lightmap=bound_lightmap;store(ocean_lightmap.data,0,2);ocean_chunks.push_back(std::move(ocean_lightmap));auto ocean_scene=decode_scene(ocean_chunks);
 const auto& water_instance=ocean_scene.instances[1];const auto& water=ocean_scene.materials[water_instance.material];const auto& original_water=ocean_scene.materials[0];
 need(ocean_scene.meshes.size()==1&&ocean_scene.instances.size()==4&&ocean_scene.materials.size()==5&&water_instance.mesh==ocean_scene.instances[0].mesh&&water_instance.ocean&&water.source_ocean,"ADF ocean placement selects isolated material without mesh copy");
 need(water.normal=="textures/ocean_bump.dds"&&water.environment.empty()&&water.diffuse==original_water.diffuse&&water.flags==original_water.flags&&water.alpha==original_water.alpha&&water.alpha_cutoff==original_water.alpha_cutoff&&water.double_sided==original_water.double_sided&&water.color==original_water.color&&water.source_color_draw==original_water.source_color_draw&&water.luminance==original_water.luminance&&water.lightmap=="bound_lm.dds","ADF ocean override preserves authored diffuse/state/lightmap and replaces only source normal/environment");
 need(!original_water.source_ocean&&original_water.normal=="textures/bound_n.dds"&&original_water.environment=="textures/bound_e.dds"&&!ocean_scene.materials[ocean_scene.instances[0].material].source_ocean&&ocean_scene.instances[2].material==UINT32_MAX&&ocean_scene.instances[2].ocean&&ocean_scene.instances[3].material==UINT32_MAX&&!ocean_scene.instances[3].ocean,"ADF shared ordinary/isik_huzme/case-distinct placements retain original material and class selection");
 const auto ocean_requested=material_resources(ocean_scene,scene_material_usage(ocean_scene));
 need(ocean_requested.textures==std::set<std::string>{"textures/bound.dds","textures/bound_n.dds","textures/bound_e.dds","textures/bound_l.dds","textures/ocean_bump.dds"}&&ocean_requested.lightmaps,"ADF ocean original bump enters existing deduplicated resource resolver");
 auto hidden_ocean=ocean_chunks;store(hidden_ocean[0].data,4,44);auto hidden_water=decode_scene(hidden_ocean);const auto& hidden_material=hidden_water.materials[hidden_water.instances[1].material];
 need(hidden_material.source_ocean&&!hidden_material.source_color_draw&&hidden_material.diffuse.empty()&&material_resources(hidden_water,scene_material_usage(hidden_water)).textures.empty(),"ADF ocean override cannot bypass source primary-diffuse residency gate");
 auto shared=chunks;real(shared[3].data,12,0);real(shared[3].data,16,0);real(shared[3].data,44,1);real(shared[3].data,48,0);real(shared[3].data,76,0);real(shared[3].data,80,1);auto shared_triangle=shared[3].data;shared[3].data.insert(shared[3].data.end(),shared_triangle.begin(),shared_triangle.end());auto indexed=decode_scene(shared);
 need(indexed.meshes[0].vertices.size()==3&&indexed.meshes[0].indices==std::vector<std::uint32_t>{0,1,2,0,1,2},"ADF identical corners share vertices and retain every reversed triangle self-check");
 for(const auto& vertex:indexed.meshes[0].vertices)need(vertex.tangent==std::array<float,3>{1,0,0},"ADF shared-index consistent tangent self-check");
 auto adjacent=shared;adjacent[2].data.resize(192);real(adjacent[2].data,144+16,10);real(adjacent[2].data,144+28,1);store(adjacent[3].data,160+4,3);auto smooth=decode_scene(adjacent);need(smooth.meshes[0].vertices.size()==4&&smooth.meshes[0].indices==std::vector<std::uint32_t>{0,1,2,0,3,2},"ADF adjacent triangles retain shared source vertices self-check");for(auto vi:{0U,2U})need(std::abs(smooth.meshes[0].vertices[vi].tangent[0]-.70710678f)<1e-6f&&std::abs(smooth.meshes[0].vertices[vi].tangent[1]-.70710678f)<1e-6f&&smooth.meshes[0].vertices[vi].tangent[2]==0,"ADF shared tangent accumulates incident faces instead of last writer self-check");
 auto seams=shared;real(seams[3].data,160+12,.25f);auto uv0_seam=decode_scene(seams);need(uv0_seam.meshes[0].vertices.size()==4&&uv0_seam.meshes[0].indices==std::vector<std::uint32_t>{0,1,2,0,1,3},"ADF UV0 seam keeps distinct indexed vertex self-check");
 seams=shared;seams.push_back(bound_lightmap);real(seams[3].data,160+20,.25f);auto uv1_seam=decode_scene(seams);need(uv1_seam.meshes[0].vertices.size()==4&&uv1_seam.meshes[0].vertices[3].uv1[0]==.25f,"ADF bound UV1 seam keeps distinct indexed vertex self-check");
 seams=shared;real(seams[3].data,160+108,.5f);auto color_seam=decode_scene(seams);need(color_seam.meshes[0].vertices.size()==4&&color_seam.meshes[0].vertices[3].color[0]==127.f/255,"ADF quantized color seam keeps distinct indexed vertex self-check");
 seams=shared;real(seams[3].data,160+108,.001f);need(decode_scene(seams).meshes[0].vertices.size()==3,"ADF equal emitted quantized colors share vertex self-check");
 seams=shared;real(seams[3].data,160+12,-0.f);need(decode_scene(seams).meshes[0].vertices.size()==4,"ADF emitted UV bit patterns remain distinct self-check");
 auto identity=shared;std::copy_n(identity[2].data.begin(),48,identity[2].data.begin()+48);std::copy_n(identity[3].data.begin()+12,32,identity[3].data.begin()+44);std::copy_n(identity[3].data.begin()+160+12,32,identity[3].data.begin()+160+44);auto identities=decode_scene(identity);need(identities.meshes[0].vertices.size()==3&&identities.meshes[0].indices.size()==6&&identities.meshes[0].indices[1]!=identities.meshes[0].indices[2],"ADF coincident authored source identities remain distinct self-check");
 auto partitioned=shared;auto extra_material=partitioned[0];store(extra_material.data,0,8);partitioned.push_back(std::move(extra_material));store(partitioned[3].data,160+156,8);auto partitions=decode_scene(partitioned);need(partitions.meshes.size()==2&&partitions.meshes[0].vertices.size()==3&&partitions.meshes[1].vertices.size()==3&&partitions.meshes[0].indices.size()==3&&partitions.meshes[1].indices.size()==3,"ADF material partitions retain independent indexed vertices self-check");
 auto ambient_shared=shared;std::copy_n("shared",7,ambient_shared[1].data.begin()+192);assets::AdfChunk source_ambient{"ambd",1,std::vector<std::uint8_t>(16)};store(source_ambient.data,0,1);for(unsigned i=0;i<3;++i)store(source_ambient.data,4+i*4,0xff000000U|(17U+i)<<16);ambient_shared.push_back(source_ambient);
 for(unsigned id=2;id<=3;++id){auto ambient_clone=ambient_shared[1];store(ambient_clone.data,8,id);store(ambient_clone.data,28,1);std::copy_n("shared",7,ambient_clone.data.begin()+128);ambient_shared.push_back(std::move(ambient_clone));auto clone_ambient=source_ambient;store(clone_ambient.data,0,id);for(unsigned i=0;i<3;++i)store(clone_ambient.data,4+i*4,0xff000000U|(id*17U+i)<<16);ambient_shared.push_back(std::move(clone_ambient));}
 auto ambient_instances=decode_scene(ambient_shared);need(ambient_instances.meshes.size()==3&&ambient_instances.instances.size()==3,"ADF authored ambient clones retain separate meshes self-check");for(unsigned i=0;i<3;++i){const auto& mesh=ambient_instances.meshes[ambient_instances.instances[i].mesh];need(mesh.vertices.size()==3&&mesh.indices==indexed.meshes[0].indices,"ADF authored ambient clones retain all shared indices self-check");for(unsigned vi=0;vi<3;++vi)need(mesh.vertices[vi].ambient==std::array<float,4>{float((i+1)*17+2-vi)/255,0,0,1},"ADF authored source/clone ambient streams stay distinct self-check");}
 auto ambient_resize=ambient_shared;for(auto& chunk:ambient_resize)if(chunk.tag=="ambd"){const auto id=word(chunk.data,0);if(id==2){chunk.data.resize(13);chunk.data[12]=0xa5;}else if(id==3){chunk.data.resize(24);store(chunk.data,16,0xff112233);store(chunk.data,20,0xff445566);}}
 auto resized_ambient=decode_consumer(ambient_resize,"COLOR1 grow/shrink/partial-DWORD");const auto& grown_colors=resized_ambient.meshes[resized_ambient.instances[1].mesh].vertices;need(grown_colors[0].ambient==std::array<float,4>{1,1,1,1}&&grown_colors[1].ambient==std::array<float,4>{35.f/255,0,0,1}&&grown_colors[2].ambient==std::array<float,4>{34.f/255,0,0,1},"ADF original COLOR1 growth preserves prefix, constructs missing words, and ignores partial DWORD bytes");
 const auto& shrunk_colors=resized_ambient.meshes[resized_ambient.instances[2].mesh].vertices;for(unsigned vi=0;vi<3;++vi)need(shrunk_colors[vi].ambient==ambient_instances.meshes[ambient_instances.instances[2].mesh].vertices[vi].ambient,"ADF original COLOR1 shrink preserves only referenced source vertex prefix");
 auto default_streams=ambient_shared;std::erase_if(default_streams,[](const auto& chunk){return chunk.tag=="ambd"&&word(chunk.data,0)!=1;});auto default_ambient=decode_consumer(default_streams,"independent default COLOR1 streams");need(default_ambient.meshes.size()==2&&default_ambient.instances[1].mesh==default_ambient.instances[2].mesh&&default_ambient.instances[1].mesh!=default_ambient.instances[0].mesh,"ADF independent default COLOR1 clones share geometry without inheriting prototype colors");for(const auto& vertex:default_ambient.meshes[default_ambient.instances[1].mesh].vertices)need(vertex.ambient==std::array<float,4>{1,1,1,1},"ADF absent per-instance COLOR1 uses exact original constructor words");
 store(chunks[3].data,0,3);bool rejected=false;try{(void)decode_scene(chunks);}catch(const std::runtime_error&){rejected=true;}need(rejected,"ADF out-of-range triangle accepted");
 chunks[3].tag="ptri";chunks[3].data.assign(116,0);store(chunks[3].data,112,7);for(unsigned i=0;i<3;++i){chunks[3].data[i*2]=std::uint8_t(i);real(chunks[3].data,8+i*32,float(i)/2);real(chunks[3].data,12+i*32,.25f);real(chunks[3].data,16+i*32,.5f);real(chunks[3].data,20+i*32,.125f);chunks[3].data[104+i*2]=chunks[3].data[105+i*2]=255;}
 auto packed=decode_scene(chunks);need(packed.meshes[0].vertices[0].z==1&&packed.meshes[0].vertices[0].uv1==std::array<float,2>{}&&packed.meshes[0].vertices[0].color[0]==240.f/255,"ADF packed triangle/inactive UV1/ARGB4 self-check");auto packed_lm=chunks;packed_lm.push_back(bound_lightmap);need(decode_scene(packed_lm).meshes[0].vertices[0].uv1[1]==.875f,"ADF packed bound UV1 self-check");
 auto packed_shared=chunks;auto packed_triangle=packed_shared[3].data;packed_shared[3].data.insert(packed_shared[3].data.end(),packed_triangle.begin(),packed_triangle.end());auto packed_indexed=decode_scene(packed_shared);need(packed_indexed.meshes[0].vertices.size()==3&&packed_indexed.meshes[0].indices==std::vector<std::uint32_t>{0,1,2,0,1,2},"ADF packed triangles retain all indices while sharing exact corners self-check");packed_shared[3].data[116+104]=254;need(decode_scene(packed_shared).meshes[0].vertices.size()==4,"ADF packed ARGB4 color seam remains distinct self-check");
 const auto reject=[](const std::vector<assets::AdfChunk>& input,const char* message){bool failed=false;try{(void)decode_scene(input);}catch(const std::runtime_error&){failed=true;}need(failed,message);};
 auto bad_ambient_header=ambient_shared;for(auto& chunk:bad_ambient_header)if(chunk.tag=="ambd"&&word(chunk.data,0)==2)chunk.data.resize(3);reject(bad_ambient_header,"ADF truncated ambient target header accepted");
 auto unused=chunks;std::fill_n(unused[1].data.begin()+128,64,0xff);std::fill_n(unused[0].data.begin()+404,64,0xff);auto inactive=decode_scene(unused);
 need(inactive.meshes.size()==1&&inactive.materials.size()==1&&inactive.materials[0].name.empty(),"ADF inactive item reuse/material name bytes self-check");
 auto duplicate=unused[0];real(duplicate.data,8,1);std::fill_n(duplicate.data.begin()+20,64,0xff);unused.push_back(std::move(duplicate));auto first=decode_scene(unused);
 need(first.materials.size()==1&&!first.materials[0].double_sided,"ADF duplicate material first registration self-check");
 auto malformed=chunks;std::fill_n(malformed[1].data.begin()+192,64,0xff);reject(malformed,"ADF unterminated own name accepted");
 malformed=unused;store(malformed[1].data,28,1);reject(malformed,"ADF unterminated active clone name accepted");
 malformed=chunks;store(malformed[0].data,4,1);std::fill_n(malformed[0].data.begin()+20,64,0xff);reject(malformed,"ADF unterminated active texture path accepted");
 auto skinned=unused;assets::AdfChunk bone{"bone",2,std::vector<std::uint8_t>(256)};store(bone.data,8,2);store(bone.data,12,1);store(bone.data,16,1);store(bone.data,20,UINT32_MAX);store(bone.data,28,UINT32_MAX);std::fill_n(bone.data.begin()+128,64,0xff);std::copy_n("BIP_test",8,bone.data.begin()+192);skinned.push_back(bone);
 assets::AdfChunk skin{"wmap",2,std::vector<std::uint8_t>(96)};for(unsigned i=0;i<3;++i){store(skin.data,i*32,i);real(skin.data,i*32+4,1);real(skin.data,i*32+28,1);}skinned.push_back(std::move(skin));auto bone_key=chunks[4];bone_key.id=2;skinned.push_back(std::move(bone_key));auto skeleton=decode_scene(skinned);
 need(skeleton.nodes.size()==2&&skeleton.nodes[1].parent==0&&skeleton.nodes[1].source_reference==-1&&skeleton.meshes[0].influences.size()==3&&skeleton.animation.size()==2,"ADF inactive bone fields/skin/animation self-check");
 {
  auto actor_model=skeleton;actor_model.nodes[0].name="BODY";
  actor_model.animation[0].keys={{0,{-.70710677f,0,0,.70710677f},{0,5,0}}};
  actor_model.animation[1].keys={{0,{0,0,0,1},{0,0,0}},{1,{0,0,0,1},{0,2,0}}};
  for(auto& track:actor_model.animation)track.controller=0;
  for(auto& influence:actor_model.meshes[0].influences)influence.position={0,1,0};
  actor_model.actor_tables.libraries.resize(1);actor_model.actor_tables.libraries[0].clips={{2,2,0,"Rest",0,10,0,10,true}};
  ActorAnimation clock;clock.library=0;actor_model.actors.push_back(clock);update_scene(actor_model,0);
  SourceActorRecord record;record.model="BODY";initialize_source_actor_root(actor_model,record);update_scene(actor_model,.5f);
  const auto& point=actor_model.meshes[0].vertices[0];
  need(point.x==0&&point.y==2&&point.z==0&&actor_model.nodes[1].world.matrix[13]==1&&actor_model.actors[0].time==.5f,
       "source actor constructor skins live BIP under fresh identity instead of archived BODY ITEM pose");
 }
 auto shared_skin=skinned;shared_skin[3]=shared[3];auto& shared_map=shared_skin[shared_skin.size()-2].data;for(unsigned i=0;i<3;++i){real(shared_map,i*32+4,.5f);real(shared_map,i*32+8,-20);real(shared_map,i*32+12,30);real(shared_map,i*32+16,40);}auto indexed_skin=decode_scene(shared_skin);
 need(indexed_skin.meshes[0].vertices.size()==3&&indexed_skin.meshes[0].indices.size()==6&&indexed_skin.meshes[0].influences.size()==3,"ADF shared-index authored influence appended once per unique vertex self-check");for(const auto& vertex:indexed_skin.meshes[0].vertices)need(vertex.x==1&&vertex.y==2&&vertex.z==1.5f,"ADF shared-index authored skin accumulation applied once self-check");
 auto skin_seams=shared_skin;real(skin_seams[3].data,160+12,.25f);auto indexed_skin_seam=decode_scene(skin_seams);need(indexed_skin_seam.meshes[0].vertices.size()==4&&indexed_skin_seam.meshes[0].indices.size()==6&&indexed_skin_seam.meshes[0].influences.size()==4,"ADF authored influence retained once for every unique seam vertex self-check");for(const auto& vertex:indexed_skin_seam.meshes[0].vertices)need(vertex.x==1&&vertex.y==2&&vertex.z==1.5f,"ADF skin seam variant retains source influence identity self-check");
 auto zero_weights=skinned;auto& zero_map=zero_weights[zero_weights.size()-2].data;zero_map.resize(192);for(unsigned i=0;i<6;++i){store(zero_map,i*32,i%3);real(zero_map,i*32+4,0);real(zero_map,i*32+8,10);real(zero_map,i*32+28,1);}auto zero_skin=decode_scene(zero_weights);
 need(zero_skin.nodes.size()==2&&zero_skin.meshes[0].vertices.size()==3&&zero_skin.meshes[0].indices.size()==3&&zero_skin.meshes[0].influences.size()==6,"ADF authored zero weights retain bones/corners/influences self-check");
 for(const auto& influence:zero_skin.meshes[0].influences)need(influence.weight==0,"ADF zero authored influence retention self-check");
 for(const auto& vertex:zero_skin.meshes[0].vertices)need(vertex.x==0&&vertex.y==0&&vertex.z==0,"ADF zero authored weights produce source zero position self-check");
 auto absent_track=skinned;auto absent_key=chunks[4];absent_key.id=UINT32_MAX;absent_track.push_back(std::move(absent_key));auto ignored=decode_scene(absent_track);
 need(ignored.nodes.size()==skeleton.nodes.size()&&ignored.animation.size()==skeleton.animation.size()&&ignored.animation[0].keys.size()==skeleton.animation[0].keys.size()&&ignored.animation[1].keys.size()==skeleton.animation[1].keys.size()&&ignored.diagnostics==std::vector<std::string>{"Original source no-op: 004a2890 ignores qkey registry misses=1"},"ADF absent-target qkey source no-op self-check");
 auto root_geometry=consumer_fixture;store(root_geometry[1].data,20,4);for(unsigned i=0;i<3;++i)for(unsigned axis=0;axis<3;++axis)store(root_geometry[2].data,i*48+24+axis*4,UINT32_MAX);
 auto root_bone=bone;store(root_bone.data,16,2);root_geometry.push_back(root_bone);auto child_bone=bone;child_bone.id=3;store(child_bone.data,8,3);store(child_bone.data,16,2);root_geometry.push_back(std::move(child_bone));
 assets::AdfChunk unread_root_map{"wmap",2,std::vector<std::uint8_t>(96,0xff)},child_map{"wmap",3,std::vector<std::uint8_t>(96)};root_geometry.push_back(std::move(unread_root_map));for(unsigned i=0;i<3;++i){store(child_map.data,i*32,i);real(child_map.data,i*32+4,1);real(child_map.data,i*32+8,i==1?20.f:0.f);real(child_map.data,i*32+16,i==2?10.f:0.f);for(unsigned axis=0;axis<3;++axis)store(child_map.data,i*32+20+axis*4,UINT32_MAX);}root_geometry.push_back(std::move(child_map));for(unsigned id:{2U,3U}){auto key=consumer_fixture[4];key.id=id;root_geometry.push_back(std::move(key));}
 auto regenerated=decode_consumer(root_geometry,"root clear/child-weighted raw normals");need(regenerated.source_surfaces.size()==1&&regenerated.source_surfaces[0].root_clears&&regenerated.meshes[0].influences.size()==3&&regenerated.source_scalar_words.size()==18,"ADF root clear ignores root maps, preserves replaced normals and unread skin normals self-check");
 const auto& regenerated_surface=regenerated.source_surfaces[0];need(regenerated_surface.heads.size()==3&&regenerated_surface.aliases.size()==3&&regenerated_surface.indices==std::vector<std::uint32_t>{2,1,0},"ADF source corner order and alias cardinality retained through root/child skin");
 // Identity child pose consumes wmap XYZ through source basis (-x,z,y); face normal is (I0-I1)x(I2-I1) over the
 // reversed CPU index array (00469580 stores src2,src1,src0; 0047cf50 reads it), i.e. a=src2, c=src0.
 const std::array<std::array<float,3>,3> expected_positions{{{0,0,0},{-2,0,0},{0,1,0}}};std::array<float,3> ab{},cb{};
 for(unsigned axis=0;axis<3;++axis){ab[axis]=expected_positions[2][axis]-expected_positions[1][axis];cb[axis]=expected_positions[0][axis]-expected_positions[1][axis];}
 const std::array<float,3> original_cross{ab[1]*cb[2]-ab[2]*cb[1],ab[2]*cb[0]-ab[0]*cb[2],ab[0]*cb[1]-ab[1]*cb[0]};
 need(original_cross[0]*original_cross[0]+original_cross[1]*original_cross[1]+original_cross[2]*original_cross[2]==4,"ADF original source face area remains raw, not normalized or a bone-normal fallback");
 for(unsigned vi=0;vi<3;++vi){const auto ci=regenerated_surface.heads[vi];need(ci<regenerated_surface.corners.size(),"ADF source head resolves a posed corner");const auto& corner=regenerated_surface.corners[ci];const auto& vertex=regenerated.meshes[corner.mesh].vertices[corner.vertex];
  need(std::array<float,3>{vertex.x,vertex.y,vertex.z}==expected_positions[vi]&&regenerated_surface.aliases[vi]==std::array<std::uint16_t,2>{std::uint16_t(vi),std::uint16_t(vi)},"ADF child weighted positions and source identity aliases retain original basis");
  need(std::array<float,3>{vertex.nx,vertex.ny,vertex.nz}==original_cross&&vertex.nx*ab[0]+vertex.ny*ab[1]+vertex.nz*ab[2]==0&&vertex.nx*cb[0]+vertex.ny*cb[1]+vertex.nz*cb[2]==0,"ADF regenerated raw normal follows original ordered cross and remains orthogonal to posed edges");
 }
 std::fill_n(skinned[skinned.size()-3].data.begin()+192,64,0xff);reject(skinned,"ADF unterminated bone name accepted");
 std::copy_n("occ_test",8,chunks[1].data.begin()+192);auto proxy=decode_scene(chunks);need(proxy.instances[0].occlusion_proxy,"original nonvisual occlusion class self-check");
 auto missing=chunks;store(missing[3].data,112,0);assets::AdfChunk lightmap{"lmpd",1,std::vector<std::uint8_t>(76)};store(lightmap.data,0,1);std::copy_n("original/proxy.dds",18,lightmap.data.begin()+4);missing.push_back(lightmap);
 auto clone=chunks[1];store(clone.data,8,2);store(clone.data,28,1);std::copy_n("occ_test",8,clone.data.begin()+128);std::fill_n(clone.data.begin()+192,64,0);std::copy_n("occ_clone",9,clone.data.begin()+192);missing.push_back(clone);auto clone_lightmap=lightmap;store(clone_lightmap.data,0,2);missing.push_back(std::move(clone_lightmap));
 for(auto type:{1U,4U}){store(missing[1].data,20,type);store(missing[6].data,20,type);auto null_proxy=decode_scene(missing);
  need(null_proxy.meshes.size()==1&&null_proxy.meshes[0].vertices.size()==3&&null_proxy.meshes[0].indices.size()==3&&null_proxy.nodes.size()==2&&null_proxy.animation.size()==1&&null_proxy.instances.size()==2&&null_proxy.instances[0].occlusion_proxy&&null_proxy.instances[1].occlusion_proxy,"ADF missing-registry occlusion geometry/clone retention self-check");
  const auto material=null_proxy.meshes[0].material;const auto& source_null=null_proxy.materials[material];need(null_proxy.materials.size()==4&&source_null.name=="DAT_0077bf6c"&&source_null.flags==0&&source_null.color==std::array<float,4>{}&&source_null.diffuse.empty()&&source_null.normal.empty()&&source_null.environment.empty()&&source_null.luminance.empty(),"ADF original zero source-null registry material self-check");
  for(const auto& instance:null_proxy.instances){need(instance.material!=UINT32_MAX&&null_proxy.materials[instance.material].name=="DAT_0077bf6c"&&null_proxy.materials[instance.material].lightmap=="original/proxy.dds","ADF source-null proxy authored lightmap retention self-check");}
  const auto usage=scene_material_usage(null_proxy);need(usage==std::vector<std::uint8_t>{0,0,0,0}&&!material_resources(null_proxy,usage).lightmaps,"ADF retained source-null lightmaps do not demand color residency self-check");
 }
 malformed=missing;std::fill_n(malformed[1].data.begin()+192,64,0);std::copy_n("visible",7,malformed[1].data.begin()+192);std::fill_n(malformed[6].data.begin()+128,64,0);std::copy_n("visible",7,malformed[6].data.begin()+128);auto null_visible=decode_consumer(malformed,"source-null visible source and valid scoped clone");need(!null_visible.instances[0].occlusion_proxy&&!null_visible.materials[null_visible.meshes[0].material].source_color_draw&&null_visible.meshes[0].indices.size()==3,"ADF visible-name source-null subset retains geometry without invented color draw");
 malformed=missing;store(malformed[3].data,112,0x3f000000);auto dangling=decode_consumer(malformed,"nonzero missing source material registry ID");need(dangling.meshes[0].source_material_id==0x3f000000&&!dangling.materials[dangling.meshes[0].material].source_color_draw,"ADF nonzero missing registry ID retains exact source reference and BSS-null semantics");
 malformed=missing;std::fill_n(malformed[6].data.begin()+192,64,0);std::copy_n("visible_clone",13,malformed[6].data.begin()+192);auto null_clone=decode_consumer(malformed,"visible source-null clone with authored lightmap");need(!null_clone.instances[1].occlusion_proxy&&!null_clone.materials[null_clone.instances[1].material].source_color_draw&&null_clone.materials[null_clone.instances[1].material].lightmap=="original/proxy.dds","ADF source-null clone copies preserve no-color provenance and authored lightmap");
 std::fill(chunks[1].data.begin()+192,chunks[1].data.end(),0);std::copy_n("skydome",7,chunks[1].data.begin()+192);auto repeated=chunks;repeated.insert(repeated.end(),{chunks[1],chunks[2],chunks[3],chunks[4],chunks[4]});auto layers=decode_scene(repeated);
 need(layers.nodes.size()==2&&layers.nodes[0].id==layers.nodes[1].id&&layers.meshes.size()==2&&layers.instances.size()==2&&layers.instances[0].sky&&layers.instances[1].sky&&layers.animation.size()==2&&layers.animation[1].keys.size()==2,"ADF duplicate-ID model scopes/key append/source sky class self-check");
 auto active_nan=consumer_fixture;store(active_nan[2].data,24,0x7fc12345);reject(active_nan,"ADF active visible normal NaN accepted");
 auto unreferenced_nan=consumer_fixture;unreferenced_nan[2].data.resize(192);store(unreferenced_nan[2].data,144+24,0x7fc12345);auto unreferenced=decode_scene(unreferenced_nan);need(unreferenced.meshes[0].vertices.size()==3&&unreferenced.source_scalar_words.size()==1&&unreferenced.source_scalar_words[0].use==SourceScalarUse::UnreferencedNormal&&unreferenced.source_scalar_words[0].bits==0x7fc12345,"ADF unreferenced normal preserves exact raw word without inventing a vertex");
 auto no_color_nan=active_nan;store(no_color_nan[0].data,4,0);auto no_color=decode_scene(no_color_nan);need(!no_color.materials[0].source_color_draw&&no_color.meshes[0].indices.size()==3&&no_color.source_scalar_words.size()==1&&no_color.source_scalar_words[0].use==SourceScalarUse::NoColorNormal,"ADF no-primary-handle normal is inactive, not a white material fallback");
 auto degenerate_nan=active_nan;for(auto offset:{12U,16U,20U})store(degenerate_nan[2].data,48+offset,word(degenerate_nan[2].data,offset));auto degenerate=decode_scene(degenerate_nan);need(degenerate.meshes[0].indices.size()==3&&degenerate.source_scalar_words.size()==1&&degenerate.source_scalar_words[0].use==SourceScalarUse::DegenerateNormal,"ADF zero-coverage triangles retain indices and raw inactive normals");
 auto uv_nan=consumer_fixture;store(uv_nan[3].data,88,0xffffffff);auto unbound_uv=decode_scene(uv_nan);need(unbound_uv.source_scalar_words.size()==1&&unbound_uv.source_scalar_words[0].use==SourceScalarUse::InactiveLightmapUv&&unbound_uv.source_scalar_words[0].offset==88,"ADF shader-inactive UV1 raw word retention");uv_nan.push_back(bound_lightmap);reject(uv_nan,"ADF active bound lightmap UV1 NaN accepted");
 auto position_nan=consumer_fixture;store(position_nan[2].data,12,0x7fc12345);reject(position_nan,"ADF nonfinite source position accepted");
 auto source_bounds=consumer_fixture;real(source_bounds[1].data,112,-4);store(source_bounds[1].data,116,0xffffffff);auto ieee_bounds=decode_scene(source_bounds);need(ieee_bounds.instances[0].source_half_extents[2]==-.2f&&std::isnan(ieee_bounds.instances[0].source_half_extents[1])&&ieee_bounds.source_scalar_words[0].use==SourceScalarUse::UnorderedBounds&&ieee_bounds.source_scalar_words[0].bits==0xffffffff,"ADF signed/IEEE bounds retained without clamping or mesh-AABB substitution");
 auto absent_lightmap=consumer_fixture;auto absent_lm=bound_lightmap;store(absent_lm.data,0,0);std::fill_n(absent_lm.data.begin()+4,64,0xff);absent_lightmap.push_back(absent_lm);auto lm_noop=decode_scene(absent_lightmap);need(lm_noop.materials.size()==1&&lm_noop.materials[0].lightmap.empty()&&lm_noop.diagnostics==std::vector<std::string>{"Original source no-op: 004a1930 ignores lightmap registry misses=1"},"ADF missing lightmap target returns before consuming inactive filename");
 auto packed_vertices=chunks;packed_vertices[2].tag="pack";packed_vertices[2].data.assign(96,0);for(unsigned i=0;i<3;++i){store(packed_vertices[2].data,i*32,i);real(packed_vertices[2].data,i*32+16,i==1?10.f:0.f);real(packed_vertices[2].data,i*32+24,i==2?10.f:0.f);}auto pack32=decode_scene(packed_vertices);need(pack32.meshes.size()==1&&pack32.meshes[0].indices.size()==3&&pack32.meshes[0].vertices[0].z==1&&pack32.meshes[0].vertices[0].nx<-.57f&&pack32.meshes[0].vertices[0].ny<-.57f&&pack32.meshes[0].vertices[0].nz<-.57f,"ADF pack32/ptri reconstructs real source geometry and packed normals");
 auto inactive_skin=shared_skin;store(inactive_skin[inactive_skin.size()-2].data,20,0x7fc12345);auto skin_words=decode_scene(inactive_skin);need(skin_words.source_scalar_words.size()==1&&skin_words.source_scalar_words[0].use==SourceScalarUse::InactiveSkinNormal&&skin_words.meshes[0].vertices[0].x==1,"ADF original skin path never consumes stored normal fields");
 auto inactive_zero=zero_weights;store(inactive_zero[inactive_zero.size()-2].data,8,0x7fc12345);auto zero_words=decode_scene(inactive_zero);need(zero_words.source_scalar_words.size()==1&&zero_words.source_scalar_words[0].use==SourceScalarUse::ZeroWeightPosition&&zero_words.meshes[0].vertices[0].x==0,"ADF zero weight skips serialized local-position read");
 std::vector<assets::AdfChunk> standalone{consumer_fixture[1],consumer_fixture[4]};auto data_only=decode_scene(standalone);need(data_only.instances.empty()&&data_only.nodes.size()==1&&data_only.animation.size()==1&&!data_only.diagnostics.empty(),"ADF data-only resource parses explicitly without becoming a blank renderable model");
 { // Texture/lightmap pack selection (004619d0 detail 0/1/2 = _4/_2/_1) with nearest-present fallback.
  const int marker=0;const auto root=std::filesystem::temp_directory_path()/("ikv-pack-selftest-"+std::to_string(reinterpret_cast<std::uintptr_t>(&marker)));
  std::filesystem::create_directories(root);
  for(const auto* name:{"textures_2.DAT","textures_1.DAT","lightmaps_4.dat"})std::ofstream(root/name).put('\0');
  const auto pick=[&](const char* kind){return texture_archive(root,kind).filename().string();};
  bool invalid_detail=false;try{set_texture_detail(3,0);}catch(const std::runtime_error&){invalid_detail=true;}
  need(invalid_detail&&pick("textures")=="textures_1.DAT"&&pick("lightmaps")=="lightmaps_4.dat","default detail 2/1 selects _1, missing lightmap packs fall back to the present one");
  set_texture_detail(0,1);need(pick("textures")=="textures_2.DAT","detail 0 falls back to the nearest present pack (_2)");
  set_texture_detail(1,2);need(pick("textures")=="textures_2.DAT"&&pick("lightmaps")=="lightmaps_4.dat"&&texture_archive(root,"objects").empty(),"detail 1 selects _2; absent kinds select nothing");
  set_texture_detail(2,1);std::filesystem::remove_all(root);
 }
}
} // namespace ikv
