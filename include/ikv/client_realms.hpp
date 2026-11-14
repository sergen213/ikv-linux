#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace ikv {
// NONCANONICAL wave 30: shard endpoint of a character (GET /v1/characters/{id}/world).
struct WorldEndpoint {
    std::string host;     // 0..253 printable ASCII, no space; empty = the account service's host
    std::uint16_t port = 0;
    std::string instance; // 1..128 bytes, no control characters
};
// Strict, bounded parser; throws std::runtime_error on any violation.
WorldEndpoint parse_world_endpoint(const nlohmann::json& body);
// --realm value from the launcher: decimal 1..32767 without sign or leading zeros; throws std::invalid_argument.
int parse_realm_id(std::string_view text);
} // namespace ikv
