#include "ikv/protocol.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ikv {
namespace {
[[noreturn]] void invalid() { throw std::invalid_argument("invalid protocol message"); }
void validate_json(std::string_view json) {
    if (json.empty() || json.size() > max_body_size - 2) invalid();
    const auto first = json.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos || json[first] != '{') invalid();
    // Bound nesting before invoking the parser; quoted braces and escaped quotes do not count.
    std::size_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (const char c : json) {
        // The JSON lexer treats raw NUL as EOF even for bounded iterator input.
        if (c == '\0') invalid();
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '{' || c == '[') {
            if (++depth > 32) invalid();
        } else if (c == '}' || c == ']') {
            if (depth == 0) invalid();
            --depth;
        }
    }
    if (quoted || depth != 0 || !nlohmann::json::accept(json.begin(), json.end())) invalid();
}
void validate(const Message& message) {
    switch (message.type) {
    case Type::hello:
        if (message.session.size() != 64 || message.character_id == 0 ||
            message.character_id > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) invalid();
        for (const unsigned char c : message.session)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) invalid();
        break;
    case Type::welcome:
        if (message.id == 0) invalid();
        break;
    case Type::input:
        if (message.sequence == 0 || message.dx < -1 || message.dx > 1 || message.dy < -1 || message.dy > 1 ||
            (message.dx != 0 && message.dy != 0)) invalid();
        break;
    case Type::snapshot: {
        if (message.players.size() > 32) invalid();
        std::uint32_t previous = 0;
        for (const auto& player : message.players) {
            if (player.id <= previous || player.x < -coordinate_limit || player.x > coordinate_limit ||
                player.y < -coordinate_limit || player.y > coordinate_limit) invalid();
            previous = player.id;
        }
        break;
    }
    case Type::goodbye: break;
    case Type::destination:
        if (message.sequence == 0 || message.x < -coordinate_limit || message.x > coordinate_limit ||
            message.y < -coordinate_limit || message.y > coordinate_limit) invalid();
        break;
    case Type::state:
    case Type::action:
        validate_json(message.json);
        break;
    default: invalid();
    }
}
void u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 24));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value));
}
void u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    u32(bytes, static_cast<std::uint32_t>(value >> 32));
    u32(bytes, static_cast<std::uint32_t>(value));
}
std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
}
std::uint64_t read_u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint64_t>(read_u32(bytes, offset)) << 32) | read_u32(bytes, offset + 4);
}
}
std::string original_character_name_error(std::string_view utf8) {
    if (utf8.empty() || utf8 == "isimsiz") return "Karakter ismi isimsiz olamaz!";
    static constexpr std::u32string_view upper = U"ABCDEFGHIJKLMNOPQRSTUVWXYZÇĞİÖŞÜ", lower = U"abcdefghijklmnopqrstuvwxyzçğıöşü";
    std::size_t letters = 0;
    bool letters_only = true, capitalised = true;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto first = static_cast<unsigned char>(utf8[i]);
        const std::size_t count = first < 0x80 ? 1 : (first & 0xe0) == 0xc0 ? 2 : (first & 0xf0) == 0xe0 ? 3 : 4;
        char32_t c = count == 1 ? first : first & (0x7f >> count);
        for (std::size_t k = 1; k < count; ++k) {
            const auto next = i + k < utf8.size() ? static_cast<unsigned char>(utf8[i + k]) : 0;
            if ((next & 0xc0) != 0x80) { c = 0; break; }
            c = (c << 6) | (next & 0x3f);
        }
        const bool is_upper = upper.find(c) != std::u32string_view::npos;
        if (!is_upper && lower.find(c) == std::u32string_view::npos) letters_only = false;
        if (letters == 0 ? !is_upper : is_upper) capitalised = false;
        ++letters; i += count;
    }
    if (letters < 4) return "Karakter ismi en az 4 harften oluşmalı!";
    if (letters > 16) return "Karakter ismi en çok 16 harften oluşmalı!";
    if (!letters_only) return "Karakter ismi yalnızca harflerden oluşmalı!";
    if (!capitalised) return "Karakter isminin ilk harfi büyük kalanı küçük olmalı!";
    return {};
}
bool valid_appearance(const nlohmann::json& appearance) {
    if (!appearance.is_object()) return false;
    const auto index = [&](const char* key, std::int64_t maximum, std::int64_t fallback = 0) {
        const auto found = appearance.find(key);
        if (found == appearance.end()) return fallback;
        if (!found->is_number_integer() || (found->is_number_unsigned() &&
            found->get<std::uint64_t>() > static_cast<std::uint64_t>(maximum))) return std::int64_t{-1};
        const auto value = found->get<std::int64_t>();
        return value >= 0 && value <= maximum ? value : std::int64_t{-1};
    };
    const auto sex = index("sex", 1, 1);
    if (sex < 0 || index("face", 9) < 0 || index("hair", sex == 0 ? 20 : 17) < 0 ||
        index("hair_color", sex == 0 ? 9 : 4) < 0 || index("beard", sex == 0 ? 24 : 14) < 0 ||
        index("beard_color", sex == 0 ? 1 : 4) < 0) return false;
    for (const auto& [key, maximum] : {std::pair{"model_member", 256U}, std::pair{"mapping_basis", 128U}}) {
        const auto found = appearance.find(key);
        if (found == appearance.end()) continue;
        if (!found->is_string()) return false;
        const auto& text = found->get_ref<const std::string&>();
        if (text.size() > maximum || text.find('\0') != std::string::npos) return false;
        if (std::string_view(key) == "model_member" && !text.empty() &&
            (text.front() == '/' || text.find('\\') != std::string::npos ||
             text == ".." || text.starts_with("../") || text.find("/../") != std::string::npos ||
             text.ends_with("/.."))) return false;
    }
    try { return appearance.dump().size() <= 384; }
    catch (const nlohmann::json::exception&) { return false; }
}
std::vector<std::uint8_t> encode(const Message& message) {
    validate(message);
    std::uint32_t length = 2;
    switch (message.type) {
    case Type::hello: length += 72; break;
    case Type::welcome: length += 8; break;
    case Type::input: length += 6; break;
    case Type::snapshot: length += 6 + static_cast<std::uint32_t>(message.players.size()) * 12; break;
    case Type::goodbye: break;
    case Type::destination: length += 12; break;
    case Type::state:
    case Type::action: length += static_cast<std::uint32_t>(message.json.size()); break;
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(length + 4);
    u32(bytes, length);
    bytes.push_back(2);
    bytes.push_back(static_cast<std::uint8_t>(message.type));
    switch (message.type) {
    case Type::hello:
        bytes.insert(bytes.end(), message.session.begin(), message.session.end());
        u64(bytes, message.character_id);
        break;
    case Type::welcome:
        u32(bytes, message.id);
        u32(bytes, message.tick);
        break;
    case Type::input:
        u32(bytes, message.sequence);
        bytes.push_back(std::bit_cast<std::uint8_t>(message.dx));
        bytes.push_back(std::bit_cast<std::uint8_t>(message.dy));
        break;
    case Type::snapshot:
        u32(bytes, message.tick);
        bytes.push_back(0);
        bytes.push_back(static_cast<std::uint8_t>(message.players.size()));
        for (const auto& player : message.players) {
            u32(bytes, player.id);
            u32(bytes, std::bit_cast<std::uint32_t>(player.x));
            u32(bytes, std::bit_cast<std::uint32_t>(player.y));
        }
        break;
    case Type::goodbye: break;
    case Type::destination:
        u32(bytes, message.sequence);
        u32(bytes, std::bit_cast<std::uint32_t>(message.x));
        u32(bytes, std::bit_cast<std::uint32_t>(message.y));
        break;
    case Type::state:
    case Type::action:
        bytes.insert(bytes.end(), message.json.begin(), message.json.end());
        break;
    }
    return bytes;
}
Message decode(std::span<const std::uint8_t> body) {
    if (body.size() < 2 || body.size() > max_body_size || body[0] != 2) invalid();
    Message message{static_cast<Type>(body[1])};
    switch (message.type) {
    case Type::hello:
        if (body.size() != 74) invalid();
        message.session.assign(reinterpret_cast<const char*>(body.data() + 2), 64);
        message.character_id = read_u64(body, 66);
        break;
    case Type::welcome:
        if (body.size() != 10) invalid();
        message.id = read_u32(body, 2);
        message.tick = read_u32(body, 6);
        break;
    case Type::input:
        if (body.size() != 8) invalid();
        message.sequence = read_u32(body, 2);
        message.dx = std::bit_cast<std::int8_t>(body[6]);
        message.dy = std::bit_cast<std::int8_t>(body[7]);
        break;
    case Type::snapshot: {
        if (body.size() < 8) invalid();
        const std::size_t count = (static_cast<std::size_t>(body[6]) << 8) | body[7];
        if (count > 32 || body.size() != 8 + count * 12) invalid();
        message.tick = read_u32(body, 2);
        message.players.reserve(count);
        for (std::size_t offset = 8; offset < body.size(); offset += 12) {
            message.players.push_back({read_u32(body, offset), std::bit_cast<std::int32_t>(read_u32(body, offset + 4)),
                                      std::bit_cast<std::int32_t>(read_u32(body, offset + 8))});
        }
        break;
    }
    case Type::goodbye:
        if (body.size() != 2) invalid();
        break;
    case Type::destination:
        if (body.size() != 14) invalid();
        message.sequence = read_u32(body, 2);
        message.x = std::bit_cast<std::int32_t>(read_u32(body, 6));
        message.y = std::bit_cast<std::int32_t>(read_u32(body, 10));
        break;
    case Type::state:
    case Type::action:
        message.json.assign(reinterpret_cast<const char*>(body.data() + 2), body.size() - 2);
        break;
    default: invalid();
    }
    validate(message);
    return message;
}
std::vector<Message> Decoder::feed(std::span<const std::uint8_t> bytes) {
    if (failed_) invalid();
    std::vector<Message> messages;
    try {
        while (!bytes.empty()) {
            const auto count = std::min(expected_ - buffered_, bytes.size());
            std::copy_n(bytes.begin(), count, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
            buffered_ += count;
            bytes = bytes.subspan(count);
            if (buffered_ != expected_) continue;
            if (expected_ == 4) {
                const auto body_size = read_u32(buffer_, 0);
                if (body_size < 2 || body_size > max_body_size) invalid();
                expected_ = static_cast<std::size_t>(body_size) + 4;
            } else {
                messages.push_back(decode(std::span(buffer_).subspan(4, expected_ - 4)));
                buffered_ = 0;
                expected_ = 4;
            }
        }
    } catch (const std::invalid_argument&) {
        failed_ = true;
        throw;
    }
    return messages;
}
void Decoder::finish() {
    if (failed_) invalid();
    if (buffered_ != 0) {
        failed_ = true;
        throw std::invalid_argument("truncated protocol frame");
    }
}
}
