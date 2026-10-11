#pragma once
#include <cstdint>
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
struct ApiError : std::runtime_error {
    int status;
    std::string code;
    ApiError(int http_status, std::string error_code, std::string message)
        : std::runtime_error(std::move(message)), status(http_status), code(std::move(error_code)) {}
};
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
    Json take_world_commands(); // [{id,command,arguments}], oldest first, at most 16 per call
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
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
