// Regenerates the world-content keys of config/gameplay.json (provenance, defaults, zones, items, quests, npcs)
// from the original data/classes.dat, animation.dat and prefixes.dat tables and the zoneNN.bin support grids.
// Every other key of the input file is preserved. The result is validated by constructing ikv::World with the
// real support grids before anything is written.
//   ikv-content-generator ASSET_BASE config/gameplay.json [extract.json]
// extract.json receives every classes.dat npc/monster/loc and every weapon/armour/potion in the native schema.
#include "ikv/scene.hpp"
#include "ikv/world.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace {
using Ordered = nlohmann::ordered_json;
using Attributes = std::map<std::string, std::string>;

constexpr uint32_t first_npc_id = 2147493649u; // 2^31 + 10001, the high uint32 NPC id range the world requires
constexpr std::pair<const char*, unsigned> zone_registry[] = {
    {"eminonu", 0}, {"misir", 1}, {"antrepo", 2}, {"labirent", 3}, {"meteor", 4}, {"ada", 5}, {"savas", 6},
    {"yeralti", 26}, {"migrat", 36}, {"siginak", 46}, {"cemberli", 66}, {"topkapi", 67}, {"buyukhol", 68},
    {"karakoy", 69}, {"karakoyic", 70}};
// Paired original <loc> doors/crystals by classes.dat uid (from -> to). Names, locationtypename and detail pair them
// (e.g. eminonumisirkapi1/misirkapi1, eminonu_to_topkapi_1/topkapi_to_eminonu_1); the destinations themselves were
// server data, so each pairing is a documented inference. Bidirectional unless marked one-way.
struct Pair { unsigned from, to; bool both; };
constexpr Pair loc_pairs[] = {
    {30000, 30001, true}, {30302, 30303, true},          // Eminönü <-> Mısır Çarşısı (Balıkpazarı, Haseki doors)
    {30005, 30006, true},                                // Antrepo door
    {30030, 30031, true}, {30035, 30030, false},         // Labirent entrance and its second exit
    {30046, 30047, true},                                // Meteor Bölgesi sign
    {30212, 30213, true}, {30051, 30052, true},          // Meteor -> Yeraltı, Meteor -> Sivri Ada
    {131144, 131145, true}, {131993, 131146, true},      // Mısır -> Sığınaklar
    {131133, 131148, true}, {131994, 131148, false},     // Sivri Ada well -> Migrat temple stairs
    {131361, 131362, true}, {131949, 131362, false},     // Meteor ventilation -> Çemberlitaş
    {131839, 131842, true}, {131840, 131841, true},      // Meteor -> Büyük Hol (Arz, Lodos)
    {131518, 131507, true}, {131517, 131516, true},      // Eminönü <-> Topkapı crystal and draisine
    {131519, 131508, true}, {131520, 131509, true}, {131521, 131510, true}, {131522, 131511, true},
    {131523, 131512, true}, {131524, 131513, true}, {131525, 131514, true}, {131526, 131515, true},
    {131844, 131507, false}, {131845, 131507, false},    // Büyük Hol crystals -> Topkapı (no return crystal)
    {132058, 132165, true}, {132164, 132166, true},      // boats Eminönü <-> Karaköy, Eminönü (Antrepo pier) <-> Karaköy
    {132061, 132178, true},                              // Karaköy Umut Dergisi <-> interior
    // Doors, barriers and elevators inside or between zones (locationname giris/cikis, SinemayaIn/SinemaHoleCik, hidragirisN/hidracikisN).
    {132059, 132177, true}, {132060, 132176, true},      // Karaköy elevator: Alt Katlar -> cinema, Üst Katlar -> Fehim's floor (interior)
    {132062, 132063, true},                              // Zeplin girişi/çıkışı
    {132069, 132070, true},                              // Karaköy sığınak kapağı <-> basamaklar
    {132183, 132185, true}, {132187, 132189, true},      // Çare Kapısı, Sonsuzluk Koridoru (interior)
    {132197, 132199, true}, {132179, 132180, true},      // Serum Oda Kapısı giriş/çıkış
    {30036, 30037, true},                                // Labirent Örümcek Kapanı portals 1 <-> 2
    {30227, 30228, true},                                // Yeraltı Mühürlü Kapı <-> Taş Kapı (merangiris/merancikis)
    {30229, 30237, true}, {30233, 30238, true}, {30234, 30239, true}, {30235, 30240, true}, {30236, 30241, true}, // Hidra barriers (Engel)
    {131387, 131388, true},                              // Çemberlitaş trap door <-> Yaşam Merkezi crystals
    {131389, 131354, true},                              // Zelal -> İstihbarat crystal <-> İstihbarat inner door
    {131364, 131491, true},                              // Yaşam Merkezi şalter (gaffaragiris) <-> Gaffar Oda (gaffardangecis)
    {131358, 131388, false},                             // Yaşam Merkezi girişi -> Yaşam Merkezi
};
// Pairs whose doors are appended after every other NPC, so earlier NPC ids stay stable. Arena Çıkışı 30201 (dikiliarena, a
// dikilitaş on the Meteor grid whose gamezonename names Eminönü, as a boat's names its destination; the arena side was server
// data): NONCANONICAL one-way exit to Eminönü beside the Meteor Girişi 30046.
constexpr Pair appended_pairs[] = {{30201, 30046, false}};
// A capitalised boat gamezonename names the destination, not the zone the loc stands in; Karaköy elevator pairs at one
// shaft stand in Karaköy (132059/132060) and Karaköy İç Mekan (132176/132177).
constexpr std::pair<unsigned, const char*> loc_zone_overrides[] = {{132058, "eminonu"}, {132165, "karakoy"}, {132164, "eminonu"}, {132166, "karakoy"},
    {132059, "karakoy"}, {132060, "karakoy"}, {132176, "karakoyic"}, {132177, "karakoyic"}};
// Instanced copies: registry rows "migrat1".."migrat9" (27-35), "siginak1".."siginak9" (37-45), "cemberli1".."cemberli19"
// (47-65) reuse the zone's scene; their support grids 8/9/10 are cell-for-cell identical to 14/15/16. Er Meydanı 2-20 are the
// savas registry rows 7-25 (zone name table 0x58ffc4..: "savas2".."savas20" share support grid 6 with "savas", Er Meydanı 1).
constexpr std::tuple<const char*, unsigned, unsigned> zone_instances[] = {{"migrat", 27, 9}, {"siginak", 37, 9}, {"cemberli", 47, 19}, {"savas", 7, 19}};
// Enemy entity ids 2147493667-2147493774 stay enemies of these zones, 9 each in this order (the former NONCANONICAL 3x3 level
// bands); they take the first 9 members of the zone's tools/data/placements.json spawn groups, every other member is appended.
constexpr const char* legacy_enemy_zones[] = {"eminonu", "antrepo", "labirent", "meteor", "ada", "yeralti", "migrat", "siginak", "cemberli", "buyukhol", "karakoy", "karakoyic"};
constexpr size_t legacy_enemies_per_zone = 9;

std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
const std::string& get(const Attributes& a, const char* key) { static const std::string empty; auto f = a.find(key); return f == a.end() ? empty : f->second; }
double number(const Attributes& a, const char* key, double fallback = 0) { const auto& s = get(a, key); try { return s.empty() ? fallback : std::stod(s); } catch (...) { return fallback; } }
int integer(const Attributes& a, const char* key, int fallback = 0) { return int(std::lround(number(a, key, fallback))); }
int clamp(double v, int lo, int hi) { return std::clamp(int(std::lround(v)), lo, hi); }

struct Grid {
    std::string name; unsigned registry = 0; ikv::Zone zone; std::vector<int32_t> label; std::vector<size_t> sizes;
    bool walk(int x, int y) const { return x >= 0 && y >= 0 && x < 2048 && y < 2048 && zone.cells[size_t(y) * 2048 + size_t(x)] == 255; }
    bool block(int x, int y, int r) const { for (int dy = -r; dy <= r; ++dy) for (int dx = -r; dx <= r; ++dx) if (!walk(x + dx, y + dy)) return false; return true; }
    int32_t component(int x, int y) const { return walk(x, y) ? label[size_t(y) * 2048 + size_t(x)] : -1; }
    void label_components() { // 4-connected, stricter than the world's no-corner-cutting 8-neighbour routes
        label.assign(2048u * 2048u, -1); int32_t next = 0;
        for (int start = 0; start < 2048 * 2048; ++start) {
            if (label[size_t(start)] >= 0 || zone.cells[size_t(start)] != 255) continue;
            std::deque<int> open{start}; label[size_t(start)] = next;
            while (!open.empty()) {
                const int c = open.front(); open.pop_front(); const int x = c % 2048, y = c / 2048;
                for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                    if (!walk(x + dx, y + dy) || label[size_t((y + dy) * 2048 + x + dx)] >= 0) continue;
                    label[size_t((y + dy) * 2048 + x + dx)] = next; open.push_back((y + dy) * 2048 + x + dx);
                }
            }
            ++next;
        }
        sizes.assign(size_t(next), 0); for (auto l : label) if (l >= 0) ++sizes[size_t(l)];
    }
    size_t reach(std::pair<int, int> c) const { const auto l = component(c.first, c.second); return l < 0 ? 0 : sizes[size_t(l)]; }
};
// Wire unit = 1/1000 scene unit; a support cell is 1.5 scene units (zone_height rounds x/1.5).
int cell(double units) { return int(std::lround(units / 1.5)) + 1024; }
double units(int c) { return (c - 1024) * 1.5; }
int32_t wire(int c) { return int32_t((c - 1024) * 1500); }
double dist(double ax, double ay, double bx, double by) { return std::hypot(ax - bx, ay - by); }

struct Trigger { unsigned uid = 0; Grid* grid = nullptr; double x = 0, y = 0, range = 0, radius = 0; int quest = -1; };
struct Placed { double x, y; };
}

