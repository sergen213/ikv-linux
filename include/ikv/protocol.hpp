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
// NONCANONICAL wave 30: frame version 3 adds the optional hello transfer ticket. Version 2 survives only as a
// decodable hello (so a server can answer an outdated client) and an encodable/decodable goodbye (that answer).
inline constexpr std::uint8_t protocol_version = 3;
inline constexpr std::uint8_t legacy_protocol_version = 2;
// Goodbye reason a server sends before closing for a wave-30 shard transfer (state event type "transfer").
inline constexpr std::string_view transfer_goodbye = "transfer";
// Goodbye (a version-2 frame, so a 0.15 client decodes it) answering a version-2 hello:
// {"reason": Turkish text shown verbatim by old clients, "code":"outdated_client", "version": server release, "protocol":3}.
inline constexpr std::string_view outdated_goodbye = "outdated_client";
struct Message;
// Builds that goodbye; server_version is the server's release version (1..32 printable ASCII bytes).
Message outdated_goodbye_message(std::string_view server_version);
bool valid_appearance(const nlohmann::json& appearance);
// Original client character-name rule (exe strings 0x5990e0..0x599300): not "isimsiz", 4-16 Turkish
// letters, first capital, rest lowercase. Returns the original message, or empty when acceptable.
std::string original_character_name_error(std::string_view utf8);
enum class Type : std::uint8_t {
    hello = 1, welcome = 2, input = 3, snapshot = 4, goodbye = 5,
    state = 6, destination = 7, action = 8,
    catalog = 9 // NONCANONICAL: learned recipes or the open bank tab with their item definitions, sent on change
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
    // hello only: empty, or the 64 lowercase hex single-use transfer ticket (NONCANONICAL wave 30).
    std::string ticket{};
    // Frame version byte: protocol_version, or legacy_protocol_version for a hello (decode only) / goodbye.
    std::uint8_t version = protocol_version;
};

// Independent authenticated v3 protocol; encode includes framing, decode takes body only.
// State/action/catalog carry one UTF-8 JSON object, nesting <= 32, with no raw NUL bytes.
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
