// NONCANONICAL wave 29: the map catalog behind the client's minimap quest markers and its Dünya Haritası (M). The original
// client had no world map and no quest markers; its Harita (H) minimap drew nearby actors as dots only. state() carries
// catalogs.map for the actor's zone (older clients ignore the key; catalogs are resent only when they change):
//   {"npcs":[{id, kind, name?, x, y, mark?, to?, banker?}], "objectives":[{quest, x, y, r}]}
// npcs: the zone's quest givers, merchants, doors, anvils and reset machines (hidden markers, gm-only doors for players and
// freed rescue NPCs are left out). mark: 1 an accepted quest handed in here that is not finished yet ('?' grey), 2 a quest
// to take here ('!'), 3 a finished quest to hand in here ('?'); the highest applies. to: a door's destination zone source
// registry id. A door carries no name (its classes.dat name is internal; the client shows the destination label).
// objectives: where the accepted, unfinished quests are worked in this zone: kill/collect monster spawn clusters (centroid,
// radius), visit spots and use targets; r in wire units (0: a point).
#include "ikv/world.hpp"
#include "ikv/protocol.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

namespace ikv {
namespace {
constexpr std::size_t objective_max = 32, cluster_max = 4;
constexpr int64_t cluster_join = 90000; // spawns within 90 scene units of a cluster centre share one objective area
bool listed(const std::string& kind) { return kind == "questgiver" || kind == "merchant" || kind == "door" || kind == "anvil" || kind == "reset"; }
// A configured NPC id or one of its instance copies (id + n*65536, world.cpp instances).
bool same_npc(uint32_t id, const Json& configured) {
    if (!configured.is_number_integer()) return false;
    const auto base = configured.get<int64_t>();
    return base > 0 && base <= UINT32_MAX && (id == base || (id > base && (id - base) % 65536 == 0));
}
int quest_total(const Json& q) { const auto& o = q.at("objective"); return o.at("kind") == "talk" || o.at("kind") == "visit" ? 1 : o.at("count").get<int>(); }
} // namespace

void World::map_index() {
    map_npcs_.clear(); map_spawns_.clear();
    std::map<std::string, std::map<std::string, std::vector<std::array<int64_t,2>>>> spawns;
    for (const auto& [id, n] : npcs_) {
        const auto& d = n.definition;
        const auto& kind = d.at("kind").get_ref<const std::string&>();
        const auto& zone = d.at("zone").get_ref<const std::string&>();
        if (kind == "enemy") { spawns[zone][d.at("template").get<std::string>()].push_back({d.at("x").get<int64_t>(), d.at("y").get<int64_t>()}); continue; }
        if (listed(kind) && !d.value("hidden", false)) map_npcs_[zone].push_back(id);
    }
    // Greedy clusters in id order (deterministic), then the largest cluster_max per template with centroid and covering radius.
    for (const auto& [zone, templates] : spawns) for (const auto& [name, points] : templates) {
        struct Cluster { int64_t sx = 0, sy = 0, n = 0; std::vector<std::array<int64_t,2>> points; };
        std::vector<Cluster> clusters;
        for (const auto& p : points) {
            auto near_cluster = std::find_if(clusters.begin(), clusters.end(), [&](const Cluster& c) {
                return std::hypot(double(p[0] - c.sx / c.n), double(p[1] - c.sy / c.n)) <= double(cluster_join); });
            if (near_cluster == clusters.end()) { clusters.emplace_back(); near_cluster = std::prev(clusters.end()); }
            near_cluster->sx += p[0]; near_cluster->sy += p[1]; ++near_cluster->n; near_cluster->points.push_back(p);
        }
        std::stable_sort(clusters.begin(), clusters.end(), [](const Cluster& a, const Cluster& b) { return a.n > b.n; });
        if (clusters.size() > cluster_max) clusters.resize(cluster_max);
        auto& out = map_spawns_[zone][name];
        for (const auto& c : clusters) {
            const int64_t cx = c.sx / c.n, cy = c.sy / c.n; double r = 0;
            for (const auto& p : c.points) r = std::max(r, std::hypot(double(p[0] - cx), double(p[1] - cy)));
            out.push_back({cx, cy, std::clamp<int64_t>(std::llround(r) + 15000, 20000, 500000)});
        }
    }
}

Json World::map_view(uint32_t id) const {
    Json npcs = Json::array(), objectives = Json::array();
    const auto found = actors_.find(id);
    if (found == actors_.end()) return {{"npcs", std::move(npcs)}, {"objectives", std::move(objectives)}};
    const auto& a = found->second;
    const auto& own = a.character.at("state");
    const int own_class = a.character.at("class_id");
    const auto& zone = own.at("zone").get_ref<const std::string&>();
    const auto& zone_row = config_.at("zones").at(zone);
    const std::string base_zone = zone_row.value("instance_of", zone);
    const auto progress = [&](const std::string& quest) { return own.at("quests").at(quest).value("progress", 0); };
    if (const auto listed_npcs = map_npcs_.find(zone); listed_npcs != map_npcs_.end()) for (const auto npc_id : listed_npcs->second) {
        const auto& n = npcs_.at(npc_id); const auto& d = n.definition;
        const auto& kind = d.at("kind").get_ref<const std::string&>();
        if (d.value("rescue", false) && !n.hp) continue; // a freed Cin has left (state() entities)
        Json row = {{"id", npc_id}, {"kind", kind}, {"x", n.x}, {"y", n.y}};
        if (kind == "door") {
            const auto& door = d.at("door");
            if (door.value("gmonly", false) && !a.admin) continue;
            if (const auto& to = config_.at("zones").at(door.at("to").get<std::string>()).value("source_registry_id", Json(nullptr)); !to.is_null()) row["to"] = to;
        } else row["name"] = d.at("name");
        if (d.value("banker", false)) row["banker"] = true;
        int mark = 0;
        if (kind == "questgiver" || kind == "merchant") for (const auto& quest : quests_at(n, own, own_class)) {
            const auto& qid = quest.get_ref<const std::string&>(); const auto& q = config_.at("quests").at(qid);
            if (own.at("quests").contains(qid)) {
                if (!same_npc(npc_id, q.contains("turn_in") ? q.at("turn_in") : q.at("giver"))) continue;
                const bool ready = q.at("objective").at("kind") == "talk" || progress(qid) >= quest_total(q);
                mark = std::max(mark, ready ? 3 : 1);
            } else if (q.value("auto", false) != same_npc(npc_id, q.at("giver"))) mark = std::max(mark, 2); // Görev al (client dialogue_quests)
        }
        if (mark) row["mark"] = mark;
        npcs.push_back(std::move(row));
    }
    const auto spot = [&](const std::string& quest, int64_t x, int64_t y, int64_t r) {
        if (objectives.size() < objective_max) objectives.push_back({{"quest", quest}, {"x", x}, {"y", y}, {"r", r}}); };
    const auto spawn_areas = [&](const std::string& quest, const Json& target) {
        const auto z = map_spawns_.find(zone); if (z == map_spawns_.end() || !target.is_string()) return;
        const auto t = z->second.find(target.get<std::string>()); if (t == z->second.end()) return;
        for (const auto& c : t->second) spot(quest, c[0], c[1], c[2]);
    };
    for (const auto& [qid, state] : own.at("quests").items()) {
        (void)state; const auto& q = config_.at("quests").at(qid); const auto& o = q.at("objective");
        const auto& kind = o.at("kind").get_ref<const std::string&>();
        if (kind == "talk" || progress(qid) >= quest_total(q)) continue; // done: the turn-in NPC's '?' is the target
        if (kind == "kill" || kind == "collect") {
            if (o.contains("target")) spawn_areas(qid, o.at("target"));
            for (const auto& also : o.value("also", Json::array())) if (also.contains("target")) spawn_areas(qid, also.at("target"));
        } else if (kind == "visit") {
            if (o.at("zone") == zone || o.at("zone") == base_zone) spot(qid, o.at("x").get<int64_t>(), o.at("y").get<int64_t>(), o.at("radius").get<int64_t>());
        } else if (kind == "use") {
            for (const auto& t : o.at("targets")) if (t.is_number_integer()) // the NPC, then its instance copies (+n*65536, contiguous)
                for (uint64_t copy = t.get<uint64_t>(); copy <= UINT32_MAX; copy += 65536) {
                    const auto n = npcs_.find(static_cast<uint32_t>(copy));
                    if (n == npcs_.end()) break;
                    if (n->second.definition.at("zone") == zone) { spot(qid, n->second.x, n->second.y, 0); break; }
                }
        }
    }
    return {{"npcs", std::move(npcs)}, {"objectives", std::move(objectives)}};
}
} // namespace ikv
