// NONCANONICAL wave 29: data and geometry of the minimap quest markers and the Dünya Haritası (M); see client_map.hpp.
#include "ikv/client_map.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string_view>

namespace ikv {
namespace {
using Json = nlohmann::json;
const Json& member(const Json& object, const char* key) {
    static const Json empty = Json::object();
    if (!object.is_object()) return empty;
    const auto found = object.find(key);
    return found == object.end() ? empty : *found;
}
float scene(const Json& value) { return value.is_number() ? value.get<float>() / 1000.f : 0.f; }
bool listed(std::string_view kind) { return kind == "questgiver" || kind == "merchant" || kind == "door" || kind == "anvil" || kind == "reset"; }
bool contains_id(const Json& list, std::uint32_t id) {
    return list.is_array() && std::any_of(list.begin(), list.end(), [&](const Json& v) { return v.is_number_integer() && v.get<std::int64_t>() == id; });
}
// White RGBA art from a signed distance `inside(x,y)` (px, positive inside), darkened within `rim` px of the edge; 4x4 supersampled.
assets::DdsImage raster(std::uint32_t size, const std::function<float(float, float)>& inside, float rim) {
    assets::DdsImage image{size, size, std::vector<std::uint8_t>(std::size_t{size} * size * 4), {}};
    for (std::uint32_t y = 0; y < size; ++y) for (std::uint32_t x = 0; x < size; ++x) {
        float cover = 0, light = 0;
        for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx) {
            const float d = inside(static_cast<float>(x) + (static_cast<float>(sx) + .5f) / 4, static_cast<float>(y) + (static_cast<float>(sy) + .5f) / 4);
            if (d >= 0) { cover += 1.f / 16; if (d >= rim) light += 1.f / 16; }
        }
        auto* p = &image.rgba[(std::size_t{y} * size + x) * 4];
        const auto shade = static_cast<std::uint8_t>(std::lround(cover > 0 ? 255 * light / cover : 0));
        p[0] = p[1] = p[2] = shade; p[3] = static_cast<std::uint8_t>(std::lround(255 * cover));
    }
    image.mips.push_back({size, size, 0, image.rgba.size()});
    return image;
}
} // namespace

int ClientMap::quest_mark(const Json& offered, std::uint32_t npc, const Json& character_state, const Json& quest_catalog) {
    const auto is = [&](const Json& c) {
        if (!c.is_number_integer() || c.get<std::int64_t>() <= 0 || c.get<std::int64_t>() > UINT32_MAX) return false;
        const auto base = c.get<std::uint32_t>();
        return npc == base || (npc > base && (npc - base) % 65536 == 0);
    };
    const Json& active = member(character_state, "quests");
    const Json& done = member(character_state, "quests_done");
    const auto finished = [&](const Json& id) { return done.is_array() && std::find(done.begin(), done.end(), id) != done.end(); };
    int mark = mark_none;
    if (!offered.is_array()) return mark;
    for (const auto& quest : offered) {
        if (!quest.is_string() || !quest_catalog.is_object() || !quest_catalog.contains(quest.get<std::string>())) continue;
        const auto& id = quest.get_ref<const std::string&>();
        const auto& q = quest_catalog.at(id);
        if (active.contains(id)) {
            if (!is(q.contains("turn_in") ? q.at("turn_in") : member(q, "giver"))) continue;
            const Json& objective = member(q, "objective");
            const auto kind = objective.value("kind", std::string{});
            const int count = kind == "talk" || kind == "visit" ? 1 : objective.value("count", 1);
            const bool ready = kind == "talk" || (member(active.at(id), "progress").is_number() && active.at(id).at("progress").get<int>() >= count);
            mark = std::max(mark, ready ? int(mark_ready) : int(mark_pending));
            continue;
        }
        bool open = !q.contains("requires") || finished(q.at("requires"));
        for (const auto& also : q.value("requires_also", Json::array())) open = open && finished(also);
        if (!finished(quest) && open && q.value("auto", false) != is(member(q, "giver"))) mark = std::max(mark, int(mark_available));
    }
    return mark;
}

