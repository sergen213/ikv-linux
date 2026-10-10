#include "ikv/original_wire.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
namespace wire = ikv::original_wire;
void require(bool condition, const char* explanation) {
    if (!condition) throw std::runtime_error(explanation);
}
template<class F> void rejects(F&& action) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "invalid original wire input was accepted");
}

// Independently reconstructed from the static startup call, NOT captured traffic.
constexpr std::array<std::uint8_t, 12> startup{
    0x08, 0x00, 0x00, 0x00, 0x64, 0x65, 0x76, 0x70, 0x08, 0x00, 0x00, 0x00
};
constexpr std::array<std::uint8_t, 4> startup_argument{8, 0, 0, 0};
void static_fixture() {
    const auto encoded = wire::encode_command(0x70766564U, startup_argument);
    require(encoded.size() == startup.size() && std::equal(encoded.begin(), encoded.end(), startup.begin()),
            "startup encoder differs from independent LE32 fixture");
    wire::Decoder decoder;
    const auto frames = decoder.feed(startup);
    require(frames.size() == 1 && frames.front().size() == 8 &&
            std::equal(frames.front().begin(), frames.front().end(), startup.begin() + 4),
            "startup fixture body changed");
    require(wire::first_client_command_id(frames.front()) == 0x70766564U, "startup raw command changed");
    decoder.finish();
}
void streaming() {
    for (std::size_t cut = 0; cut <= startup.size(); ++cut) {
        wire::Decoder decoder;
        const auto first = decoder.feed(std::span(startup).first(cut));
        const auto second = decoder.feed(std::span(startup).subspan(cut));
        require(first.size() + second.size() == 1, "fragmented frame lost or duplicated");
        const auto& body = first.empty() ? second.front() : first.front();
        require(std::equal(body.begin(), body.end(), startup.begin() + 4, startup.end()),
                "fragmented body changed");
        decoder.finish();
    }
    wire::Decoder bytewise;
    std::size_t delivered = 0;
    for (const auto& byte : startup) delivered += bytewise.feed(std::span(&byte, 1)).size();
    require(delivered == 1 && bytewise.feed({}).empty(), "bytewise framing failed");
    bytewise.finish();

    wire::Frame stream(4, 0); // Zero-sized frame before coalesced nonempty frames.
    for (int i = 0; i < 1000; ++i) stream.insert(stream.end(), startup.begin(), startup.end());
    stream.insert(stream.end(), 4, 0);
    wire::Decoder coalesced;
    const auto frames = coalesced.feed(stream);
    require(frames.size() == 1002 && frames.front().empty() && frames.back().empty() &&
            frames[1].size() == 8 && frames[1000].size() == 8, "coalesced or empty frames failed");
    coalesced.finish();

    wire::Decoder suffix;
    stream.assign(startup.begin(), startup.end());
    stream.insert(stream.end(), startup.begin(), startup.begin() + 6);
    require(suffix.feed(stream).size() == 1, "partial suffix blocked a complete frame");
    require(suffix.feed(std::span(startup).subspan(6)).size() == 1, "partial suffix was not retained");
    suffix.finish();
}
void eof_and_failure() {
    wire::Decoder empty;
    empty.finish();
    for (std::size_t size = 1; size < startup.size(); ++size) {
        wire::Decoder partial;
        require(partial.feed(std::span(startup).first(size)).empty(), "incomplete frame emitted");
        rejects([&] { partial.finish(); });
        rejects([&] { partial.feed(startup); });
        rejects([&] { partial.finish(); });
    }
    wire::Frame suffix(startup.begin(), startup.end());
    suffix.push_back(8);
    wire::Decoder partial;
    require(partial.feed(suffix).size() == 1, "complete prefix was not emitted");
    rejects([&] { partial.finish(); });

    for (const auto& header : std::array<std::array<std::uint8_t, 4>, 2>{
             std::array<std::uint8_t, 4>{0, 0, 1, 0}, // LE32 65536, not a BE header.
             std::array<std::uint8_t, 4>{255, 255, 255, 255}}) {
        wire::Decoder oversized;
        require(oversized.feed(std::span(header).first(3)).empty(), "partial length header emitted");
        rejects([&] { oversized.feed(std::span(header).last(1)); });
        rejects([&] { oversized.feed(startup); });
        rejects([&] { oversized.finish(); });
    }
}
void command_bounds() {
    wire::Frame arguments(wire::max_body_size - 4, 0xa5);
    const auto largest = wire::encode_command(0xffffffffU, arguments);
    require(largest.size() == wire::max_body_size + 4 && largest[0] == 255 && largest[1] == 255 &&
            largest[2] == 0 && largest[3] == 0, "maximum body length was masked or encoded incorrectly");
    wire::Decoder decoder;
    const auto frames = decoder.feed(largest);
    require(frames.size() == 1 && frames.front().size() == wire::max_body_size &&
            wire::first_client_command_id(frames.front()) == 0xffffffffU &&
            std::equal(arguments.begin(), arguments.end(), frames.front().begin() + 4),
            "maximum body/command changed");
    decoder.finish();
    arguments.push_back(0);
    rejects([&] { wire::encode_command(0, arguments); });
    wire::Frame oversized(wire::max_body_size + 1, 0);
    rejects([&] { wire::first_client_command_id(oversized); });

    // Envelope framing imposes no minimum body; the explicit command view does.
    for (std::uint8_t size = 0; size < 4; ++size) {
        wire::Frame tiny{size, 0, 0, 0};
        tiny.insert(tiny.end(), size, 0xff);
        wire::Decoder raw;
        const auto opaque = raw.feed(tiny);
        require(opaque.size() == 1 && opaque.front().size() == size, "short opaque frame rejected");
        rejects([&] { wire::first_client_command_id(opaque.front()); });
        raw.finish();
    }
    const auto no_arguments = wire::encode_command(0, {});
    wire::Decoder zero_id;
    const auto body = zero_id.feed(no_arguments);
    require(body.size() == 1 && body.front().size() == 4 && wire::first_client_command_id(body.front()) == 0,
            "raw zero command or empty argument list rejected");
    zero_id.finish();

    // A first-command view must not attempt to split the opaque remainder.
    constexpr std::array<std::uint8_t, 8> unschematized_body{0x78, 0x56, 0x34, 0x12, 0xff, 0xff, 0xff, 0xff};
    require(wire::first_client_command_id(unschematized_body) == 0x12345678U,
            "first-command view inferred opaque remainder semantics");
}
} // namespace

int main() {
    try {
        static_fixture();
        streaming();
        eof_and_failure();
        command_bounds();
        std::cout << "Original wire static-derived fixture and defensive boundaries passed (not captured traffic)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Original wire test: " << error.what() << '\n';
        return 1;
    }
}
