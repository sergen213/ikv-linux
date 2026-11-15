#pragma once
#include <cstdint>
#include <optional>
#include <vector>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace ikv {
using Json = nlohmann::json;
struct Identity { std::int64_t account_id; bool admin; bool plus = false, kasa = false; }; // Plus Paket (accounts.plus), Kasa Erişimi (accounts.kasa)
struct CharacterSave { std::int64_t account_id; std::int64_t character_id; Json state; Json bank = nullptr; }; // bank null: unchanged
// NONCANONICAL wave 30: one ikv-server process (shard) of a realm. host is the public address clients connect to
// (empty: the address the client already used for HTTPS); capacity is at most 32 players.
struct ShardInfo { std::string instance; int realm_id = 1; std::string host; std::uint16_t port = 0; int players = 0, capacity = 32; };
struct ApiError : std::runtime_error {
    int status;
    std::string code;
    ApiError(int http_status, std::string error_code, std::string message)
        : std::runtime_error(std::move(message)), status(http_status), code(std::move(error_code)) {}
};
// Validated World::admin arguments for one /v1/admin/world command (also used by the in-game Yönetici Paneli); throws ApiError 400.
Json admin_world_arguments(const std::string& command, const Json& body);
class Store {
public:
    explicit Store(std::string dsn);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    void migrate(const std::filesystem::path& dir);
    Identity authenticate(std::string_view raw_session);
    Json character(std::int64_t account_id, std::int64_t character_id);
    Json enter_character(const Identity&, std::int64_t character_id, std::string_view world_instance);
    void leave_character(std::int64_t account_id, std::int64_t character_id);
    void save_character(std::int64_t account_id, std::int64_t character_id, const Json& state);
    void save_characters(std::span<const CharacterSave> states);
    Json api(std::string_view method, std::string_view path, std::string_view bearer, const Json& body, std::string_view peer_key);
    void world_status(const Json& status);
    // [{id,command,arguments}], oldest first, at most 16 per call. Wave 30: a shard passes its instance and realm and receives
    // only commands targeted at it, at its realm or untargeted; an empty instance takes any command (pre-realm behaviour).
    Json take_world_commands(std::string_view instance = {}, int realm_id = 0);
    void finish_world_command(std::int64_t id, const Json& result);
    Json world_settings(); // persisted administrator rates/zone switches, {} when never set
    // Administrator-only raw SQL dump bytes for GET /v1/admin/backups/{name}; throws ApiError like api().
    std::string backup_download(std::string_view bearer, std::string_view name, std::string_view peer_key);
    void bootstrap_admin(std::string_view username);
    Json backup(const std::filesystem::path& directory);
    void restore(const std::filesystem::path& file);
    // Server-side bots (NONCANONICAL): a reserved account that can never log in (sentinel credential) owns their characters.
    std::int64_t bot_account();
    Json bot_characters(std::int64_t account); // [{id,name,level,leased}] of live bot characters
    // Bypasses the per-account eight-character limit; throws ApiError 409 "conflict" when the name is taken.
    std::int64_t create_bot_character(std::int64_t account, const std::string& name, int class_id, const Json& appearance, const Json& state);
    // ---- Realms, shards and phasing (wave 30, NONCANONICAL; migration 009) ----
    Json realms(std::int64_t account_id);                       // [{id,name,kind,status,population,characters}]
    void register_shard(const ShardInfo&);                     // also the heartbeat (players); throws ApiError
    void retire_shard(std::string_view instance);              // clean stop: the row stops being live at once (not after 15 s)
    std::optional<ShardInfo> pick_shard(int realm_id);         // least loaded live shard with room; none for disabled/deleted realms
    std::optional<ShardInfo> shard(std::string_view instance); // live shard (heartbeat within 15 s)
    // NONCANONICAL wave 31 host layout (migration 010) for the launcher supervisor; the HTTPS service authenticates the
    // host token on the private listener before calling these. host_layout: {realms:[{id,name,shards,capacity,enabled}],
    // revision} of every non-deleted realm. set_host_layout {realms:[{id,shards,capacity?}]} is authoritative: listed realms
    // get those desired shards/capacity, unlisted realms 0 shards; returns host_layout(). Throws ApiError 400/409.
    Json host_layout();
    Json set_host_layout(const Json& body);
    std::optional<ShardInfo> locate(int realm_id, std::string_view name); // online character's live shard
    std::vector<ShardInfo> shards(int realm_id = 0);           // live shards (0: every realm), by realm then instance
    int character_realm(std::int64_t character_id);            // home realm; throws ApiError 404
    // Raw 64-hex ticket, valid 60 s, single use, bound to character and destination. The caller must hold the character's
    // lease as `from` and should save the character first; the ticket releases nothing. A newer ticket voids older ones.
    std::string issue_ticket(std::int64_t character_id, std::string_view from, std::string_view to,
                             std::string_view reason, const Json& payload);
    Json redeem_ticket(std::string_view raw, std::int64_t character_id, std::string_view to); // payload; throws ApiError 403
    Json lockouts(std::int64_t character_id);                  // {zone: expires_unix}, live lockouts only
    void set_lockout(std::int64_t character_id, std::string_view zone, std::int64_t expires_unix); // past expiry clears
    // Durable shard bus: shard_messages rows plus NOTIFY ikv_bus carrying only the destination instance. bus_fd opens a
    // dedicated LISTEN connection on first use; when it is readable (and periodically, e.g. at the heartbeat) call
    // bus_take, again while it returns 64 bodies. Bodies are JSON objects of at most 4 KiB.
    int bus_fd();
    void bus_send(std::string_view to_instance, const Json& body);
    std::vector<Json> bus_take(std::string_view instance);     // consume pending, oldest first, bounded 64
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
