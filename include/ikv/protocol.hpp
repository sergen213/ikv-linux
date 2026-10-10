#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace ikv {
inline constexpr std::uint32_t max_body_size = 65535;
inline constexpr std::int32_t coordinate_limit = 1536000;
bool valid_appearance(const nlohmann::json& appearance);
// Original client character-name rule (exe strings 0x5990e0..0x599300): not "isimsiz", 4-16 Turkish
// letters, first capital, rest lowercase. Returns the original message, or empty when acceptable.
std::string original_character_name_error(std::string_view utf8);
enum class Type : std::uint8_t {
    hello = 1, welcome = 2, input = 3, snapshot = 4, goodbye = 5,
    state = 6, destination = 7, action = 8
};
struct Player {
    std::uint32_t id;
    std::int32_t x;
    std::int32_t y;
};
struct Message {
    Type type;
    std::string session{};
    std::uint64_t character_id = 0;
    std::uint32_t id = 0;
    std::uint32_t tick = 0;
    std::uint32_t sequence = 0;
    std::int8_t dx = 0;
    std::int8_t dy = 0;
    std::vector<Player> players{};
    std::string json{};
    std::int32_t x = 0;
    std::int32_t y = 0;
};

// Independent authenticated v2 protocol; encode includes framing, decode takes body only.
// State/action carry one UTF-8 JSON object, nesting <= 32, with no raw NUL bytes.
// JSON escapes (including \u0000) are valid; all payload bytes must be consumed.
std::vector<std::uint8_t> encode(const Message& message);
Message decode(std::span<const std::uint8_t> body);
class Decoder {
public:
    // A parse error poisons the decoder: the consumer must close that connection.
    std::vector<Message> feed(std::span<const std::uint8_t> bytes);
    // At EOF, reject incomplete framing and poison the decoder on truncation.
    void finish();
private:
    std::array<std::uint8_t, max_body_size + 4> buffer_;
    std::size_t buffered_ = 0;
    std::size_t expected_ = 4;
    bool failed_ = false;
};
}
