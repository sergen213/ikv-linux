#include "ikv/original_wire.hpp"

#include <algorithm>
#include <stdexcept>

namespace ikv::original_wire {
namespace {
std::uint32_t read_le32(std::span<const std::uint8_t> bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}
void append_le32(Frame& bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
} // namespace

std::vector<Frame> Decoder::feed(std::span<const std::uint8_t> bytes) {
    if (failed_) throw std::invalid_argument("original frame decoder already failed");
    std::vector<Frame> frames;
    while (!bytes.empty()) {
        const auto count = std::min(expected_ - buffered_, bytes.size());
        std::copy_n(bytes.begin(), count, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
        buffered_ += count;
        bytes = bytes.subspan(count);
        if (buffered_ != expected_) continue;
        if (expected_ == 4) {
            const auto size = read_le32(std::span(buffer_).first(4));
            if (size > max_body_size) {
                failed_ = true;
                throw std::invalid_argument("original frame exceeds local 65535-byte body limit");
            }
            expected_ = static_cast<std::size_t>(size) + 4;
            if (size != 0) continue;
        }
        frames.emplace_back(buffer_.begin() + 4,
                            buffer_.begin() + static_cast<std::ptrdiff_t>(expected_));
        buffered_ = 0;
        expected_ = 4;
    }
    return frames;
}

void Decoder::finish() {
    if (failed_) throw std::invalid_argument("original frame decoder already failed");
    if (buffered_ != 0) {
        failed_ = true;
        throw std::invalid_argument("truncated original frame at EOF");
    }
}

Frame encode_command(std::uint32_t command, std::span<const std::uint8_t> arguments) {
    if (arguments.size() > max_body_size - 4) {
        throw std::invalid_argument("original command exceeds local 65535-byte body limit");
    }
    Frame frame;
    frame.reserve(arguments.size() + 8);
    append_le32(frame, static_cast<std::uint32_t>(arguments.size() + 4));
    append_le32(frame, command);
    frame.insert(frame.end(), arguments.begin(), arguments.end());
    return frame;
}

std::uint32_t first_client_command_id(std::span<const std::uint8_t> body) {
    if (body.size() < 4 || body.size() > max_body_size) {
        throw std::invalid_argument("invalid original client-command body size");
    }
    return read_le32(body.first(4));
}
} // namespace ikv::original_wire
