#pragma once

#include "ikv/protocol.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace ikv {
// NONCANONICAL wave 30 shard transfer: state event {type:"transfer",host,port,ticket,reason,realm}. After it the
// server closes with goodbye reason "transfer"; the client reconnects to host:port with hello.ticket.
struct Transfer {
    std::string host;   // 0..253 printable ASCII, no space; empty = the host the current session reaches
    std::uint16_t port = 0;
    std::string ticket; // 64 lowercase hex, single use
    std::string reason; // party | home | layer | instance
    std::string realm;  // destination realm name, 0..64 bytes (empty: unnamed)
};
// Strict parse of a transfer event; throws std::runtime_error("invalid server message") on any violation.
Transfer parse_transfer(const nlohmann::json& event);
// NONCANONICAL wave 30 realm names: a row's `name` with its `realm` as "Ad (Realm)" (rows without realm: the name).
std::string realm_label(const nlohmann::json& row, const char* name_key = "name", const char* realm_key = "realm");
// "Ad" or "Ad-Realm" (original names have no '-', so the first '-' separates). nullopt when empty or oversized.
struct RealmName { std::string name, realm; };
std::optional<RealmName> split_realm_name(std::string_view text);
// Add {name, realm?} to an action addressed by name (group_invite, guild_invite, private chat).
void address_by_name(nlohmann::json& action, const RealmName& who);

// Authenticated, explicitly configured TLS transport; pump is the only I/O driver
// after the bounded constructor handshake. This class is single-threaded.
class ClientSession {
public:
    // A nonempty ticket (64 lowercase hex) is sent in hello to redeem a wave-30 shard transfer.
    ClientSession(std::string host, std::uint16_t port, std::filesystem::path ca,
                  std::string_view token, std::uint64_t character, std::string_view ticket = {});
    ~ClientSession();
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    std::uint32_t self = 0;
    std::uint32_t tick = 0;
    std::vector<Player> players;
    // Authoritative view includes zone/source_registry_id/bounds, character and
    // visible appearance/equipment metadata, and explicit event identities.
    nlohmann::json state;
    bool has_snapshot = false;
    bool eof = false;
    std::string farewell; // NONCANONICAL: the server goodbye's reason, shown when it ends the session
    // Goodbye "code" (e.g. ikv::outdated_goodbye) and "version" (the server release) when present, ≤ 64 bytes each.
    std::string farewell_code, farewell_version;
    // NONCANONICAL wave 30: the validated transfer event of an accepted state; the session then expects the close.
    std::optional<Transfer> transfer;
    // The handoff may start: a transfer arrived and the old shard closed, or `grace` passed since it arrived.
    bool handoff_due(std::chrono::steady_clock::time_point now) const noexcept;
    static constexpr std::chrono::seconds handoff_grace{5};

    void pump(int wait_ms, const std::function<void(Type)>& on_message = {});
    void destination(std::int32_t x, std::int32_t y);
    void action(const nlohmann::json& body);
    // NONCANONICAL headless helper: a deterministic 100-fixed-point-unit cardinal
    // destination per snapshot, never client-side integration or native WASD.
    void intent(int dx, int dy);
    void stop();
    bool drained() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void receive(Message message);
};
} // namespace ikv
