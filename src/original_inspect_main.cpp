#include "ikv/original_wire.hpp"

#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void usage() {
    std::cerr << "Usage: ikv-original-inspect [--client-commands] STREAM\n"
                 "Read a local binary stream using static-only recovered LE32 framing.\n"
                 "Output sizes only; --client-commands reports the first raw command ID\n"
                 "only when the user identifies the stream as client-to-server commands.\n"
                 "No payload output, network access, or original compatibility claim.\n";
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        usage();
        return 0;
    }
    const bool commands = argc == 3 && std::string_view(argv[1]) == "--client-commands";
    if ((!commands && argc != 2) || std::string_view(argv[argc - 1]).starts_with("--")) {
        usage();
        return 2;
    }
    try {
        std::ifstream input(argv[commands ? 2 : 1], std::ios::binary);
        if (!input) throw std::runtime_error("cannot open local input stream");
        std::cout << "evidence=static-only-original-framing local_body_limit=65535 mode="
                  << (commands ? "first-client-command-id" : "opaque") << '\n';
        ikv::original_wire::Decoder decoder;
        std::array<std::uint8_t, 4096> bytes;
        std::uint64_t frame_count = 0;
        for (;;) {
            input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            const auto count = static_cast<std::size_t>(input.gcount());
            for (const auto& body : decoder.feed(std::span(bytes).first(count))) {
                // An original zero-length envelope is ignored by its dispatcher.
                const auto command = commands && !body.empty()
                    ? ikv::original_wire::first_client_command_id(body) : 0;
                std::cout << "frame=" << ++frame_count << " body_bytes=" << body.size();
                if (commands && !body.empty()) {
                    std::cout << " first_client_command_id=0x" << std::hex << std::setw(8)
                              << std::setfill('0') << command << std::dec << std::setfill(' ');
                }
                std::cout << '\n';
            }
            if (input.bad() || (input.fail() && !input.eof())) {
                throw std::runtime_error("local input stream read failed");
            }
            if (input.eof()) break;
        }
        decoder.finish();
        std::cout << "frames=" << frame_count << " complete=yes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ikv-original-inspect: " << error.what() << '\n';
        return 1;
    }
}
