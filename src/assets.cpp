#include "ikv/assets.hpp"

#include <algorithm>
#include <bit>
#include <deque>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ikv::assets {
namespace {
[[noreturn]] void invalid(const char* message) { throw std::runtime_error(message); }
std::uint32_t le32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}
std::string join(std::string_view parent, std::string_view name) {
    std::string result;
    result.reserve(parent.size() + (parent.empty() ? 0 : 1) + name.size());
    result.append(parent);
    if (!parent.empty()) result.push_back('/');
    result.append(name);
    return result;
}
class IndexReader {
public:
    IndexReader(std::istream& input, std::uint64_t file_size)
        : input_(input), limit_(std::min(file_size, max_index_bytes)), archive_{file_size, 0, {}} {}
    Archive parse() {
        std::array<char, 4> signature{};
        read(signature.data(), signature.size());
        if (signature != std::array<char, 4>{'S', 'K', 'Y', 'F'}) invalid("invalid SKYF signature");
        if (u16() != 1) invalid("unsupported SKYF version (only v1 is supported)");
        directory({}, 1);
        archive_.index_size = position_;
        std::vector<std::string_view> paths;
        paths.reserve(directories_.size() + archive_.entries.size());
        for (const auto& path : directories_) paths.emplace_back(path);
        for (const auto& entry : archive_.entries) paths.emplace_back(entry.path);
        std::sort(paths.begin(), paths.end());
        if (std::adjacent_find(paths.begin(), paths.end()) != paths.end()) invalid("duplicate SKYF path");
        std::uint64_t next = position_;
        for (const auto& entry : archive_.entries) {
            if (entry.offset < position_) invalid("SKYF payload overlaps index");
            if (entry.offset != next) invalid("SKYF payload is not contiguous in index traversal order");
            next += entry.size; // Each entry's range was checked against file_size before insertion.
        }
        if (next != archive_.file_size) invalid("SKYF payload does not end at exact EOF");
        return std::move(archive_);
    }
private:
    void room(std::uint64_t bytes) const {
        if (bytes > limit_ - position_) invalid("SKYF index exceeds file size or 64 MiB budget");
    }
    void read(void* output, std::size_t bytes) {
        room(bytes);
        input_.read(static_cast<char*>(output), static_cast<std::streamsize>(bytes));
        if (input_.gcount() != static_cast<std::streamsize>(bytes) || !input_)
            invalid("truncated SKYF index");
        position_ += bytes;
    }
    std::uint16_t u16() {
        std::array<std::uint8_t, 2> bytes{};
        read(bytes.data(), bytes.size());
        return static_cast<std::uint16_t>(static_cast<unsigned>(bytes[0]) |
                                          (static_cast<unsigned>(bytes[1]) << 8));
    }
    std::uint32_t u32() {
        std::array<std::uint8_t, 4> bytes{};
        read(bytes.data(), bytes.size());
        return le32(bytes.data());
    }
    template<std::size_t N> std::string name() {
        std::array<char, N> bytes{};
        read(bytes.data(), bytes.size());
        const auto end = std::find(bytes.begin(), bytes.end(), '\0');
        if (end == bytes.end() || end == bytes.begin()) invalid("SKYF name is empty or not NUL terminated");
        if (!std::all_of(end, bytes.end(), [](char c) { return c == '\0'; }))
            invalid("SKYF name has nonzero NUL padding");
        const std::string_view component(bytes.data(), static_cast<std::size_t>(end - bytes.begin()));
        if (component == "." || component == "..") invalid("unsafe SKYF path component");
        for (const unsigned char c : component) {
            // Byte-level safety only; the original name encoding is not established.
            if (c < 32 || (c >= 127 && c <= 159) || c == '/' || c == '\\')
                invalid("unsafe SKYF path component");
        }
        return std::string(component);
    }
    std::string checked_path(std::string_view parent, std::string_view name) {
        const auto bytes = parent.size() + (parent.empty() ? 0 : 1) + name.size();
        if (bytes > max_path_bytes - path_bytes_) invalid("SKYF expanded paths exceed 64 MiB budget");
        path_bytes_ += bytes;
        return join(parent, name);
    }
    void directory(std::string_view parent, unsigned depth) {
        if (depth > max_directory_depth) invalid("SKYF directory nesting exceeds 64 levels");
        const auto component = name<64>();
        const auto children = u16();
        const auto files = u16();
        // Minimum fixed bytes required before any count-driven iteration or allocation.
        room(static_cast<std::uint64_t>(children) * 68 + static_cast<std::uint64_t>(files) * 136);
        constexpr auto max_entries = max_index_bytes / 136;
        if (files > max_entries - archive_.entries.size()) invalid("SKYF entry count exceeds index budget");
        if (depth == max_directory_depth && children != 0) invalid("SKYF directory nesting exceeds 64 levels");
        directories_.push_back(checked_path(parent, component));
        // deque keeps this reference stable while recursively appending child paths.
        const auto& path = directories_.back();
        for (unsigned i = 0; i < files; ++i) {
            const auto filename = name<128>();
            const auto offset = u32();
            const auto size = u32();
            if (offset > archive_.file_size || size > archive_.file_size - offset)
                invalid("SKYF entry range exceeds EOF");
            archive_.entries.push_back({checked_path(path, filename), offset, size});
        }
        for (unsigned i = 0; i < children; ++i) directory(path, depth + 1);
    }
    std::istream& input_;
    std::uint64_t limit_;
    std::uint64_t position_ = 0;
    std::uint64_t path_bytes_ = 0;
    Archive archive_;
    std::deque<std::string> directories_;
};
} // namespace

