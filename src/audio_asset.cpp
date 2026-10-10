#include "ikv/audio_asset.hpp"

#include <istream>
#include <limits>
#include <stdexcept>

namespace ikv::assets {
namespace {

bool ascii_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const char c = left[i];
        const char folded = c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        if (folded != right[i]) return false;
    }
    return true;
}

bool qualified_path(std::string_view path) {
    auto end = path.find_first_of("/\\");
    if (end == std::string_view::npos) return false;
    const auto root = path.substr(0, end);
    auto begin = end + 1;
    if (ascii_equal(root, "base")) {
        end = path.find_first_of("/\\", begin);
        if (end == std::string_view::npos ||
            !ascii_equal(path.substr(begin, end - begin), "audio")) return false;
        begin = end + 1;
    } else if (!ascii_equal(root, "audio")) {
        return false;
    }
    while (begin < path.size()) {
        end = path.find_first_of("/\\", begin);
        const auto segment = path.substr(begin, end == std::string_view::npos ?
                                                    path.size() - begin : end - begin);
        if (segment.empty() || segment == "." || segment == "..") return false;
        if (end == std::string_view::npos)
            return segment.size() > 4 && ascii_equal(segment.substr(segment.size() - 4), ".raw");
        begin = end + 1;
    }
    return false;
}

void validate(std::uint64_t byte_size, std::string_view asset_path) {
    if (!qualified_path(asset_path)) throw std::runtime_error("RAW audio path is not qualified");
    // 1 byte has no complete frame; a longer odd file keeps its whole frames (see inspect_raw_audio).
    if (byte_size < 2) throw std::runtime_error("RAW audio has no complete 16-bit sample");
    if (byte_size > max_raw_audio_bytes) throw std::runtime_error("RAW audio exceeds byte limit");
}

} // namespace

RawAudioView inspect_raw_audio(std::span<const std::uint8_t> bytes,
                               std::string_view asset_path) {
    validate(bytes.size(), asset_path);
    // Original window open/close/warning RAW files are odd-sized and played by the original
    // (0x00453580 locks the full length into a block-aligned DirectSound buffer). Only whole
    // two-byte frames are audible, so the incomplete trailing byte is excluded, never padded.
    return {44100, 1, bytes.first(bytes.size() & ~std::size_t{1})};
}

std::vector<std::uint8_t> read_raw_audio(std::istream& input, std::uint64_t byte_size,
                                       std::string_view asset_path) {
    validate(byte_size, asset_path);
    if (byte_size > std::numeric_limits<std::size_t>::max() ||
        byte_size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error("RAW audio size is not addressable");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(byte_size));
    const auto count = static_cast<std::streamsize>(byte_size);
    try {
        input.read(reinterpret_cast<char*>(bytes.data()), count);
    } catch (const std::ios_base::failure&) {
        throw std::runtime_error("RAW audio stream read failed");
    }
    if (!input || input.gcount() != count) throw std::runtime_error("RAW audio stream is short");
    return bytes;
}

} // namespace ikv::assets
