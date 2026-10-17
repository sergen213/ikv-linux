#include "ikv/protocol.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <span>
#include <source_location>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void require(bool condition, const char* explanation) {
    if (!condition) throw std::runtime_error(explanation);
}
template<class F> void rejects(F&& action, std::source_location location = std::source_location::current()) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected) std::cerr << location.file_name() << ':' << location.line() << ": ";
    require(rejected, "invalid protocol value was accepted");
}
ikv::Message consume(const std::vector<std::uint8_t>& frame) {
    ikv::Decoder decoder;
    auto messages = decoder.feed(frame);
    require(messages.size() == 1, "consumer did not receive one message");
    return std::move(messages.front());
}
ikv::Message authenticated_hello() {
    ikv::Message message{ikv::Type::hello};
    message.session = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    message.character_id = 0x0102030405060708ULL;
    return message;
}
std::vector<std::uint8_t> json_body(ikv::Type type, const std::string& json) {
    std::vector<std::uint8_t> body{2, static_cast<std::uint8_t>(type)};
    body.insert(body.end(), json.begin(), json.end());
    return body;
}
void independent_v2_bytes() {
    std::vector<std::uint8_t> hello{2, 1};
    const std::string session = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    hello.insert(hello.end(), session.begin(), session.end());
    hello.insert(hello.end(), {1, 2, 3, 4, 5, 6, 7, 8});
    const auto received = ikv::decode(hello);
    require(received.session == session && received.character_id == 0x0102030405060708ULL,
            "independent hello identity decoded incorrectly");
    auto encoded = ikv::encode(received);
    require(encoded.size() == 78 && encoded[0] == 0 && encoded[1] == 0 && encoded[2] == 0 &&
            encoded[3] == 74 && std::equal(hello.begin(), hello.end(), encoded.begin() + 4),
            "hello encoder violates fixed v2 layout");

    const std::array<std::uint8_t, 14> destination{2, 7, 1, 2, 3, 4,
                                                0xff, 0xe8, 0x90, 0, 0, 0x17, 0x70, 0};
    const auto target = ikv::decode(destination);
    require(target.sequence == 0x01020304U && target.x == -1536000 && target.y == 1536000,
            "independent destination decoded incorrectly");
    encoded = ikv::encode(target);
    require(encoded.size() == 18 && encoded[3] == 14 &&
            std::equal(destination.begin(), destination.end(), encoded.begin() + 4),
            "destination encoder violates independent wire layout");
    // Wave 16: + u32 see-through door id (x,y then in its linked zone); id 0 is only the 14-byte form.
    const std::array<std::uint8_t, 18> through{2, 7, 0, 0, 0, 9, 0, 0, 0x03, 0xe8, 0xff, 0xff, 0xfc, 0x18, 0x80, 0, 0x27, 0x31};
    const auto portal = ikv::decode(through);
    require(portal.sequence == 9 && portal.x == 1000 && portal.y == -1000 && portal.id == 2147493681U, "portal destination decoded incorrectly");
    encoded = ikv::encode(portal);
    require(encoded.size() == 22 && encoded[3] == 18 && std::equal(through.begin(), through.end(), encoded.begin() + 4), "portal destination encoder violates wire layout");

    for (const auto type : {ikv::Type::state, ikv::Type::action}) {
        const auto body = json_body(type, "{\"text\":\"Türkçe\",\"literal\":\"[{}]\"}");
        const auto message = ikv::decode(body);
        require(message.type == type && message.json == "{\"text\":\"Türkçe\",\"literal\":\"[{}]\"}",
                "independent UTF8 JSON payload changed");
        encoded = ikv::encode(message);
        require(encoded[3] == body.size() &&
                std::equal(body.begin(), body.end(), encoded.begin() + 4),
                "JSON encoder normalized or changed raw bytes");
    }
}
void json_bounds_and_validation() {
    for (const auto type : {ikv::Type::state, ikv::Type::action}) {
        ikv::Message message{type};
        for (const std::string& json : std::vector<std::string>{
                 "{}", " \r\n\t{\"ok\":true} \t", R"({"text":"quote: \" brackets [{]} slash: \\","empty":{}})",
                 R"({"escaped":"\u0000\t\n","surrogates":"\uD83D\uDE00"})",
                 "{\"nested\":" + std::string(31, '[') + "0" + std::string(31, ']') + "}"}) {
            message.json = json;
            require(consume(ikv::encode(message)).json == json, "valid JSON or depth 32 rejected");
        }
        for (const std::string& json : std::vector<std::string>{
                 "", "0", "null", "[]", "{", "{}{}", "{\"x\":}", "{\"x\":NaN}", "{\"x\":01}",
                 "{/* comment */}", "{\"x\":\"unterminated}", std::string("{\"x\":\"bad\n\"}"),
                 std::string("{\"x\":\"") + '\0' + "\"}", "{\"x\":\"\\uD800\"}",
                 std::string("{}") + '\0', std::string("{}") + '\0' + "junk",
                 std::string("{}") + '\0' + "{}", std::string("{") + '\0' + "}",
                 std::string("{\"x\":") + '\0' + "0}",
                 std::string("{\"x\":\"") + "\xc0\xaf" + "\"}",
                 std::string("{\"x\":\"") + "\xed\xa0\x80" + "\"}",
                 std::string("{\"x\":\"") + "\xf4\x90\x80\x80" + "\"}",
                 std::string("{\"x\":\"") + "\xc3" + "\"}",
                 "{\"nested\":" + std::string(32, '[') + "0" + std::string(32, ']') + "}"}) {
            message.json = json;
            rejects([&] { ikv::encode(message); });
            rejects([&] { ikv::decode(json_body(type, json)); });
        }
        const std::string prefix = "{\"pad\":\"";
        message.json = prefix + std::string(ikv::max_body_size - 2 - prefix.size() - 2, 'a') + "\"}";
        const auto largest = ikv::encode(message);
        require(largest.size() == 65539 && largest[0] == 0 && largest[1] == 0 &&
                largest[2] == 255 && largest[3] == 255, "maximum JSON frame length encoded incorrectly");
        require(consume(largest).json == message.json, "maximum JSON frame rejected");
        ikv::Decoder fragmented;
        require(fragmented.feed(std::span(largest).first(65536)).empty(), "maximum frame delivered prematurely");
        const auto suffix = fragmented.feed(std::span(largest).subspan(65536));
        require(suffix.size() == 1 && suffix.front().json == message.json, "maximum frame suffix lost");
        fragmented.finish();
        message.json.insert(prefix.size(), 1, 'a');
        rejects([&] { ikv::encode(message); });
        rejects([&] { ikv::decode(json_body(type, message.json)); });
    }
}
void appearance_contract() {
    using nlohmann::json;
    require(ikv::coordinate_limit == 1536000, "coordinate limit differs from source footprint");
    // Original client name rule messages (exe 0x5990e0..0x599300).
    require(ikv::original_character_name_error("Buoyunnedir").empty() && ikv::original_character_name_error("Şükrü").empty() &&
            ikv::original_character_name_error("İğneçöşü").empty(), "legal original names rejected");
    for (const auto& [name, message] : std::array<std::pair<const char*, const char*>, 8>{{
             {"", "Karakter ismi isimsiz olamaz!"}, {"isimsiz", "Karakter ismi isimsiz olamaz!"},
             {"Ali", "Karakter ismi en az 4 harften oluşmalı!"}, {"Abcdefghijklmnopq", "Karakter ismi en çok 16 harften oluşmalı!"},
             {"Ali2", "Karakter ismi yalnızca harflerden oluşmalı!"}, {"Ali Can", "Karakter ismi yalnızca harflerden oluşmalı!"},
             {"ahmet", "Karakter isminin ilk harfi büyük kalanı küçük olmalı!"}, {"AhMet", "Karakter isminin ilk harfi büyük kalanı küçük olmalı!"}}})
        require(ikv::original_character_name_error(name) == message, "original name rule message mismatch");
    require(ikv::original_character_name_error("Abc\xc3") == "Karakter ismi yalnızca harflerden oluşmalı!" &&
            ikv::original_character_name_error("Abcd\xc3\x28") == "Karakter ismi yalnızca harflerden oluşmalı!", "malformed UTF-8 accepted as letters");
    require(ikv::valid_appearance(json::object()), "default appearance rejected");
    for (const auto& value : {json(), json::array(), json(false), json(0), json("appearance")})
        require(!ikv::valid_appearance(value), "nonobject appearance accepted");
    for (const int sex : {0, 1}) {
        const std::array<std::pair<const char*, int>, 6> limits{{
            {"sex", 1}, {"face", 9}, {"hair", sex == 0 ? 20 : 17},
            {"hair_color", sex == 0 ? 9 : 4}, {"beard", sex == 0 ? 24 : 14},
            {"beard_color", sex == 0 ? 1 : 4}}};
        json maxima{{"sex", sex}};
        for (const auto& [field, maximum] : limits)
            if (std::string(field) != "sex") maxima[field] = maximum;
        require(ikv::valid_appearance(maxima), "sex-specific cosmetic maxima rejected");
        for (const auto& [field, maximum] : limits) {
            for (const int boundary : {0, maximum}) {
                json appearance{{"sex", sex}};
                appearance[field] = boundary;
                require(ikv::valid_appearance(appearance), "legal cosmetic boundary rejected");
            }
            for (const auto& invalid : {json(-1), json(maximum + 1), json(0.0), json(true),
                                       json("0"), json(), json::array(), json::object(),
                                       json(std::numeric_limits<std::uint64_t>::max())}) {
                json appearance{{"sex", sex}};
                appearance[field] = invalid;
                require(!ikv::valid_appearance(appearance), "invalid known cosmetic field accepted");
            }
        }
    }
    require(ikv::valid_appearance(json{{"hair", 17}, {"hair_color", 4}, {"beard", 14}, {"beard_color", 4}}),
            "omitted sex did not default to male");
    require(!ikv::valid_appearance(json{{"hair", 18}}), "omitted sex used female cosmetic bounds");
    for (const auto& [field, maximum] :
         std::array<std::pair<const char*, std::size_t>, 2>{{{"model_member", 256}, {"mapping_basis", 128}}}) {
        for (const auto& text : {std::string(), std::string(maximum, 'a')})
            require(ikv::valid_appearance(json{{field, text}}), "legal appearance string rejected");
        for (const auto& invalid : {json(std::string(maximum + 1, 'a')), json(std::string("a\0b", 3)),
                                   json(std::string("\xc0\xaf", 2)), json(std::string("\xc3", 1)),
                                   json(), json(1), json(false), json::array(), json::object()})
            require(!ikv::valid_appearance(json{{field, invalid}}), "malformed appearance string accepted");
    }
    require(ikv::valid_appearance(json{{"model_member", "actors/female/model.msh"}}),
            "relative original model member rejected");
    for (const auto* path : {"/actors/model.msh", "..", "../model.msh", "actors/../model.msh",
                             "actors/..", "actors\\model.msh"})
        require(!ikv::valid_appearance(json{{"model_member", path}}), "unsafe model member accepted");
    json unknown{{"extra", json{{"nested", json::array({true, 2, "value"})}}}};
    const auto original = unknown;
    require(ikv::valid_appearance(unknown) && unknown == original, "unknown appearance fields changed or rejected");
    json capped{{"extra", ""}};
    capped["extra"] = std::string(384 - capped.dump().size(), 'a');
    require(capped.dump().size() == 384 && ikv::valid_appearance(capped), "384-byte appearance rejected");
    capped["extra"].get_ref<std::string&>().push_back('a');
    require(!ikv::valid_appearance(capped), "oversized appearance accepted");
    capped["extra"] = std::string(100, '\n');
    require(capped.dump().size() < 384 && ikv::valid_appearance(capped), "escaped unknown field rejected");
    capped["extra"] = std::string(200, '\n');
    require(capped.dump().size() > 384 && !ikv::valid_appearance(capped), "encoded appearance size ignored");
}
void hello_destination_rejections() {
    const auto frame = ikv::encode(authenticated_hello());
    const auto valid = std::vector<std::uint8_t>(frame.begin() + 4, frame.end());
    for (const std::uint8_t bad : {std::uint8_t{'A'}, std::uint8_t{'g'}, std::uint8_t{0}, std::uint8_t{128}}) {
        auto body = valid;
        body[2] = bad;
        rejects([&] { ikv::decode(body); });
    }
    auto body = valid;
    std::fill(body.begin() + 66, body.end(), 0);
    rejects([&] { ikv::decode(body); });
    body[66] = 0x80;
    rejects([&] { ikv::decode(body); });
    std::fill(body.begin() + 66, body.end(), 255);
    rejects([&] { ikv::decode(body); });
    body = valid;
    body[0] = 1;
    rejects([&] { ikv::decode(body); });
    for (std::size_t cut = 0; cut < valid.size(); ++cut) rejects([&] { ikv::decode(std::span(valid).first(cut)); });
    body = valid;
    body.push_back(0);
    rejects([&] { ikv::decode(body); });

    std::vector<std::uint8_t> target{2, 7, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    for (const auto offset : {6U, 10U}) {
        auto invalid = target;
        invalid[offset] = 0;
        invalid[offset + 1] = 0x17;
        invalid[offset + 2] = 0x70;
        invalid[offset + 3] = 0x01;
        rejects([&] { ikv::decode(invalid); });
        invalid[offset] = 255;
        invalid[offset + 1] = 0xe8;
        invalid[offset + 2] = 0x8f;
        invalid[offset + 3] = 0xff;
        rejects([&] { ikv::decode(invalid); });
    }
    for (std::size_t cut = 0; cut < target.size(); ++cut) rejects([&] { ikv::decode(std::span(target).first(cut)); });
    target.push_back(0);
    rejects([&] { ikv::decode(target); });
    for (const std::size_t size : {16U, 17U, 18U, 19U}) { // 18 bytes with door id 0 is non-canonical
        auto sized = target; sized.resize(size, 0);
        rejects([&] { ikv::decode(sized); });
    }
    target.resize(18, 0); target[17] = 1;
    require(ikv::decode(target).id == 1, "18-byte destination carries the door id");
}
void malformed_frame_poison() {
    const auto good = ikv::encode({ikv::Type::goodbye});
    for (const auto& bad : std::vector<std::vector<std::uint8_t>>{
             {0, 0, 0, 2, 1, 5}, {0, 0, 0, 3, 2, 6, '{'}, {0, 0, 0, 2, 2, 9},
             {0, 0, 0, 5, 2, static_cast<std::uint8_t>(ikv::Type::state), '{', '}', 0},
             {0, 0, 0, 5, 2, static_cast<std::uint8_t>(ikv::Type::action), '{', '}', 0}}) {
        for (const bool prefix : {false, true}) {
            auto stream = prefix ? good : std::vector<std::uint8_t>{};
            stream.insert(stream.end(), bad.begin(), bad.end());
            stream.insert(stream.end(), good.begin(), good.end());
            ikv::Decoder decoder;
            rejects([&] { decoder.feed(stream); });
            rejects([&] { decoder.feed(good); });
            rejects([&] { decoder.feed({}); });
            rejects([&] { decoder.finish(); });
        }
    }
}
void round_trip_semantics() {
    auto hello = authenticated_hello();
    auto identity = consume(ikv::encode(hello));
    require(identity.session == hello.session && identity.character_id == hello.character_id, "hello identity changed");
    hello.character_id = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    require(consume(ikv::encode(hello)).character_id == hello.character_id, "maximum character ID rejected");
    hello.character_id = 1;
    require(consume(ikv::encode(hello)).character_id == 1, "minimum character ID rejected");

    ikv::Message welcome{ikv::Type::welcome};
    welcome.id = 0xffffffffU;
    welcome.tick = 0xffffffffU;
    auto received = consume(ikv::encode(welcome));
    require(received.id == welcome.id && received.tick == welcome.tick, "welcome integer boundaries changed");

    ikv::Message input{ikv::Type::input};
    input.sequence = 1;
    input.dx = -1;
    received = consume(ikv::encode(input));
    require(received.sequence == 1 && received.dx == -1 && received.dy == 0, "negative input changed");
    input.sequence = 0xffffffffU;
    input.dx = 0;
    input.dy = 1;
    require(consume(ikv::encode(input)).sequence == input.sequence, "maximum sequence rejected");

    ikv::Message snapshot{ikv::Type::snapshot};
    snapshot.tick = 7;
    for (std::uint32_t id = 1; id <= 32; ++id) snapshot.players.push_back({id, -1536000, 1536000});
    received = consume(ikv::encode(snapshot));
    require(received.tick == 7 && received.players.size() == 32, "snapshot count/tick changed");
    for (std::size_t i = 0; i < received.players.size(); ++i) {
        const auto& p = received.players[i];
        require(p.id == i + 1 && p.x == -1536000 && p.y == 1536000, "authoritative coordinates changed");
    }
    snapshot.players.clear();
    require(consume(ikv::encode(snapshot)).players.empty(), "empty snapshot rejected");
    require(consume(ikv::encode({ikv::Type::goodbye})).type == ikv::Type::goodbye, "goodbye lost");

    // Independent bytes exercise endian and signed decoding, not only self-round-trips.
    const std::array<std::uint8_t, 20> body{2, 4, 0, 0, 1, 2, 0, 1, 0, 0, 0, 9,
                                           0xff, 0xe8, 0x90, 0, 0, 0x17, 0x70, 0};
    received = ikv::decode(body);
    require(received.tick == 258 && received.players[0].id == 9 && received.players[0].x == -1536000 &&
            received.players[0].y == 1536000, "independent wire snapshot decoded incorrectly");
    const auto encoded = ikv::encode(received);
    require(encoded.size() == body.size() + 4 && encoded[3] == 20 &&
            std::equal(body.begin(), body.end(), encoded.begin() + 4), "encoder violates independent wire layout");
}
void streaming() {
    const auto hello = authenticated_hello();
    const auto frame = ikv::encode(hello);
    for (std::size_t cut = 0; cut <= frame.size(); ++cut) {
        ikv::Decoder decoder;
        auto first = decoder.feed(std::span(frame).first(cut));
        auto second = decoder.feed(std::span(frame).subspan(cut));
        require(first.size() + second.size() == 1, "split frame lost/duplicated");
        const auto& message = first.empty() ? second.front() : first.front();
        require(message.session == hello.session && message.character_id == hello.character_id,
                "split frame content changed");
    }
    ikv::Message destination{ikv::Type::destination};
    destination.sequence = 1;
    destination.x = -1536000;
    destination.y = 1536000;
    ikv::Message state{ikv::Type::state};
    state.json = "{\"text\":\"İstanbul\"}";
    auto action = state;
    action.type = ikv::Type::action;
    for (const auto& expected : {destination, state, action}) {
        const auto bytes = ikv::encode(expected);
        for (std::size_t cut = 0; cut <= bytes.size(); ++cut) {
            ikv::Decoder decoder;
            auto first = decoder.feed(std::span(bytes).first(cut));
            auto second = decoder.feed(std::span(bytes).subspan(cut));
            require(first.size() + second.size() == 1, "new v2 split frame lost/duplicated");
            const auto& message = first.empty() ? second.front() : first.front();
            require(ikv::encode(message) == bytes, "new v2 split frame bytes changed");
            decoder.finish();
        }
    }
    ikv::Decoder bytewise;
    std::size_t delivered = 0;
    for (const auto byte : frame) delivered += bytewise.feed(std::span(&byte, 1)).size();
    require(delivered == 1, "bytewise header/body failed");
    require(bytewise.feed({}).empty(), "empty feed emitted a message");

    std::vector<std::uint8_t> stream;
    for (int i = 0; i < 1000; ++i) stream.insert(stream.end(), frame.begin(), frame.end());
    ikv::Decoder coalesced;
    auto messages = coalesced.feed(stream);
    require(messages.size() == 1000 && messages.back().session == hello.session, "large coalesced stream failed");
    stream = frame;
    stream.insert(stream.end(), frame.begin(), frame.begin() + 5);
    ikv::Decoder partial;
    require(partial.feed(stream).size() == 1, "complete prefix blocked by partial suffix");
    require(partial.feed(std::span(frame).subspan(5)).size() == 1, "partial suffix not retained");
}
void eof_boundaries() {
    ikv::Message welcome{ikv::Type::welcome};
    welcome.id = 1;
    ikv::Message destination{ikv::Type::destination};
    destination.sequence = 1;
    ikv::Message state{ikv::Type::state};
    state.json = "{}";
    auto action = state;
    action.type = ikv::Type::action;
    for (const auto& message : {welcome, authenticated_hello(), destination, state, action,
                               ikv::Message{ikv::Type::goodbye}}) {
        const auto frame = ikv::encode(message);
        for (int complete_prefix = 0; complete_prefix <= 1; ++complete_prefix) {
            for (std::size_t cut = 0; cut <= frame.size(); ++cut) {
                ikv::Decoder decoder;
                if (complete_prefix) require(decoder.feed(frame).size() == 1, "complete prefix lost");
                const auto messages = decoder.feed(std::span(frame).first(cut));
                require(messages.size() == (cut == frame.size() ? 1U : 0U), "EOF prefix delivery changed");
                if (cut == 0 || cut == frame.size()) {
                    decoder.finish();
                    decoder.finish();
                } else {
                    rejects([&] { decoder.finish(); });
                    rejects([&] { decoder.feed(std::span(frame).subspan(cut)); });
                    rejects([&] { decoder.finish(); });
                }
            }
        }
    }
}
void invalid_encoding() {
    auto hello = authenticated_hello();
    for (const auto& session : std::vector<std::string>{
             "", std::string(63, 'a'), std::string(65, 'a'), std::string(64, 'A'),
             std::string(64, 'g'), std::string(64, '\n'), std::string(64, '\x80')}) {
        hello.session = session;
        rejects([&] { ikv::encode(hello); });
    }
    hello = authenticated_hello();
    for (const auto character : {0ULL, 0x8000000000000000ULL, 0xffffffffffffffffULL}) {
        hello.character_id = character;
        rejects([&] { ikv::encode(hello); });
    }
    ikv::Message destination{ikv::Type::destination};
    rejects([&] { ikv::encode(destination); });
    destination.sequence = 1;
    destination.x = -1536001;
    rejects([&] { ikv::encode(destination); });
    destination.x = 0;
    destination.y = 1536001;
    rejects([&] { ikv::encode(destination); });
    ikv::Message welcome{ikv::Type::welcome};
    rejects([&] { ikv::encode(welcome); });
    ikv::Message input{ikv::Type::input};
    rejects([&] { ikv::encode(input); });
    input.sequence = 1;
    input.dx = 2;
    rejects([&] { ikv::encode(input); });
    input.dx = -1;
    input.dy = 1;
    rejects([&] { ikv::encode(input); });
    ikv::Message snapshot{ikv::Type::snapshot};
    for (const auto& players : std::vector<std::vector<ikv::Player>>{
             {{0, 0, 0}}, {{1, 1536001, 0}}, {{1, 0, -1536001}}, {{2, 0, 0}, {1, 0, 0}},
             {{1, 0, 0}, {1, 0, 0}}, std::vector<ikv::Player>(33, {1, 0, 0})}) {
        snapshot.players = players;
        rejects([&] { ikv::encode(snapshot); });
    }
    rejects([] { ikv::encode({static_cast<ikv::Type>(0)}); });
}
void invalid_decoding() {
    for (const auto& body : std::vector<std::vector<std::uint8_t>>{
             {}, {2}, {1, 5}, {3, 5}, {2, 0}, {2, 9}, {2, 5, 0}, {2, 1}, {2, 1, 2, 'a'},
             {2, 2, 0, 0, 0, 0, 0, 0, 0, 0},
             {2, 3, 0, 0, 0, 0, 0, 0}, {2, 3, 0, 0, 0, 1, 255, 1},
             {2, 3, 0, 0, 0, 1, 128, 0}, {2, 4, 0, 0, 0, 0, 0, 33},
             {2, 4, 0, 0, 0, 0, 0, 1}, {2, 4, 0, 0, 0, 0, 0, 0, 0},
             {2, 7}, {2, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}) {
        rejects([&] { ikv::decode(body); });
    }
    for (std::uint32_t length : {0U, 1U, ikv::max_body_size + 1, 0xffffffffU}) {
        std::array<std::uint8_t, 4> header{static_cast<std::uint8_t>(length >> 24), static_cast<std::uint8_t>(length >> 16),
                                           static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length)};
        ikv::Decoder decoder;
        require(decoder.feed(std::span(header).first(3)).empty(), "partial header prematurely rejected");
        rejects([&] { decoder.feed(std::span(header).subspan(3)); });
        rejects([&] { decoder.feed({}); });
        rejects([&] { decoder.finish(); });
    }
    std::vector<std::uint8_t> oversized(ikv::max_body_size + 1, 0);
    rejects([&] { ikv::decode(oversized); });
    // Every truncation/trailing-byte variant of a valid body is invalid.
    ikv::Message snapshot{ikv::Type::snapshot};
    snapshot.players = {{1, -1536000, 1536000}, {2, 0, -1}};
    const auto frame = ikv::encode(snapshot);
    const auto body = std::span(frame).subspan(4);
    for (std::size_t n = 0; n < body.size(); ++n) rejects([&] { ikv::decode(body.first(n)); });
    auto trailing = std::vector<std::uint8_t>(body.begin(), body.end());
    trailing.push_back(0);
    rejects([&] { ikv::decode(trailing); });
    auto invalid_id = std::vector<std::uint8_t>(body.begin(), body.end());
    std::fill(invalid_id.begin() + 8, invalid_id.begin() + 12, 0);
    rejects([&] { ikv::decode(invalid_id); });
    auto duplicate = std::vector<std::uint8_t>(body.begin(), body.end());
    duplicate[23] = 1;
    rejects([&] { ikv::decode(duplicate); });
    for (const auto offset : {12U, 16U}) {
        for (const std::uint32_t value : {0x00177001U, 0xffe88fffU}) {
            auto position = std::vector<std::uint8_t>(body.begin(), body.end());
            for (unsigned i = 0; i < 4; ++i)
                position[offset + i] = static_cast<std::uint8_t>(value >> (24 - i * 8));
            rejects([&] { ikv::decode(position); });
        }
    }
}
void generated_codec_properties() {
    std::mt19937 generator(0x494b5632U);
    const auto random = [&] { return static_cast<std::uint32_t>(generator()); };
    constexpr char hex[] = "0123456789abcdef";
    const auto coordinate = [&] { return static_cast<std::int32_t>(random() % 3072001U) - 1536000; };
    std::vector<std::vector<std::uint8_t>> expected_frames;
    std::vector<std::uint8_t> stream;
    for (std::uint32_t iteration = 0; iteration < 1024; ++iteration) {
        ikv::Message message{static_cast<ikv::Type>(iteration % 9 + 1)};
        switch (message.type) {
        case ikv::Type::hello:
            for (int i = 0; i < 64; ++i) message.session.push_back(hex[random() % 16]);
            message.character_id = static_cast<std::uint64_t>(random()) << 32;
            message.character_id |= random();
            message.character_id &= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
            message.character_id |= 1;
            break;
        case ikv::Type::welcome:
            message.id = random() | 1U;
            message.tick = random();
            break;
        case ikv::Type::input:
            message.sequence = random() | 1U;
            if (random() % 2) message.dx = static_cast<std::int8_t>(static_cast<int>(random() % 3) - 1);
            else message.dy = static_cast<std::int8_t>(static_cast<int>(random() % 3) - 1);
            break;
        case ikv::Type::snapshot: {
            message.tick = random();
            const auto count = random() % 33;
            std::uint32_t id = 0;
            for (std::uint32_t i = 0; i < count; ++i) {
                id += random() % 128 + 1;
                message.players.push_back({id, coordinate(), coordinate()});
            }
            break;
        }
        case ikv::Type::goodbye: break;
        case ikv::Type::destination:
            message.sequence = random() | 1U;
            message.x = coordinate();
            message.y = coordinate();
            break;
        case ikv::Type::state:
        case ikv::Type::action:
        case ikv::Type::catalog: {
            const auto nesting = random() % 32;
            message.json = "{\"value\":" + std::string(nesting, '[') + std::to_string(random()) +
                           std::string(nesting, ']') + R"(,"text":"Türkçe \" [{}] \\","escaped":"\u0000"})";
            break;
        }
        }
        const auto frame = ikv::encode(message);
        const auto body = std::span(frame).subspan(4);
        require(ikv::encode(ikv::decode(body)) == frame, "generated valid message roundtrip changed bytes");
        ikv::Decoder fragmented;
        std::size_t delivered = 0;
        for (std::size_t offset = 0; offset < frame.size();) {
            const auto count = std::min(frame.size() - offset, static_cast<std::size_t>(random() % 31 + 1));
            for (const auto& decoded : fragmented.feed(std::span(frame).subspan(offset, count))) {
                require(ikv::encode(decoded) == frame, "random fragment changed generated message");
                ++delivered;
            }
            offset += count;
        }
        require(delivered == 1, "random fragments lost or duplicated generated message");
        fragmented.finish();
        stream.insert(stream.end(), frame.begin(), frame.end());
        expected_frames.push_back(frame);

        auto bad_body = std::vector<std::uint8_t>(body.begin(), body.end());
        bad_body[0] = static_cast<std::uint8_t>(random() % 253 + 3);
        rejects([&] { ikv::decode(bad_body); });
        bad_body[0] = 1;
        rejects([&] { ikv::decode(bad_body); });
        bad_body[0] = 2;
        bad_body[1] = static_cast<std::uint8_t>(random() % 246 + 10);
        rejects([&] { ikv::decode(bad_body); });
        bad_body[1] = static_cast<std::uint8_t>(message.type);
        switch (message.type) {
        case ikv::Type::hello: bad_body[2] = static_cast<std::uint8_t>('A' + random() % 26); break;
        case ikv::Type::welcome:
        case ikv::Type::destination: std::fill(bad_body.begin() + 2, bad_body.begin() + 6, 0); break;
        case ikv::Type::input: bad_body[6] = 2; break;
        case ikv::Type::snapshot: bad_body[6] = 255; bad_body[7] = 255; break;
        case ikv::Type::goodbye:
        case ikv::Type::state:
        case ikv::Type::action:
        case ikv::Type::catalog: bad_body.push_back(0); break;
        }
        rejects([&] { ikv::decode(bad_body); });
        auto bad_frame = std::vector<std::uint8_t>(4, 0);
        const auto size = static_cast<std::uint32_t>(bad_body.size());
        for (unsigned i = 0; i < 4; ++i) bad_frame[i] = static_cast<std::uint8_t>(size >> (24 - i * 8));
        bad_frame.insert(bad_frame.end(), bad_body.begin(), bad_body.end());
        ikv::Decoder poisoned;
        rejects([&] { poisoned.feed(bad_frame); });
        rejects([&] { poisoned.feed(frame); });
        rejects([&] { poisoned.finish(); });
    }
    ikv::Decoder coalesced;
    std::size_t delivered = 0;
    for (std::size_t offset = 0; offset < stream.size();) {
        const auto count = std::min(stream.size() - offset, static_cast<std::size_t>(random() % 2048 + 1));
        for (const auto& message : coalesced.feed(std::span(stream).subspan(offset, count))) {
            require(delivered < expected_frames.size() && ikv::encode(message) == expected_frames[delivered],
                    "random coalescing changed message order or bytes");
            ++delivered;
        }
        offset += count;
    }
    require(delivered == expected_frames.size(), "random coalescing lost generated messages");
    coalesced.finish();
}
}
int main() {
    try {
        independent_v2_bytes();
        json_bounds_and_validation();
        hello_destination_rejections();
        appearance_contract();
        malformed_frame_poison();
        round_trip_semantics();
        streaming();
        eof_boundaries();
        invalid_encoding();
        invalid_decoding();
        generated_codec_properties();
        std::cout << "protocol checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "protocol check failed: " << error.what() << '\n';
        return 1;
    }
}
