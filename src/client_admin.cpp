#include "ikv/client_admin.hpp"

#include <algorithm>
#include <charconv>
#include <utility>

namespace ikv {
namespace {
using Json = ClientAdmin::Json;

// Byte limits per field (UTF-8); numeric fields are ASCII.
constexpr std::array<std::size_t, ClientAdmin::field_count> limits{64, 4, 10, 256, 32, 2, 3};

bool numeric(int field) {
    return field == ClientAdmin::field_quantity || field == ClientAdmin::field_amount || field == ClientAdmin::field_hour ||
           field == ClientAdmin::field_level;
}
// Server rows are untrusted: wrong types read as defaults instead of throwing.
std::int64_t integer(const Json& row, const char* key) {
    const auto found = row.find(key);
    if (found == row.end() || !found->is_number()) return 0;
    return found->is_number_float() ? static_cast<std::int64_t>(found->get<double>()) : found->get<std::int64_t>();
}
std::string string(const Json& row, const char* key) {
    const auto found = row.find(key);
    return found != row.end() && found->is_string() ? found->get<std::string>() : std::string{};
}
bool flag(const Json& row, const char* key) {
    const auto found = row.find(key);
    return found != row.end() && found->is_boolean() && found->get<bool>();
}
const Json& array(const Json& result, const char* key) {
    static const Json empty = Json::array();
    const auto found = result.find(key);
    return found != result.end() && found->is_array() ? *found : empty;
}
// summon's destination keys: {"character_id"} -> {"to_character_id"}, {"target"} -> {"to_target"}.
void merge_destination(Json& args, const Json& selector) {
    for (const auto& [key, value] : selector.items()) args["to_" + key] = value;
}
} // namespace

ClientAdmin::ClientAdmin() { fields[field_quantity] = "1"; }

void ClientAdmin::type(std::string_view utf8) {
    if (focus < 0 || focus >= field_count) return;
    auto& text = fields[static_cast<std::size_t>(focus)];
    const auto limit = limits[static_cast<std::size_t>(focus)];
    if (!numeric(focus)) {
        if (text.size() + utf8.size() <= limit) text.append(utf8); // whole SDL chunks keep UTF-8 scalars intact
        return;
    }
    for (const char c : utf8) {
        if (text.size() >= limit) break;
        if ((c >= '0' && c <= '9') || (c == '-' && focus == field_amount && text.empty())) text.push_back(c);
    }
}

void ClientAdmin::backspace() {
    if (focus < 0 || focus >= field_count) return;
    auto& text = fields[static_cast<std::size_t>(focus)];
    while (!text.empty()) {
        const auto c = static_cast<unsigned char>(text.back());
        text.pop_back();
        if ((c & 0xc0) != 0x80) break;
    }
}

const char* ClientAdmin::hint(int field) {
    switch (field) {
    case field_search: return "Eşya adı veya kimliği";
    case field_quantity: return "Adet";
    case field_amount: return "Miktar (Para eksi olabilir)";
    case field_announce: return "Duyuru metni";
    case field_zone: return "Bölge anahtarı";
    case field_hour: return "Saat";
    case field_level: return "Seviye (boş: hedefin seviyesi)";
    default: return "";
    }
}

void ClientAdmin::select_player(std::size_t row) {
    if (row >= players_.size()) return;
    target_ = Json{{"character_id", players_[row].character_id}};
    target_name_ = players_[row].name;
}

void ClientAdmin::select_entity(std::uint32_t entity, std::string name) {
    target_ = Json{{"target", entity}};
    target_name_ = std::move(name);
}

int ClientAdmin::selected_player() const {
    if (target_.is_null()) return -1;
    for (std::size_t i = 0; i < players_.size(); ++i) {
        if ((target_.contains("character_id") && target_.at("character_id") == players_[i].character_id) ||
            (target_.contains("target") && target_.at("target") == players_[i].entity))
            return static_cast<int>(i);
    }
    return -1;
}

Json ClientAdmin::request(std::string_view command, Json args) {
    return Json{{"action", "admin"}, {"command", std::string(command)}, {"args", std::move(args)}};
}

std::optional<Json> ClientAdmin::targeted(std::string_view command, Json args) {
    if (target_.is_null()) { fail("Önce bir hedef seçin."); return std::nullopt; }
    args.update(target_);
    return request(command, std::move(args));
}

std::optional<std::int64_t> ClientAdmin::number(int field, std::int64_t low, std::int64_t high, const char* what) {
    const auto& text = fields[static_cast<std::size_t>(field)];
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || value < low || value > high) {
        fail(std::string(what) + ": " + std::to_string(low) + " ile " + std::to_string(high) + " arası sayı yazın.");
        return std::nullopt;
    }
    return value;
}

std::optional<Json> ClientAdmin::search_items() const {
    return request("items", Json{{"search", fields[field_search]}});
}

