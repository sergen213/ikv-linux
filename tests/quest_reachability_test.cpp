// Real-content reachability (wave 19, the quest audit's fix.py fixpoint): every configured quest is completable on foot from the
// Eminönü spawn, and every door arrival walks back to its zone's spawn. Regions are the 4-connected walkable components of each
// zone's original support grid (World routes take a diagonal only when both orthogonals pass); a door joins each region within
// interaction_range of it to its arrival's region; a quest-gated door is open once its quest is taken or done (door_gate) and
// required_death doors count as open. A quest is taken once its prerequisites are done and its giver is reached (auto quests at
// once), done once every spot (giver, turn-in, use props, kill/collect spawns within 20 units, visit spot) is reached with it taken.
//   quest-reachability-test config/gameplay.json   (original/app/base beside config/; skipped without the grids)
#include "ikv/scene.hpp"
#include "ikv/world.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>

namespace {
using Region = std::pair<std::string, int32_t>;
using Regions = std::set<Region>;
int cell(double wire) { return int(std::floor(wire / 1000 * (2.0 / 3) + 0.5)) + 1024; }
}

int main(int argc, char** argv) try {
    if (argc != 2) throw std::runtime_error("usage: quest-reachability-test config/gameplay.json");
    const auto assets = std::filesystem::path(argv[1]).parent_path().parent_path() / "original" / "app" / "base";
    if (!std::filesystem::exists(assets / "zone00.bin")) { std::cout << "skipped: no original walk grids in " << assets << '\n'; return 0; }
    ikv::Json config; { std::ifstream in(argv[1]); in >> config; }
    const auto& quests = config.at("quests"); const auto& npcs = config.at("npcs");
    const double reach = config.at("interaction_range").get<double>();

    std::map<std::string, const std::vector<int32_t>*> labels; std::map<unsigned, std::vector<int32_t>> by_support;
    for (const auto& [name, zone] : config.at("zones").items()) {
        const unsigned support = ikv::source_scene_descriptor(zone.at("source_registry_id").get<unsigned>()).support_id;
        if (!by_support.contains(support)) {
            const auto grid = ikv::load_zone(assets, support); auto& label = by_support[support]; label.assign(grid.cells.size(), 0);
            const int w = int(grid.width), h = int(grid.height); int32_t next = 0;
            for (int start = 0; start < w * h; ++start) {
                if (label[size_t(start)] || grid.cells[size_t(start)] != 255) continue;
                std::deque<int> open{start}; label[size_t(start)] = ++next;
                while (!open.empty()) {
                    const int c = open.front(); open.pop_front(); const int x = c % w, y = c / w;
                    for (const auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                        const int nx = x + dx, ny = y + dy, n = ny * w + nx;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h || label[size_t(n)] || grid.cells[size_t(n)] != 255) continue;
                        label[size_t(n)] = next; open.push_back(n);
                    }
                }
            }
        }
        labels[name] = &by_support.at(support);
    }
    const auto near = [&](const std::string& zone, double x, double y, double r) {
        Regions out; const auto& label = *labels.at(zone); const int cx = cell(x), cy = cell(y), k = int(r / 1500) + 1;
        for (int gy = std::max(0, cy - k); gy <= std::min(2047, cy + k); ++gy)
            for (int gx = std::max(0, cx - k); gx <= std::min(2047, cx + k); ++gx)
                if (const auto l = label[size_t(gy) * 2048 + size_t(gx)]; l && (double(gx - cx) * (gx - cx) + double(gy - cy) * (gy - cy)) * 2250000 <= r * r) out.insert({zone, l});
        return out;
    };
    const auto at = [&](const std::string& zone, const ikv::Json& p) { return near(zone, p[0].get<double>(), p[1].get<double>(), 1500); };
    std::map<int, std::string> by_source; for (const auto& [id, q] : quests.items()) if (q.contains("source_quest_id")) by_source[q.at("source_quest_id").get<int>()] = id;
    struct Door { std::string name, gate; Regions from, to; };
    std::vector<Door> doors; std::map<Region, std::vector<size_t>> leaving;
    for (const auto& n : npcs) if (n.at("kind") == "door") {
        const auto& d = n.at("door"); Door door{n.at("name"), "", near(n.at("zone"), n.at("x").get<double>(), n.at("y").get<double>(), reach), at(d.at("to"), d.at("arrive"))};
        if (d.contains("required_quest") && by_source.contains(d.at("required_quest").get<int>())) door.gate = by_source.at(d.at("required_quest").get<int>());
        for (const auto& r : door.from) leaving[r].push_back(doors.size());
        doors.push_back(std::move(door));
    }
    const auto walk = [&](Regions start, const std::function<bool(const Door&)>& open) {
        std::vector<Region> todo(start.begin(), start.end());
        while (!todo.empty()) { const auto r = todo.back(); todo.pop_back();
            if (const auto l = leaving.find(r); l != leaving.end()) for (auto i : l->second) if (open(doors[i])) for (const auto& t : doors[i].to) if (start.insert(t).second) todo.push_back(t); }
        return start;
    };
    std::map<std::string, Regions> spawn; for (const auto& [name, zone] : config.at("zones").items()) spawn[name] = at(name, zone.at("spawn"));
    int failures = 0;
    for (const auto& door : doors) {
        if (door.to.empty()) { std::cout << "FAIL door " << door.name << ": arrival off the walk grid\n"; ++failures; continue; }
        const auto& home = spawn.at(door.to.begin()->first); const auto seen = walk(door.to, [](const Door&) { return true; });
        if (std::none_of(home.begin(), home.end(), [&](const Region& r) { return seen.contains(r); })) { std::cout << "FAIL door " << door.name << ": its arrival cannot walk back to the " << door.to.begin()->first << " spawn\n"; ++failures; }
    }

    std::map<uint32_t, const ikv::Json*> by_id; for (const auto& n : npcs) by_id[n.at("id").get<uint32_t>()] = &n;
    struct Spot { std::string role; Regions regions; };
    std::map<std::string, std::vector<Spot>> spots;
    for (const auto& [id, q] : quests.items()) {
        auto& list = spots[id]; const auto& o = q.at("objective");
        const auto of = [&](const char* role, const std::vector<const ikv::Json*>& ns, double r) { Spot s{role, {}}; for (const auto* n : ns) s.regions.merge(near(n->at("zone"), n->at("x").get<double>(), n->at("y").get<double>(), r)); list.push_back(std::move(s)); };
        const auto family = [&](const ikv::Json& target) { std::vector<const ikv::Json*> out; for (const auto& n : npcs) if (n.value("template", std::string()) == target.get<std::string>()) out.push_back(&n); return out; };
        of("giver", {by_id.at(q.at("giver").get<uint32_t>())}, reach);
        if (q.contains("turn_in")) of("turn_in", {by_id.at(q.at("turn_in").get<uint32_t>())}, reach);
        if (o.at("kind") == "use") {
            std::set<uint32_t> uids; for (const auto& t : o.at("targets")) uids.insert(by_id.at(t.get<uint32_t>())->at("source_uid").get<uint32_t>());
            std::vector<const ikv::Json*> props; for (const auto& n : npcs) if (n.at("kind") != "enemy" && n.contains("source_uid") && uids.contains(n.at("source_uid").get<uint32_t>())) props.push_back(&n);
            of("use", props, reach);
        }
        if (o.contains("target")) of("target", family(o.at("target")), 20000);
        for (const auto& v : o.value("also", ikv::Json::array())) if (v.contains("target")) of("also-target", family(v.at("target")), 20000);
        if (o.at("kind") == "visit") list.push_back({"visit", near(o.at("zone"), o.at("x").get<double>(), o.at("y").get<double>(), o.at("radius").get<double>())});
    }
    const auto prerequisites = [&](const ikv::Json& q, const std::set<std::string>& done) {
        if (q.contains("requires") && !done.contains(q.at("requires").get<std::string>())) return false;
        for (const auto& r : q.value("requires_also", ikv::Json::array())) if (!done.contains(r.get<std::string>())) return false;
        return true;
    };
    const auto meets = [](const Regions& a, const Regions& b) { return std::any_of(a.begin(), a.end(), [&](const Region& r) { return b.contains(r); }); };
    const auto reached = [&](const std::set<std::string>& open) { return walk(spawn.at(config.at("defaults").at("zone")), [&](const Door& d) { return d.gate.empty() || open.contains(d.gate); }); };
    std::set<std::string> done, taken;
    for (bool grew = true; grew;) {
        std::set<std::string> open = done; open.insert(taken.begin(), taken.end()); const auto before = reached(open);
        std::set<std::string> take;
        for (const auto& [id, q] : quests.items()) if (!done.contains(id) && prerequisites(q, done) && (q.value("auto", false) || meets(spots.at(id)[0].regions, before))) take.insert(id);
        open.insert(take.begin(), take.end()); const auto after = reached(open);
        grew = false;
        for (const auto& id : take) {
            grew |= taken.insert(id).second;
            if (std::all_of(spots.at(id).begin(), spots.at(id).end(), [&](const Spot& s) { return meets(s.regions, after); })) grew |= done.insert(id).second;
        }
    }
    for (const auto& [id, q] : quests.items()) if (!done.contains(id)) {
        std::cout << "FAIL quest " << id << ' ' << q.at("name").get<std::string>() << ": never completable on foot";
        if (prerequisites(q, done)) { std::set<std::string> open = done; open.insert(taken.begin(), taken.end()); open.insert(id); const auto r = reached(open);
            for (const auto& s : spots.at(id)) if (!meets(s.regions, r)) std::cout << " (" << s.role << " unreachable)"; }
        std::cout << '\n'; ++failures;
    }
    std::cout << done.size() << '/' << quests.size() << " quests completable on foot, " << doors.size() << " door arrivals checked\n";
    return failures ? 1 : 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
