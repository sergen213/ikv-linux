#include "ikv/assets.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <zlib.h>

namespace ikv::assets {
namespace {
[[noreturn]] void invalid_adf(const char* message) { throw std::runtime_error(message); }
std::uint32_t adf_le32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

// Original istanbul.exe: 0040fba0/0040fc00 (adaptive model), 0040f860/0040fa10 (arithmetic reader).
// Symbols 1..256 initially represent bytes 0..255; symbol 257 is the end marker.
class LegacyArithmetic {
public:
    explicit LegacyArithmetic(std::span<const std::uint8_t> bytes) : bytes_(bytes) {
        for (unsigned i = 0; i <= 257; ++i) {
            frequency_[i] = 1;
            cumulative_[i] = 257 - i;
            if (i >= 1 && i <= 256) symbols_[i] = static_cast<std::uint8_t>(i - 1);
        }
        frequency_[0] = 0;
        for (unsigned i = 0; i < 16; ++i) code_ = code_ * 2 + bit();
    }
    unsigned symbol(bool renormalize) {
        if (code_ < low_ || code_ > high_) invalid_adf("invalid ADF arithmetic code interval");
        const auto range = high_ - low_ + 1;
        const auto scaled = ((code_ - low_ + 1) * cumulative_[0] - 1) / range;
        unsigned symbol = 1;
        while (scaled < cumulative_[symbol]) ++symbol;
        high_ = low_ + cumulative_[symbol - 1] * range / cumulative_[0] - 1;
        low_ += cumulative_[symbol] * range / cumulative_[0];
        // ADF declares the finite output length. The last symbol needs no next-symbol lookahead.
        if (!renormalize) return symbol;
        for (;;) {
            if (high_ < 32768) {
                // Lower half: no interval translation.
            } else if (low_ >= 32768) {
                code_ -= 32768; low_ -= 32768; high_ -= 32768;
            } else if (low_ >= 16384 && high_ < 49152) {
                code_ -= 16384; low_ -= 16384; high_ -= 16384;
            } else {
                break;
            }
            low_ *= 2;
            high_ = high_ * 2 + 1;
            code_ = code_ * 2 + bit();
        }
        return symbol;
    }
    std::uint8_t byte(unsigned symbol) const { return symbols_[symbol]; }
    void update(unsigned symbol) {
        if (cumulative_[0] == 16383) {
            std::uint32_t sum = 0;
            for (unsigned i = 257; i != 0; --i) {
                cumulative_[i] = sum;
                frequency_[i] = (frequency_[i] + 1) / 2;
                sum += frequency_[i];
            }
            cumulative_[0] = sum;
        }
        unsigned first = symbol;
        while (frequency_[first] == frequency_[first - 1]) --first;
        if (first != symbol) std::swap(symbols_[first], symbols_[symbol]);
        ++frequency_[first];
        for (unsigned i = first; i != 0; --i) ++cumulative_[i - 1];
    }
private:
    unsigned bit() {
        // The original reader looks beyond the final stored byte while finishing the arithmetic interval.
        // Permit only its 16-bit zero lookahead, never an unchecked source read.
        const auto stored_bits = static_cast<std::uint64_t>(bytes_.size()) * 8;
        if (bit_position_ >= stored_bits + 16) invalid_adf("truncated ADF arithmetic bitstream");
        const auto value = bit_position_ < stored_bits
            ? (bytes_[static_cast<std::size_t>(bit_position_ / 8)] >> (bit_position_ % 8)) & 1U : 0U;
        ++bit_position_;
        return value;
    }
    std::span<const std::uint8_t> bytes_;
    std::uint64_t bit_position_ = 0;
    std::array<std::uint32_t, 258> frequency_{};
    std::array<std::uint32_t, 258> cumulative_{};
    std::array<std::uint8_t, 258> symbols_{};
    std::uint32_t low_ = 0, high_ = 65535, code_ = 0;
};

std::vector<std::uint8_t> decode_legacy(std::span<const std::uint8_t> bytes, std::size_t decoded_size) {
    if (decoded_size == 0) return {};
    LegacyArithmetic decoder(bytes);
    std::vector<std::uint8_t> output(decoded_size);
    for (std::size_t position = 0; position < decoded_size; ++position) {
        const bool more = position + 1 < decoded_size;
        const auto symbol = decoder.symbol(more);
        if (symbol == 257) invalid_adf("ADF arithmetic end marker precedes declared decoded size");
        output[position] = decoder.byte(symbol);
        if (more) decoder.update(symbol);
    }
    return output;
}
} // namespace

namespace {
std::vector<std::uint8_t> decode_zlib(std::span<const std::uint8_t> bytes, std::size_t decoded_size) {
    if (bytes.size() > std::numeric_limits<uInt>::max() ||
        decoded_size > std::numeric_limits<uInt>::max()) invalid_adf("ADF zlib size exceeds API bounds");
    std::vector<std::uint8_t> output(decoded_size);
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) invalid_adf("could not initialize ADF zlib decoder");
    struct EndInflate {
        z_stream* stream;
        ~EndInflate() { inflateEnd(stream); }
    } end{&stream};
    std::uint8_t overrun = 0;
    stream.next_in = const_cast<Bytef*>(bytes.data());
    stream.avail_in = static_cast<uInt>(bytes.size());
    stream.next_out = output.empty() ? &overrun : output.data();
    stream.avail_out = output.empty() ? 1 : static_cast<uInt>(output.size());
    for (;;) {
        const auto input_before = stream.total_in;
        const auto output_before = stream.total_out;
        const auto status = inflate(&stream, Z_NO_FLUSH);
        if (stream.total_out > decoded_size) invalid_adf("ADF zlib stream exceeds declared decoded size");
        if (status == Z_STREAM_END) {
            if (stream.total_out != decoded_size || stream.avail_in != 0)
                invalid_adf("ADF zlib size mismatch or trailing compressed data");
            return output;
        }
        if (status != Z_OK || (stream.total_in == input_before && stream.total_out == output_before))
            invalid_adf("invalid or truncated ADF zlib stream");
        if (stream.avail_out == 0) {
            stream.next_out = &overrun;
            stream.avail_out = 1;
        }
    }
}
} // namespace