std::optional<Json> ClientAdmin::load_inventory() {
    auto result = targeted("inventory");
    if (result) pending_inventory_ = target_;
    return result;
}

std::optional<Json> ClientAdmin::give_item() {
    if (selected_item < 0 || static_cast<std::size_t>(selected_item) >= items_.size()) { fail("Önce listeden bir eşya seçin."); return std::nullopt; }
    const auto quantity = number(field_quantity, 1, 1000, "Adet");
    if (!quantity) return std::nullopt;
    return targeted("give_item", Json{{"item", items_[static_cast<std::size_t>(selected_item)].id}, {"quantity", *quantity}});
}

std::optional<Json> ClientAdmin::take_item() {
    if (inventory_target_.is_null() || selected_row < 0 || static_cast<std::size_t>(selected_row) >= inventory_.size()) {
        fail("Önce envanterden bir eşya seçin."); return std::nullopt;
    }
    const auto& row = inventory_[static_cast<std::size_t>(selected_row)];
    if (row.equipped) { fail("Kuşanılmış eşya alınamaz."); return std::nullopt; }
    const auto quantity = number(field_quantity, 1, 1000, "Adet");
    if (!quantity) return std::nullopt;
    Json args{{"uid", row.uid}, {"quantity", std::min(*quantity, std::max<std::int64_t>(row.quantity, 1))}};
    args.update(inventory_target_); // from the character whose inventory is shown, not the current target
    return request("take_item", std::move(args));
}

std::optional<Json> ClientAdmin::currency() {
    const auto amount = number(field_amount, -999000000, 999000000, "Para");
    if (!amount) return std::nullopt;
    if (!*amount) { fail("Para: sıfırdan farklı bir miktar yazın."); return std::nullopt; }
    return targeted("currency", Json{{"amount", *amount}});
}

std::optional<Json> ClientAdmin::give_xp() {
    const auto amount = number(field_amount, 1, 10000000, "XP");
    if (!amount) return std::nullopt;
    return targeted("give_xp", Json{{"amount", *amount}});
}

std::optional<Json> ClientAdmin::summon_here(std::uint32_t self) {
    return targeted("summon", Json{{"to_target", self}});
}

std::optional<Json> ClientAdmin::go_to(std::uint32_t self) {
    if (target_.is_null()) { fail("Önce bir hedef seçin."); return std::nullopt; }
    Json args{{"target", self}};
    merge_destination(args, target_);
    return request("summon", std::move(args));
}

std::optional<Json> ClientAdmin::teleport_self(std::uint32_t self) {
    if (fields[field_zone].empty()) { fail("Işınlanmak için bölge anahtarı yazın."); return std::nullopt; }
    return request("teleport", Json{{"target", self}, {"zone", fields[field_zone]}});
}

std::optional<Json> ClientAdmin::announce() {
    if (fields[field_announce].empty()) { fail("Duyuru metni boş olamaz."); return std::nullopt; }
    auto result = request("announce", Json{{"message", fields[field_announce]}});
    fields[field_announce].clear();
    return result;
}

std::optional<Json> ClientAdmin::clock() {
    const auto hour = number(field_hour, 0, 23, "Saat");
    if (!hour) return std::nullopt;
    return request("clock", Json{{"hour", *hour}});
}

std::optional<Json> ClientAdmin::respawn() const {
    return request("respawn", fields[field_zone].empty() ? Json::object() : Json{{"zone", fields[field_zone]}});
}

bool ClientAdmin::kit_level_arg(Json& args) {
    if (fields[field_level].empty()) return true;
    const auto level = number(field_level, 1, 999, "Seviye");
    if (level) args["level"] = *level;
    return level.has_value();
}

std::optional<Json> ClientAdmin::preview_kits() {
    Json args = Json::object();
    if (!kit_level_arg(args)) return std::nullopt;
    return targeted("kits", std::move(args));
}

std::optional<Json> ClientAdmin::give_kit(bool consumables) {
    Json args{{"kit", consumables ? "consumables" : "equipment"}};
    if (!consumables) args["equip"] = kit_equip;
    if (!kit_level_arg(args)) return std::nullopt;
    return targeted("give_kit", std::move(args));
}