Archive read_archive(std::istream& input, std::uint64_t file_size) {
    return IndexReader(input, file_size).parse();
}
Prefix read_prefix(std::istream& input, const Entry& entry) {
    Prefix prefix;
    prefix.size = std::min<std::size_t>(prefix.bytes.size(), entry.size);
    if (entry.offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
        invalid("SKYF entry offset cannot be represented by this stream");
    input.seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
    if (!input) invalid("cannot seek to SKYF entry");
    if (prefix.size != 0) {
        input.read(reinterpret_cast<char*>(prefix.bytes.data()), static_cast<std::streamsize>(prefix.size));
        if (input.gcount() != static_cast<std::streamsize>(prefix.size) || !input)
            invalid("truncated SKYF payload prefix");
    }
    return prefix;
}
std::vector<std::uint8_t> read_entry(std::istream& input, const Entry& entry, std::uint64_t max_bytes) {
    const auto end = static_cast<std::uint64_t>(entry.offset) + entry.size;
    if (entry.size > max_bytes || entry.size > std::numeric_limits<std::size_t>::max() ||
        entry.size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()) ||
        end > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
        invalid("SKYF entry exceeds byte budget or stream range");
    input.seekg(0, std::ios::end);
    const auto file_end = input.tellg();
    if (!input || file_end < 0 || end > static_cast<std::uint64_t>(file_end))
        invalid("SKYF entry range exceeds stream EOF");
    input.seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
    if (!input) invalid("cannot seek to SKYF entry");
    std::vector<std::uint8_t> bytes(entry.size);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size()))
            invalid("truncated SKYF payload");
    }
    return bytes;
}
DdsHeader read_dds_header(std::span<const std::uint8_t> prefix, std::uint64_t entry_size) {
    if (prefix.size() < 128 || entry_size < 128) invalid("truncated DDS header");
    if (prefix[0] != 'D' || prefix[1] != 'D' || prefix[2] != 'S' || prefix[3] != ' ')
        invalid("invalid DDS signature");
    const auto field = [prefix](std::size_t offset) { return le32(prefix.data() + offset); };
    if (field(4) != 124 || field(76) != 32) invalid("invalid DDS header structure size");
    const auto flags = field(8);
    const auto caps = field(108);
    if ((flags & 0x1007U) != 0x1007U || (caps & 0x1000U) == 0)
        invalid("DDS header lacks required flags or texture capability");
    if (field(112) != 0 || field(24) > 1 || (flags & 0x800000U) != 0)
        invalid("unsupported DDS cube, volume or extended surface header");
    DdsHeader header{field(16), field(12), 1, {}, 0};
    if (header.width == 0 || header.height == 0) invalid("invalid DDS dimensions");
    if ((flags & 0x20000U) != 0) {
        header.mip_levels = field(28);
        unsigned maximum = 1;
        for (auto side = std::max(header.width, header.height); side > 1; side >>= 1) ++maximum;
        if (header.mip_levels == 0 || header.mip_levels > maximum) invalid("invalid DDS mip count");
        if (header.mip_levels > 1 && (caps & 0x400008U) != 0x400008U)
            invalid("DDS mip chain lacks required capabilities");
    } else if (field(28) > 1) {
        invalid("DDS mip count is present without its header flag");
    }
    const auto pixel_flags = field(80);
    unsigned block_bytes = 0;
    if ((pixel_flags & 4U) != 0) {
        if ((pixel_flags & ~5U) != 0 || field(88) != 0 || field(92) != 0 || field(96) != 0 ||
            field(100) != 0 || field(104) != 0) invalid("invalid DDS FourCC pixel format");
        for (std::size_t i = 0; i < 4; ++i) header.fourcc[i] = static_cast<char>(prefix[84 + i]);
        const std::string_view code(header.fourcc.data(), header.fourcc.size());
        if (code == "DX10") invalid("unsupported DDS DX10 header (requires bytes beyond the 128-byte inspection limit)");
        if (code == "DXT1" || code == "ATI1" || code == "BC4U" || code == "BC4S") block_bytes = 8;
        else if (code == "DXT2" || code == "DXT3" || code == "DXT4" || code == "DXT5" ||
                 code == "ATI2" || code == "BC5U" || code == "BC5S") block_bytes = 16;
        else invalid("unsupported DDS FourCC");
    } else {
        const bool rgb = (pixel_flags & 0x40U) != 0;
        const bool luminance = (pixel_flags & 0x20000U) != 0;
        if (rgb == luminance || (pixel_flags & ~(0x40U | 0x20000U | 1U)) != 0 || field(84) != 0)
            invalid("unsupported DDS pixel format");
        header.bits_per_pixel = field(88);
        const auto bits = header.bits_per_pixel;
        if (!(bits == 8 || bits == 16 || (rgb && (bits == 24 || bits == 32))))
            invalid("unsupported DDS pixel bit count");
        const std::uint32_t allowed = bits == 32 ? std::numeric_limits<std::uint32_t>::max() : (1U << bits) - 1;
        const auto red = field(92), green = field(96), blue = field(100), alpha = field(104);
        const auto combined = red | green | blue | alpha;
        if (red == 0 || (rgb && (green == 0 || blue == 0)) || (luminance && (green != 0 || blue != 0)) ||
            (combined & ~allowed) != 0 || (red & (green | blue | alpha)) != 0 ||
            (green & (blue | alpha)) != 0 || (blue & alpha) != 0 ||
            (((pixel_flags & 1U) != 0) != (alpha != 0))) invalid("invalid DDS pixel masks");
    }
    // Check minimum space for the declared surfaces without reading or decoding payload.
    auto available = entry_size - 128;
    auto width = header.width, height = header.height;
    for (std::uint32_t level = 0; level < header.mip_levels; ++level) {
        const std::uint64_t rows = block_bytes ? (static_cast<std::uint64_t>(height) + 3) / 4 : height;
        const std::uint64_t row_bytes = block_bytes ? ((static_cast<std::uint64_t>(width) + 3) / 4) * block_bytes
                                                  : (static_cast<std::uint64_t>(width) * header.bits_per_pixel + 7) / 8;
        if (row_bytes > available / rows) invalid("DDS entry is too short for its declared dimensions and mips");
        available -= row_bytes * rows;
        width = std::max(1U, width / 2);
        height = std::max(1U, height / 2);
    }
    return header;
}
namespace {
using Rgba = std::array<std::uint8_t, 4>;
Rgba rgb565(std::uint16_t value) {
    const auto r = (value >> 11) & 31U, g = (value >> 5) & 63U, b = value & 31U;
    return {static_cast<std::uint8_t>((r << 3) | (r >> 2)),
            static_cast<std::uint8_t>((g << 2) | (g >> 4)),
            static_cast<std::uint8_t>((b << 3) | (b >> 2)), 255};
}
std::array<Rgba, 4> dxt_colors(const std::uint8_t* block, bool dxt1) {
    const auto first = static_cast<std::uint16_t>(block[0] | (static_cast<unsigned>(block[1]) << 8));
    const auto second = static_cast<std::uint16_t>(block[2] | (static_cast<unsigned>(block[3]) << 8));
    std::array<Rgba, 4> colors{rgb565(first), rgb565(second), Rgba{0, 0, 0, 255}, Rgba{0, 0, 0, 255}};
    for (unsigned channel = 0; channel < 3; ++channel) {
        if (first > second || !dxt1) {
            colors[2][channel] = static_cast<std::uint8_t>((2U * colors[0][channel] + colors[1][channel]) / 3);
            colors[3][channel] = static_cast<std::uint8_t>((colors[0][channel] + 2U * colors[1][channel]) / 3);
        } else {
            colors[2][channel] = static_cast<std::uint8_t>((colors[0][channel] + colors[1][channel]) / 2);
        }
    }
    if (dxt1 && first <= second) colors[3][3] = 0;
    return colors;
}
struct ChannelMask {
    std::uint32_t mask;
    unsigned shift;
    std::uint32_t maximum;
    explicit ChannelMask(std::uint32_t value)
        : mask(value), shift(value == 0 ? 0 : std::countr_zero(value)), maximum(value >> shift) {
        if (maximum != 0 && (maximum & (maximum + 1U)) != 0)
            invalid("unsupported non-contiguous DDS channel mask");
    }
    std::uint8_t decode(std::uint32_t value, std::uint8_t absent = 0) const {
        if (mask == 0) return absent;
        return static_cast<std::uint8_t>((static_cast<std::uint64_t>((value & mask) >> shift) * 255 +
                                          maximum / 2) / maximum);
    }
};
// One mip's DXT1/3/5 blocks (tightly packed, source order) to RGBA8 rows of mip.width*4 bytes.
void decode_blocks(const std::uint8_t* block, std::uint32_t width, std::uint32_t height, DdsFormat format,
                   std::uint8_t* output_base) {
    const bool is_dxt1 = format == DdsFormat::bc1, is_dxt3 = format == DdsFormat::bc2, is_dxt5 = format == DdsFormat::bc3;
    const unsigned block_bytes = is_dxt1 ? 8 : 16;
    const auto blocks_x = (static_cast<std::uint64_t>(width) + 3) / 4;
    const auto blocks_y = (static_cast<std::uint64_t>(height) + 3) / 4;
    for (std::uint64_t by = 0; by < blocks_y; ++by) {
        for (std::uint64_t bx = 0; bx < blocks_x; ++bx, block += block_bytes) {
            const auto* color_block = block + (is_dxt1 ? 0 : 8);
            const auto colors = dxt_colors(color_block, is_dxt1);
            const auto selectors = le32(color_block + 4);
            std::array<std::uint8_t, 8> alphas{};
            std::uint64_t alpha_selectors = 0;
            if (is_dxt5) {
                alphas[0] = block[0];
                alphas[1] = block[1];
                if (alphas[0] > alphas[1]) {
                    for (unsigned i = 2; i < 8; ++i)
                        alphas[i] = static_cast<std::uint8_t>(((8 - i) * alphas[0] + (i - 1) * alphas[1]) / 7);
                } else {
                    for (unsigned i = 2; i < 6; ++i)
                        alphas[i] = static_cast<std::uint8_t>(((6 - i) * alphas[0] + (i - 1) * alphas[1]) / 5);
                    alphas[6] = 0;
                    alphas[7] = 255;
                }
                for (unsigned i = 0; i < 6; ++i) alpha_selectors |= static_cast<std::uint64_t>(block[2 + i]) << (8 * i);
            }
            for (unsigned i = 0; i < 16; ++i) {
                const auto x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x >= width || y >= height) continue;
                auto color = colors[(selectors >> (2 * i)) & 3U];
                if (is_dxt3) color[3] = static_cast<std::uint8_t>(((block[i / 2] >> (4 * (i % 2))) & 15U) * 17);
                else if (is_dxt5) color[3] = alphas[(alpha_selectors >> (3 * i)) & 7U];
                std::copy(color.begin(), color.end(), output_base + static_cast<std::size_t>((y * width + x) * 4));
            }
        }
    }
}
} // namespace

