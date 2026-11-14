#include "ikv/effects.hpp"
#include "ikv/assets.hpp"
#include "ikv/scene.hpp"
#include <nlohmann/json.hpp>
#include <set>
#include <cmath>
#include <algorithm>
#include "ikv/pbr.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void append_le(std::string& bytes, std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<char>((value >> (8 * i)) & 255));
}
void replace_le(std::string& bytes, std::size_t position, std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.at(position + i) = static_cast<char>((value >> (8 * i)) & 255);
}
void self_test() {
    std::string valid = "SKYF";
    append_le(valid, 1, 2);
    valid += std::string("root") + std::string(60, '\0');
    append_le(valid, 0, 2);
    append_le(valid, 1, 2);
    valid += std::string("a.bin") + std::string(123, '\0');
    append_le(valid, 210, 4);
    append_le(valid, 4, 4);
    valid += "DATA";
    std::istringstream input(valid, std::ios::in | std::ios::binary);
    const auto archive = ikv::assets::read_archive(input, valid.size());
    require(archive.index_size == 210 && archive.file_size == 214 && archive.entries.size() == 1,
            "self-test: minimal archive summary");
    const auto& entry = archive.entries.front();
    require(entry.path == "root/a.bin" && entry.offset == 210 && entry.size == 4,
            "self-test: minimal archive entry");
    const auto prefix = ikv::assets::read_prefix(input, entry);
    require(prefix.size == 4 && std::equal(prefix.bytes.begin(), prefix.bytes.begin() + 4, "DATA"),
            "self-test: payload prefix");
    const auto rejects = [](const std::string& bytes, std::uint64_t size, const char* name) {
        std::istringstream stream(bytes, std::ios::in | std::ios::binary);
        try { (void)ikv::assets::read_archive(stream, size); }
        catch (const std::runtime_error&) { return; }
        throw std::runtime_error(std::string("self-test: accepted ") + name);
    };
    auto bad = valid;
    bad.resize(100);
    rejects(bad, valid.size(), "truncated index");
    bad = valid;
    replace_le(bad, 202, 209, 4);
    rejects(bad, bad.size(), "index overlap");
    bad = valid;
    replace_le(bad, 206, 5, 4);
    rejects(bad, bad.size(), "range past EOF");
    bad = valid;
    bad.replace(74, 128, std::string("..") + std::string(126, '\0'));
    rejects(bad, bad.size(), "unsafe path component");
    bad = valid;
    replace_le(bad, 4, 2, 2);
    rejects(bad, bad.size(), "unknown version");
    bad = valid;
    bad[0] = 'X';
    rejects(bad, bad.size(), "bad signature");
    bad = valid;
    bad[80] = 'x';
    rejects(bad, bad.size(), "nonzero name padding");
    bad = valid;
    replace_le(bad, 72, 65535, 2);
    rejects(bad, bad.size(), "unbounded entry count");
    bad = valid + "x";
    rejects(bad, bad.size(), "unindexed trailing payload");
    std::cout << "SKYF parser self-test passed (valid prefix + 9 rejection cases)\n";
    ikv::assets::decoder_self_test();
    ikv::scene_decoder_self_test();
    ikv::EffectSystem::self_test();
    ikv::pbr_self_test();
    std::cout << "DDS/ADF/scene/animation/zone/Turkish-resource/particle-effect decoder self-tests passed\n";
}
void print_quoted(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    std::cout << '"';
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') std::cout << '\\' << static_cast<char>(c);
        else if (c >= 32 && c < 127) std::cout << static_cast<char>(c);
        else std::cout << "\\x" << hex[c >> 4] << hex[c & 15];
    }
    std::cout << '"';
}
bool extension_is(std::string_view path, std::string_view suffix) {
    if (path.size() < suffix.size()) return false;
    path.remove_prefix(path.size() - suffix.size());
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(path[i]);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + ('a' - 'A'));
        if (c != static_cast<unsigned char>(suffix[i])) return false;
    }
    return true;
}
void inspect(std::istream& input, const ikv::assets::Entry& entry) {
    const auto prefix = ikv::assets::read_prefix(input, entry);
    std::cout << "path=";
    print_quoted(entry.path);
    std::cout << " offset=" << entry.offset << " size=" << entry.size << " prefix_bytes=" << prefix.size
              << "\nsignature=";
    print_quoted(std::string_view(reinterpret_cast<const char*>(prefix.bytes.data()), std::min(prefix.size, std::size_t{4})));
    std::cout << "\nheader_hex=";
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < prefix.size; ++i) {
        if (i != 0) std::cout << ' ';
        const auto c = prefix.bytes[i];
        std::cout << hex[c >> 4] << hex[c & 15];
    }
    std::cout << '\n';
    const bool dds_magic = prefix.size >= 4 && prefix.bytes[0] == 'D' && prefix.bytes[1] == 'D' &&
                           prefix.bytes[2] == 'S' && prefix.bytes[3] == ' ';
    if (dds_magic || extension_is(entry.path, ".dds")) {
        const auto header = ikv::assets::read_dds_header(
            std::span<const std::uint8_t>(prefix.bytes.data(), prefix.size), entry.size);
        std::cout << "DDS legacy 2D header: width=" << header.width << " height=" << header.height
                  << " mips=" << header.mip_levels << " FourCC=";
        if (header.fourcc[0] != '\0') print_quoted(std::string_view(header.fourcc.data(), header.fourcc.size()));
        else std::cout << "none bits_per_pixel=" << header.bits_per_pixel;
        const auto image=ikv::assets::decode_dds(ikv::assets::read_entry(input,entry));
        std::cout << " decoded_rgba8_bytes=" << image.rgba.size() << '\n';
    } else if (extension_is(entry.path, ".adf")) {
        const auto chunks=ikv::assets::decode_adf(ikv::assets::read_entry(input,entry));
        std::cout << "ADF decoded_chunks=" << chunks.size() << '\n';
        for(const auto& chunk:chunks)std::cout << chunk.tag << " id=" << chunk.id << " decoded_bytes=" << chunk.data.size() << '\n';
    } else {
        std::cout << "Payload format unresolved; raw prefix only\n";
    }
}
void usage() {
    std::cerr << "Usage: ikv-assets list ARCHIVE\n"
                 "       ikv-assets inspect ARCHIVE ENTRY\n"
                 "       ikv-assets scene ASSET_ROOT MEMBER [TIME_SECONDS]\n"
                 "       ikv-assets --self-test\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
            self_test();
            return 0;
        }
        const std::string_view command = argc > 1 ? argv[1] : "";
        if(command=="scene"&&(argc==4||argc==5)) {
            auto scene=ikv::load_scene(argv[2],argv[3]);
            float time=0;if(argc==5){std::size_t consumed=0;time=std::stof(argv[4],&consumed);require(consumed==std::string(argv[4]).size(),"scene time is not a scalar");}
            ikv::update_scene(scene,time);
            std::size_t vertices=0,indices=0,keys=0,influences=0;
            for(const auto& m:scene.meshes){vertices+=m.vertices.size();indices+=m.indices.size();influences+=m.influences.size();}
            std::uint64_t texture_bytes=0; // resident GPU payload: authored BC blocks or RGBA8
            for(const auto& t:scene.animation)keys+=t.keys.size();
            for(const auto& t:scene.textures)texture_bytes+=t.data.size();
            std::cout << "{\"meshes\":" << scene.meshes.size() << ",\"vertices\":" << vertices << ",\"indices\":" << indices << ",\"instances\":" << scene.instances.size() << ",\"materials\":" << scene.materials.size() << ",\"textures\":" << scene.textures.size() << ",\"texture_bytes\":" << texture_bytes << ",\"nodes\":" << scene.nodes.size() << ",\"animation_keys\":" << keys << ",\"skin_influences\":" << influences << ",\"help_pages\":" << scene.help.size() << ",\"geometry_revision\":" << scene.geometry_revision << ",\"diagnostics\":";
            std::cout << nlohmann::json(scene.diagnostics).dump() << "}\n";return 0;
        }
        if(command=="pbr-keys"&&argc==3) { // NONCANONICAL wave 29: every PBR-pack-eligible diffuse key of the original zones
            nlohmann::json keys=nlohmann::json::object();std::set<std::string> loaded;
            for(unsigned id=0;id<71;++id) {
                const auto& d=ikv::source_scene_descriptor(id);
                if(d.members.empty()||!loaded.insert(d.members.front()).second)continue;
                const auto scene=ikv::load_source_scene(argv[2],id);
                const auto zone=ikv::load_zone(argv[2],d.support_id);
                const auto eligible=ikv::pbr_eligible_materials(scene);
                std::vector<std::size_t> uses(scene.materials.size());
                for(const auto& in:scene.instances){const auto m=in.material==UINT32_MAX?scene.meshes[in.mesh].material:in.material;if(m<uses.size())++uses[m];}
                // Largest placement per material: world centre of its mesh's AABB (capture spawn hints).
                std::vector<std::array<float,4>> largest(scene.materials.size(),{0,0,0,-1});
                for(const auto& in:scene.instances){
                    const auto m=in.material==UINT32_MAX?scene.meshes[in.mesh].material:in.material;
                    if(m>=largest.size()||scene.meshes[in.mesh].vertices.empty())continue;
                    float lo[3]{INFINITY,INFINITY,INFINITY},hi[3]{-INFINITY,-INFINITY,-INFINITY};
                    for(const auto& v:scene.meshes[in.mesh].vertices){const auto& t=in.transform.matrix;
                        const float w[3]{t[0]*v.x+t[4]*v.y+t[8]*v.z+t[12],t[1]*v.x+t[5]*v.y+t[9]*v.z+t[13],t[2]*v.x+t[6]*v.y+t[10]*v.z+t[14]};
                        for(int k=0;k<3;++k){lo[k]=std::min(lo[k],w[k]);hi[k]=std::max(hi[k],w[k]);}}
                    const float r=std::hypot(hi[0]-lo[0],hi[2]-lo[2]);
                    if(!(std::isfinite(r)&&r>largest[m][3]))continue;
                    // The up-facing walkable (zone grid) triangle nearest the AABB centre: a capture spawn on the surface (x,z x 1000 = wire).
                    const auto& mesh=scene.meshes[in.mesh];const auto& t=in.transform.matrix;float cx=(lo[0]+hi[0])/2,cz=(lo[2]+hi[2])/2,best=INFINITY;
                    std::array<float,3> at{};
                    for(std::size_t i=0;i+2<mesh.indices.size();i+=3){float c[3]{},n=0;
                        for(int k=0;k<3;++k){const auto& v=mesh.vertices[mesh.indices[i+k]];c[0]+=(t[0]*v.x+t[4]*v.y+t[8]*v.z+t[12])/3;c[1]+=(t[1]*v.x+t[5]*v.y+t[9]*v.z+t[13])/3;c[2]+=(t[2]*v.x+t[6]*v.y+t[10]*v.z+t[14])/3;n+=(t[1]*v.nx+t[5]*v.ny+t[9]*v.nz)/3;}
                        const float gap=(c[0]-cx)*(c[0]-cx)+(c[2]-cz)*(c[2]-cz);const auto ground=ikv::zone_height(zone,c[0],c[2]);if(std::abs(n)>.5f&&ground&&std::abs(*ground-c[1])<6&&gap<best){best=gap;at={c[0],c[1],c[2]};}}
                    if(std::isfinite(best))largest[m]={at[0],at[1],at[2],r}; // placements without a walkable point are skipped
                }
                for(std::size_t m=0;m<scene.materials.size();++m) {
                    if(!eligible[m])continue;
                    const auto key=ikv::pbr_texture_key(scene.materials[m].diffuse);auto& entry=keys[key];
                    if(entry.is_null()){entry={{"zones",nlohmann::json::array()},{"placements",0},{"materials",nlohmann::json::array()}};
                        for(const auto& t:scene.textures)if(t.name==scene.materials[m].diffuse){entry["width"]=t.width;entry["height"]=t.height;break;}}
                    if(std::find(entry["zones"].begin(),entry["zones"].end(),d.key.substr(0,d.key.find_first_of("0123456789")))==entry["zones"].end())
                        entry["zones"].push_back(d.key.substr(0,d.key.find_first_of("0123456789")));
                    entry["placements"]=entry["placements"].get<std::size_t>()+uses[m];
                    if(largest[m][3]>=0&&(!entry.contains("largest")||entry["largest"]["extent"].get<float>()<largest[m][3]))
                        entry["largest"]={{"zone",d.key},{"center",{largest[m][0],largest[m][1],largest[m][2]}},{"extent",largest[m][3]}};
                    if(!scene.materials[m].name.empty()&&std::find(entry["materials"].begin(),entry["materials"].end(),scene.materials[m].name)==entry["materials"].end())
                        entry["materials"].push_back(scene.materials[m].name);
                    // The MAT+14 secondary fade layer drawn under this key (renderers map its own pack material too).
                    if(ikv::pbr_fade_layer(scene.materials[m])) {
                        if(!entry.contains("fade_layers"))entry["fade_layers"]=nlohmann::json::array();
                        const auto fade=ikv::pbr_texture_key(scene.materials[m].luminance);
                        if(std::find(entry["fade_layers"].begin(),entry["fade_layers"].end(),fade)==entry["fade_layers"].end())entry["fade_layers"].push_back(fade);
                    }
                }
            }
            std::cout<<keys.dump(1)<<"\n";return 0;
        }
        if (!((command == "list" && argc == 3) || (command == "inspect" && argc == 4))) {
            usage();
            return 2;
        }
        std::ifstream input(argv[2], std::ios::binary);
        require(input.is_open(), "cannot open archive read-only");
        input.seekg(0, std::ios::end);
        const auto end = input.tellg();
        require(end != std::streampos(-1), "cannot measure archive");
        const auto length = static_cast<std::streamoff>(end);
        require(length >= 0, "negative archive size");
        input.seekg(0, std::ios::beg);
        require(static_cast<bool>(input), "cannot rewind archive");
        const auto archive = ikv::assets::read_archive(input, static_cast<std::uint64_t>(length));
        if (command == "list") {
            for (const auto& entry : archive.entries) {
                print_quoted(entry.path);
                std::cout << " offset=" << entry.offset << " size=" << entry.size << '\n';
            }
            std::cout << "SKYF v1 entries=" << archive.entries.size() << " index_bytes=" << archive.index_size
                      << " payload_bytes=" << archive.file_size - archive.index_size
                      << " archive_bytes=" << archive.file_size << '\n';
        } else {
            const std::string_view path = argv[3];
            const auto found = std::find_if(archive.entries.begin(), archive.entries.end(),
                                           [path](const auto& entry) { return entry.path == path; });
            require(found != archive.entries.end(), "entry path not found (exact byte match required)");
            inspect(input, *found);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ikv-assets: " << error.what() << '\n';
        return 1;
    }
}
