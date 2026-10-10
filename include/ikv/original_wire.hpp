#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ikv::original_wire {
// Local defensive subset, not a recovered original receive maximum.
inline constexpr std::uint32_t max_body_size = 65535;
using Frame = std::vector<std::uint8_t>;

// Static-only recovered LE32 framing. Bodies remain opaque, including empty bodies.
class Decoder {
public:
    std::vector<Frame> feed(std::span<const std::uint8_t> bytes);
    // Reject and poison the decoder on a partial header/body or an earlier error.
    void finish();
private:
    std::array<std::uint8_t, max_body_size + 4> buffer_;
    std::size_t buffered_ = 0;
    std::size_t expected_ = 4;
    bool failed_ = false;
};

// Construct one known client-to-server command envelope; never mask/truncate length.
Frame encode_command(std::uint32_t command, std::span<const std::uint8_t> arguments);
// Only for an explicitly identified client-command body. The remainder stays opaque:
// it may contain further commands, whose boundaries require ID-specific schemas.
std::uint32_t first_client_command_id(std::span<const std::uint8_t> body);
} // namespace ikv::original_wire