std::uint64_t dds_mip_bytes(DdsFormat format, std::uint32_t width, std::uint32_t height) {
    if (format == DdsFormat::rgba8) return static_cast<std::uint64_t>(width) * height * 4;
    return ((static_cast<std::uint64_t>(width) + 3) / 4) * ((static_cast<std::uint64_t>(height) + 3) / 4) *
           (format == DdsFormat::bc1 ? 8 : 16);
}

DdsImage decode_dds(std::span<const std::uint8_t> bytes, bool keep_blocks) {
    if (bytes.size() > max_entry_bytes) invalid("DDS input exceeds 512 MiB budget");
    if (bytes.size() < 128) invalid("truncated DDS header");
    const auto field = [bytes](std::size_t offset) { return le32(bytes.data() + offset); };
    auto width = field(16), height = field(12);
    const auto mip_levels = (field(8) & 0x20000U) != 0 ? field(28) : 1U;
    const auto maximum = static_cast<unsigned>(std::bit_width(std::max(width, height)));
    if (mip_levels == 0 || mip_levels > maximum) invalid("invalid DDS mip count");
    // Budget the whole chain before payload validation, without allocating large input or output.
    const auto decoded_limit = std::min(max_entry_bytes,
                                       static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()));
    std::array<DdsMip, 32> mips{};
    std::uint64_t decoded_bytes = 0;
    for (std::uint32_t level = 0; level < mip_levels; ++level) {
        const auto pixels = static_cast<std::uint64_t>(width) * height;
        if (pixels > (decoded_limit - decoded_bytes) / 4)
            invalid("DDS decoded RGBA exceeds 512 MiB budget or addressable size");
        const auto size = pixels * 4;
        mips[level] = {width, height, static_cast<std::size_t>(decoded_bytes), static_cast<std::size_t>(size)};
        decoded_bytes += size;
        width = std::max(1U, width / 2);
        height = std::max(1U, height / 2);
    }
    const auto header = read_dds_header(bytes, bytes.size());
    const std::string_view code(header.fourcc.data(), header.fourcc.size());
    const bool compressed = (field(80) & 4U) != 0;
    const bool is_dxt1 = code == "DXT1", is_dxt3 = code == "DXT3", is_dxt5 = code == "DXT5";
    if (compressed ? (!is_dxt1 && !is_dxt3 && !is_dxt5)
                   : ((field(80) & 0x40U) == 0 || (header.bits_per_pixel != 16 && header.bits_per_pixel != 32)))
        invalid("unsupported DDS decoder format (requires DXT1/3/5 or RGB16/RGB32)");
    const unsigned block_bytes = is_dxt1 ? 8 : 16;
    const unsigned pixel_bytes = header.bits_per_pixel / 8;
    const auto tight_pitch = static_cast<std::uint64_t>(header.width) * pixel_bytes;
    const bool has_pitch = (field(8) & 8U) != 0;
    if (compressed && has_pitch) invalid("unsupported compressed DDS row pitch");
    const auto pitch = !compressed && has_pitch ? static_cast<std::uint64_t>(field(20)) : tight_pitch;
    if (!compressed && pitch < tight_pitch) invalid("DDS pitch is shorter than its pixel row");
    // Only the top mip has an explicit pitch in legacy DDS. Lower mips are tightly packed.
    struct SourceMip { std::size_t offset, pitch; };
    std::array<SourceMip, 32> sources{};
    std::uint64_t source_offset = 128;
    for (std::uint32_t level = 0; level < header.mip_levels; ++level) {
        const auto& mip = mips[level];
        const std::uint64_t rows = compressed ? (static_cast<std::uint64_t>(mip.height) + 3) / 4 : mip.height;
        const std::uint64_t row_bytes = compressed ? ((static_cast<std::uint64_t>(mip.width) + 3) / 4) * block_bytes
                                                  : (level == 0 ? pitch : static_cast<std::uint64_t>(mip.width) * pixel_bytes);
        if (row_bytes > (bytes.size() - source_offset) / rows)
            invalid("truncated DDS pitched surface or mip payload");
        sources[level] = {static_cast<std::size_t>(source_offset), static_cast<std::size_t>(row_bytes)};
        source_offset += row_bytes * rows;
    }
    const auto format = !compressed ? DdsFormat::rgba8 : is_dxt1 ? DdsFormat::bc1 : is_dxt3 ? DdsFormat::bc2 : DdsFormat::bc3;
    if (keep_blocks && compressed) { // Mips are consecutive in the source: copy the validated payload once.
        DdsImage image{header.width, header.height, {}, {}, format};
        image.mips.assign(mips.begin(), mips.begin() + header.mip_levels);
        for (std::uint32_t level = 0; level < header.mip_levels; ++level) {
            auto& mip = image.mips[level];
            mip.offset = sources[level].offset - 128;
            mip.size = static_cast<std::size_t>(dds_mip_bytes(format, mip.width, mip.height));
        }
        image.rgba.assign(bytes.begin() + 128, bytes.begin() + static_cast<std::ptrdiff_t>(source_offset));
        return image;
    }
    const ChannelMask red(field(92)), green(field(96)), blue(field(100)), alpha(field(104));
    DdsImage image{header.width, header.height, {}, {}};
    image.mips.assign(mips.begin(), mips.begin() + header.mip_levels);
    image.rgba.resize(static_cast<std::size_t>(decoded_bytes));
    for (std::uint32_t level = 0; level < header.mip_levels; ++level) {
        const auto& mip = image.mips[level];
        const auto& source = sources[level];
        auto* output_base = image.rgba.data() + mip.offset;
        if (!compressed) {
            for (std::uint32_t y = 0; y < mip.height; ++y) {
                const auto* row = bytes.data() + source.offset + static_cast<std::size_t>(y) * source.pitch;
                auto* output = output_base + static_cast<std::size_t>(y) * mip.width * 4;
                for (std::uint32_t x = 0; x < mip.width; ++x) {
                    std::uint32_t value = 0;
                    for (unsigned b = 0; b < pixel_bytes; ++b) value |= static_cast<std::uint32_t>(*row++) << (8 * b);
                    *output++ = red.decode(value);
                    *output++ = green.decode(value);
                    *output++ = blue.decode(value);
                    *output++ = alpha.decode(value, 255);
                }
            }
            continue;
        }
        decode_blocks(bytes.data() + source.offset, mip.width, mip.height, format, output_base);
    }
    return image;
}