std::vector<AdfChunk> decode_adf(std::span<const std::uint8_t> bytes) {
    constexpr std::size_t header_size = 24;
    constexpr std::size_t record_size = 32;
    if (bytes.size() > max_entry_bytes) invalid_adf("ADF input exceeds byte limit");
    if (bytes.size() < header_size || !std::equal(bytes.begin(), bytes.begin() + 4, "root"))
        invalid_adf("invalid ADF root header");
    const auto count = adf_le32(bytes.data() + 4);
    if (adf_le32(bytes.data() + 8) != record_size) invalid_adf("invalid ADF record stride");
    using Range = std::pair<std::size_t, std::size_t>;
    if (count > (bytes.size() - header_size) / record_size ||
        count > (max_index_bytes - header_size) / record_size ||
        count > max_index_bytes / (sizeof(AdfChunk) + sizeof(Range)))
        invalid_adf("ADF table exceeds input or metadata limits");
    const auto table_end = header_size + static_cast<std::size_t>(count) * record_size;
    const auto codec = adf_le32(bytes.data() + 12);
    std::uint64_t decoded_total = 0;
    // Validate every declaration before allocating payloads or dispatching any codec.
    for (std::size_t i = 0; i < count; ++i) {
        const auto* record = bytes.data() + header_size + i * record_size;
        const auto stored_size = adf_le32(record + 8);
        const auto decoded_size = adf_le32(record + 12);
        const auto flag = adf_le32(record + 16);
        const auto offset = adf_le32(record + 20);
        if (offset < table_end || offset > bytes.size() || stored_size > bytes.size() - offset)
            invalid_adf("ADF payload lies outside data area");
        if (stored_size > max_entry_bytes || decoded_size > max_entry_bytes ||
            decoded_size > max_entry_bytes - decoded_total)
            invalid_adf("ADF payload exceeds decoded byte limit");
        if (flag == 0 && stored_size != decoded_size) invalid_adf("ADF stored size mismatch");
        decoded_total += decoded_size;
    }
    std::vector<Range> ranges;
    ranges.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto* record = bytes.data() + header_size + i * record_size;
        const auto size = adf_le32(record + 8);
        const auto offset = adf_le32(record + 20);
        if (size != 0) ranges.emplace_back(offset, static_cast<std::size_t>(offset) + size);
    }
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t i = 1; i < ranges.size(); ++i)
        if (ranges[i].first < ranges[i - 1].second) invalid_adf("overlapping ADF payloads");
    std::vector<AdfChunk> chunks;
    chunks.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto* record = bytes.data() + header_size + i * record_size;
        const auto stored_size = adf_le32(record + 8);
        const auto decoded_size = adf_le32(record + 12);
        const auto flag = adf_le32(record + 16);
        const auto payload = bytes.subspan(adf_le32(record + 20), stored_size);
        auto data = flag == 0 ? std::vector<std::uint8_t>(payload.begin(), payload.end()) :
                    codec == 255 ? decode_zlib(payload, decoded_size) : decode_legacy(payload, decoded_size);
        chunks.push_back({std::string(reinterpret_cast<const char*>(record), 4),
                          adf_le32(record + 4), std::move(data)});
    }
    return chunks;
}

