#include "ikv/audio_asset.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef NDEBUG
#error "audio_asset_check requires assertions; compile without NDEBUG"
#endif

namespace {
template <typename Function>
void expect_runtime_error(Function&& operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    assert(rejected);
}

void selfcheck() {
    // Headerless signed LE16 samples: -32768, -1, 0, 32767, 0x1234, -0x1234.
    constexpr std::array<std::uint8_t, 12> pcm{
        0x00, 0x80, 0xff, 0xff, 0x00, 0x00,
        0xff, 0x7f, 0x34, 0x12, 0xcc, 0xed
    };
    constexpr std::array accepted_paths{
        std::string_view("audio/x.raw"),
        std::string_view("base/audio/sub/x.raw"),
        std::string_view("AUDIO/X.RAW"),
        std::string_view("BASE\\AUDIO\\SUB\\X.RAW"),
        std::string_view("base/audio\\sub/x.raw")
    };
    constexpr std::array rejected_paths{
        std::string_view(""),
        std::string_view("music1.raw"),
        std::string_view("sound1.raw"),
        std::string_view("other/x.raw"),
        std::string_view("other/audio/x.raw"),
        std::string_view("base/other/x.raw"),
        std::string_view("audio/x"),
        std::string_view("audio/x.wav"),
        std::string_view("audio/.raw"),
        std::string_view("audio/../x.raw"),
        std::string_view("audio/./x.raw"),
        std::string_view("base/../audio/x.raw"),
        std::string_view("./audio/x.raw"),
        std::string_view("audio\\..\\x.raw"),
        std::string_view("audio\\.\\x.raw"),
        std::string_view("audio//x.raw"),
        std::string_view("base//audio/x.raw"),
        std::string_view("audio\\\\x.raw"),
        std::string_view("audio/x.raw/"),
        std::string_view("audio/x.raw\\"),
        std::string_view("/audio/x.raw"),
        std::string_view("\\audio\\x.raw"),
        std::string_view("C:\\audio\\x.raw")
    };
    const std::string bytes(reinterpret_cast<const char*>(pcm.data()), pcm.size());
    const std::string framed = "skip" + bytes + "tail";

    for (const auto path : accepted_paths) {
        const auto view = ikv::assets::inspect_raw_audio(pcm, path);
        assert(view.sample_rate == 44100);
        assert(view.channels == 1);
        assert(view.pcm.data() == pcm.data());
        assert(view.pcm.size() == pcm.size());
        assert(std::equal(view.pcm.begin(), view.pcm.end(), pcm.begin(), pcm.end()));

        std::istringstream input(framed, std::ios::in | std::ios::binary);
        input.seekg(4);
        const auto loaded = ikv::assets::read_raw_audio(input, pcm.size(), path);
        assert(std::equal(loaded.begin(), loaded.end(), pcm.begin(), pcm.end()));
        assert(input.tellg() == std::streampos(4 + pcm.size()));
        std::array<char, 4> suffix{};
        input.read(suffix.data(), suffix.size());
        assert(std::string_view(suffix.data(), suffix.size()) == "tail");
    }

    expect_runtime_error([&] {
        ikv::assets::inspect_raw_audio(std::span<const std::uint8_t>{}, "audio/x.raw");
    });
    expect_runtime_error([&] {
        ikv::assets::inspect_raw_audio(std::span(pcm).first(1), "audio/x.raw");
    });
    // Odd original files (ui/windowopen.raw) keep whole frames; the trailing byte is not a sample.
    assert(ikv::assets::inspect_raw_audio(std::span(pcm).first(3), "audio/x.raw").pcm.size() == 2);

    std::istringstream invalid_input(framed, std::ios::in | std::ios::binary);
    invalid_input.seekg(4);
    const auto original_position = invalid_input.tellg();
    constexpr std::array<std::uint64_t, 5> rejected_sizes{
        0, 1, ikv::assets::max_raw_audio_bytes + 1,
        ikv::assets::max_raw_audio_bytes + 2, std::numeric_limits<std::uint64_t>::max()
    };
    for (const auto size : rejected_sizes) {
        expect_runtime_error([&] {
            ikv::assets::read_raw_audio(invalid_input, size, "audio/x.raw");
        });
        assert(invalid_input.good());
        assert(invalid_input.tellg() == original_position);
    }
    for (const auto path : rejected_paths) {
        expect_runtime_error([&] { ikv::assets::inspect_raw_audio(pcm, path); });
        expect_runtime_error([&] {
            ikv::assets::read_raw_audio(invalid_input, pcm.size(), path);
        });
        assert(invalid_input.good());
        assert(invalid_input.tellg() == original_position);
    }

    std::istringstream short_input(bytes.substr(0, 2), std::ios::in | std::ios::binary);
    expect_runtime_error([&] {
        ikv::assets::read_raw_audio(short_input, pcm.size(), "audio/x.raw");
    });
    std::istringstream throwing_input(bytes.substr(0, 2), std::ios::in | std::ios::binary);
    throwing_input.exceptions(std::ios::failbit | std::ios::badbit);
    expect_runtime_error([&] {
        ikv::assets::read_raw_audio(throwing_input, pcm.size(), "audio/x.raw");
    });

    std::istringstream exact_input(bytes, std::ios::in | std::ios::binary);
    const auto loaded = ikv::assets::read_raw_audio(exact_input, pcm.size(), "audio/x.raw");
    assert(std::equal(loaded.begin(), loaded.end(), pcm.begin(), pcm.end()));
    assert(exact_input.good());
    assert(exact_input.tellg() == std::streampos(pcm.size()));
}

void check_file(const char* filesystem_path, std::string_view asset_path) {
    std::ifstream input(filesystem_path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open RAW audio file");
    const auto size = input.tellg();
    if (size < std::streampos(0)) throw std::runtime_error("cannot measure RAW audio file");
    input.seekg(0);
    if (!input) throw std::runtime_error("cannot seek RAW audio file");
    const auto bytes = ikv::assets::read_raw_audio(
        input, static_cast<std::uint64_t>(static_cast<std::streamoff>(size)), asset_path);
    const auto view = ikv::assets::inspect_raw_audio(bytes, asset_path);
    assert(view.sample_rate == 44100);
    assert(view.channels == 1);
    assert(!view.pcm.empty());
    assert(view.pcm.size() % 2 == 0);
    assert(view.pcm.data() == bytes.data());
    assert(view.pcm.size() == (bytes.size() & ~std::size_t{1}));
    std::cout << "RAW audio checked: " << bytes.size() << " bytes, mono LE16 44100 Hz\n";
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 3) {
        std::cerr << "Usage: audio_asset_check [FILE [ARCHIVE_PATH]]\n"
                     "An absolute FILE requires an audio/...raw or base/audio/...raw ARCHIVE_PATH.\n";
        return 2;
    }
    try {
        selfcheck();
        if (argc >= 2) check_file(argv[1], argc == 3 ? argv[2] : argv[1]);
        std::cout << "RAW audio selfcheck passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "audio_asset_check: " << error.what() << '\n';
        return 1;
    }
}