int main(int argc, char** argv) try {
    if (argc != 3 && argc != 4) throw std::runtime_error("usage: ikv-content-generator ASSET_BASE config/gameplay.json [extract.json]");
    const std::filesystem::path base = argv[1];
    Ordered config = [&] { std::ifstream in(argv[2]); if (!in) throw std::runtime_error("cannot read gameplay config"); return Ordered::parse(in); }();
    const auto classes = ikv::load_source_records(base, "classes.dat");
    const auto animation = ikv::load_source_records(base, "animation.dat");
    const auto prefixes = ikv::load_source_records(base, "prefixes.dat");
    // Hand-curated NPC spots, spawn groups and extra node spots, each row with its provenance (tools/data/placements.json).
    const Ordered placements = [] { std::ifstream in(IKV_PLACEMENTS); if (!in) throw std::runtime_error("cannot read " IKV_PLACEMENTS); return Ordered::parse(in); }();
    // "map": original minimap readout/texel (u, v) = 1024 - 2/3 position (client 00477775); "at": wire units.
    const auto spot = [](const Ordered& row) {
        if (row.contains("map")) return std::pair{(1024 - row.at("map")[0].get<double>()) * 1.5, (1024 - row.at("map")[1].get<double>()) * 1.5};
        return std::pair{row.at("at")[0].get<double>() / 1000, row.at("at")[1].get<double>() / 1000};
    };
    const auto placement = [&](unsigned uid) -> const Ordered* { const auto key = std::to_string(uid); return placements.at("npcs").contains(key) ? &placements.at("npcs").at(key) : nullptr; };

    std::map<std::string, Grid> grids;
    for (const auto& [name, registry] : zone_registry) {
        auto& g = grids[name]; g.name = name; g.registry = registry;
        g.zone = ikv::load_zone(base, ikv::source_scene_descriptor(registry).support_id); g.label_components();
    }
    const auto in_triggers = [](const std::vector<Trigger>& triggers, const Grid& g, double x, double y, double margin) {
        return std::any_of(triggers.begin(), triggers.end(), [&](const Trigger& t) { return t.grid == &g && dist(t.x, t.y, x, y) <= t.range + margin; });
    };
    // First cell (ring by ring, angle by angle) satisfying ok(); deterministic and independent of grid scan order.
    const auto ring_search = [](double cx, double cy, double rmin, double rmax, double angle0, const auto& ok) -> std::optional<std::pair<int, int>> {
        for (double r = rmin; r <= rmax; r += 1.5)
            for (double a = 0; a < 360; a += 10) {
                const double t = (angle0 + a) * M_PI / 180;
                const int x = cell(cx + r * std::cos(t)), y = cell(cy + r * std::sin(t));
                if (ok(x, y)) return std::pair{x, y};
            }
        return std::nullopt;
    };

    // ---- <loc> records: zone, centre and door footprint ----
    std::map<unsigned, Attributes> locs; Ordered extract_locs = Ordered::array();
    std::map<unsigned, Trigger> triggers_by_uid;
    for (const auto& r : classes) {
        if (r.tag != "loc") continue;
        const unsigned uid = unsigned(integer(r.attributes, "uid")); locs[uid] = r.attributes;
        Ordered entry = {{"source_uid", uid}, {"name", get(r.attributes, "name")}, {"locationname", get(r.attributes, "locationname")},
                         {"locationtypename", get(r.attributes, "locationtypename")}, {"gamezonename", get(r.attributes, "gamezonename")},
                         {"requiredquestid", integer(r.attributes, "requiredquestid", -1)}, {"gmonly", integer(r.attributes, "gmonly")}};
        std::istringstream center(get(r.attributes, "areacenter")); std::string part; std::vector<double> c;
        while (std::getline(center, part, ',')) c.push_back(std::stod(part));
        if (c.size() >= 3) {
            Trigger t; t.uid = uid; t.x = c[0]; t.y = c[2]; t.radius = number(r.attributes, "arearadius", 5); t.quest = integer(r.attributes, "requiredquestid", -1);
            // Zone: gamezonename first, otherwise the grid that has walkable support inside the original area radius.
            auto zone_name = lower(get(r.attributes, "gamezonename"));
            for (const auto& [override_uid, override_zone] : loc_zone_overrides) if (override_uid == uid) zone_name = override_zone;
            // A zone's sub-grids follow it ("Karakoy" locs inside Karaköy İç Mekan), then every other grid.
            std::vector<Grid*> candidates; if (grids.contains(zone_name)) candidates.push_back(&grids.at(zone_name));
            for (auto& [name, g] : grids) if (!zone_name.empty() && name != zone_name && name.starts_with(zone_name)) candidates.push_back(&g);
            for (auto& [name, g] : grids) if (std::find(candidates.begin(), candidates.end(), &g) == candidates.end()) candidates.push_back(&g);
            for (auto* g : candidates) {
                // Footprint: the smallest radius <= the original arearadius (20-50 units, an interaction distance that overlaps
                // neighbouring crystals) reaching walkable support. The door entity stands there; arrivals and NPCs keep clear of it.
                for (double range = std::min(t.radius, 4.0); range <= t.radius + 1e-9; range += 1.5) {
                    bool reachable = false;
                    for (int y = cell(t.y - range); y <= cell(t.y + range) && !reachable; ++y)
                        for (int x = cell(t.x - range); x <= cell(t.x + range) && !reachable; ++x)
                            reachable = g->block(x, y, 1) && dist(units(x), units(y), t.x, t.y) <= range;
                    if (reachable) { t.grid = g; t.range = range; break; }
                }
                if (t.grid) break;
            }
            entry["areacenter"] = {c[0], c[2]}; entry["arearadius"] = t.radius;
            if (t.grid) { entry["zone"] = t.grid->name; entry["at"] = {int32_t(std::lround(t.x * 1000)), int32_t(std::lround(t.y * 1000))}; triggers_by_uid[uid] = t; }
        }
        extract_locs.push_back(std::move(entry));
    }
    std::vector<Trigger> triggers; std::vector<std::pair<unsigned, unsigned>> edges;
    std::vector<Pair> pairs(std::begin(loc_pairs), std::end(loc_pairs)); for (const Pair& p : appended_pairs) pairs.push_back(p); // GCC 16 -O3 -Warray-bounds false positive on range insert
    for (const auto& p : pairs) {
        for (int direction = 0; direction < (p.both ? 2 : 1); ++direction) {
            const unsigned from = direction ? p.to : p.from, to = direction ? p.from : p.to;
            if (!triggers_by_uid.contains(from) || !triggers_by_uid.contains(to)) { std::cerr << "skipped unresolved loc pair " << from << "->" << to << '\n'; continue; }
            if (std::none_of(triggers.begin(), triggers.end(), [&](const Trigger& t) { return t.uid == from; })) triggers.push_back(triggers_by_uid.at(from));
            edges.push_back({from, to});
        }
    }
    // Arrival: nearest 3x3-walkable cell just outside the partner door's footprint, connected to it, outside every footprint.
    std::map<std::string, std::pair<int, int>> spawns{{"eminonu", {cell(386.0), cell(806.0)}}}; // Yeni Cami courtyard
    Ordered zones = Ordered::object();
    for (const auto& [name, registry] : zone_registry) zones[name] = {{"source_registry_id", registry}, {"bounds", {-ikv::coordinate_limit, -ikv::coordinate_limit, ikv::coordinate_limit - 1500, ikv::coordinate_limit - 1500}}, {"spawn", nullptr}, {"collision", Ordered::array()}};
    for (const auto& [name, first, count] : zone_instances) { Ordered rows = Ordered::array(); for (unsigned n = 0; n < count; ++n) rows.push_back(first + n); zones[name]["instances"] = std::move(rows); }
    std::vector<std::pair<unsigned, Ordered>> doors, late_doors, final_doors; // source loc uid -> door definition (placed as NPCs after the
                                                                              // town; late: narrow-arrival doors after the traps; final: appended_pairs, last)
    for (const auto& [from, to] : edges) {
        const auto& source = triggers_by_uid.at(from); const auto& target = triggers_by_uid.at(to); const Grid& g = *target.grid;
        int32_t partner = -1; double best = 1e9;
        for (int y = cell(target.y - target.range); y <= cell(target.y + target.range); ++y)
            for (int x = cell(target.x - target.range); x <= cell(target.x + target.range); ++x)
                if (g.walk(x, y) && dist(units(x), units(y), target.x, target.y) < best) { best = dist(units(x), units(y), target.x, target.y); partner = g.component(x, y); }
        auto arrive = ring_search(target.x, target.y, target.range + 3, target.range + 40, 0, [&](int x, int y) {
            return g.block(x, y, 1) && g.component(x, y) == partner && !in_triggers(triggers, g, units(x), units(y), 1.5); });
        // A narrow rail/ledge (Topkapı draisine 131516) has no 3x3 cell: fall back to one walkable connected cell.
        const bool narrow = !arrive;
        if (narrow) arrive = ring_search(target.x, target.y, target.range + 1.5, target.range + 40, 0, [&](int x, int y) {
            return g.walk(x, y) && g.component(x, y) == partner && !in_triggers(triggers, g, units(x), units(y), 0); });
        if (!arrive) { std::cerr << "skipped loc " << from << "->" << to << ": no arrival cell\n"; continue; }
        Ordered door = {{"to", g.name}, {"arrive", {wire(arrive->first), wire(arrive->second)}}, {"source_destination_uid", to}};
        if (source.quest >= 0) door["required_quest"] = source.quest; // classes.dat requiredquestid
        if (const int death = integer(locs.at(from), "requiredentitydeathid", -1); death > 0) door["required_death"] = death; // a classid
        if (integer(locs.at(from), "gmonly") == 1) door["gmonly"] = true;
        std::cout << "door " << from << ' ' << source.grid->name << " r" << source.range << " -> " << to << ' ' << g.name << '\n';
        for (const auto* list : {&doors, &late_doors, &final_doors})
            if (std::any_of(list->begin(), list->end(), [&](const auto& d) { return d.first == from; })) throw std::runtime_error("loc used as two doors: " + std::to_string(from));
        const bool appended = std::any_of(std::begin(appended_pairs), std::end(appended_pairs), [&](const Pair& p) { return p.from == from || (p.both && p.to == from); });
        (appended ? final_doors : narrow ? late_doors : doors).push_back({from, std::move(door)});
        // Zone spawn: the arrival with the largest connected walkable area (Eminönü keeps the Yeni Cami courtyard).
        if (!narrow && (!spawns.contains(g.name) || (g.name != "eminonu" && g.reach(*arrive) > g.reach(spawns.at(g.name))))) spawns[g.name] = *arrive;
    }
    for (auto& [name, g] : grids) if (!spawns.contains(name)) {
        // Zones without an inbound door (Er Meydanı arena): nearest open support to the grid centre.
        const auto open = ring_search(0, 0, 0, 1500, 0, [&](int x, int y) { return g.block(x, y, 2); });
        if (!open) throw std::runtime_error("zone without walkable support: " + name);
        spawns[name] = *open;
    }
    for (auto& [name, s] : spawns) {
        zones[name]["spawn"] = {wire(s.first), wire(s.second)};
        const auto& g = grids.at(name);
        std::cout << "spawn " << name << " [" << wire(s.first) << ',' << wire(s.second) << "] component cells " << g.reach(s) << '\n';
    }
    // Components players arrive on (the zone spawn and every door arrival): placed NPCs and spawn groups must stand on one.
    std::map<std::string, std::set<int32_t>> entries;
    for (const auto& [name, s] : spawns) entries[name].insert(grids.at(name).component(s.first, s.second));
    for (const auto* list : {&doors, &late_doors, &final_doors})
        for (const auto& [uid, door] : *list) { const auto& g = grids.at(door.at("to").get<std::string>()); entries[g.name].insert(g.component(door.at("arrive")[0].get<int>() / 1500 + 1024, door.at("arrive")[1].get<int>() / 1500 + 1024)); }

    // ---- prefixes (prefixes.dat, loader 0043aac0): server-side Efsun table; the client reads prefixes.dat itself ----
    // attreffects ids are character attribute ids (names: original 004338f0; Karakter Bilgisi getters 00402f60).
    Ordered prefix_table = Ordered::object();
    for (const auto& r : prefixes) {
        const auto& a = r.attributes; Ordered attributes = Ordered::array();
        std::istringstream effects(get(a, "attreffects")); std::string attribute, value;
        while (std::getline(effects, attribute, ';') && std::getline(effects, value, ';')) attributes.push_back({std::stoi(attribute), std::stod(value)});
        if (attributes.empty() || attributes.size() > 8 || attributes[0][0] == 4 || attributes[0][0] == 5) continue; // 4/5: potion prefixes below
        Ordered p = {{"name", get(a, "name")}, {"index", integer(a, "index")}, {"level", std::clamp(integer(a, "level", 1), 1, 100)},
                     {"rarity", integer(a, "rarity")}, {"flag", integer(a, "itemflag")}, {"attributes", std::move(attributes)}};
        if (integer(a, "classlimit", -1) >= 0 && integer(a, "classlimit", -1) <= 2) p["class"] = integer(a, "classlimit");
        prefix_table[get(a, "uniqueid")] = std::move(p);
    }
    // ---- items: weapons, armour, prefix potions (classes.dat objects / prefixes.dat) ----
    Ordered items = Ordered::object(); std::map<std::string, std::tuple<std::string, int, int>> item_kind; // id -> (kind, level, class)
    std::vector<std::pair<int, std::string>> droppable; // classes.dat rarity > 0: unprefixed base items that drop with random prefixes (VOD 34:00)
    // suitableslot: 2 gövde, 3 bacak, 4 el, 5 ayak (bodypiece slots of 0043fe40), 7 head rigid, 12 zırh overlay (0047a2d0 rigid dispatch).
    const std::map<int, const char*> armour_slots{{2, "body"}, {3, "legs"}, {4, "hands"}, {5, "feet"}, {7, "head"}, {12, "armor"}};
    const auto fits = [](const std::string& id, const Ordered& item) { return Ordered(id).dump().size() <= 34 && item.at("name").dump().size() <= 64; };
    for (const auto& r : classes) {
        if (r.tag != "object") continue;
        const auto& a = r.attributes; const auto type = get(a, "objecttype"); const int uid = integer(a, "uid");
        const int price = integer(a, "price"), level = std::clamp(integer(a, "itemlevel", 1), 1, 100), limit = integer(a, "classlimit", -1);
        // Tooltip 0x438378..0x438474: price is Lira, pvpprice Klan Parası ("Fiyat: %s Lira / %s Klan Parası"), akceprice the İKV
        // Dükkan Akçe price ("Fiyat: %s Akçe"); minloncalevel (classes loader 0043a50c -> item +0x38) the buy check's lonca level (00431700).
        const int clan_price = std::clamp(integer(a, "pvpprice"), 0, 999000000), akce_price = std::clamp(integer(a, "akceprice"), 0, 999000000);
        if (get(a, "name").empty() || (price < 1 && clan_price < 1 && akce_price < 1)) continue;
        Ordered item = {{"name", get(a, "name")}, {"stack", 1}, {"price", std::clamp(price, 0, 999000000)}, {"level", level}};
        if (clan_price > 0) item["clan_price"] = clan_price;
        if (akce_price > 0) item["akce_price"] = akce_price;
        if (integer(a, "minloncalevel") > 0) item["guild_level"] = std::clamp(integer(a, "minloncalevel"), 1, 100);
        const auto hundredths = [](double v) { return std::round(std::clamp(v, 0.0, 10000.0) * 100) / 100; };
        if (type.starts_with("weapon") && integer(a, "suitableslot") == 11) {
            std::string types; for (char t : get(a, "attacktype")) if (t == 'K' || t == 'D' || t == 'E') types += t; // Kesici/Delici/Ezici vs monster dmgred
            item.update({{"slot", "weapon"}, {"source_uid", uid}, {"source_slot", 11},
                         {"damage", clamp((number(a, "damagelow") + number(a, "damagehigh")) / 2, 1, 10000)},
                         {"damage_range", Ordered::array({hundredths(std::min(number(a, "damagelow"), number(a, "damagehigh"))), hundredths(number(a, "damagehigh"))})}, // 00402f60 Silah Hasarı inputs
                         {"attack_cooldown", clamp(number(a, "attackspeed") / 50, 1, 10000)},
                         {"attack_range", clamp(number(a, "attackrange") * 1000, 1000, 60000)},
                         {"crit", Ordered::array({clamp(number(a, "criticalchance"), 0, 100), clamp(number(a, "criticalmultiplier", 2), 1, 10)})}}); // tooltip "Kritik Vuruş Yüzdesi: %.1fx%.2f"
            if (!types.empty() && types.size() <= 3) item["attack_type"] = types;
        } else if (type == "defensive" && armour_slots.contains(integer(a, "suitableslot"))) {
            const int slot = integer(a, "suitableslot");
            item.update({{"slot", armour_slots.at(slot)}, {"source_uid", uid}, {"source_slot", slot},
                         {"armor", clamp(number(a, "dmgreduction") * 10000, 0, 10000)}}); // dmgreduction in basis points (00402f60 Zırh %)
            if (integer(a, "sexlimit", -1) == 0 || integer(a, "sexlimit", -1) == 1) item["sex"] = integer(a, "sexlimit");
        } else continue;
        if (limit >= 0 && limit <= 2) item["class"] = limit;
        else if (limit != -1) continue; // classlimit 3: NPC-only arsenal
        item["durability"] = clamp(number(a, "durability", 1000), 1, 10000); // tooltip 0x437590 "Eskime: [%.0f/%.0f]"
        Ordered fixed = Ordered::array();
        for (const char* key : {"prefix1", "prefix2"}) if (prefix_table.contains(get(a, key))) fixed.push_back(integer(a, key));
        if (!fixed.empty()) item["prefixes"] = std::move(fixed);
        const std::string id = "o" + std::to_string(uid);
        if (!fits(id, item)) continue;
        if (integer(a, "rarity") > 0 && !item.contains("prefixes")) droppable.push_back({level, id});
        // Akçe (İKV Dükkan) and lonca goods form their own NONCANONICAL stock lists instead of crowding the level-banded ones.
        const char* kind = akce_price > 0 ? "akce" : item.contains("guild_level") ? "lonca" : type == "defensive" ? "armour" : "weapon";
        item_kind[id] = {kind, level, limit}; items[id] = std::move(item);
    }
    for (const auto& r : prefixes) {
        // Potions are the İksir prototype (uid 14001, icon 108) with an index-3 prefix; attribute 4 heals, 5 restores Kudret.
        const auto& a = r.attributes; if (get(a, "index") != "3") continue;
        std::istringstream effects(get(a, "attreffects")); std::string attribute, value;
        if (!std::getline(effects, attribute, ';') || !std::getline(effects, value, ';') || (attribute != "4" && attribute != "5")) continue;
        const int level = std::clamp(integer(a, "level", 1), 1, 100);
        Ordered item = {{"name", get(a, "name") + " İksir"}, {"stack", 20}, {"price", 10 * (1 + level)}, {"level", level}, {"source_uid", 14001},
                        {attribute == "4" ? "heal" : "mana", clamp(std::stod(value), 1, 10000)}};
        const std::string id = "p" + get(a, "uniqueid");
        if (fits(id, item)) { item_kind[id] = {"potion", level, -1}; items[id] = std::move(item); }
    }
    // ---- Tılsım (classes.dat objecttype glyph, 179): the client only shows them ("[Tılsım]" 0x592318 in tooltip 00437590 via
    // glyph type 0x592908, then the detail text); how a Tılsım took effect was server-only. NONCANONICAL: a Tılsım carried in the
    // bag modifies its skill; the families below map their detail text onto damage/heal (%), attack (%), mana cost (−%), duration
    // (%) or cooldown (−seconds), the value being the detail's first number. NONCANONICAL riders for the remaining families (see
    // glyphed()/glyph() in src/world.cpp): `detail` picks a family's variant by its text, `field` names the boosted proc, `fixed`
    // values digit-less details, `scale` keeps decimals ("4.50 katı" -> 450) and `after` reads the number after that text.
    // Unpriced glyphs cost 100000.
    struct GlyphRule { const char* family; std::vector<const char*> skills; const char* kind; const char* detail = ""; const char* field = ""; int fixed = -1; int scale = 1; const char* after = ""; };
    const std::vector<GlyphRule> glyph_rules{
        {"Depar", {"depar"}, "cooldown"}, {"Ofansif Dövüşme", {"ofansif_dovus"}, "attack"}, {"Ofansif Dövüşme 2", {"sert_vurus", "agir_vurus"}, "damage"},
        {"Kışkırtma 2", {"kiskirtma"}, "mana"}, {"Durdurma 2", {"durdurma"}, "duration"}, {"Kanatma", {"boz_ayi"}, "damage"},
        {"Savaş Narası", {"savas_narasi"}, "mana"}, {"Ağır Vuruş", {"agir_vurus"}, "cooldown"}, {"Süpürme Saldırısı 1", {"supurme_vurusu"}, "damage"},
        {"Süpürme Saldırısı 2", {"supurme_vurusu"}, "cooldown"}, {"Meteorit 1", {"meteorit"}, "cooldown"}, {"Meditasyon 2", {"meditasyon_buyucu", "meditasyon_sifaci"}, "duration"},
        {"Fiziksel Bilgi", {"meteorit"}, "damage"}, {"Buz Oku 2", {"buz_oku"}, "duration"}, {"Buz Bilgisi", {"buz_oku", "kutup_ruzgari"}, "damage"},
        {"Ateş Çemberi 2", {"ates_cemberi"}, "cooldown"}, {"Ateş Bilgisi", {"ates_cemberi"}, "damage"}, {"Elektrik Bilgisi", {"yildirim", "tesla_kuresi"}, "damage"},
        {"Tesla Küresi", {"tesla_kuresi"}, "damage"}, {"İyileştirme", {"iyilestirme"}, "cooldown"}, {"Şifa Bilgisi", {"iyilestirme", "iyilestirme_cemberi"}, "damage"},
        {"Asit Saldırısı 2", {"asit_saldirisi"}, "mana"}, {"Asit Bilgisi", {"asit_saldirisi"}, "damage"}, {"Fiziksel Direnç Alanı", {"fiziksel_direnc_alani"}, "duration"},
        {"Element Direnç Alanı", {"element_direnc_alani"}, "duration"}, {"Ruh Kalkanı", {"ruh_kalkani"}, "attack"}, {"Zehirleme 1", {"zehirleme"}, "damage"},
        {"Zehirleme 2", {"zehirleme"}, "mana"}, {"Çağrı", {"cagri"}, "damage"}, {"Can Kurtaran", {"can_kurtaran"}, "cooldown"},
        {"İyileştirme Çemberi 2", {"iyilestirme_cemberi"}, "duration"},
        // Cast-definition riders (glyphed()).
        {"Sert Vuruş 1", {"sert_vurus"}, "proc", "", "free"}, {"Durdurma 1", {"durdurma"}, "proc", "", "silence"}, {"Zihin Toplama", {"zihin_toplama"}, "proc", "", "cleanse_all"},
        {"Büyü Bozma", {"buyu_bozma_buyucu"}, "proc", "Savaşçı ve şifacı tarafından", "cleanse_all"}, {"Büyü Bozma", {"buyu_bozma_sifaci"}, "proc", "Büyücü ve savaşçının", "cleanse_all"},
        {"Ateş Çemberi 1", {"ates_cemberi"}, "proc", "", "multiply", -1, 1, "%%"}, {"Dikkat Dağıtma", {"dikkat_dagitma"}, "slow"}, {"Buz Oku 3", {"buz_oku"}, "slow"},
        {"Direnç Kırma Alanı", {"direnc_kirma_alani"}, "vulnerable", "dirençlerini"}, {"Direnç Kırma Alanı", {"direnc_kirma_alani"}, "duration", "süresini"},
        {"Kışkırtma 1", {"kiskirtma"}, "defense_break"}, {"Buz Oku 1", {"buz_oku"}, "crit_duration"}, {"Meteorit", {"meteorit"}, "crit_seconds"},
        {"Meteorit 2", {"meteorit"}, "crit_armor_break"}, {"Zehirleme", {"zehirleme"}, "hit_seconds"}, {"Konsantrasyon 1", {"konsantrasyon"}, "speed"},
        {"Defansif Dövüşme 2", {"supurme_vurusu"}, "mana", "Süpürme", "", 100}, {"Kutup Rüzgarı", {"kutup_ruzgari"}, "self_stun", "", "", 50},
        {"Yıldırım 2", {"yildirim"}, "chain", "", "", 150}, {"Sert Vuruş 2", {"sert_vurus"}, "silenced_damage"}, {"Ofansif Dövüşme 1", {"agir_vurus"}, "crit_refund"},
        {"Yıldırım 1", {"yildirim"}, "electric_damage", "", "", -1, 1, "%%"}, {"Asit Saldırısı 1", {"asit_saldirisi"}, "heal_cut"},
        {"İyileştirme Çemberi 1", {"iyilestirme"}, "circle_heal"}, {"Gazap", {"iyilestirme", "iyilestirme_cemberi", "can_kurtaran", "can_verme"}, "wrath_heal"},
        {"Hedef Saptırma", {"hedef_saptirma"}, "redirect"},
        // Character-state riders read while the skill's own buff is active (glyph()).
        {"Defansif Dövüşme 1", {"defansif_dovusme"}, "armor_energy", "katı", "", -1, 100}, {"Defansif Dövüşme 1", {"defansif_dovusme"}, "armor_dodge", "üçte biri", "", 33},
        {"Defansif Dövüşme 2", {"defansif_dovusme"}, "stance_damage", "Defansif dövüşmede", "", 0}, {"Sakınma", {"sakinma"}, "crit_armor"},
        {"Konsantrasyon", {"konsantrasyon"}, "stance_crit"}, {"Meditasyon 1", {"meditasyon_buyucu", "meditasyon_sifaci"}, "stance_crit"},
        {"Gazap 2", {"gazap"}, "stance_crit"}, {"Gazap 1", {"gazap"}, "efsun_boost"}};
    std::vector<std::string> later_glyphs; // later tiers (with components); those without a recipe scroll drop from Şefs
    for (const auto& r : classes) {
        const auto& a = r.attributes;
        if (r.tag != "object" || get(a, "objecttype") != "glyph" || get(a, "name").empty()) continue;
        const auto name = get(a, "name"), detail = get(a, "detail");
        const std::string_view lead = "Tılsım - "; const auto tier = name.rfind(" (");
        const auto start = name.starts_with(lead) ? lead.size() : 0; const auto family = name.substr(start, tier == std::string::npos || tier < start ? std::string::npos : tier - start);
        Ordered item = {{"name", name}, {"stack", 1}, {"price", integer(a, "price") > 0 ? std::clamp(integer(a, "price"), 1, 999000000) : 100000}, {"level", 1},
                        {"source_uid", integer(a, "uid")}, {"detail", detail}};
        if (integer(a, "classlimit", -1) >= 0 && integer(a, "classlimit", -1) <= 2) item["class"] = integer(a, "classlimit");
        for (const auto& rule : glyph_rules) if (family == rule.family && detail.find(rule.detail) != std::string::npos) {
            const auto from = detail.find(rule.after), digit = from == std::string::npos ? from : detail.find_first_of("0123456789", from);
            if (digit == std::string::npos && rule.fixed < 0) continue;
            const int value = digit == std::string::npos ? rule.fixed : int(std::lround(std::stod(detail.substr(digit)) * rule.scale));
            Ordered skills = Ordered::array(); for (const char* s : rule.skills) if (config.at("skills").contains(s)) skills.push_back(s);
            if (skills.empty()) continue;
            item["glyph"] = {{"skills", std::move(skills)}, {"kind", rule.kind}, {"value", std::clamp(value, 0, 1000)}};
            if (*rule.field) item["glyph"]["field"] = rule.field;
        }
        const auto id = "o" + get(a, "uid");
        if (!fits(id, item)) continue;
        if (get(a, "components").empty()) item_kind[id] = {"glyph", 1, integer(a, "classlimit", -1)}; // first tiers are sold; later tiers are Lokman recipes or Şef drops
        else if (item.contains("glyph")) later_glyphs.push_back(id);
        items[id] = std::move(item);
    }
    // ---- Üretim: components (objecttype component) and recipe scrolls (objecttype recipe: product, crafttype, itemlevel) ----
    // A recipe's inputs are its product's components/amounts; crafttype indexes the profession table 0x5d6150 (Silahtar,
    // Zırhçı, Kimyacı, Madenci, Sarraf, Lokman). NONCANONICAL: component stack 20, scroll name "<product> Reçetesi".
    constexpr const char* professions[] = {"silahtar", "zirhci", "kimyaci", "madenci", "sarraf", "lokman"};
    std::map<int, const Attributes*> objects; for (const auto& r : classes) if (r.tag == "object") objects[integer(r.attributes, "uid")] = &r.attributes;
    const auto numbers = [](const std::string& s) { std::vector<int> v; std::istringstream in(s); std::string part; while (std::getline(in, part, ';')) if (!part.empty()) v.push_back(std::stoi(part)); return v; };
    for (const auto& [uid, a] : objects) if (get(*a, "objecttype") == "component" && !get(*a, "name").empty()) {
        Ordered item = {{"name", get(*a, "name")}, {"stack", 20}, {"price", std::clamp(integer(*a, "price"), 1, 999000000)}, {"level", std::clamp(integer(*a, "itemlevel", 1), 1, 100)}, {"source_uid", uid}}; // Teşkilat Bonosu 10m: 10000000
        if (const auto id = "o" + std::to_string(uid); fits(id, item)) items[id] = std::move(item);
    }
    Ordered recipes = Ordered::object(); std::map<int, std::vector<std::string>> recipe_scrolls; // crafttype -> scroll ids
    for (const auto& [uid, a] : objects) {
        if (get(*a, "objecttype") != "recipe") continue;
        const int craft = integer(*a, "crafttype", -1), product = integer(*a, "product");
        const auto product_id = "o" + std::to_string(product), id = "o" + std::to_string(uid);
        if (craft < 0 || craft > 5 || !objects.contains(product) || !items.contains(product_id)) continue; // products without a native item
        const auto inputs = numbers(get(*objects.at(product), "components")), amounts = numbers(get(*objects.at(product), "amounts"));
        bool ok = !inputs.empty() && inputs.size() == amounts.size() && inputs.size() <= 16; Ordered components = Ordered::array();
        for (size_t n = 0; ok && n < inputs.size(); ++n) { ok = items.contains("o" + std::to_string(inputs[n])) && amounts[n] > 0 && amounts[n] <= 1000; components.push_back({{"item", "o" + std::to_string(inputs[n])}, {"quantity", amounts[n]}}); }
        Ordered scroll = {{"name", items.at(product_id).at("name").get<std::string>() + " Reçetesi"}, {"stack", 1}, {"price", std::clamp(integer(*a, "price"), 1, 1000000)},
                          {"level", std::clamp(integer(*a, "itemlevel", 1), 1, 100)}, {"source_uid", uid}};
        if (!ok || !fits(id, scroll)) continue;
        items[id] = std::move(scroll); recipe_scrolls[craft].push_back(id);
        recipes[id] = {{"craft", professions[craft]}, {"level", std::max(0, integer(*a, "itemlevel"))}, {"product", product_id}, {"quantity", 1}, {"components", std::move(components)}};
    }
    // classes.dat itemflag bit 2 (unsellable/indestructible) and bit 4 (bound / Klan Parası goods), client 004311b0/00431220/004344b0:
    // every item built from a classes.dat object carries its itemflag when either bit is set.
    for (auto& entry : items.items()) if (entry.key()[0] == 'o')
        if (const auto o = objects.find(std::stoi(entry.key().substr(1))); o != objects.end() && (integer(*o->second, "itemflag") & 6)) entry.value()["flags"] = integer(*o->second, "itemflag");
    // NONCANONICAL drop tables (server data): up to six droppable base items nearest the monster level.
    const auto drops = [&](int level) {
        std::vector<std::tuple<int, int, std::string>> ranked;
        for (const auto& [item_level, id] : droppable) if (item_level <= level + 1) ranked.push_back({std::abs(item_level - level), item_level, id});
        std::sort(ranked.begin(), ranked.end()); Ordered list = Ordered::array();
        for (size_t n = 0; n < ranked.size() && n < 6; ++n) list.push_back(std::get<2>(ranked[n]));
        return list;
    };
    const auto stock = [&](const char* kind, int lo, int hi) {
        std::vector<std::pair<int, std::string>> found;
        for (const auto& [id, k] : item_kind) if (std::get<0>(k) == kind && std::get<1>(k) >= lo && std::get<1>(k) <= hi) found.push_back({std::get<1>(k), id});
        std::sort(found.begin(), found.end()); Ordered shop = Ordered::array();
        for (size_t n = 0; n < found.size() && n < 16; ++n) shop.push_back(found[n].second);
        return shop;
    };

    // ---- NPC and monster definitions ----
    std::map<std::string, int> clip_speed, walk_speed; // animation.dat cliplist classid -> run (state 1) else walk (state 2) forwardspeed; walk only
    for (const auto& list : animation) {
        double run = 0, walk = 0;
        for (const auto& clip : list.children) { const auto state = get(clip.attributes, "state"); if (state == "1" && !run) run = number(clip.attributes, "forwardspeed"); if (state == "2" && !walk) walk = number(clip.attributes, "forwardspeed"); }
        if (run || walk) clip_speed.emplace(get(list.attributes, "classid"), clamp((run ? run : walk) * 50, 1, 100000));
        if (walk) walk_speed.emplace(get(list.attributes, "classid"), clamp(walk * 50, 1, 100000));
    }
    std::map<unsigned, Attributes> actors; Ordered extract_actors = Ordered::array();
    // classes.dat monster combat stats in original units (World land_npc / NPC bites): attack/defense ratings, dmgmin-dmgmax,
    // criticalchance x criticalscale, dmgredslash/pierce/bludge per attack type, fire/cold/electric/acid/poison resistance points.
    const auto enemy = [&](const Attributes& a) {
        const int aggro = clamp(number(a, "triggerrange", 20) * 1000, 1000, 100000);
        const double low = std::clamp(number(a, "dmgmin"), 0.0, 100000.0), high = std::clamp(number(a, "dmgmax"), low, 100000.0);
        const auto fraction = [&](const char* key) { return std::clamp(number(a, key), 0.0, 1.0); };
        Ordered resist = Ordered::array(); for (const char* key : {"fireresist", "coldresist", "electricresist", "acidresist", "poisonresist"}) resist.push_back(std::clamp(number(a, key), 0.0, 1000.0));
        Ordered n{{"source_uid", integer(a, "uid")}, {"source_class", std::max(1, integer(a, "classid"))}, {"kind", "enemy"},
            {"template", "m" + get(a, "uid")}, {"name", get(a, "name")}, {"level", integer(a, "level", 1)},
            {"hp", clamp(number(a, "hp", 100), 1, 100000)}, {"damage", clamp((low + high) / 2, 1, 100000)}, {"damage_range", Ordered::array({low, high})},
            {"attack", std::clamp(number(a, "attack"), 0.0, 100000.0)}, {"defense", std::clamp(number(a, "defense"), 0.0, 100000.0)},
            {"crit", Ordered::array({std::clamp(number(a, "criticalchance"), 0.0, 100.0), std::clamp(number(a, "criticalscale", 2), 1.0, 20.0)})},
            {"dmgred", {{"K", fraction("dmgredslash")}, {"D", fraction("dmgredpierce")}, {"E", fraction("dmgredbludge")}}}, {"resist", std::move(resist)},
            {"attack_cooldown", clamp(number(a, "attackrate", 1000) / 50, 1, 100000)}, {"attack_range", clamp(number(a, "attackrange", 3) * 1000, 1000, 60000)},
            {"aggro", aggro}, {"leash", std::min(100000, aggro * 3)},
            {"speed", clip_speed.contains(get(a, "classid")) ? clip_speed.at(get(a, "classid")) : 110},
            {"respawn", 600}, {"xp", clamp(number(a, "experience", 1), 1, 100000)}, {"currency", clamp(number(a, "dropmoney", 1), 1, 100000)}};
        // Original behaviour fields no client code reads (server semantics NONCANONICAL): attackrangeremote (ranged strike reach,
        // World bites from max(attack_range, ranged_range)), canmove/canattack 0 (the banners), infinitefollower 1 (group-zone
        // mobs that never give up a chase: the maximum leash).
        if (number(a, "attackrangeremote") > 0) n["ranged_range"] = clamp(number(a, "attackrangeremote") * 1000, 1000, 200000);
        if (get(a, "canmove") == "0") n["immobile"] = true;
        if (get(a, "canattack") == "0") n["passive"] = true;
        if (get(a, "infinitefollower") == "1") { n["infinite_follow"] = true; n["leash"] = 100000; }
        return n;
    };
    for (const auto& r : classes) {
        if (r.tag != "npc" && r.tag != "monster") continue;
        actors[unsigned(integer(r.attributes, "uid"))] = r.attributes;
        Ordered entry = enemy(r.attributes); entry["tag"] = r.tag;
        for (const char* flag : {"talker", "merchant", "questGiver", "banker"}) entry[flag] = integer(r.attributes, flag) == 1;
        extract_actors.push_back(std::move(entry));
    }
    Ordered npcs = Ordered::array(); std::map<std::string, std::vector<Placed>> placed;
    // `apart`/`gap`: extra spacing from a second set of spots (Şefs keep their aggro discs apart).
    const auto place = [&](const std::string& zone, double rmin, double rmax, int block, double spacing, double margin, double angle0, std::optional<std::pair<double, double>> center = std::nullopt,
                           const std::vector<Placed>* apart = nullptr, double gap = 0) {
        const Grid& g = grids.at(zone); const auto [sx, sy] = spawns.at(zone); const auto home = g.component(sx, sy);
        const auto [cx, cy] = center.value_or(std::pair{units(sx), units(sy)});
        const auto clear = [&](const std::vector<Placed>& spots, double d, int x, int y) { return std::none_of(spots.begin(), spots.end(), [&](const Placed& p) { return dist(p.x, p.y, units(x), units(y)) < d; }); };
        const auto found = ring_search(cx, cy, rmin, rmax, angle0, [&](int x, int y) {
            return g.block(x, y, block) && g.component(x, y) == home && !in_triggers(triggers, g, units(x), units(y), margin) &&
                   clear(placed[zone], spacing, x, y) && (!apart || clear(*apart, gap, x, y)); });
        if (found) placed[zone].push_back({units(found->first), units(found->second)});
        return found;
    };
    // Models the client cannot assemble (original biped/binding mismatches) are left out of the population.
    ikv::Scene probe; std::map<unsigned, bool> assembles, dresses; // dresses: actor uid -> its rhweaponid/lhweaponid dress
    // classes.dat idlewalktend (server field, see World::idle_walk) replaces the NONCANONICAL patrol wherever the record has
    // one (0 = stands); walkers move at their animation.dat walk speed.
    const auto add_npc = [&](Ordered n, const std::string& zone, std::pair<int, int> at) {
        n["id"] = first_npc_id + uint32_t(npcs.size()); n["zone"] = zone; n["x"] = wire(at.first); n["y"] = wire(at.second);
        if (!n.contains("quests")) n["quests"] = Ordered::array();
        if (!n.contains("shop")) n["shop"] = Ordered::array();
        if (const auto a = n.contains("source_uid") ? actors.find(n.at("source_uid").get<unsigned>()) : actors.end(); a != actors.end() && a->second.contains("idlewalktend")) {
            const int tend = clamp(number(a->second, "idlewalktend"), 0, 100); const auto walk = walk_speed.find(get(a->second, "classid"));
            n["idlewalktend"] = walk != walk_speed.end() ? tend : 0; // no walk clip: stands
            if (walk != walk_speed.end() && tend) n["walk_speed"] = walk->second;
            if (n.contains("patrol")) n["patrol"] = Ordered::array();
        }
        Ordered ordered = {{"id", n.at("id")}}; ordered.update(n);
        // classes.dat rhweaponid/lhweaponid (the exe never reads them; server-sent equipment): the client's visible-equipment
        // shape, slot = the weapon object's suitableslot (11 right hand, 12 left hand).
        if (const auto a = actors.find(ordered.value("source_uid", 0u)); a != actors.end() && !ordered.contains("equipment")) {
            Ordered equipment = Ordered::array();
            for (const char* key : {"rhweaponid", "lhweaponid"})
                if (const auto o = objects.find(integer(a->second, key, -1)); o != objects.end()) equipment.push_back({{"slot", integer(*o->second, "suitableslot")}, {"source_uid", o->first}});
            // Equipment the client cannot dress on this skeleton (no original rigid anchor path) is left out, logged.
            std::vector<ikv::SourceEquipment> visual;
            for (const auto& item : equipment) visual.push_back({item.at("slot").get<unsigned>(), item.at("source_uid").get<unsigned>()});
            if (!visual.empty()) {
                auto [known, fresh] = dresses.try_emplace(a->first, true);
                if (fresh) try { ikv::attach_source_npc(probe, base, a->first, 2, visual); } catch (const std::exception& error) { known->second = false; std::cout << "excluded equipment of actor " << a->first << ": " << error.what() << '\n'; }
                if (fresh) ikv::detach_source_actor(probe, 2);
                if (known->second) ordered["equipment"] = std::move(equipment);
            }
        }
        npcs.push_back(std::move(ordered));
    };
    const auto renders = [&](unsigned uid) {
        if (const auto found = assembles.find(uid); found != assembles.end()) return found->second;
        bool ok = true;
        try { ikv::attach_source_npc(probe, base, uid, 1); } catch (const std::exception& error) { ok = false; std::cout << "excluded actor " << uid << ": " << error.what() << '\n'; }
        ikv::detach_source_actor(probe, 1);
        return assembles[uid] = ok;
    };
    // A tools/data/placements.json spot: the free cell nearest it (3x3 support, on an arrival component, `margin` off every door
    // footprint, `spacing` from the zone's earlier NPCs) within `reach` units.
    const auto place_at = [&](const std::string& zone, std::pair<double, double> c, double reach, double spacing, double margin) {
        const Grid& g = grids.at(zone);
        const auto found = ring_search(c.first, c.second, 0, reach, 0, [&](int x, int y) {
            return g.block(x, y, 1) && entries[zone].contains(g.component(x, y)) && !in_triggers(triggers, g, units(x), units(y), margin) &&
                   std::none_of(placed[zone].begin(), placed[zone].end(), [&](const Placed& p) { return dist(p.x, p.y, units(x), units(y)) < spacing; }); });
        if (found) placed[zone].push_back({units(found->first), units(found->second)});
        return found;
    };
    // An NPC's placements.json row (zone + spot) or, logged, the NONCANONICAL ring round the zone spawn in `zone`.
    std::set<unsigned> placed_uids; // placements.json rows used by the town/questgiver tables; the rest are appended at the end
    const auto place_npc = [&](unsigned uid, std::string zone) -> std::pair<std::string, std::optional<std::pair<int, int>>> {
        if (const auto* p = placement(uid)) {
            zone = p->at("zone").get<std::string>(); placed_uids.insert(uid);
            if (const auto at = place_at(zone, spot(*p), 30, 4, 3)) return {zone, at};
            std::cerr << "placement fallback " << uid << ": no free cell near its spot in " << zone << '\n';
        } else std::cerr << "placement fallback " << uid << ": no placements.json row\n";
        return {zone, place(zone, 14, 150, 1, 10, 10, 40.0 * double(placed[zone].size()))};
    };
    struct Town { const char* zone; unsigned uid; const char* role; const char* kind; int lo, hi; };
    // Roles follow the original merchant/banker/questGiver flags, the NPC trades and the guide/news (#11): İşportacı Genç is the
    // starting street vendor (guide l.1713), Kuyumcu Agop sells the Teşkilat Bonosu (NEWS-450), the Mısır Çarşısı tradesmen
    // (aktar Şevket, demirci Rüstem, terzi Fahri) stand at their marks, Demirci Dilek, Elif and Ayşe Teyze in Yeni Bab-ı Ali
    // (W-AGL lv30). NONCANONICAL: every stock list; the zone here is the fallback, placements.json places each NPC.
    constexpr Town town[] = {
        {"eminonu", 1001, "quest", "", 0, 0}, {"misir", 1002, "merchant", "jeweller", 0, 0},
        {"eminonu", 1012, "banker", "", 0, 0}, {"eminonu", 1017, "merchant", "weapon", 1, 5},
        {"meteor", 1057, "merchant", "potion", 26, 40}, {"eminonu", 1041, "merchant", "armour", 1, 4},
        {"eminonu", 1000, "talker", "", 0, 0},
        {"misir", 1003, "merchant", "potion", 1, 25}, {"meteor", 1025, "merchant", "weapon", 26, 35},
        {"misir", 1006, "merchant", "armour", 5, 12}, {"meteor", 1024, "merchant", "potion", 26, 55},
        {"topkapi", 131527, "merchant", "armour", 13, 20}, {"topkapi", 131561, "merchant", "weapon", 16, 30},
        {"karakoy", 131810, "merchant", "armour", 21, 30},
        // Klan: Arz/Lodos Muhafızı (claninviter) guard their clan's Büyük Hol gate signs in Meteor Bölgesi (<loc> 131839
        // ID_ARZ_TABELA, 131840 ID_LODOS_TABELA); Tüccar (Arz/Lodos) stand at the Maden Ocağı hub of their half of Büyük Hol
        // (zone12.dds: Arz exit 131842 in the north-west half, Lodos exit 131841 in the south-east one). NONCANONICAL spots.
        {"meteor", 1033, "recruiter", "", 1, 0}, {"meteor", 1034, "recruiter", "", 2, 0},
        {"buyukhol", 1058, "clanshop", "Arzın Çocukları", 1, 0}, {"buyukhol", 1046, "clanshop", "Lodos", 2, 0}};
    // Guide "Klan OOK'leri": clan NPCs attack the other clan's players and serve no one while dead. They keep their classes.dat
    // combat stats (enemy()) beside their talk/trade kind; claninviter (entity flag 0x20000) marks the Klana Katıl recruiters.
    // NONCANONICAL: `reach` caps the classes.dat triggerrange (the shared Meteor gate puts both recruiters 40 units apart).
    const auto clan_guard = [&](Ordered& n, const Attributes& a, int clan, int reach) {
        const Ordered stats = enemy(a);
        for (const auto& field : stats.items()) if (field.key() != "kind" && field.key() != "template" && field.key() != "name" && field.key() != "source_uid" && field.key() != "source_class") n[field.key()] = field.value();
        n["clan"] = clan; n["aggro"] = std::min(reach, n.at("aggro").get<int>()); n["leash"] = std::min(100000, n.at("aggro").get<int>() * 3);
        n["patrol"] = Ordered::array(); n["loot"] = Ordered::array();
        if (integer(a, "claninviter") == 1) n["recruiter"] = true;
    };
    for (const auto& t : town) {
        if (!renders(t.uid)) throw std::runtime_error("town NPC model does not assemble: " + std::to_string(t.uid));
        const auto& a = actors.at(t.uid); Ordered n = {{"source_uid", t.uid}, {"source_class", std::max(1, integer(a, "classid"))}, {"name", get(a, "name")}, {"hp", 100}};
        const std::string role = t.role;
        if (role == "merchant") {
            n["kind"] = "merchant"; n["dialogue"] = "Buyurun, mallarıma bir göz atın."; n["shop"] = stock(t.kind, t.lo, t.hi);
            // NONCANONICAL: the Eminönü vendor also sells the first potions, Halime Teyze the Akçe (İKV Dükkan) goods, the Topkapı
            // terzi the lonca goods, the Topkapı silahçı the first-tier Tılsım (all of each, by id); the Kuyumcu the two bonds.
            if (t.uid == 1017) for (const auto& id : stock("potion", 1, 5)) n["shop"].push_back(id);
            if (t.uid == 1002) { n["dialogue"] = "Kuyumcu Agop'a hoş geldiniz. Teşkilat Bonosu da bende bulunur."; for (const char* id : {"o13616", "o13615"}) if (items.contains(id)) n["shop"].push_back(id); }
            if (const char* extra = t.uid == 1041 ? "akce" : t.uid == 131527 ? "lonca" : t.uid == 131561 ? "glyph" : nullptr)
                for (const auto& [id, k] : item_kind) if (std::get<0>(k) == extra) n["shop"].push_back(id);
        }
        else if (role == "quest") { n["kind"] = "questgiver"; n["dialogue"] = "Hoş geldin evladım. Teşkilat'ın senin gibi gençlere ihtiyacı var."; } // NONCANONICAL greeting; quests: tools/data/quests_original.json
        else if (role == "banker") { n["kind"] = "questgiver"; n["banker"] = true; n["dialogue"] = "Eşyalarınızı ve paranızı kasanızda saklayabilirim. (İşlemler → Kasamı göster)"; }
        else if (role == "recruiter") { n["kind"] = "questgiver"; n["dialogue"] = "Klanımıza katılmak istersen İşlemler menüsünden Klana Katıl'ı seç."; clan_guard(n, a, t.lo, 15000); }
        else if (role == "clanshop") {
            // Clan goods: classes.dat objects with a pvpprice (Klan Parası) named after the clan, lowest level first.
            std::vector<std::pair<int, std::string>> goods;
            for (const auto& [uid, o] : objects) if (number(*o, "pvpprice") > 0 && get(*o, "name").starts_with(t.kind) && items.contains("o" + std::to_string(uid))) goods.push_back({integer(*o, "itemlevel"), "o" + std::to_string(uid)});
            std::sort(goods.begin(), goods.end()); Ordered shop = Ordered::array();
            for (size_t k = 0; k < goods.size() && k < 16; ++k) shop.push_back(goods[k].second);
            n["kind"] = "merchant"; n["dialogue"] = "Klan mallarım Klan Parası ile satılır."; n["shop"] = std::move(shop); clan_guard(n, a, t.lo, 100000);
        }
        else { n["kind"] = "questgiver"; n["dialogue"] = "Hoş geldin yolcu."; }
        const auto [zone, at] = place_npc(t.uid, t.zone);
        if (!at) throw std::runtime_error("no free walkable placement for NPC " + std::to_string(t.uid) + " in " + zone);
        add_npc(std::move(n), zone, *at);
    }
    // Short straight patrol (every sample keeps 3x3 support, so the world's segment trace stays on support).
    const auto patrol_from = [](const Grid& g, std::pair<int, int> at, int member) {
        Ordered patrol = Ordered::array();
        for (int direction = 0; direction < 8 && patrol.empty(); ++direction) {
            const double d = (45.0 * direction + 90 * member) * M_PI / 180; bool clear = true;
            for (double s = 0; s <= 6 && clear; s += 0.5) clear = g.block(cell(units(at.first) + s * std::cos(d)), cell(units(at.second) + s * std::sin(d)), 1);
            if (clear) patrol = {{wire(at.first), wire(at.second)}, {wire(cell(units(at.first) + 6 * std::cos(d))), wire(cell(units(at.second) + 6 * std::sin(d)))}};
        }
        return patrol;
    };
    // ---- monster spawn groups (placements.json "spawns", replacing the former 3x3 level bands): family uid, zone, centre, radius
    // (units), count, respawn (seconds) and provenance. Members spread over the disc on a Vogel spiral, each on the free cell
    // nearest its spiral point (5 units apart, 10 units off door footprints).
    std::map<std::string, std::vector<std::pair<Ordered, std::pair<int, int>>>> members; // zone -> members in group order
    std::vector<std::tuple<std::string, Ordered, std::pair<int, int>>> appended; // "append" groups: after every other NPC (stable ids)
    for (const auto& group : placements.at("spawns")) {
        const unsigned uid = group.at("family"); const std::string zone = group.at("zone");
        if (!actors.contains(uid) || !renders(uid)) throw std::runtime_error("spawn group family does not assemble: " + std::to_string(uid));
        const auto [cx, cy] = spot(group); const double radius = group.at("radius"); const int count = group.at("count"); int done = 0;
        for (int i = 0; i < count; ++i) {
            const double r = radius * std::sqrt((i + 0.5) / count), angle = i * 2.399963229728653; // golden angle
            const auto at = place_at(zone, {cx + r * std::cos(angle), cy + r * std::sin(angle)}, radius + 10, 5, 10);
            if (!at) continue;
            Ordered n = enemy(actors.at(uid)); n["loot"] = Ordered::array(); n["drops"] = drops(n.at("level").get<int>());
            n["respawn"] = std::clamp(group.at("respawn").get<int>() * 20, 20, 72000); n["patrol"] = patrol_from(grids.at(zone), *at, i);
            // "rescue": trapped quest NPCs (Kurtarıcımız: Biçare Folklor Cini, classes.dat interactable, attack 0) are freed by Kullan
            // (World::quest_use), never fought; a freed one leaves until its respawn. NONCANONICAL dialogue.
            if (group.contains("rescue")) n = {{"source_uid", uid}, {"source_class", n.at("source_class")}, {"name", n.at("name")}, {"hp", 1}, {"kind", "prop"},
                {"interactable", true}, {"rescue", true}, {"respawn", n.at("respawn")}, {"dialogue", "Tuzağa yakalanmış bir " + n.at("name").get<std::string>() + "."}};
            if (group.value("append", false)) appended.push_back({zone, std::move(n), *at}); else members[zone].push_back({std::move(n), *at});
            ++done;
        }
        if (done < count) std::cerr << "spawn group " << uid << " in " << zone << ": placed " << done << " of " << count << '\n';
    }
    // A legacy slot takes the zone's first member of the family that id had in the input config, when the zone still spawns that
    // family, so the id keeps its source_uid across regenerations; otherwise the zone's next member in group order.
    std::map<uint32_t, unsigned> previous_uid;
    for (const auto& n : config.at("npcs")) if (n.contains("source_uid")) previous_uid[n.at("id").get<uint32_t>()] = n.at("source_uid").get<unsigned>();
    for (const char* zone : legacy_enemy_zones) {
        auto& list = members[zone];
        if (list.size() < legacy_enemies_per_zone) throw std::runtime_error(std::string("fewer than 9 spawn group members in ") + zone);
        for (size_t k = 0; k < legacy_enemies_per_zone; ++k) {
            const auto old = previous_uid.find(first_npc_id + uint32_t(npcs.size()));
            auto pick = list.begin() + k;
            if (old != previous_uid.end())
                if (const auto same = std::find_if(pick, list.end(), [&](const auto& m) { return m.first.at("source_uid").template get<unsigned>() == old->second; }); same != list.end()) pick = same;
            std::rotate(list.begin() + k, pick, pick + 1); // move the pick to slot k, keeping the others in group order
            add_npc(list[k].first, zone, list[k].second);
        }
    }
    // ---- Toplama nodes: classes.dat <loc> locationtypename maden* (ID_MADEN_TAS ore, ID_MADEN_ELMAS gem, ID_CICEK herb) ----
    // Each yields the rarity-500 raw component its type spells (madenBakir1 -> Bakır); that component's crafttype is the
    // gathering profession and its itemlevel the required points (VOD 06:30: Ökse Otu itemlevel 7 = "7 seviye Lokman").
    // NONCANONICAL: 30 s regrowth and the rare pair (next uid, same crafttype/itemlevel, rarity 0, e.g. Saf Bakır).
    const auto fold = [](const std::string& s) {
        static const std::pair<const char*, char> turkish[] = {{"ı", 'i'}, {"İ", 'i'}, {"ş", 's'}, {"Ş", 's'}, {"ğ", 'g'}, {"Ğ", 'g'}, {"ç", 'c'}, {"Ç", 'c'}, {"ö", 'o'}, {"Ö", 'o'}, {"ü", 'u'}, {"Ü", 'u'}};
        std::string out;
        for (size_t i = 0; i < s.size();) {
            const auto hit = std::find_if(std::begin(turkish), std::end(turkish), [&](const auto& t) { return s.compare(i, 2, t.first) == 0; });
            if (hit != std::end(turkish)) { out += hit->second; i += 2; continue; }
            if (std::isalpha(static_cast<unsigned char>(s[i]))) out += char(std::tolower(static_cast<unsigned char>(s[i])));
            ++i;
        }
        return out;
    };
    // The raw component a maden*/gather_* type yields: rarity 500 named after the type, else (Çamur, Yeraltı Mantarı and the Büyük
    // Hol gather_* minerals have no rarity-500 entry) the component whose folded name starts with the type's.
    const auto raw_component = [&](const std::string& type, bool exact) -> std::pair<int, const Attributes*> {
        const auto key = fold(type.substr(type.starts_with("gather_") ? 7 : 5)); const Attributes* raw = nullptr; int raw_uid = 0;
        for (const auto& [cuid, c] : objects)
            if (get(*c, "objecttype") == "component" && (exact ? fold(get(*c, "name")).starts_with(key) : integer(*c, "rarity") == 500 && fold(get(*c, "name")).find(key) != std::string::npos) &&
                (!raw || get(*c, "name").size() < get(*raw, "name").size())) { raw = c; raw_uid = cuid; }
        return {raw_uid, raw};
    };
    const auto gather_node = [&](unsigned uid, const Attributes& a, int raw_uid, const Attributes& raw) {
        Ordered gather = {{"craft", professions[integer(raw, "crafttype")]}, {"level", integer(raw, "itemlevel")}, {"item", "o" + std::to_string(raw_uid)}, {"respawn", 600}};
        if (const auto pair = objects.find(raw_uid + 1); pair != objects.end() && get(*pair->second, "objecttype") == "component" && integer(*pair->second, "rarity") == 0 &&
            get(*pair->second, "crafttype") == get(raw, "crafttype") && get(*pair->second, "itemlevel") == get(raw, "itemlevel") && get(*pair->second, "components").empty() &&
            items.contains("o" + std::to_string(raw_uid + 1))) gather["rare"] = "o" + std::to_string(raw_uid + 1);
        return Ordered{{"source_uid", uid}, {"source_class", std::max(1, integer(a, "classid"))}, {"name", get(raw, "name")}, {"hp", 100}, {"kind", "node"},
                       {"dialogue", get(a, "detail").empty() ? get(raw, "name") : get(a, "detail")}, {"gather", std::move(gather)}};
    };
    std::vector<unsigned> late_nodes; // skipped here, retried after every other NPC so earlier NPC ids stay stable
    for (const auto& [uid, a] : locs) {
        const auto type = get(a, "locationtypename");
        if (!type.starts_with("maden") || !triggers_by_uid.contains(uid)) continue;
        const auto [raw_uid, raw] = raw_component(type, false);
        const auto& t = triggers_by_uid.at(uid); const Grid& g = *t.grid;
        const auto at = ring_search(t.x, t.y, 0, 6, 0, [&](int x, int y) { return g.walk(x, y); });
        if (!raw || integer(*raw, "crafttype") < 3 || integer(*raw, "crafttype") > 5 || (t.x == 0 && t.y == 0) || !at || !items.contains("o" + std::to_string(raw_uid)) || !renders(uid)) {
            late_nodes.push_back(uid); continue;
        }
        add_npc(gather_node(uid, a, raw_uid, *raw), g.name, *at);
    }
    // ---- örs: classes.dat <loc> locationtypename orseminonu (loc\id_ors.ADF, detail "Örs"). helps.xml Para: right-clicking one
    // repairs the worn-down equipment. Position: the loc areacenter's nearest walkable cell (the model stands at the centre).
    for (const auto& [uid, a] : locs) {
        if (get(a, "locationtypename") != "orseminonu" || !triggers_by_uid.contains(uid)) continue;
        const auto& t = triggers_by_uid.at(uid); const Grid& g = *t.grid;
        const auto at = ring_search(t.x, t.y, 0, 6, 0, [&](int x, int y) { return g.walk(x, y); });
        if (!at || !renders(uid)) { std::cerr << "skipped anvil " << uid << '\n'; continue; }
        add_npc({{"source_uid", uid}, {"source_class", std::max(1, integer(a, "classid"))}, {"name", "Örs"}, {"hp", 100}, {"kind", "anvil"},
                 {"dialogue", "Eşyalarınızı örs ile tamir edebilirsiniz."}}, g.name, *at);
    }
    // ---- doors: each paired <loc> (door, Ruh Taşı crystal, Hidra Engel barrier, elevator, boat) is a visible entity of its own
    // classes.dat model; clicking it (cursor_interact) moves the user to door.arrive. It stands on the walkable cell nearest the
    // areacenter inside its footprint (models have no recorded rotation and keep the default heading).
    const auto place_door = [&](unsigned uid, Ordered& door) {
        const auto& t = triggers_by_uid.at(uid); const Grid& g = *t.grid; const auto& a = locs.at(uid);
        const auto at = ring_search(t.x, t.y, 0, t.range, 0, [&](int x, int y) { return g.block(x, y, 1); });
        if (!at) { std::cerr << "skipped door " << uid << ": no walkable footprint cell\n"; return; }
        if (!renders(uid)) std::cerr << "door " << uid << " model does not assemble (label only)\n";
        add_npc({{"source_uid", uid}, {"source_class", std::max(1, integer(a, "classid"))}, {"name", get(a, "name")}, {"hp", 100}, {"kind", "door"},
                 {"dialogue", get(a, "detail").empty() ? get(a, "name") : get(a, "detail")}, {"door", std::move(door)}}, g.name, *at);
    };
    for (auto& [uid, door] : doors) place_door(uid, door);

    // ---- questgivers (rows 7, 18): every interactable classes.dat questGiver NPC the town table does not place, at its
    // placements.json spot (the zones here are the fallback and match it); interactable=0 questGivers (fans, robots, the
    // crystal, the clock) cannot be talked to in the original. GB-ANI (exe 0x592f88 "Mısır Çarşısındaki GB-Anı") re-offers
    // auto quests. The quests themselves are read from tools/data/quests_original.json after every entity is placed.
    constexpr std::pair<unsigned, const char*> quest_homes[] = {
        {1005, "misir"}, {1007, "eminonu"}, {1008, "eminonu"}, {1009, "eminonu"}, {1010, "eminonu"}, {1011, "eminonu"}, {1013, "eminonu"},
        {1018, "eminonu"}, {1019, "eminonu"}, {1020, "eminonu"}, {1021, "eminonu"}, {1022, "eminonu"}, {1023, "eminonu"}, {1035, "eminonu"},
        {1038, "eminonu"}, {1039, "eminonu"}, {1040, "eminonu"}, {1042, "eminonu"}, {1043, "eminonu"}, {1048, "eminonu"}, {1054, "meteor"},
        {1056, "ada"}, {1004, "misir"}, {1026, "yeralti"}, {1027, "meteor"}, {1028, "yeralti"}, {17112, "misir"},
        {1016, "yeralti"}, {17068, "ada"}, {17069, "yeralti"}, {17110, "yeralti"}, {17046, "eminonu"}, {17047, "eminonu"}, {17048, "meteor"},
        {1029, "eminonu"}, {1030, "eminonu"}, {131366, "meteor"}, {131342, "ada"}, {131490, "ada"}, {131833, "yeralti"}, {131835, "yeralti"},
        {131500, "migrat"}, {131108, "siginak"}, {131109, "migrat"}, {131112, "siginak"}, {131113, "siginak"}, {131114, "siginak"},
        {131115, "siginak"}, {131116, "siginak"}, {131117, "siginak"}, {131301, "cemberli"}, {131302, "cemberli"}, {131309, "cemberli"},
        {131310, "cemberli"}, {131311, "cemberli"}, {131312, "cemberli"}, {131313, "cemberli"}, {131314, "cemberli"}, {131315, "cemberli"},
        {131316, "cemberli"}, {131317, "cemberli"}, {131318, "cemberli"}, {131323, "cemberli"}, {1036, "meteor"}, {1045, "meteor"},
        {1053, "meteor"}, {1055, "ada"}, {1069, "yeralti"}, {1070, "topkapi"}, {1031, "buyukhol"}, {1032, "buyukhol"},
        {131811, "karakoy"}, {131812, "karakoy"}, {131813, "karakoy"}, {131814, "karakoy"}, {131830, "karakoy"}, {131877, "karakoy"},
        {131878, "karakoy"}, {131879, "karakoy"}, {131880, "karakoy"}, {131881, "karakoy"}, {131882, "karakoy"}, {131883, "karakoy"},
        {131884, "karakoy"}, {131885, "karakoy"}, {131886, "karakoy"}, {131888, "karakoy"}, {131889, "karakoy"}, {131890, "karakoy"},
        {131891, "karakoy"}, {131892, "karakoy"}, {131893, "karakoy"}, {131894, "karakoy"}, {131895, "karakoy"}, {132054, "karakoy"},
        {132055, "karakoy"}, {132056, "karakoy"}, {131896, "karakoyic"}, {131897, "karakoyic"}, {131898, "karakoyic"}, {131899, "karakoy"},
        {131995, "karakoyic"}, {132041, "karakoyic"}, {132042, "karakoyic"}, {132048, "karakoyic"}, {132075, "karakoyic"}, {132076, "karakoyic"},
        {132078, "karakoyic"}, {132081, "karakoyic"}, {132082, "karakoyic"}, {132085, "karakoyic"}, {132191, "karakoyic"}};
    std::map<unsigned, size_t> giver_index; // classes.dat uid -> npcs index
    for (size_t i = 0; i < npcs.size(); ++i) if (npcs[i].at("kind") == "questgiver" || npcs[i].at("kind") == "merchant") giver_index.emplace(npcs[i].at("source_uid").get<unsigned>(), i);
    for (const auto& [uid, home] : quest_homes) {
        if (giver_index.contains(uid)) continue;
        const auto& a = actors.at(uid);
        if (!renders(uid)) { std::cerr << "skipped questgiver " << uid << ": model does not assemble\n"; continue; }
        const auto [zone, at] = place_npc(uid, home);
        if (!at) { std::cerr << "skipped questgiver " << uid << ": no walkable placement in " << zone << '\n'; continue; }
        Ordered n = {{"source_uid", uid}, {"source_class", std::max(1, integer(a, "classid"))}, {"name", get(a, "name")}, {"hp", 100}, {"kind", "questgiver"},
                     {"dialogue", "Selam yolcu. Sana verecek bir işim var."}};
        if (uid == 17112) { n["auto_quests"] = true; n["dialogue"] = "Otomatik gelen görevlerini sildiysen onları yalnızca benden yeniden alabilirsin."; }
        // Demirci Rüstem (classes.dat merchant, demrc model) trades at the Mısır Çarşısı Demirci mark. NONCANONICAL stock.
        if (uid == 1004) { n["kind"] = "merchant"; n["shop"] = stock("weapon", 6, 15); n["dialogue"] = "Rüstem'in demircisine hoş geldin. Silahlarıma bir göz at."; }
        giver_index.emplace(uid, npcs.size()); add_npc(std::move(n), zone, *at);
    }
    for (const auto& [uid, a] : actors)
        if (integer(a, "questGiver") == 1 && integer(a, "interactable") == 1 && !giver_index.contains(uid) &&
            std::none_of(std::begin(quest_homes), std::end(quest_homes), [&](const auto& h) { return h.first == uid; }))
            throw std::runtime_error("questGiver without a home zone: " + std::to_string(uid));
    // ---- requiredentitydeathid guardians: the classid each gated door waits for (518 GBM X, 523 Semiha Hanım, 384 Gaffar Bey) stands
    // by the loc it guards. NONCANONICAL spots (spawns were server data): GBM X at the Yaşam Merkezi entrance 131358, Semiha Hanım at
    // the Şalter 131364 inside it, Gaffar Bey in Gaffar Oda 131491 (the Şalter's destination); no patrol.
    constexpr std::pair<int, unsigned> guardians[] = {{518, 131358}, {523, 131364}, {384, 131491}};
    for (const auto& [classid, anchor] : guardians) {
        const auto guardian = std::find_if(actors.begin(), actors.end(), [&](const auto& e) { return integer(e.second, "classid") == classid; });
        if (guardian == actors.end() || !triggers_by_uid.contains(anchor) || !renders(guardian->first)) { std::cerr << "skipped guardian " << classid << '\n'; continue; }
        const auto& t = triggers_by_uid.at(anchor); const Grid& g = *t.grid;
        int home = -1; for (int r = 0; r <= 40 && home < 0; ++r) if (const auto c = ring_search(t.x, t.y, r * 1.5, r * 1.5, 0, [&](int x, int y) { return g.walk(x, y); })) home = g.component(c->first, c->second);
        const auto at = ring_search(t.x, t.y, t.range + 6, t.range + 40, 0, [&](int x, int y) { return g.block(x, y, 1) && g.component(x, y) == home && !in_triggers(triggers, g, units(x), units(y), 3); });
        if (!at) { std::cerr << "skipped guardian " << classid << ": no free cell\n"; continue; }
        Ordered n = enemy(guardian->second); n["loot"] = Ordered::array(); n["drops"] = drops(n.at("level").get<int>()); n["patrol"] = Ordered::array();
        add_npc(std::move(n), g.name, *at);
    }
    // ---- traps: <loc> locationtypename damage1 (Labirent tuzak1/tuzak2 dummy markers) hold a Labirent Tuzağı (17109 ID_LAB_TUZAK,
    // not interactable): it strikes players within its attackrange at its attackrate. NONCANONICAL damage (server skill group 23).
    constexpr int trap_damage = 25;
    for (const auto& [uid, a] : locs) {
        if (get(a, "locationtypename") != "damage1" || !triggers_by_uid.contains(uid) || !actors.contains(17109)) continue;
        const auto& t = triggers_by_uid.at(uid); const Grid& g = *t.grid; const auto& trap = actors.at(17109);
        const auto at = ring_search(t.x, t.y, 0, 6, 0, [&](int x, int y) { return g.walk(x, y); });
        if (!at || !renders(17109)) { std::cerr << "skipped trap " << uid << '\n'; continue; }
        add_npc({{"source_uid", 17109}, {"source_class", std::max(1, integer(trap, "classid"))}, {"name", get(trap, "name")}, {"hp", 1}, {"kind", "trap"},
                 {"dialogue", get(trap, "name")}, {"attack", std::clamp(number(trap, "attack"), 0.0, 100000.0)},
                 {"trap", {{"range", clamp(number(trap, "attackrange", 3) * 1000, 1000, 200000)}, {"damage", trap_damage}, {"cooldown", clamp(number(trap, "attackrate", 1000) / 50, 1, 100000)}}}}, g.name, *at);
    }
    for (auto& [uid, door] : late_doors) place_door(uid, door);
    // ---- Şefs (guide "Grup bölgelerindeki bazı Şef yaratıkları ... önemli bazı eşyaları"): classes.dat bosses (experience >= 2000)
    // with their original models and stats at the <loc> quest triggers naming them: Büyük Bronz at questtriggerbronzx 30195 (Sivri
    // Ada), Yeşil Hidra at questtriggeryesilhidra 30258 (Yeraltı, behind the Hidra Engel barriers), Kara Kürk at karakurkini 131076
    // (Meteor). NONCANONICAL group-zone Şefs by the zones' level bands: Dev Ana and Dev Ana'nın Gözdesi at questtriggertangriss
    // 131157 (Migrat), Mağara Şamanı at the Aktarıcı 131246 (Sığınaklar); the Çemberlitaş guardians above are Şefs too. One hour
    // respawn, while a group-zone copy resets them whenever its last player leaves.
    // NONCANONICAL Şef skills: classes.dat normalskillgroup (a cast every 10 s) and onhitskillgroup (a rider on a bite that draws
    // blood, every 5 s at most) index server skill tables the client never loads. Each group id becomes the configured skill whose
    // effect fits the monsters sharing it: 1 snakes/spiders/scorpions and 18 Dev Komodo -> Zehirleme, 3 Boz Ayı/Kurt Alfa/Taş Kanat
    // claws -> Kanatma, 6 Tesla Küresi/Kulesi and 33 GBM 70/71 machines -> Yıldırım, 27 Buz Büyücüsü -> Buz Oku, 28/12/59 hydra,
    // cobra and worm spit -> Asit Saldırısı, 29/49 fire -> Ateş Çemberi, 9/32 shamans and B.B. apprentices -> Direnç Kırma Alanı,
    // 26/48 Meran curse and Semiha* -> Zihin Saldırısı, 13/44/58 giants and warriors -> Süpürme Vuruşu, the rest by name. `true`:
    // the cast strikes every player within attackrange + 10 units; damage max(classes.dat damage, 8 x level) as the earlier
    // slam; stun/silence effects last at most 3 s, others 10 s.
    constexpr std::tuple<int, const char*, bool> skill_groups[] = {
        {1, "zehirleme", false}, {3, "kanatma", false}, {6, "yildirim", false}, {7, "durdurma", false}, {8, "dikkat_dagitma", false},
        {9, "direnc_kirma_alani", true}, {11, "meteorit", false}, {12, "asit_saldirisi", false}, {13, "supurme_vurusu", true},
        {15, "agir_vurus", false}, {18, "zehirleme", false}, {26, "zihin_saldirisi", false}, {27, "buz_oku", false}, {28, "asit_saldirisi", false},
        {29, "ates_cemberi", true}, {30, "kutup_ruzgari", true}, {31, "dikkat_dagitma", false}, {32, "direnc_kirma_alani", true},
        {33, "yildirim", false}, {35, "durdurma", false}, {39, "agir_vurus", false}, {40, "kanatma", false}, {44, "supurme_vurusu", true},
        {46, "yildirim", true}, {47, "zehirleme", true}, {48, "zihin_saldirisi", true}, {49, "ates_cemberi", true}, {50, "meteorit", false},
        {57, "kutup_ruzgari", true}, {58, "supurme_vurusu", true}, {59, "asit_saldirisi", true}, {60, "yildirim", true}, {61, "agir_vurus", true}};
    constexpr std::pair<int, unsigned> chiefs[] = {{173, 30195}, {32, 30258}, {168, 131076}, {133, 131157}, {301, 131157}, {298, 131246}};
    const auto chief = [&](Ordered& n, const Attributes& a) {
        n["boss"] = true; n["respawn"] = 72000; n["patrol"] = Ordered::array();
        const int damage = std::max(n.at("damage").get<int>(), n.at("level").get<int>() * 8), radius = n.at("attack_range").get<int>() + 10000;
        Ordered list = Ordered::array();
        for (const auto& [key, on, cooldown] : {std::tuple{"normalskillgroup", "cast", 200}, std::tuple{"onhitskillgroup", "hit", 100}}) {
            const int group = integer(a, key, -1);
            const auto found = std::find_if(std::begin(skill_groups), std::end(skill_groups), [&](const auto& s) { return std::get<0>(s) == group; });
            if (found == std::end(skill_groups)) continue;
            const auto& [id, skill, area] = *found; const auto& definition = config.at("skills").at(skill);
            Ordered e = {{"group", id}, {"on", on}, {"skill", skill}, {"damage", damage}, {"radius", area && std::string_view(on) == "cast" ? radius : 0}, {"cooldown", cooldown}};
            if (definition.contains("duration")) e["duration"] = std::min(definition.at("duration").get<int>(), definition.contains("stun") || definition.contains("silence") ? 60 : 200);
            list.push_back(std::move(e));
        }
        if (!list.empty()) n["skills"] = std::move(list);
    };
    for (auto& n : npcs) for (const auto& [classid, anchor] : guardians) if (n.value("source_class", 0) == classid && n.at("kind") == "enemy") chief(n, actors.at(n.at("source_uid").get<unsigned>()));
    std::set<std::pair<int, int>> taken;
    // A free cell near a <loc> anchor on the walkable component nearest its centre.
    const auto anchored = [&](const Trigger& t) {
        const Grid& g = *t.grid;
        int home = -1; for (int r = 0; r <= 40 && home < 0; ++r) if (const auto c = ring_search(t.x, t.y, r * 1.5, r * 1.5, 0, [&](int x, int y) { return g.walk(x, y); })) home = g.component(c->first, c->second);
        const auto at = ring_search(t.x, t.y, 0, t.range + 40, 0, [&](int x, int y) { return g.block(x, y, 1) && g.component(x, y) == home && !taken.contains({x, y}) && !in_triggers(triggers, g, units(x), units(y), 3); });
        if (at) for (int dy = -4; dy <= 4; ++dy) for (int dx = -4; dx <= 4; ++dx) taken.insert({at->first + dx, at->second + dy});
        return at;
    };
    for (const auto& [classid, anchor] : chiefs) {
        const auto boss = std::find_if(actors.begin(), actors.end(), [&](const auto& e) { return integer(e.second, "classid") == classid; });
        if (boss == actors.end() || !triggers_by_uid.contains(anchor) || !renders(boss->first)) { std::cerr << "skipped chief " << classid << '\n'; continue; }
        const auto at = anchored(triggers_by_uid.at(anchor));
        if (!at) { std::cerr << "skipped chief " << classid << ": no free cell\n"; continue; }
        Ordered n = enemy(boss->second); n["loot"] = Ordered::array(); n["drops"] = drops(n.at("level").get<int>()); chief(n, boss->second);
        add_npc(std::move(n), triggers_by_uid.at(anchor).grid->name, *at);
    }
    // ---- Sancaklar (classes.dat npc clan="arz"/"lodos", loc\ID_ARZ_KALE_SANCAK / ID_LODOS_SANCAK): Arz/Lodos Sancağı 1-3 carry the
    // detail "Er Meydanı Arz/Lodos N" and stand in Er Meydanı; Arz/Lodos Sancağı A stand at the Karaköy <loc> 30203/30204
    // "Arzın Çocukları Sancağı"/"Lodos Sancağı" (guide: every PvP zone has both clans' banners; clan members resurrect there).
    // NONCANONICAL: the arena banner spots (Arz west, Lodos east of the savas spawn) and respawn times.
    const auto banner = [&](unsigned uid, int respawn) {
        Ordered n = enemy(actors.at(uid)); n["banner"] = true; n["clan"] = get(actors.at(uid), "clan") == "arz" ? 1 : 2;
        n["aggro"] = 1000; n["leash"] = 1000; n["respawn"] = respawn; n["xp"] = 1; n["loot"] = Ordered::array(); n["drops"] = Ordered::array(); n["patrol"] = Ordered::array();
        return n;
    };
    constexpr std::tuple<unsigned, double, double> arena_banners[] = {{1059, -300, -300}, {1060, -350, 0}, {1061, -300, 300}, {1062, 450, -300}, {1063, 500, 0}, {1064, 450, 300}};
    for (const auto& [uid, x, y] : arena_banners) {
        const auto at = actors.contains(uid) && renders(uid) ? place("savas", 0, 120, 1, 12, 3, 0, std::pair{x, y}) : std::nullopt;
        if (!at) { std::cerr << "skipped arena banner " << uid << '\n'; continue; }
        add_npc(banner(uid, 1200), "savas", *at);
    }
    for (const auto& [uid, anchor] : {std::pair{131498u, 30203u}, std::pair{131499u, 30204u}}) {
        if (!actors.contains(uid) || !triggers_by_uid.contains(anchor) || !renders(uid)) { std::cerr << "skipped clan banner " << uid << '\n'; continue; }
        const auto& t = triggers_by_uid.at(anchor); const Grid& g = *t.grid;
        const auto at = ring_search(t.x, t.y, 0, 30, 0, [&](int x, int y) { return g.block(x, y, 1); });
        if (!at) { std::cerr << "skipped clan banner " << uid << ": no free cell\n"; continue; }
        add_npc(banner(uid, 6000), g.name, *at);
    }
    // ---- the remaining classes.dat Şefs (experience >= 2000), appended so earlier NPC ids stay stable. NONCANONICAL placements: no
    // client data places them; each stands in the zone whose level band, monster family, quest trigger or classes.dat uid
    // neighbours fit its name and model, at that <loc> anchor or else on the zone's main walkable area around its spawn. Only a
    // spot players can walk to from the zone spawn or a door arrival is accepted.
    struct LateChief { unsigned uid; const char* zone; unsigned anchor; };
    constexpr LateChief late_chiefs[] = {
        {17083, "siginak", 0},        // Cevriye Sultan (ID_K_CETECI_BOSS, level 34): leads the Sığınaklar looters (Başıboş Yağmacı/Tüftüfçü)
        {17077, "migrat", 0},         // Şah Kobra (level 36): the temple's snakes and scorpions (Sarı Akrep)
        {17078, "meteor", 30190},     // Toprak Şamanı (ID_KURT_SAMAN): Kara Kürk's wolves, at the crater trigger questtriggerkrater
        {17055, "buyukhol", 0},       // Dev Komodo (level 46): Büyük Hol's Evcil Komodo
        {17066, "cemberli", 0},       // Yol Savaşçısı (ID_ZICAN_DIS, level 49): a Çemberlitaş boss (fandom Çemberlitaş loot list)
        {17071, "migrat", 131147},    // Junon (level 49): Migrat boss in "Junon'un hapishanesi" (fandom Migrat loot; W-AGL lv47), at the Kafes
        {17090, "yeralti", 30246},    // İfliss (ID_MERAN_IRI, the great Meran): questtriggermeranmeydan
        {17087, "yeralti", 30258},    // Kadim Hidra: the hydra lair of Yeşil Hidra (questtriggeryesilhidra)
        {131107, "siginak", 0},       // Kenan: classes.dat uid beside the Sığınaklar quest givers 131108-131117
        {131284, "cemberli", 131380}, // Stuart Efendi: uid beside GBM X 131285; questtriggerlaboratuargiris
        {131296, "cemberli", 131368}, // Semiha Hanım* (ID_SEMIHA_02): questtriggerkhaos
        {131876, "karakoyic", 0},     // Kraliçe Tırtıl (ID_WORMQUEEN, level 54): the Tünelci tunnels
        {131869, "karakoyic", 0},     // Büyük Büyücü (ID_GIZIT_BOSS): over the Gizit Milis
        {131867, "karakoyic", 0},     // GBZ (ID_GZ42, level 58)
        {131874, "karakoy", 0},       // Yüce Komiser (level 58)
        {131870, "karakoy", 132063}}; // General Hakir (level 59): on the zeppelin (Zeplin Çıkışı) with Zeplin Bombası (level 59)
    // `entries` (arrival components) is extended by every door arrival below.
    const auto arrival = [&](const Ordered& door) { // also kept clear of Şefs
        const auto& g = grids.at(door.at("to").get<std::string>()); const int x = door.at("arrive")[0].get<int>() / 1500 + 1024, y = door.at("arrive")[1].get<int>() / 1500 + 1024;
        entries[g.name].insert(g.component(x, y));
        for (int dy = -4; dy <= 4; ++dy) for (int dx = -4; dx <= 4; ++dx) taken.insert({x + dx, y + dy});
    };
    for (const auto& n : npcs) if (n.contains("door")) arrival(n.at("door")); // placed doors hold their (moved) definitions
    // Şef spots per zone: a new Şef stands 200 units (two maximum aggro radii) from every other, so a fight draws one Şef; a zone
    // too small for that (Sığınaklar) takes 130 units, then the plain spacing.
    std::map<std::string, std::vector<Placed>> chief_spots;
    for (const auto& n : npcs) if (n.value("boss", false)) {
        const Placed p{n.at("x").get<double>() / 1000, n.at("y").get<double>() / 1000};
        placed[n.at("zone").get<std::string>()].push_back(p); chief_spots[n.at("zone").get<std::string>()].push_back(p); // apart from earlier Şefs
    }
    for (const auto& [uid, door] : final_doors) arrival(door);
    for (const auto& [uid, zone, anchor] : late_chiefs) {
        if (!actors.contains(uid) || !renders(uid) || (anchor && !triggers_by_uid.contains(anchor))) { std::cerr << "skipped chief " << uid << '\n'; continue; }
        const Grid& g = grids.at(zone);
        if (anchor && triggers_by_uid.at(anchor).grid != &g) throw std::runtime_error("chief anchor outside its zone: " + std::to_string(uid));
        std::optional<std::pair<int, int>> at;
        if (anchor) at = anchored(triggers_by_uid.at(anchor));
        else for (const double gap : {200.0, 130.0, 0.0}) if ((at = place(zone, 160, 600, 2, 40, 25, 60.0 * double(placed[zone].size()), std::nullopt, &chief_spots[zone], gap))) break; // beyond aggro of the spawn
        if (!at || !entries[zone].contains(g.component(at->first, at->second))) { std::cerr << "skipped chief " << uid << ": no reachable free cell in " << zone << '\n'; continue; }
        Ordered n = enemy(actors.at(uid)); n["loot"] = Ordered::array(); n["drops"] = drops(n.at("level").get<int>()); chief(n, actors.at(uid));
        std::cout << "chief " << get(actors.at(uid), "name") << ' ' << zone << " [" << wire(at->first) << ',' << wire(at->second) << "] component cells " << g.reach(*at) << '\n';
        add_npc(std::move(n), zone, *at); chief_spots[zone].push_back({units(at->first), units(at->second)});
    }
    for (auto& [uid, door] : final_doors) place_door(uid, door);
    // ---- Clans: Er Meydanı guardians (guide "Karşı klandan oyuncu veya muhafızları öldürmek ... puan kazandırır"), appended last so
    // earlier NPC ids stay stable. NONCANONICAL: one copy of the clan's Muhafız (1033 Arz / 1034 Lodos, Klan OOK combat stats, no
    // recruiting) beside each arena banner; the instance loader clones them into savas2..savas20 with the banners.
    for (const auto& [uid, x, y] : arena_banners) {
        if (!actors.contains(uid) || !renders(uid)) continue;
        const int clan = get(actors.at(uid), "clan") == "arz" ? 1 : 2; const unsigned guardian = clan == 1 ? 1033 : 1034;
        const auto& a = actors.at(guardian);
        Ordered n = {{"source_uid", guardian}, {"source_class", std::max(1, integer(a, "classid"))}, {"name", get(a, "name")}, {"hp", 100}, {"kind", "questgiver"},
                     {"dialogue", "Er Meydanında klanımızın sancaklarını koruyorum."}};
        clan_guard(n, a, clan, 15000); n.erase("recruiter");
        const auto at = place("savas", 0, 120, 1, 12, 3, 0, std::pair{x, y});
        if (!at) { std::cerr << "skipped arena guardian by banner " << uid << '\n'; continue; }
        add_npc(std::move(n), "savas", *at);
    }
    // ---- the remaining placeable classes.dat traps and the caged Folklor Cini, appended last so earlier NPC ids stay stable.
    // NONCANONICAL spots from <loc> evidence (no client data places them): Tangriss Tuzak 131247 at questtriggertangriss 131157
    // (Migrat), Salon Tuzak 131497 at the Sığınaklar salon (Piano 131139 beside Gramofon 131137), Biçare Folklor Cini 17096
    // (ID_TUZAK_GOB, interactable, attack 0) inside the Migrat Kafes 131147 (ID_ZINDAN_KAFES). The traps keep their classes.dat
    // attackrange/attackrate (skill group 38 damage is server-only: the native trap_damage); the Cin only talks.
    for (const auto& [uid, anchor] : {std::pair{131247u, 131157u}, std::pair{131497u, 131139u}, std::pair{17096u, 131147u}}) {
        if (!actors.contains(uid) || !triggers_by_uid.contains(anchor) || !renders(uid)) { std::cerr << "skipped " << uid << '\n'; continue; }
        const auto& t = triggers_by_uid.at(anchor); const Grid& g = *t.grid; const auto& a = actors.at(uid);
        // Not on top of another NPC (Dev Ana shares the Tangriss trigger): at least 4 units from every NPC already in the zone.
        const auto free = [&](int x, int y) { return std::none_of(npcs.begin(), npcs.end(), [&](const Ordered& o) {
            return o.at("zone") == g.name && dist(o.at("x").get<double>() / 1000, o.at("y").get<double>() / 1000, units(x), units(y)) < 4; }); };
        const auto at = ring_search(t.x, t.y, 0, 12, 0, [&](int x, int y) { return g.walk(x, y) && free(x, y); });
        if (!at) { std::cerr << "skipped " << uid << ": no walkable cell at loc " << anchor << '\n'; continue; }
        Ordered n = {{"source_uid", uid}, {"source_class", std::max(1, integer(a, "classid"))}, {"name", get(a, "name")}, {"hp", 1}};
        if (number(a, "attack") > 0) {
            n["kind"] = "trap"; n["dialogue"] = get(a, "name"); n["attack"] = std::clamp(number(a, "attack"), 0.0, 100000.0);
            n["trap"] = {{"range", clamp(number(a, "attackrange", 3) * 1000, 1000, 200000)}, {"damage", trap_damage}, {"cooldown", clamp(number(a, "attackrate", 1000) / 50, 1, 100000)}};
        } else { n["kind"] = "questgiver"; n["dialogue"] = "Kurtar beni yolcu! Bu kafesten kendi başıma çıkamam."; }
        std::cout << "placed " << get(a, "name") << ' ' << g.name << " [" << wire(at->first) << ',' << wire(at->second) << "]\n";
        add_npc(std::move(n), g.name, *at);
    }
    // Otomat 131496 (ID_IKSIR_MAKINE "potion machine", level 50) and Leyla 131500 are the only classes.dat merchant=2 records,
    // read as the entity flag 0x400000 (+0x82 & 0x40) that 004344b0 refuses with "Bu hedef, eşya satın almaz." (0x591f54): they
    // sell but never buy. NONCANONICAL: the Otomat stands beside the Karaköy terzi 131810 with the level 26-55 potions.
    for (auto& n : npcs) if (const auto a = actors.find(n.at("source_uid").get<unsigned>()); a != actors.end() && integer(a->second, "merchant") == 2) n["no_buy"] = true;
    if (actors.contains(131496) && renders(131496)) {
        // Keep the potion machine outside every Karaköy monster's aggro disc (Bronz Akrep group bit buyers walking up to it).
        std::vector<Placed> monsters; double reach = 0;
        for (const auto& n : npcs) if (n.at("zone") == "karakoy" && n.at("kind") == "enemy") {
            monsters.push_back({n.at("x").get<double>() / 1000, n.at("y").get<double>() / 1000}); reach = std::max(reach, n.at("aggro").get<double>() / 1000);
        }
        std::optional<std::pair<int, int>> at;
        for (const auto& n : npcs) if (n.at("source_uid") == 131810) at = place(n.at("zone").get<std::string>(), 3, 160, 1, 4, 3, 0, std::pair{n.at("x").get<double>() / 1000, n.at("y").get<double>() / 1000}, &monsters, reach + 10);
        if (!at) std::cerr << "skipped Otomat 131496\n";
        else { const auto& a = actors.at(131496);
            add_npc({{"source_uid", 131496}, {"source_class", std::max(1, integer(a, "classid"))}, {"name", "Otomat"}, {"hp", 100}, {"kind", "merchant"},
                     {"dialogue", "İksir otomatı: bir iksir seçin."}, {"shop", stock("potion", 26, 55)}, {"no_buy", true}}, "karakoy", *at); }
    }
    // GBM-0 (locationtypename resetleyici) and GBM 0-B (uretimsifirlayici), loc\ID_RESET_MAKINE, appended last so earlier NPC ids stay
    // stable. Guide: right-clicking one resets yetenek/direnç (GBM-0) or üretim/toplama (GBM 0-B) points for a level-based price
    // (point_reset, NONCANONICAL), Plus Paket 1/3. Position: the loc trigger's nearest walkable cell. resetleyici2 (gamezonename meteor)
    // has no Meteor support near its centre but Büyük Hol support within its arearadius 2: it stands in that hall, like the trigger finds.
    for (const auto& [uid, a] : locs) {
        const auto type = get(a, "locationtypename");
        if ((type != "resetleyici" && type != "uretimsifirlayici") || !triggers_by_uid.contains(uid)) continue;
        const auto& t = triggers_by_uid.at(uid); const Grid& g = *t.grid;
        const auto at = ring_search(t.x, t.y, 0, 6, 0, [&](int x, int y) { return g.walk(x, y); });
        if (!at || !renders(uid)) { std::cerr << "skipped reset machine " << uid << '\n'; continue; }
        const bool skills = type == "resetleyici";
        add_npc({{"source_uid", uid}, {"source_class", std::max(1, integer(a, "classid"))}, {"name", get(a, "name")}, {"hp", 100}, {"kind", "reset"},
                 {"reset", skills ? "skills" : "crafts"},
                 {"dialogue", skills ? "Yetenek ve direnç puanlarınızı sıfırlar." : "Üretim ve toplama puanlarınızı sıfırlar."}}, g.name, *at);
    }
    // ---- appended so earlier NPC ids stay stable (#12) ----
    // 1. maden* nodes skipped above: the (0,0) per-type templates stand at their placements.json "nodes" spot (NONCANONICAL
    // spread, VOD: herb bushes in streets/parks/quay, ore beams on squares); Çamur/Yeraltı Mantarı yield their rarity-0
    // component; a centre with no support within 6 units takes the nearest arrival-component cell within 20.
    for (const unsigned uid : late_nodes) {
        const auto& a = locs.at(uid); const auto type = get(a, "locationtypename"); const auto& t = triggers_by_uid.at(uid);
        auto [raw_uid, raw] = raw_component(type, false); if (!raw) std::tie(raw_uid, raw) = raw_component(type, true);
        const auto key = std::to_string(uid); const Ordered* row = placements.at("nodes").contains(key) ? &placements.at("nodes").at(key) : nullptr;
        const std::string zone = row ? row->at("zone").get<std::string>() : t.grid->name; const Grid& g = grids.at(zone);
        std::optional<std::pair<int, int>> at;
        if (row) at = place_at(zone, spot(*row), 20, 3, 3);
        else if (t.x != 0 || t.y != 0) at = ring_search(t.x, t.y, 0, 20, 0, [&](int x, int y) { return g.walk(x, y) && entries[zone].contains(g.component(x, y)); });
        if (!raw || integer(*raw, "crafttype") < 3 || integer(*raw, "crafttype") > 5 || !at || !items.contains("o" + std::to_string(raw_uid)) || !renders(uid)) {
            std::cerr << "skipped gather node " << uid << ' ' << type << (row || t.x != 0 || t.y != 0 ? "" : ": (0,0) template without a placements.json spot") << '\n'; continue;
        }
        add_npc(gather_node(uid, a, raw_uid, *raw), zone, *at);
    }
    // 2. classes.dat npc records the tables above do not place, at their placements.json rows: bankers (Kasa, Sandık, Bankacı
    // Fare), clan chests (Gözlükler for Klan Parası, guide F7), talkers, and interactable=0 props (flags, birds, fans, robots).
    for (const auto& [key, row] : placements.at("npcs").items()) {
        const unsigned uid = unsigned(std::stoul(key));
        if (placed_uids.contains(uid)) continue;
        if (!actors.contains(uid) || !renders(uid)) { std::cerr << "skipped placed npc " << uid << ": model does not assemble\n"; continue; }
        const auto& a = actors.at(uid); const std::string zone = row.at("zone");
        const auto at = place_at(zone, spot(row), 30, 4, 3);
        if (!at) { std::cerr << "skipped placed npc " << uid << ": no free cell near its spot in " << zone << '\n'; continue; }
        Ordered n = {{"source_uid", uid}, {"source_class", std::max(1, integer(a, "classid"))}, {"name", get(a, "name")}, {"hp", 100}};
        if (get(a, "interactable") == "0") { n["kind"] = "prop"; n["interactable"] = false; n["dialogue"] = get(a, "name"); }
        else if (integer(a, "banker") == 1) { n["kind"] = "questgiver"; n["banker"] = true; n["dialogue"] = "Kasanıza buradan ulaşabilirsiniz. (İşlemler → Kasamı göster)"; }
        else if (integer(a, "merchant") == 1 && !get(a, "clan").empty()) {
            const std::string prefix = get(a, "clan") == "arz" ? "Arzın Çocukları Gözlüğü" : "Lodos Gözlüğü"; Ordered shop = Ordered::array();
            for (const auto& [ouid, o] : objects) if (number(*o, "pvpprice") > 0 && get(*o, "name").starts_with(prefix) && items.contains("o" + std::to_string(ouid)) && shop.size() < 16) shop.push_back("o" + std::to_string(ouid));
            n["kind"] = "merchant"; n["shop"] = std::move(shop); n["dialogue"] = "Klan sandığı: gözlükler Klan Parası ile satılır.";
        } else { n["kind"] = "questgiver"; n["dialogue"] = "Selam yolcu."; }
        add_npc(std::move(n), zone, *at);
    }
    // 3. every other <loc> with a centre (quest props and trigger markers): kind "prop" carrying its locationname, arearadius
    // and requiredquestid for quest objectives. Interactable props show their model; id_dummy markers (and models that do not
    // assemble) are hidden from clients. Skipped: maden*/damage1 locs, (0,0) centres, and stacks of 3+ locs parked on one centre
    // (the Karaköy Telsiz/Diafon/Sandal... set at Çemberlitaş [294,1035]).
    std::set<unsigned> represented; for (const auto& n : npcs) represented.insert(n.at("source_uid").get<unsigned>());
    std::map<std::pair<double, double>, int> stacked; for (const auto& [uid, t] : triggers_by_uid) ++stacked[{t.x, t.y}];
    for (const auto& [uid, t] : triggers_by_uid) {
        const auto& a = locs.at(uid); const auto type = get(a, "locationtypename");
        if (represented.contains(uid) || type.starts_with("maden") || type == "damage1") continue;
        if ((t.x == 0 && t.y == 0) || stacked.at({t.x, t.y}) >= 3) { std::cerr << "skipped prop " << uid << ' ' << get(a, "locationname") << ": parked centre\n"; continue; }
        const Grid& g = *t.grid;
        const auto at = ring_search(t.x, t.y, 0, t.radius + 6, 0, [&](int x, int y) { return g.walk(x, y); });
        if (!at) { std::cerr << "skipped prop " << uid << ": no support near its centre\n"; continue; }
        if (type.starts_with("gather_")) { // Büyük Hol / Karaköy mineral and flower spots (detail "Maden")
            const auto [raw_uid, raw] = raw_component(type, true);
            if (raw && integer(*raw, "crafttype") >= 3 && integer(*raw, "crafttype") <= 5 && items.contains("o" + std::to_string(raw_uid)) && renders(uid)) { add_npc(gather_node(uid, a, raw_uid, *raw), g.name, *at); continue; }
        }
        const bool hidden = lower(get(a, "path")).find("id_dummy") != std::string::npos || !renders(uid);
        Ordered n = {{"source_uid", uid}, {"source_class", std::max(1, integer(a, "classid"))}, {"name", get(a, "name")}, {"hp", 100}, {"kind", "prop"},
                     {"dialogue", get(a, "detail").empty() || hidden ? get(a, "name") : get(a, "detail")}, {"interactable", !hidden && integer(a, "interactable") == 1},
                     {"loc", get(a, "locationname")}, {"radius", clamp(t.radius * 1000, 1000, 200000)}};
        if (hidden) n["hidden"] = true;
        if (t.quest >= 0) n["required_quest"] = t.quest;
        add_npc(std::move(n), g.name, *at);
    }
    // 4. every spawn group member beyond the legacy enemy slots.
    for (const char* zone : legacy_enemy_zones) members[zone].erase(members[zone].begin(), members[zone].begin() + legacy_enemies_per_zone);
    for (const auto& [zone, list] : members) for (const auto& [n, at] : list) add_npc(n, zone, at);
    for (const auto& [zone, n, at] : appended) add_npc(n, zone, at); // 5. "append" spawn groups, in placements.json order
    // NONCANONICAL recipe sellers (scroll sources were server data), by trade: Kuyumcu Agop sarraf, İşportacı Genç madenci, Aktar
    // Şevket kimyacı and lokman, Demirci Rüstem silahtar, Terzi Fahri zırhçı (professions[] order).
    constexpr std::pair<unsigned, int> scroll_sellers[] = {{1002, 4}, {1017, 3}, {1003, 2}, {1003, 5}, {1004, 0}, {1006, 1}};
    for (auto& n : npcs)
        for (const auto& [seller, craft] : scroll_sellers)
            if (n.at("source_uid") == seller && recipe_scrolls.contains(craft)) for (const auto& id : recipe_scrolls.at(craft)) n["shop"].push_back(id);
    // NONCANONICAL: later-tier Tılsım without a recipe scroll (drop tables were server data) join the Şef tables in turn
    // (guide: Şefs drop important items).
    std::set<std::string> crafted; for (const auto& [id, r] : recipes.items()) crafted.insert(r.at("product").get<std::string>());
    std::vector<Ordered*> chiefs_in_order; for (auto& n : npcs) if (n.value("boss", false)) chiefs_in_order.push_back(&n);
    size_t turn = 0;
    for (const auto& id : later_glyphs) if (!crafted.contains(id) && !chiefs_in_order.empty()) {
        for (size_t tries = 0; tries < chiefs_in_order.size(); ++tries, ++turn) if (auto& d = (*chiefs_in_order[turn % chiefs_in_order.size()])["drops"]; d.size() < 16) { d.push_back(id); ++turn; break; }
    }
    // ---- quests (rows 7, 18; wave 11 Gaps #4/#5/#6/#13): the original chains from tools/data/quests_original.json, built by
    // analysis/porting-wave11/Quests/build_quests.py from the fandom/official quest lists (ids = original server quest numbers,
    // matching classes.dat <loc> requiredquestid and quest_NNN_finish markers), the VOD and the reward pages; provenance per quest.
    // classes.dat uids resolve to the first placed NPC/prop/door with that source uid, monster uids to their spawned template,
    // <loc> uids to the prop/door standing there. "{at:npc|mon|loc:UID}" in a text becomes that entity's minimap coordinate
    // "[x,y]" (texel = 1024 - 2/3 position, client 00477775; a monster family: the medoid member of its most populated zone).
    const Ordered quest_data = [] { std::ifstream in(IKV_QUESTS); if (!in) throw std::runtime_error("cannot read " IKV_QUESTS); return Ordered::parse(in); }();
    std::map<unsigned, size_t> placed_by_uid; std::map<std::string, std::vector<size_t>> spawned_by_template;
    for (size_t i = 0; i < npcs.size(); ++i) // rescue props (trapped quest NPCs) count as their family's spawns for {at:mon:UID}
        if (npcs[i].at("kind") == "enemy") spawned_by_template[npcs[i].at("template").get<std::string>()].push_back(i);
        else if (npcs[i].value("rescue", false)) spawned_by_template["m" + npcs[i].at("source_uid").dump()].push_back(i);
        else placed_by_uid.emplace(npcs[i].at("source_uid").get<unsigned>(), i);
    const auto entity_of = [&](const Ordered& uid) -> Ordered& {
        const auto f = placed_by_uid.find(uid.get<unsigned>()); if (f == placed_by_uid.end()) throw std::runtime_error("quest entity not placed: " + uid.dump());
        return npcs[f->second];
    };
    const auto family = [&](const Ordered& uid) -> const Ordered& { // the medoid member of the family's most populated zone
        const auto f = spawned_by_template.find("m" + uid.dump()); if (f == spawned_by_template.end()) throw std::runtime_error("quest monster not spawned: " + uid.dump());
        std::map<std::string, std::vector<size_t>> by_zone; for (auto i : f->second) by_zone[npcs[i].at("zone").get<std::string>()].push_back(i);
        const auto& list = std::max_element(by_zone.begin(), by_zone.end(), [](const auto& l, const auto& r) { return l.second.size() < r.second.size(); })->second;
        size_t best = list.front(); double least = 1e300;
        for (auto i : list) { double sum = 0; for (auto j : list) sum += std::hypot(npcs[i].at("x").get<double>() - npcs[j].at("x").get<double>(), npcs[i].at("y").get<double>() - npcs[j].at("y").get<double>()); if (sum < least) { least = sum; best = i; } }
        return npcs[best];
    };
    const auto hinted = [&](std::string text) {
        for (size_t at; (at = text.find("{at:")) != std::string::npos;) {
            const size_t colon = text.find(':', at + 4), end = text.find('}', at);
            const auto kind = text.substr(at + 4, colon - at - 4); const auto uid = Ordered::parse(text.substr(colon + 1, end - colon - 1));
            const Ordered& e = kind == "mon" ? family(uid) : entity_of(uid);
            text.replace(at, end - at + 1, "[" + std::to_string(std::lround(1024 - e.at("x").get<double>() / 1500)) + "," + std::to_string(std::lround(1024 - e.at("y").get<double>() / 1500)) + "]");
        }
        return text;
    };
    Ordered quests = Ordered::object();
    for (const auto& d : quest_data.at("quests")) {
        const std::string id = d.at("id");
        Ordered q = {{"name", d.at("name")}, {"giver", entity_of(d.at("giver")).at("id")}};
        if (d.contains("turn_in")) q["turn_in"] = entity_of(d.at("turn_in")).at("id");
        q["level"] = d.at("level");
        for (const char* key : {"min_level", "class", "time_limit", "source_quest_id"}) if (d.contains(key)) q[key] = d.at(key);
        if (d.value("auto", false)) q["auto"] = true;
        if (d.contains("requires")) { // the first prerequisite; any further ones must be done as well
            q["requires"] = d.at("requires")[0];
            if (d.at("requires").size() > 1) { q["requires_also"] = d.at("requires"); q["requires_also"].erase(0); }
        }
        const auto& o = d.at("objective"); const std::string kind = o.at("kind"); Ordered objective = {{"kind", kind}};
        if (kind == "kill" || (kind == "collect" && o.contains("monster"))) { family(o.at("monster")); objective["target"] = "m" + o.at("monster").dump(); }
        if (kind == "kill") objective["count"] = o.at("count");
        if (kind == "collect") { objective["item"] = o.at("item"); objective["count"] = o.at("count");
            if (o.contains("also")) { objective["also"] = Ordered::array(); // an 'also' item may drop from its own monster family
                for (const auto& v : o.at("also")) { Ordered a = {{"item", v.at("item")}, {"count", v.at("count")}}; if (v.contains("monster")) { family(v.at("monster")); a["target"] = "m" + v.at("monster").dump(); } objective["also"].push_back(std::move(a)); } } }
        if (kind == "visit") { // a <loc> prop/trigger (its arearadius) or an NPC's spot (10 units)
            const Ordered& e = entity_of(o.contains("loc") ? o.at("loc") : o.at("npc"));
            objective.update({{"zone", e.at("zone")}, {"x", e.at("x")}, {"y", e.at("y")}, {"radius", o.contains("loc") ? e.value("radius", 20000) : 10000}});
        }
        if (kind == "use") {
            Ordered targets = Ordered::array(); for (const auto& u : o.contains("locs") ? o.at("locs") : o.at("npcs")) targets.push_back(entity_of(u).at("id"));
            objective["targets"] = std::move(targets); objective["count"] = o.at("count"); if (o.contains("cooldown")) objective["cooldown"] = o.at("cooldown");
        }
        q["objective"] = std::move(objective);
        for (const char* key : {"objective_text", "description", "complete_text"}) if (d.contains(key)) q[key] = hinted(d.at(key));
        q["reward"] = d.at("reward"); // provenance stays in the data file (the quest definition travels in every state catalog)
        if (!q.value("auto", false)) entity_of(d.at("giver"))["quests"].push_back(id);
        if (d.contains("turn_in") || q.value("auto", false)) entity_of(d.contains("turn_in") ? d.at("turn_in") : d.at("giver"))["quests"].push_back(id);
        quests[id] = std::move(q);
    }
    for (auto& n : npcs) if (n.contains("quests")) { auto& list = n["quests"]; std::vector<Ordered> v(list.begin(), list.end()); v.erase(std::unique(v.begin(), v.end()), v.end()); list = Ordered(v); }
    // ---- see-through doorways (wave 16): tools/data/portals.json rows by <loc> uid (Doorways door wall planes plus NONCANONICAL approach
    // aprons, analysis/porting-wave16/impl/Server/author.py); "pair" becomes the partner door's NPC id. World validates them.
    const Ordered portal_data = [] { std::ifstream in(IKV_PORTALS); if (!in) throw std::runtime_error("cannot read " IKV_PORTALS); return Ordered::parse(in); }();
    const auto door_of = [&](unsigned uid) -> Ordered& {
        for (auto& n : npcs) if (n.at("kind") == "door" && n.at("source_uid") == uid) return n;
        throw std::runtime_error("portal door not placed: " + std::to_string(uid));
    };
    for (const auto& [uid, p] : portal_data.at("portals").items())
        door_of(unsigned(std::stoul(uid)))["door"]["portal"] = {{"pair", door_of(p.at("pair").get<unsigned>()).at("id")}, {"threshold", p.at("threshold")}, {"floor", p.at("floor")}, {"height", p.at("height")}, {"apron", p.at("apron")}};
    Ordered defaults = config.at("defaults"); defaults["zone"] = "eminonu"; defaults["x"] = wire(spawns.at("eminonu").first); defaults["y"] = wire(spawns.at("eminonu").second);
    defaults["inventory"] = Ordered::array({Ordered{{"item", "p253"}, {"quantity", 3}}});
    Ordered used = Ordered::object();
    const auto use = [&](const std::string& id) { if (!items.contains(id)) throw std::runtime_error("unknown generated item " + id); used[id] = items.at(id); };
    for (const auto& n : npcs) { for (const auto& id : n.at("shop")) use(id); if (n.contains("drops")) for (const auto& id : n.at("drops")) use(id); }
    use("p253");
    for (const auto& n : npcs) if (n.contains("gather")) for (const char* key : {"item", "rare"}) if (n.at("gather").contains(key)) use(n.at("gather").at(key).get<std::string>());
    for (const auto& [id, r] : recipes.items()) { use(id); use(r.at("product").get<std::string>()); for (const auto& c : r.at("components")) use(c.at("item").get<std::string>()); }
    // Quest items, rewards and every class/sex variant of a reward item (the server hands out the variant the character can
    // wear: W-REW lists one reward line per class) plus their reward prefixes.
    std::set<std::string> reward_names; Ordered reward_prefixes = Ordered::array();
    for (const auto& [id, q] : quests.items()) {
        for (const auto& v : q.at("reward").at("items")) { use(v.at("item").get<std::string>()); reward_names.insert(items.at(v.at("item").get<std::string>()).at("name").get<std::string>()); if (v.contains("prefixes")) for (const auto& p : v.at("prefixes")) reward_prefixes.push_back(p); }
        if (q.at("objective").contains("item")) use(q.at("objective").at("item").get<std::string>());
        if (q.at("objective").contains("also")) for (const auto& v : q.at("objective").at("also")) use(v.at("item").get<std::string>());
    }
    for (const auto& [id, item] : items.items()) if (item.contains("slot") && (item.contains("class") || item.contains("sex")) && reward_names.contains(item.at("name").get<std::string>())) use(id);
    // Every prefix any catalog item can carry: fixed classes.dat prefix1/prefix2 plus the random drop pools (rarity > 0).
    Ordered used_prefixes = Ordered::object();
    for (const auto& [id, p] : prefix_table.items()) if (p.at("rarity").get<int>() > 0) used_prefixes[id] = p;
    for (const auto& [id, item] : used.items()) if (item.contains("prefixes")) for (const auto& p : item.at("prefixes")) used_prefixes[std::to_string(p.get<int>())] = prefix_table.at(std::to_string(p.get<int>()));
    for (const auto& p : reward_prefixes) { // a reward prefix and its same-name/same-index siblings (other class limits)
        const auto& want = prefix_table.at(std::to_string(p.get<int>()));
        for (const auto& [id, other] : prefix_table.items()) if (other.at("name") == want.at("name") && other.at("index") == want.at("index")) used_prefixes[id] = other;
    }
    config["provenance"] = "NONCANONICAL playable world generated by tools/generate_content.cpp. Original classes.dat: NPC/monster names, model UIDs, "
        "levels, combat stats (scaled by 100/383, VOD level-2 Enerji 383 = native 100 HP), weapon/armour stats, slots, class/sex limits, icons, prices, "
        "durability, rarity and fixed prefix1/prefix2; prefixes.dat potion names/amounts and Efsun prefixes (attribute ids per client 004338f0/00402f60); "
        "animation.dat run/walk speeds; classes.dat idlewalktend (idle walk percent; replaces the patrol where a record has it); <loc> door/crystal models, "
        "centres, requiredquestid and gmonly. Zone registry/support grids and instance rows are original. "
        "NONCANONICAL: loc pairings (inferred from names), door footprints, per-group instances, NPC/monster placement, shop stock, drop tables, potion prices, dialogue, "
        "quest and respawn timers, idle walk timing/radius. Distances are wire units (1000 = one source scene unit; a source actor is ~7.5 units tall).";
    config["defaults"] = std::move(defaults); config["zones"] = std::move(zones); config["items"] = std::move(used); config["prefixes"] = std::move(used_prefixes);
    config["quests"] = std::move(quests); config["npcs"] = std::move(npcs); config["recipes"] = std::move(recipes);

    // ---- validate exactly as the server will ----
    std::map<std::string, std::shared_ptr<const ikv::Zone>> source_zones;
    for (auto& [name, g] : grids) source_zones.emplace(name, std::make_shared<const ikv::Zone>(std::move(g.zone)));
    ikv::World world(ikv::Json::parse(config.dump()), [](std::span<const ikv::CharacterSave>) {});
    world.set_source_zones(std::move(source_zones));

    // One entry per line below each catalog key, matching the hand-maintained layout.
    std::ostringstream out; out << "{\n"; size_t index = 0;
    for (const auto& [key, value] : config.items()) {
        out << "  " << Ordered(key).dump() << ": ";
        if ((value.is_object() && key != "defaults" && !value.empty()) || (value.is_array() && !value.empty() && value.front().is_object())) {
            out << (value.is_object() ? "{\n" : "[\n"); size_t n = 0;
            for (const auto& [k, v] : value.items()) out << "    " << (value.is_object() ? Ordered(k).dump() + ": " : "") << v.dump() << (++n < value.size() ? ",\n" : "\n");
            out << (value.is_object() ? "  }" : "  ]");
        } else out << value.dump();
        out << (++index < config.size() ? ",\n" : "\n");
    }
    out << "}\n";
    std::ofstream(argv[2], std::ios::binary) << out.str();
    if (argc == 4) std::ofstream(argv[3], std::ios::binary) << Ordered{{"provenance", "Normalized original classes.dat/prefixes.dat records in the native schema (tools/generate_content.cpp)."},
        {"actors", extract_actors}, {"items", items}, {"locs", extract_locs}}.dump(1) << '\n';
    size_t door_count = 0; for (const auto& n : config.at("npcs")) door_count += n.at("kind") == "door";
    std::cout << "zones " << config.at("zones").size() << ", doors " << door_count << " (of " << extract_locs.size() << " locs), npcs "
              << config.at("npcs").size() << ", items " << config.at("items").size() << " (of " << items.size() << " extracted), actors "
              << extract_actors.size() << '\n';
    return 0;
} catch (const std::exception& error) { std::cerr << "content generation failed: " << error.what() << '\n'; return 1; }