DdsImage decompress_dds(const DdsImage& image) {
    if (image.format == DdsFormat::rgba8) return image;
    if (image.mips.empty() || image.mips.size() > 32) invalid("BC image lacks a bounded mip chain");
    DdsImage output{image.width, image.height, {}, {}};
    std::uint64_t source = 0, decoded = 0;
    for (const auto& mip : image.mips) {
        if (mip.offset != source || mip.size != dds_mip_bytes(image.format, mip.width, mip.height) ||
            mip.size > image.rgba.size() - std::min<std::size_t>(mip.offset, image.rgba.size()))
            invalid("invalid BC mip layout");
        const auto size = dds_mip_bytes(DdsFormat::rgba8, mip.width, mip.height);
        if (size > max_entry_bytes - decoded) invalid("BC image decoded RGBA exceeds 512 MiB budget");
        output.mips.push_back({mip.width, mip.height, static_cast<std::size_t>(decoded), static_cast<std::size_t>(size)});
        source += mip.size; decoded += size;
    }
    if (source != image.rgba.size()) invalid("trailing BC mip bytes");
    output.rgba.resize(static_cast<std::size_t>(decoded));
    for (std::size_t level = 0; level < image.mips.size(); ++level)
        decode_blocks(image.rgba.data() + image.mips[level].offset, image.mips[level].width, image.mips[level].height,
                      image.format, output.rgba.data() + output.mips[level].offset);
    return output;
}

