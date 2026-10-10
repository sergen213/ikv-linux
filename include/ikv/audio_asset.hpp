#pragma once

#include <cstdint>
#include <iosfwd>
#include <span>
#include <string_view>
#include <vector>

namespace ikv::assets {

inline constexpr std::uint64_t max_raw_audio_bytes = 32 * 1024 * 1024;

// Original-client proof: selector 0x4580f0 matches "raw" at 0x5937b0,
// then 0x457770 -> 0x4534f0 -> vtable 0x593580 -> decoder 0x453580.
// WAVEFORMATEX: PCM tag 1, mono, 44100 Hz, 88200 bytes/s, block alignment 2,
// signed little-endian 16-bit samples, cbSize 0. File length at 0x452d90 and
// direct fread at 0x452e60 into DirectSound Lock prove headerless whole-file PCM;
// there is no compression, header, resampling, or sample padding to recover.
struct RawAudioView {
    std::uint32_t sample_rate = 44100;
    std::uint16_t channels = 1;
    std::span<const std::uint8_t> pcm;
};

// Provenance, not a content signature: only audio/...raw or base/audio/...raw
// qualify (ASCII case-insensitive, '/' or '\\' separators). Absolute paths,
// empty segments and '.'/'..' segments are rejected. Root musicNN.raw and
// soundNN.raw are not this format. Throws std::runtime_error for invalid paths
// or data without a complete frame (<2 bytes) or over the limit. An odd trailing
// byte is excluded from the view (whole frames only). The view borrows bytes without copying;
// its caller must keep the underlying bytes alive and unmodified while in use.
RawAudioView inspect_raw_audio(std::span<const std::uint8_t> bytes,
                               std::string_view asset_path);

// Read exactly byte_size bytes at the current stream position, starting at PCM
// byte zero. Validate path and bounds before allocation; leave any trailing
// stream bytes untouched. Throws std::runtime_error for invalid input or a short
// read. The returned bytes are the whole file; inspect_raw_audio selects its frames.
std::vector<std::uint8_t> read_raw_audio(std::istream& input, std::uint64_t byte_size,
                                       std::string_view asset_path);

} // namespace ikv::assets