void ClientMap::update(const Json& state, std::uint32_t self, const Json& group) {
    npcs_.clear(); objectives_.clear(); party_.clear();
    const Json& catalogs = member(state, "catalogs");
    const Json& quests = member(catalogs, "quests");
    const auto quest_name = [&](const std::string& id) { return quests.contains(id) ? member(quests, id.c_str()).value("name", id) : id; };
    const Json& map = member(catalogs, "map");
    authoritative_ = map.contains("npcs");
    const Json& character_state = member(member(state, "character"), "state");
    if (authoritative_) {
        for (const auto& n : map.at("npcs"))
            npcs_.push_back({n.at("id").get<std::uint32_t>(), n.at("kind").get<std::string>(), n.value("name", std::string{}), scene(n.at("x")), scene(n.at("y")),
                             n.value("mark", 0), n.value("to", -1), n.value("banker", false)});
        for (const auto& o : member(map, "objectives")) {
            const auto quest = o.at("quest").get<std::string>();
            objectives_.push_back({quest, quest_name(quest), scene(o.at("x")), scene(o.at("y")), scene(o.at("r"))});
        }
    } else { // pre-wave-29 server: the visible NPCs and the active quests' visit spots in this zone
        for (const auto& e : member(state, "entities")) {
            const auto kind = e.value("kind", std::string{});
            if (!listed(kind)) continue;
            const auto id = e.value("id", 0u);
            npcs_.push_back({id, kind, kind == "door" ? std::string{} : e.value("name", std::string{}), scene(member(e, "x")), scene(member(e, "y")),
                             quest_mark(member(e, "quests"), id, character_state, quests), -1, e.value("banker", false)});
        }
        const auto zone = state.value("zone", std::string{});
        for (const auto& [id, progress] : member(character_state, "quests").items()) {
            (void)progress;
            const Json& objective = member(member(quests, id.c_str()), "objective");
            if (objective.value("kind", std::string{}) == "visit" && objective.value("zone", std::string{}) == zone)
                objectives_.push_back({id, quest_name(id), scene(member(objective, "x")), scene(member(objective, "y")), scene(member(objective, "radius"))});
        }
    }
    const auto zone = state.value("zone", std::string{});
    for (const auto& row : member(state, "party"))
        if (row.value("zone", std::string{}) == zone && row.contains("x") && row.contains("y"))
            party_.push_back({row.value("id", 0u), row.value("name", std::string{}), scene(row.at("x")), scene(row.at("y"))});
    for (const auto& e : member(state, "entities")) {
        const auto id = e.value("id", 0u);
        if (id != self && e.value("kind", std::string{}) == "player" && contains_id(group, id))
            party_.push_back({id, e.value("name", std::string{}), scene(member(e, "x")), scene(member(e, "y"))});
    }
}

ClientMap::Edge ClientMap::clamp_to_rect(float px, float py, float l, float t, float r, float b, float inset) {
    l += inset; t += inset; r -= inset; b -= inset;
    const float cx = (l + r) / 2, cy = (t + b) / 2, dx = px - cx, dy = py - cy;
    Edge edge{px, py, std::atan2(dy, dx), false};
    if (px >= l && px <= r && py >= t && py <= b) return edge;
    float scale = 1;
    if (std::abs(dx) > 1e-6f) scale = std::min(scale, (r - cx) / std::abs(dx));
    if (std::abs(dy) > 1e-6f) scale = std::min(scale, (b - cy) / std::abs(dy));
    edge.x = cx + dx * scale; edge.y = cy + dy * scale; edge.clamped = true;
    return edge;
}

const char* ClientMap::role(const Npc& npc) {
    if (npc.banker) return "Bankacı";
    if (npc.kind == "questgiver") return "Görev veren";
    if (npc.kind == "merchant") return "Satıcı";
    if (npc.kind == "door") return "Geçit";
    if (npc.kind == "anvil") return "Örs";
    if (npc.kind == "reset") return "Sıfırlayıcı";
    return "";
}

void ClientMap::clamp_view() {
    zoom = std::clamp(zoom, 1.f, 8.f);
    const float h = half();
    centre_u = std::clamp(centre_u, h, 2048 - h); centre_v = std::clamp(centre_v, h, 2048 - h);
}
void ClientMap::zoom_at(int steps, float u, float v) {
    const float h = half(), fu = (u - (centre_u - h)) / (2 * h), fv = (v - (centre_v - h)) / (2 * h);
    zoom *= std::pow(1.25f, static_cast<float>(steps));
    zoom = std::clamp(zoom, 1.f, 8.f);
    const float n = half();
    centre_u = u + n - 2 * fu * n; centre_v = v + n - 2 * fv * n;
    clamp_view();
}
void ClientMap::pan(float du, float dv) { centre_u += du; centre_v += dv; clamp_view(); }

assets::DdsImage ClientMap::arrow_image() {
    // Apex (16,2), base corners (3,29) and (29,29): signed distance inside the triangle, 2 px dark rim.
    constexpr std::array<std::array<float, 2>, 3> p{{{16, 2}, {29, 29}, {3, 29}}};
    return raster(32, [&](float x, float y) {
        float d = 1e9f;
        for (std::size_t i = 0; i < 3; ++i) {
            const auto& a = p[i]; const auto& b = p[(i + 1) % 3];
            const float ex = b[0] - a[0], ey = b[1] - a[1], len = std::hypot(ex, ey);
            d = std::min(d, ((x - a[0]) * ey - (y - a[1]) * ex) / len * -1.f);
        }
        return d;
    }, 2);
}
assets::DdsImage ClientMap::disc_image() {
    return raster(32, [](float x, float y) { return 15 - std::hypot(x - 16, y - 16); }, 2);
}
assets::DdsImage ClientMap::ring_image() {
    return raster(64, [](float x, float y) { const float r = std::hypot(x - 32, y - 32); return std::min(31 - r, r - 27); }, 0);
}
} // namespace ikv
