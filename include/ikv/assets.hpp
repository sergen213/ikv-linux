#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <span>
#include <string>
#include <vector>

namespace ikv::assets {
inline constexpr std::uint64_t max_index_bytes = 64ULL * 1024 * 1024;
inline constexpr std::uint64_t max_path_bytes = 64ULL * 1024 * 1024;
inline constexpr unsigned max_directory_depth = 64;
inline constexpr std::uint64_t max_entry_bytes = 512ULL * 1024 * 1024;

struct Entry {
    std::string path;
    std::uint32_t offset;
    std::uint32_t size;
};
struct Archive {
    std::uint64_t file_size;
    std::uint64_t index_size;
    std::vector<Entry> entries; // Original depth-first order, including the root name.
};
struct Prefix {
    std::array<std::uint8_t, 128> bytes{};
    std::size_t size = 0;
};
struct DdsHeader {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t mip_levels;
    std::array<char, 4> fourcc{}; // Zero for a supported non-FourCC pixel format.
    std::uint32_t bits_per_pixel;
};
struct DdsMip {
    std::uint32_t width;
    std::uint32_t height;
    std::size_t offset; // Byte offset into DdsImage::rgba.
    std::size_t size; // width * height * 4 RGBA8 bytes.
};
struct DdsImage {
    std::uint32_t width;
    std::uint32_t height;
    std::vector<std::uint8_t> rgba; // All original RGBA8 mips, contiguous, original DDS row order.
    std::vector<DdsMip> mips; // Base mip first; no generated levels.
};
struct AdfChunk {
    std::string tag;
    std::uint32_t id;
    std::vector<std::uint8_t> data;
};

// Stream starts at byte zero. Reads the index only; file_size is supplied by the caller.
// Invalid/unsupported input and short reads throw std::runtime_error.
Archive read_archive(std::istream& input, std::uint64_t file_size);
// Seeks to a validated entry and reads at most 128 bytes; never writes or extracts.
Prefix read_prefix(std::istream& input, const Entry& entry);
// Validates the stream range before allocation; oversized/truncated entries reject.
std::vector<std::uint8_t> read_entry(std::istream& input, const Entry& entry,
                                   std::uint64_t max_bytes = max_entry_bytes);
// Supports legacy 2D DDS headers only. DX10, cube/volume and unknown pixel formats reject.
// Reports header metadata, not a decoded or fully validated texture.
DdsHeader read_dds_header(std::span<const std::uint8_t> prefix, std::uint64_t entry_size);
// Legacy 2D DXT1/3/5 or RGB16/RGB32 with contiguous, nonoverlapping channel masks.
// Honors top RGB pitch; lower mips are tightly packed. Validates all declared mip payloads.
// Decodes every original mip; caps input and aggregate RGBA output at 512 MiB.
// Invalid input throws std::runtime_error.
DdsImage decode_dds(std::span<const std::uint8_t> bytes);
// Deterministic decoder/read_entry checks, callable by the asset tool's self-test.
void decoder_self_test();
// Reads the original ADF chunk table; bounded aggregate decompression (512 MiB).
// Supports stored, zlib and the original adaptive arithmetic chunk codec.
// Arithmetic chunks stop at declared decoded length, without requiring a trailing EOF symbol.
std::vector<AdfChunk> decode_adf(std::span<const std::uint8_t> bytes);
void adf_decoder_self_test();
} // namespace ikv::assets
