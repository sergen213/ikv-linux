#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ikv {
// NONCANONICAL in-game Yönetici Paneli (the original GM tools were chat commands such as "|tele"). Single-threaded panel
// state: typed fields, the chosen target, the last authoritative admin_result lists and status line. Builds
// {"action":"admin","command":...,"args":{...}} requests; the server authorizes and validates every command, so nothing
// here grants privilege. Server text (names, errors) is kept as plain strings for the text path only.
class ClientAdmin {
public:
    using Json = nlohmann::json;
    enum Tab { tab_players, tab_items, tab_inventory, tab_character, tab_kits, tab_world, tab_count };
    enum Field { field_search, field_quantity, field_amount, field_announce, field_zone, field_hour, field_level, field_count };
    struct PlayerRow { std::uint32_t entity = 0; std::int64_t character_id = 0; std::string name, zone; std::int64_t level = 0, hp = 0, max_hp = 0; bool muted = false; };
    struct ItemRow { std::string id, name; std::int64_t stack = 0, level = 0; };
    struct InventoryRow { std::string uid, item, name; std::int64_t quantity = 0; bool equipped = false; };
    struct KitEquipment { std::string item, name, slot; std::int64_t level = 0, score = 0; };
    struct KitConsumable { std::string item, name; std::int64_t quantity = 0, level = 0; };

    int tab = tab_players;
    int focus = -1; // Field receiving typed text; -1: keys and text go to the game as usual
    std::array<std::string, field_count> fields{};
    int selected_item = -1, selected_row = -1; // items() / inventory() indices
    bool kit_equip = false; // Setler "Giydir": give_kit equipment also equips it

    ClientAdmin();
    bool typing() const noexcept { return focus >= 0; }
    // SDL text-input UTF-8 for the focused field; numeric fields keep digits (and a leading '-' for the amount).
    void type(std::string_view utf8);
    void backspace();
    static const char* hint(int field);

    void select_player(std::size_t row);
    void select_entity(std::uint32_t entity, std::string name);
    bool has_target() const noexcept { return !target_.is_null(); }
    const std::string& target_name() const noexcept { return target_name_; }
    int selected_player() const; // players() row of the current target, or -1

    const std::vector<PlayerRow>& players() const noexcept { return players_; }
    const std::vector<ItemRow>& items() const noexcept { return items_; }
    const std::vector<InventoryRow>& inventory() const noexcept { return inventory_; }
    std::int64_t items_total() const noexcept { return items_total_; }
    const std::string& inventory_owner() const noexcept { return inventory_owner_; }
    std::int64_t inventory_level() const noexcept { return inventory_level_; }
    std::int64_t inventory_currency() const noexcept { return inventory_currency_; }
    bool inventory_loaded() const noexcept { return !inventory_target_.is_null(); }
    const std::vector<KitEquipment>& kit_equipment() const noexcept { return kit_equipment_; }
    const std::vector<KitConsumable>& kit_consumables() const noexcept { return kit_consumables_; }
    bool kit_loaded() const noexcept { return kit_loaded_; }
    std::int64_t kit_level() const noexcept { return kit_level_; }

    // Requests. std::nullopt when a local precondition fails; status() then says why.
    static Json request(std::string_view command, Json args = Json::object());
    std::optional<Json> list_players() const { return request("players"); }
    std::optional<Json> search_items() const;
    std::optional<Json> load_inventory();
    std::optional<Json> give_item();
    std::optional<Json> take_item();
    std::optional<Json> currency();
    std::optional<Json> give_xp();
    std::optional<Json> heal() { return targeted("heal"); }
    std::optional<Json> kick() { return targeted("kick"); }
    std::optional<Json> summon_here(std::uint32_t self);
    std::optional<Json> go_to(std::uint32_t self);
    std::optional<Json> teleport_self(std::uint32_t self);
    std::optional<Json> announce();
    std::optional<Json> clock();
    std::optional<Json> respawn() const;
    // NONCANONICAL wave 27: force a weather kind or season at once ("auto" returns to the simulation).
    static Json weather(std::string_view key, std::string_view value) { return request("weather", Json{{std::string(key), std::string(value)}, {"instant", true}}); }
    // Setler: kits previews the level-appropriate sets; give_kit hands one ("equipment" honours kit_equip). A blank level
    // field leaves "level" out, so the server uses the target's own level.
    std::optional<Json> preview_kits();
    std::optional<Json> give_kit(bool consumables);

    // admin_result event. Returns a follow-up request (inventory refresh after a change to the shown character).
    std::optional<Json> receive(const Json& event);
    void fail(std::string message) { status_ = std::move(message); status_ok_ = false; }
    const std::string& status() const noexcept { return status_; }
    bool status_ok() const noexcept { return status_ok_; }
    // Reconnect: lists, target and selection belong to the previous world.
    void reset();

private:
    std::optional<Json> targeted(std::string_view command, Json args = Json::object());
    std::optional<std::int64_t> number(int field, std::int64_t low, std::int64_t high, const char* what);
    bool kit_level_arg(Json& args); // false (status set) when a typed level is invalid

    Json target_;            // {"character_id":N} or {"target":entity}
    Json pending_inventory_; // selector of the last inventory request
    Json inventory_target_;  // selector whose inventory is shown
    std::string target_name_, inventory_owner_, status_;
    bool status_ok_ = true;
    std::vector<PlayerRow> players_;
    std::vector<ItemRow> items_;
    std::vector<InventoryRow> inventory_;
    std::vector<KitEquipment> kit_equipment_;
    std::vector<KitConsumable> kit_consumables_;
    bool kit_loaded_ = false;
    std::int64_t kit_level_ = 0;
    std::int64_t items_total_ = 0, inventory_level_ = 0, inventory_currency_ = 0;
};
} // namespace ikv
