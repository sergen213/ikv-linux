#include "ikv/client_raid.hpp"

#include "ikv/client_ui.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace ikv {
namespace {
using Json = ClientRaid::Json;
bool id_value(const Json& v) { return v.is_number_unsigned() && v.get<std::uint64_t>() >= 1 && v.get<std::uint64_t>() <= UINT32_MAX; }
int number(const Json& v, int lo, int hi, int fallback) {
    if (!v.is_number_integer()) return fallback;
    const auto n = v.get<std::int64_t>();
    return n < lo || n > hi ? fallback : static_cast<int>(n);
}
std::string name(const Json& v) { return v.is_string() && !v.get_ref<const std::string&>().empty() && v.get_ref<const std::string&>().size() <= 128 ? v.get<std::string>() : std::string{}; }
int percent(const Json& row) { // {hp, max_hp} of a party row, entity or own state
    const auto hp = row.value("hp", std::int64_t{0}), max = std::max<std::int64_t>(1, row.value("max_hp", std::int64_t{1}));
    return hp <= 0 ? 0 : static_cast<int>(std::clamp<std::int64_t>(hp * 100 / max, 1, 100));
}
}

bool ClientRaid::parties_from(const Json& value, std::uint32_t leader) {
    if (!value.is_array() || value.size() > 4) return false;
    std::array<std::vector<std::uint32_t>, 4> parties{};
    std::set<std::uint32_t> seen;
    for (std::size_t p = 0; p < value.size(); ++p) {
        if (!value[p].is_array() || value[p].size() > 6) return false;
        for (const auto& m : value[p]) {
            if (!id_value(m) || !seen.insert(m.get<std::uint32_t>()).second) return false;
            parties[p].push_back(m.get<std::uint32_t>());
        }
    }
    if (leader && !seen.contains(leader)) return false;
    leader_ = leader; parties_ = std::move(parties);
    for (auto it = known_.begin(); it != known_.end();) if (!seen.contains(it->first)) it = known_.erase(it); else ++it;
    return true;
}

bool ClientRaid::receive(const Json& event) {
    if (!event.is_object() || event.value("type", std::string{}) != "raid") return false;
    const auto& leader = event.contains("leader") ? event.at("leader") : Json();
    if (leader == 0) { reset(); return true; }
    if (!id_value(leader) || !event.contains("parties")) return false;
    return parties_from(event.at("parties"), leader.get<std::uint32_t>());
}

void ClientRaid::update(const Json& state, std::uint32_t self) {
    if (const auto raid = state.find("raid"); raid != state.end() && raid->is_object() && raid->contains("leader") && id_value(raid->at("leader")) && raid->contains("parties")) {
        if (!parties_from(raid->at("parties"), raid->at("leader").get<std::uint32_t>())) return;
        missing_ = 0;
        if (const auto rows = raid->find("rows"); rows != raid->end() && rows->is_array())
            for (const auto& row : *rows) {
                if (!row.is_array() || row.size() != 6 || !id_value(row[0])) continue;
                Member m{row[0].get<std::uint32_t>(), name(row[1]), number(row[2], 1, 3, 0), number(row[3], 1, 1000, 0), number(row[4], 0, 100, 0)};
                known_[m.id] = std::move(m);
            }
    }
    else if (active() && ++missing_ >= 100) reset(); // gone from the server (another shard), not merely omitted from a full frame
    if (!active()) return;
    // The own party: "party" rows (out of view), player entities (in view) and the own character.
    const auto learn = [&](std::uint32_t id, const Json& row) {
        if (std::none_of(parties_.begin(), parties_.end(), [&](const auto& p) { return std::find(p.begin(), p.end(), id) != p.end(); })) return;
        auto& m = known_[id]; m.id = id;
        if (const auto n = name(row.value("name", Json())); !n.empty()) m.name = n;
        m.class_id = number(row.value("class_id", Json()), 1, 3, m.class_id); m.level = number(row.value("level", Json()), 1, 1000, m.level); m.percent = percent(row);
    };
    if (const auto party = state.find("party"); party != state.end() && party->is_array())
        for (const auto& row : *party) if (row.is_object() && id_value(row.value("id", Json()))) learn(row.at("id").get<std::uint32_t>(), row);
    if (const auto entities = state.find("entities"); entities != state.end() && entities->is_array())
        for (const auto& e : *entities) if (e.is_object() && e.value("kind", std::string{}) == "player" && id_value(e.value("id", Json()))) learn(e.at("id").get<std::uint32_t>(), e);
    if (const auto character = state.find("character"); character != state.end() && character->is_object() && character->contains("state")) {
        Json own = character->at("state"); own["name"] = character->value("name", Json()); own["class_id"] = character->value("class_id", Json());
        learn(self, own);
    }
}

const ClientRaid::Member* ClientRaid::member(std::uint32_t id) const {
    const auto it = known_.find(id);
    return it == known_.end() ? nullptr : &it->second;
}

void ClientRaid::reset() { leader_ = 0; parties_ = {}; known_.clear(); missing_ = 0; }

std::array<int, 2> ClientRaid::paint(ClientUI& ui, int x, int y, float scale) const {
    if (!active()) return {0, 0};
    const auto S = [&](float v) { return static_cast<int>(std::lround(v * scale)); };
    std::size_t columns = 0;
    for (std::size_t p = 0; p < parties_.size(); ++p) if (!parties_[p].empty()) columns = p + 1;
    const int lh = ui.line_height(), column = S(96), row = lh + S(6), pad = S(4);
    const int width = static_cast<int>(columns) * column + pad, height = lh + S(4) + 6 * row + pad;
    const std::array<std::uint8_t, 4> gold{230, 200, 120, 255}, white{255, 255, 255, 255}, dim{150, 150, 150, 255}, back{8, 8, 12, 170},
        bar{40, 40, 40, 220}, hp{200, 40, 40, 255};
    ui.rect(x, y, width, height, back);
    for (std::size_t p = 0; p < columns; ++p) {
        const int cx = x + pad + static_cast<int>(p) * column;
        ui.text(cx, y + S(2), "Grup " + std::to_string(p + 1), gold);
        int ry = y + lh + S(4);
        for (const auto id : parties_[p]) {
            const auto* m = member(id);
            std::string label = m && !m->name.empty() ? m->name : "#" + std::to_string(id);
            if (id == leader_) label = "* " + label;
            while (label.size() > 1 && ui.text_width(label) > column - pad) { label.pop_back(); while (!label.empty() && (static_cast<unsigned char>(label.back()) & 0xc0) == 0x80) label.pop_back(); }
            const int pct = m ? m->percent : 0;
            ui.text(cx, ry, label, pct > 0 ? white : dim);
            ui.rect(cx, ry + lh, column - pad * 2, S(3), bar);
            if (pct > 0) ui.rect(cx, ry + lh, (column - pad * 2) * pct / 100, S(3), hp);
            ry += row;
        }
    }
    return {width, height};
}
} // namespace ikv