void decoder_self_test() {
    const auto require = [](bool condition) { if (!condition) invalid("DDS decoder self-test failed"); };
    const auto put = [](std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    const auto fixture = [&](std::uint32_t width, std::uint32_t height, std::size_t payload,
                             std::string_view fourcc) {
        std::vector<std::uint8_t> bytes(128 + payload);
        std::copy_n("DDS ", 4, bytes.begin());
        put(bytes, 4, 124); put(bytes, 8, 0x1007); put(bytes, 12, height); put(bytes, 16, width);
        put(bytes, 76, 32); put(bytes, 108, 0x1000);
        if (!fourcc.empty()) {
            put(bytes, 80, 4);
            std::copy_n(fourcc.begin(), 4, bytes.begin() + 84);
        }
        return bytes;
    };
    const auto rejects = [&](const std::vector<std::uint8_t>& bytes) {
        bool rejected = false;
        try { (void)decode_dds(bytes); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected);
    };
    const auto require_mip = [&](const DdsImage& image, std::size_t level, std::uint32_t width,
                                 std::uint32_t height, std::size_t offset, std::size_t size) {
        require(level < image.mips.size());
        const auto& mip = image.mips[level];
        require(mip.width == width && mip.height == height && mip.offset == offset && mip.size == size);
        require(offset <= image.rgba.size() && size <= image.rgba.size() - offset);
    };
    const auto require_single_mip = [&](const DdsImage& image) {
        require(image.mips.size() == 1);
        require_mip(image, 0, image.width, image.height, 0, image.rgba.size());
        require(image.rgba.size() == static_cast<std::uint64_t>(image.width) * image.height * 4);
    };
    auto dxt1 = fixture(4, 4, 8, "DXT1");
    dxt1[128] = 0; dxt1[129] = 0xf8; dxt1[130] = 0xe0; dxt1[131] = 7;
    put(dxt1, 132, 0xe4e4e4e4);
    const auto decoded1 = decode_dds(dxt1);
    const std::array<std::uint8_t, 16> row{255, 0, 0, 255, 0, 255, 0, 255, 170, 85, 0, 255, 85, 170, 0, 255};
    require(decoded1.width == 4 && decoded1.height == 4 && decoded1.rgba.size() == 64);
    require_single_mip(decoded1);
    for (unsigned y = 0; y < 4; ++y) require(std::equal(row.begin(), row.end(), decoded1.rgba.begin() + y * 16));
    auto transparent = fixture(1, 1, 8, "DXT1");
    transparent[130] = 255; transparent[131] = 255; put(transparent, 132, 0xffffffff);
    const auto decoded_transparent = decode_dds(transparent);
    require(decoded_transparent.rgba == std::vector<std::uint8_t>({0, 0, 0, 0}));
    require_single_mip(decoded_transparent);
    auto dxt5 = fixture(4, 4, 16, "DXT5");
    dxt5[128] = 200; dxt5[129] = 100; dxt5[138] = 255; dxt5[139] = 255;
    put(dxt5, 140, 0xaaaaaaaa);
    const auto set_alpha = [&](unsigned selector) {
        std::uint64_t packed = 0;
        for (unsigned i = 0; i < 16; ++i) packed |= static_cast<std::uint64_t>(selector) << (3 * i);
        for (unsigned i = 0; i < 6; ++i) dxt5[130 + i] = static_cast<std::uint8_t>(packed >> (8 * i));
    };
    set_alpha(3);
    const auto decoded5 = decode_dds(dxt5);
    require_single_mip(decoded5);
    for (unsigned i = 0; i < 16; ++i) {
        require(decoded5.rgba[i * 4] == 85 && decoded5.rgba[i * 4 + 1] == 85 &&
                decoded5.rgba[i * 4 + 2] == 85 && decoded5.rgba[i * 4 + 3] == 171);
    }
    dxt5[128] = 10; dxt5[129] = 20;
    for (const unsigned selector : {3U, 6U, 7U}) {
        set_alpha(selector);
        const auto image = decode_dds(dxt5);
        require_single_mip(image);
        for (unsigned i = 0; i < 16; ++i) require(image.rgba[i * 4 + 3] == (selector == 3 ? 14 : selector == 6 ? 0 : 255));
    }
    auto dxt3 = fixture(4, 4, 16, "DXT3");
    std::fill_n(dxt3.begin() + 128, 8, 0xf0);
    dxt3[137] = 0xf8;
    const auto decoded3 = decode_dds(dxt3);
    require_single_mip(decoded3);
    for (unsigned i = 0; i < 16; ++i) require(decoded3.rgba[i * 4] == 255 && decoded3.rgba[i * 4 + 3] == (i % 2 ? 255 : 0));
    auto rgb16 = fixture(1, 2, 8, {});
    put(rgb16, 8, 0x100f); put(rgb16, 20, 4); put(rgb16, 80, 0x40); put(rgb16, 88, 16);
    put(rgb16, 92, 0xf800); put(rgb16, 96, 0x07e0); put(rgb16, 100, 0x001f);
    rgb16[129] = 0xf8; rgb16[132] = 0x1f;
    const auto decoded16 = decode_dds(rgb16);
    require(decoded16.rgba == std::vector<std::uint8_t>({255, 0, 0, 255, 0, 0, 255, 255}));
    require_single_mip(decoded16);
    auto rgb32 = fixture(1, 1, 4, {});
    put(rgb32, 80, 0x41); put(rgb32, 88, 32); put(rgb32, 92, 0xff00);
    put(rgb32, 96, 0xff0000); put(rgb32, 100, 0xff); put(rgb32, 104, 0xff000000);
    put(rgb32, 128, 0x28140a1e);
    const auto decoded32 = decode_dds(rgb32);
    require(decoded32.rgba == std::vector<std::uint8_t>({10, 20, 30, 40}));
    require_single_mip(decoded32);
    auto mip = dxt1;
    mip.resize(144);
    put(mip, 8, 0x21007); put(mip, 28, 2); put(mip, 108, 0x401008);
    mip[136] = 0x1f; mip[138] = 0xe0; mip[139] = 7; put(mip, 140, 0x404);
    const auto decoded_mip = decode_dds(mip);
    require(decoded_mip.width == 4 && decoded_mip.height == 4 && decoded_mip.rgba.size() == 80 &&
            decoded_mip.mips.size() == 2);
    require_mip(decoded_mip, 0, 4, 4, 0, 64);
    require_mip(decoded_mip, 1, 2, 2, 64, 16);
    require(std::equal(decoded1.rgba.begin(), decoded1.rgba.end(), decoded_mip.rgba.begin()));
    const std::array<std::uint8_t, 16> mip_pixels{0, 0, 255, 255, 0, 255, 0, 255,
                                               0, 0, 255, 255, 0, 255, 0, 255};
    require(std::equal(mip_pixels.begin(), mip_pixels.end(), decoded_mip.rgba.begin() + 64));
    auto truncated_mip = mip; truncated_mip.pop_back(); rejects(truncated_mip);
    mip.resize(152); put(mip, 28, 3); mip[144] = 0xe0; mip[145] = 0xff;
    const auto decoded_chain = decode_dds(mip);
    require(decoded_chain.mips.size() == 3 && decoded_chain.rgba.size() == 84);
    require_mip(decoded_chain, 0, 4, 4, 0, 64);
    require_mip(decoded_chain, 1, 2, 2, 64, 16);
    require_mip(decoded_chain, 2, 1, 1, 80, 4);
    require(std::equal(decoded_mip.rgba.begin(), decoded_mip.rgba.end(), decoded_chain.rgba.begin()));
    // keep_blocks keeps the authored payload byte-for-byte; decompress_dds reproduces decode_dds exactly.
    const auto require_blocks = [&](const std::vector<std::uint8_t>& bytes, DdsFormat format) {
        const auto decoded = decode_dds(bytes), blocks = decode_dds(bytes, true);
        require(blocks.format == format && blocks.mips.size() == decoded.mips.size());
        require(std::equal(blocks.rgba.begin(), blocks.rgba.end(), bytes.begin() + 128) && blocks.rgba.size() == bytes.size() - 128);
        std::size_t offset = 0;
        for (const auto& level : blocks.mips) {
            require(level.offset == offset && level.size == dds_mip_bytes(format, level.width, level.height));
            offset += level.size;
        }
        const auto expanded = decompress_dds(blocks);
        require(expanded.format == DdsFormat::rgba8 && expanded.rgba == decoded.rgba && expanded.mips.size() == decoded.mips.size());
        for (std::size_t level = 0; level < decoded.mips.size(); ++level)
            require(expanded.mips[level].offset == decoded.mips[level].offset && expanded.mips[level].size == decoded.mips[level].size);
        auto broken = blocks; broken.rgba.pop_back();
        bool rejected = false;
        try { (void)decompress_dds(broken); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected);
    };
    require_blocks(mip, DdsFormat::bc1);
    require_blocks(transparent, DdsFormat::bc1);
    require(decode_dds(rgb16, true).format == DdsFormat::rgba8 && decode_dds(rgb16, true).rgba == decoded16.rgba);
    const std::array<std::uint8_t, 4> last_pixel{255, 255, 0, 255};
    require(std::equal(last_pixel.begin(), last_pixel.end(), decoded_chain.rgba.begin() + 80));
    for (const auto* base : {&dxt3, &dxt5}) {
        mip = *base; mip.resize(160);
        put(mip, 8, 0x21007); put(mip, 28, 2); put(mip, 108, 0x401008);
        mip[144] = 0xff; mip[145] = 0xff;
        mip[153] = 0xf8;
        const auto image = decode_dds(mip);
        const auto base_image = decode_dds(*base);
        require(image.mips.size() == 2 && image.rgba.size() == 80);
        require_mip(image, 0, 4, 4, 0, 64);
        require_mip(image, 1, 2, 2, 64, 16);
        require(std::equal(base_image.rgba.begin(), base_image.rgba.end(), image.rgba.begin()));
        for (unsigned i = 0; i < 4; ++i) {
            const auto offset = 64 + i * 4;
            const auto expected_alpha = base == &dxt3 && i >= 2 ? 0 : 255;
            require(image.rgba[offset] == 255 && image.rgba[offset + 1] == 0 &&
                    image.rgba[offset + 2] == 0 && image.rgba[offset + 3] == expected_alpha);
        }
        require_blocks(mip, base == &dxt3 ? DdsFormat::bc2 : DdsFormat::bc3);
        truncated_mip = mip; truncated_mip.pop_back(); rejects(truncated_mip);
    }
    mip = rgb16; mip.resize(138);
    put(mip, 8, 0x2100f); put(mip, 28, 2); put(mip, 108, 0x401008);
    mip[136] = 0xe0; mip[137] = 7;
    const auto decoded_rgb_mip = decode_dds(mip);
    require(decoded_rgb_mip.mips.size() == 2 && decoded_rgb_mip.rgba.size() == 12);
    require_mip(decoded_rgb_mip, 0, 1, 2, 0, 8);
    require_mip(decoded_rgb_mip, 1, 1, 1, 8, 4);
    require(std::equal(decoded16.rgba.begin(), decoded16.rgba.end(), decoded_rgb_mip.rgba.begin()));
    const std::array<std::uint8_t, 4> green_pixel{0, 255, 0, 255};
    require(std::equal(green_pixel.begin(), green_pixel.end(), decoded_rgb_mip.rgba.begin() + 8));
    truncated_mip = mip; truncated_mip.pop_back(); rejects(truncated_mip);
    mip = rgb32; mip.resize(148);
    put(mip, 16, 2); put(mip, 12, 2);
    put(mip, 8, 0x21007); put(mip, 28, 2); put(mip, 108, 0x401008);
    for (unsigned i = 0; i < 4; ++i) put(mip, 128 + i * 4, 0x28140a1e);
    put(mip, 144, 0x50463c32);
    const auto decoded_rgb32_mip = decode_dds(mip);
    require(decoded_rgb32_mip.mips.size() == 2 && decoded_rgb32_mip.rgba.size() == 20);
    require_mip(decoded_rgb32_mip, 0, 2, 2, 0, 16);
    require_mip(decoded_rgb32_mip, 1, 1, 1, 16, 4);
    for (unsigned i = 0; i < 4; ++i)
        require(std::equal(decoded32.rgba.begin(), decoded32.rgba.end(), decoded_rgb32_mip.rgba.begin() + i * 4));
    const std::array<std::uint8_t, 4> rgb32_mip_pixel{60, 70, 50, 80};
    require(std::equal(rgb32_mip_pixel.begin(), rgb32_mip_pixel.end(), decoded_rgb32_mip.rgba.begin() + 16));
    truncated_mip = mip; truncated_mip.pop_back(); rejects(truncated_mip);
    auto bad = dxt1;
    bad.pop_back(); rejects(bad);
    bad = dxt1; bad.resize(127); rejects(bad);
    bad = dxt1; put(bad, 16, 0); rejects(bad);
    bad = dxt1; bad[0] = 0; rejects(bad);
    bad = dxt1; std::copy_n("DX10", 4, bad.begin() + 84); rejects(bad);
    bad = dxt1; std::copy_n("DXT2", 4, bad.begin() + 84); rejects(bad);
    bad = dxt1; std::copy_n("ATI1", 4, bad.begin() + 84); rejects(bad);
    bad = dxt1; put(bad, 112, 0x200); rejects(bad);
    bad = dxt1; put(bad, 8, 0x21007); put(bad, 28, 2); put(bad, 108, 0x401008); rejects(bad);
    bad = dxt1; put(bad, 16, 0xffffffff); put(bad, 12, 0xffffffff); rejects(bad);
    bad = rgb16; put(bad, 20, 1); rejects(bad);
    bad = rgb16; put(bad, 20, 0xffffffff); rejects(bad);
    bad = rgb16; bad.resize(134); rejects(bad);
    bad = rgb32; put(bad, 92, 0x0a00); rejects(bad);
    bad = rgb32; put(bad, 96, 0xff00); rejects(bad);
    bad = fixture(32768, 4097, 8, "DXT1");
    bool bomb_rejected = false;
    try { (void)decode_dds(bad); }
    catch (const std::runtime_error& error) {
        bomb_rejected = std::string_view(error.what()).starts_with("DDS decoded RGBA exceeds");
    }
    require(bomb_rejected);
    bad = fixture(32768, 4096, 0, "DXT1");
    put(bad, 8, 0x21007); put(bad, 28, 2); put(bad, 108, 0x401008);
    bomb_rejected = false;
    try { (void)decode_dds(bad); }
    catch (const std::runtime_error& error) {
        bomb_rejected = std::string_view(error.what()).starts_with("DDS decoded RGBA exceeds");
    }
    require(bomb_rejected);
    std::istringstream input("prefixDATA");
    require(read_entry(input, Entry{"test", 6, 4}, 4) == std::vector<std::uint8_t>({'D', 'A', 'T', 'A'}));
    for (const Entry& entry : {Entry{"over-budget", 6, 4}, Entry{"past-EOF", 9, 2}}) {
        bool rejected = false;
        try { (void)read_entry(input, entry, entry.offset == 6 ? 3 : 4); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected);
    }
}
} // namespace ikv::assets