void adf_decoder_self_test() {
    const auto require = [](bool condition) {
        if (!condition) throw std::runtime_error("ADF decoder self-test failed");
    };
    const auto put32 = [&](std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
        require(value <= std::numeric_limits<std::uint32_t>::max());
        for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    const auto fixture = [&](std::span<const std::uint8_t> payload, std::size_t decoded_size,
                             std::uint32_t flag, std::uint32_t codec) {
        std::vector<std::uint8_t> bytes(56, 0xff);
        std::copy_n("root", 4, bytes.begin());
        put32(bytes, 4, 1);
        put32(bytes, 8, 32);
        put32(bytes, 12, codec);
        const std::array<std::uint8_t, 4> tag{'A', 0, 'F', '!'};
        std::copy(tag.begin(), tag.end(), bytes.begin() + 24);
        put32(bytes, 28, 42);
        put32(bytes, 32, payload.size());
        put32(bytes, 36, decoded_size);
        put32(bytes, 40, flag);
        put32(bytes, 44, 56);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        return bytes;
    };
    const auto reject = [&](const std::vector<std::uint8_t>& bytes) {
        try {
            (void)decode_adf(bytes);
        } catch (const std::runtime_error&) {
            return;
        }
        throw std::runtime_error("ADF decoder accepted malformed self-test input");
    };
    const std::vector<std::uint8_t> payload{'A', 0, 0xff, 'D', 'F'};
    const auto check = [&](const std::vector<std::uint8_t>& bytes,
                           const std::vector<std::uint8_t>& expected) {
        const auto chunks = decode_adf(bytes);
        require(chunks.size() == 1 && chunks[0].tag == std::string("A\0F!", 4) &&
                chunks[0].id == 42 && chunks[0].data == expected);
    };
    const auto compressed = [&](std::span<const std::uint8_t> source) {
        std::vector<std::uint8_t> result(compressBound(static_cast<uLong>(source.size())));
        uLongf size = result.size();
        const std::uint8_t empty = 0;
        require(compress2(result.data(), &size, source.empty() ? &empty : source.data(),
                          static_cast<uLong>(source.size()), Z_BEST_COMPRESSION) == Z_OK);
        result.resize(size);
        return result;
    };
    const auto stored = fixture(payload, payload.size(), 0, 255);
    check(stored, payload);
    check(fixture({}, 0, 0, 7), {});
    const auto zlib_bytes = compressed(payload);
    const auto zipped = fixture(zlib_bytes, payload.size(), 2, 255);
    check(zipped, payload);
    check(fixture(compressed({}), 0, 1, 255), {});
    const std::array<std::uint8_t, 1> arithmetic_eof{0};
    check(fixture(arithmetic_eof, 0, 1, 0), {});
    check(fixture(arithmetic_eof, 0, 9, 254), {});
    const std::array<std::uint8_t, 2> canonical_empty{0x00, 0x02};
    const std::array<std::uint8_t, 3> canonical_a{0x7d, 0x82, 0x02};
    const std::array<std::uint8_t, 7> canonical_abc{0x7d, 0xbf, 0x41, 0xdb, 0x5f, 0x02, 0x45};
    check(fixture(canonical_empty, 0, 1, 0), {});
    check(fixture(canonical_a, 1, 1, 0), {'A'});
    check(fixture(canonical_abc, 6, 1, 0), {'A', 'B', 'C', 'A', 'B', 'C'});
    check(fixture(std::span<const std::uint8_t>(canonical_a).first(2), 1, 1, 0), {'A'});
    check(fixture(canonical_abc, 3, 1, 0), {'A', 'B', 'C'});
    std::vector<std::uint8_t> rescale(257, 0xff);
    rescale[0] = 0xfd;
    rescale[1] = 0xfc;
    rescale[254] = 0x7f;
    rescale[255] = 0x13;
    rescale[256] = 0x3b;
    check(fixture(rescale, 20000, 1, 0), std::vector<std::uint8_t>(20000, 'A'));
    check(fixture(canonical_a, 0, 1, 0), {});
    reject(fixture(canonical_a, 2, 1, 0));
    reject(fixture(compressed(std::vector<std::uint8_t>(65536, 'A')), 1, 1, 255));

    auto malformed = stored;
    malformed.resize(23);
    reject(malformed);
    malformed = stored;
    malformed[0] = 'R';
    reject(malformed);
    for (const auto stride : {0U, 31U, 33U}) {
        malformed = stored;
        put32(malformed, 8, stride);
        reject(malformed);
    }
    for (const auto count : {2U, std::numeric_limits<std::uint32_t>::max()}) {
        malformed = stored;
        put32(malformed, 4, count);
        reject(malformed);
    }
    for (const auto offset : {0U, 24U, 55U, static_cast<std::uint32_t>(stored.size() + 1),
                              std::numeric_limits<std::uint32_t>::max()}) {
        malformed = stored;
        put32(malformed, 44, offset);
        reject(malformed);
    }
    malformed = stored;
    put32(malformed, 32, std::numeric_limits<std::uint32_t>::max());
    reject(malformed);
    malformed = stored;
    put32(malformed, 36, payload.size() + 1);
    reject(malformed);
    malformed = zipped;
    put32(malformed, 36, static_cast<std::uint32_t>(max_entry_bytes + 1));
    reject(malformed);
    for (const auto size : {0U, static_cast<unsigned>(payload.size() - 1),
                            static_cast<unsigned>(payload.size() + 1)}) {
        malformed = zipped;
        put32(malformed, 36, size);
        reject(malformed);
    }
    malformed = zipped;
    malformed.back() ^= 1;
    reject(malformed);
    malformed = zipped;
    malformed.pop_back();
    put32(malformed, 32, zlib_bytes.size() - 1);
    reject(malformed);
    malformed = zipped;
    malformed.push_back(0);
    put32(malformed, 32, zlib_bytes.size() + 1);
    reject(malformed);
    reject(fixture({}, 0, 1, 255));
    reject(fixture(payload, payload.size(), 1, 255));

    // Unsorted, adjacent payloads, padding and opaque pointer-like words are legal.
    constexpr std::size_t table_end = 24 + 2 * 32;
    const auto second_offset = table_end + 5;
    const auto first_offset = second_offset + payload.size();
    std::vector<std::uint8_t> two(first_offset + payload.size() + 3);
    std::copy_n(stored.begin(), 56, two.begin());
    std::copy_n(stored.begin() + 24, 32, two.begin() + 56);
    put32(two, 4, 2);
    put32(two, 44, first_offset);
    put32(two, 76, second_offset);
    put32(two, 60, 43);
    std::copy(payload.begin(), payload.end(), two.begin() + first_offset);
    std::copy(payload.rbegin(), payload.rend(), two.begin() + second_offset);
    const auto ordered = decode_adf(two);
    require(ordered.size() == 2 && ordered[0].id == 42 && ordered[1].id == 43 &&
            ordered[0].data == payload &&
            ordered[1].data == std::vector<std::uint8_t>(payload.rbegin(), payload.rend()));
    malformed = two;
    put32(malformed, 76, first_offset);
    reject(malformed);
    malformed = two;
    put32(malformed, 76, first_offset + 1);
    reject(malformed);
    malformed = two;
    put32(malformed, 40, 1);
    put32(malformed, 72, 1);
    put32(malformed, 36, static_cast<std::uint32_t>(max_entry_bytes / 2 + 1));
    put32(malformed, 68, static_cast<std::uint32_t>(max_entry_bytes / 2 + 1));
    reject(malformed);
    std::vector<std::uint8_t> empty_root(stored.begin(), stored.begin() + 24);
    put32(empty_root, 4, 0);
    require(decode_adf(empty_root).empty());
}
} // namespace ikv::assets
