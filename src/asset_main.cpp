#include "ikv/effects.hpp"
#include "ikv/assets.hpp"
#include "ikv/scene.hpp"
#include <nlohmann/json.hpp>

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
