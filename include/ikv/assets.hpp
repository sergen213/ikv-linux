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
// rgba8: decoded RGBA8 texels. bc1/bc2/bc3: the authored DXT1/DXT3/DXT5 4x4 blocks, byte-identical to the DDS payload
// (BC1 keeps DXT1's 1-bit alpha: GPUs sample it as the RGBA variant).
enum class DdsFormat : std::uint8_t { rgba8, bc1, bc2, bc3 };
struct DdsMip {
    std::uint32_t width;
    std::uint32_t height;
    std::size_t offset; // Byte offset into DdsImage::rgba.
    std::size_t size; // dds_mip_bytes(format, width, height).
};
struct DdsImage {
    std::uint32_t width;
    std::uint32_t height;
    std::vector<std::uint8_t> rgba; // All original mips in `format`, contiguous, original DDS row order.
    std::vector<DdsMip> mips; // Base mip first; no generated levels.
    DdsFormat format = DdsFormat::rgba8;
};
// Bytes of one mip level: width*height*4 for rgba8, ceil(w/4)*ceil(h/4)*(8 for bc1, 16 for bc2/bc3).
std::uint64_t dds_mip_bytes(DdsFormat format, std::uint32_t width, std::uint32_t height);
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
// keep_blocks: DXT1/3/5 keep their validated authored blocks (format bc1/bc2/bc3) for GPU-native sampling; RGB still decodes.
// Invalid input throws std::runtime_error.
DdsImage decode_dds(std::span<const std::uint8_t> bytes, bool keep_blocks = false);
// Decodes a bc1/bc2/bc3 image to RGBA8 exactly like decode_dds (renderers lacking BC sampling); rgba8 is returned as is.
DdsImage decompress_dds(const DdsImage& image);
// Deterministic decoder/read_entry checks, callable by the asset tool's self-test.
void decoder_self_test();
// Reads the original ADF chunk table; bounded aggregate decompression (512 MiB).
// Supports stored, zlib and the original adaptive arithmetic chunk codec.
// Arithmetic chunks stop at declared decoded length, without requiring a trailing EOF symbol.
std::vector<AdfChunk> decode_adf(std::span<const std::uint8_t> bytes);
void adf_decoder_self_test();
} // namespace ikv::assets
