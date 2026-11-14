#include "ikv/client_realms.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace ikv {
namespace {
using Json = nlohmann::json;
[[noreturn]] void bad_world() { throw std::runtime_error("account service returned an invalid world endpoint"); }
// A nonempty string of at most `limit` bytes without control characters.
std::string bounded(const Json& value, std::size_t limit) {
    if (!value.is_string()) bad_world();
    const auto& s = value.get_ref<const std::string&>();
    if (s.empty() || s.size() > limit || std::any_of(s.begin(), s.end(), [](unsigned char ch) { return ch < 32 || ch == 127; })) bad_world();
    return s;
}
} // namespace

// GET /v1/characters/{id}/world → {host, port, instance}; an empty host (a shard without --public-host) means the
// account service's own host.
WorldEndpoint parse_world_endpoint(const Json& body) try {
    if (!body.is_object() || !body.at("host").is_string()) bad_world();
    WorldEndpoint endpoint;
    if (!body.at("host").get_ref<const std::string&>().empty()) endpoint.host = bounded(body.at("host"), 253);
    if (std::any_of(endpoint.host.begin(), endpoint.host.end(), [](unsigned char ch) { return ch <= 32 || ch >= 127; })) bad_world();
    const auto& port = body.at("port");
    if (!port.is_number_integer() || (port.is_number_unsigned() && port.get<std::uint64_t>() > 65535) ||
        port.get<std::int64_t>() < 1 || port.get<std::int64_t>() > 65535) bad_world();
    endpoint.port = static_cast<std::uint16_t>(port.get<std::int64_t>());
    endpoint.instance = bounded(body.at("instance"), 128);
    return endpoint;
} catch (const Json::exception&) { bad_world(); }

int parse_realm_id(std::string_view text) {
    if (text.empty() || text.size() > 5 || text.front() == '0' ||
        !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
        throw std::invalid_argument("--realm requires a realm id 1..32767");
    int value = 0;
    for (const char c : text) value = value * 10 + (c - '0');
    if (value > 32767) throw std::invalid_argument("--realm requires a realm id 1..32767");
    return value;
}
} // namespace ikv