std::optional<Json> ClientAdmin::receive(const Json& event) {
    const auto command = string(event, "command");
    const auto found = event.find("result");
    static const Json none = Json::object();
    const Json& result = found != event.end() && found->is_object() ? *found : none;
    if (!flag(result, "ok")) {
        auto error = string(result, "error");
        if (error.empty()) error = string(event, "error");
        fail(command + " başarısız: " + (error.empty() ? std::string("bilinmeyen hata") : error));
        return std::nullopt;
    }
    status_ok_ = true;
    if (command == "players") {
        players_.clear();
        for (const auto& row : array(result, "players")) {
            if (!row.is_object()) continue;
            players_.push_back({static_cast<std::uint32_t>(std::clamp<std::int64_t>(integer(row, "entity"), 0, UINT32_MAX)), integer(row, "character_id"),
                                string(row, "name"), string(row, "zone"), integer(row, "level"), integer(row, "hp"), integer(row, "max_hp"), flag(row, "muted")});
        }
        status_ = std::to_string(players_.size()) + " oyuncu çevrimiçi.";
    } else if (command == "items") {
        items_.clear(); selected_item = -1;
        for (const auto& row : array(result, "items"))
            if (row.is_object()) items_.push_back({string(row, "id"), string(row, "name"), integer(row, "stack"), integer(row, "level")});
        items_total_ = std::max<std::int64_t>(integer(result, "total"), static_cast<std::int64_t>(items_.size()));
        status_ = std::to_string(items_.size()) + " eşya gösteriliyor (toplam " + std::to_string(items_total_) + ").";
    } else if (command == "inventory") {
        inventory_.clear(); selected_row = -1;
        for (const auto& row : array(result, "inventory"))
            if (row.is_object()) inventory_.push_back({string(row, "uid"), string(row, "item"), string(row, "name"), integer(row, "quantity"), flag(row, "equipped")});
        inventory_owner_ = string(result, "name");
        inventory_level_ = integer(result, "level");
        inventory_currency_ = integer(result, "currency");
        inventory_target_ = pending_inventory_.is_null() ? Json{{"target", integer(result, "player")}} : pending_inventory_;
        status_ = inventory_owner_ + " envanteri: " + std::to_string(inventory_.size()) + " eşya.";
    } else if (command == "kits") {
        kit_equipment_.clear(); kit_consumables_.clear();
        for (const auto& row : array(result, "equipment"))
            if (row.is_object()) kit_equipment_.push_back({string(row, "item"), string(row, "name"), string(row, "slot"), integer(row, "level"), integer(row, "score")});
        for (const auto& row : array(result, "consumables"))
            if (row.is_object()) kit_consumables_.push_back({string(row, "item"), string(row, "name"), integer(row, "quantity"), integer(row, "level")});
        kit_level_ = integer(result, "level"); kit_loaded_ = true;
        status_ = "Sv " + std::to_string(kit_level_) + " seti: " + std::to_string(kit_equipment_.size()) + " ekipman, " +
                  std::to_string(kit_consumables_.size()) + " iksir.";
    } else {
        if (command == "give_item") status_ = "Eşya verildi.";
        else if (command == "take_item") status_ = "Eşya alındı.";
        else if (command == "currency") status_ = "Para güncellendi.";
        else if (command == "give_xp") status_ = "Tecrübe verildi.";
        else if (command == "heal") status_ = "İyileştirildi.";
        else if (command == "kick") status_ = "Oyuncu oyundan atıldı.";
        else if (command == "summon") status_ = "Işınlama tamamlandı.";
        else if (command == "teleport") status_ = "Işınlandı: " + string(result, "zone");
        else if (command == "announce") status_ = "Duyuru gönderildi (" + std::to_string(integer(result, "recipients")) + " alıcı).";
        else if (command == "respawn") status_ = std::to_string(integer(result, "respawned")) + " NPC canlandırıldı.";
        else if (command == "clock") status_ = "Oyun saati ayarlandı.";
        else if (command == "weather") { // NONCANONICAL wave 27
            const auto it = result.find("weather"); const Json& w = it != result.end() && it->is_object() ? *it : none;
            status_ = "Hava: " + string(w, "kind") + ", mevsim: " + string(w, "season") + ".";
        }
        else if (command == "give_kit") {
            const auto given = array(result, "items").size(), worn = array(result, "equipped").size();
            status_ = "Set verildi: " + std::to_string(given) + " eşya" + (worn ? ", " + std::to_string(worn) + " kuşanıldı." : std::string("."));
        }
        else status_ = command + ": tamam.";
        // A change to the character whose inventory is shown refreshes that view.
        if (!inventory_target_.is_null() && (command == "give_item" || command == "take_item" || command == "currency" || command == "give_kit")) {
            pending_inventory_ = inventory_target_;
            Json args = inventory_target_;
            return request("inventory", std::move(args));
        }
    }
    return std::nullopt;
}

void ClientAdmin::reset() {
    focus = -1; selected_item = selected_row = -1;
    target_ = pending_inventory_ = inventory_target_ = nullptr;
    target_name_.clear(); inventory_owner_.clear(); status_.clear(); status_ok_ = true;
    players_.clear(); items_.clear(); inventory_.clear(); kit_equipment_.clear(); kit_consumables_.clear();
    kit_loaded_ = false;
    items_total_ = inventory_level_ = inventory_currency_ = kit_level_ = 0;
}
} // namespace ikv
