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
// NONCANONICAL monster level bands per zone (spawn tables were server data).
constexpr std::tuple<const char*, int, int> monster_bands[] = {
    {"eminonu", 1, 5}, {"antrepo", 2, 8}, {"labirent", 6, 14}, {"meteor", 10, 20}, {"ada", 15, 25},
    {"yeralti", 20, 30}, {"migrat", 25, 35}, {"siginak", 30, 40}, {"cemberli", 35, 45}, {"buyukhol", 40, 50},
    {"karakoy", 45, 55}, {"karakoyic", 50, 60}};

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
        Ordered item = {{"name", get(*a, "name")}, {"stack", 20}, {"price", std::clamp(integer(*a, "price"), 1, 1000000)}, {"level", std::clamp(integer(*a, "itemlevel", 1), 1, 100)}, {"source_uid", uid}};
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
        return Ordered{{"source_uid", integer(a, "uid")}, {"source_class", std::max(1, integer(a, "classid"))}, {"kind", "enemy"},
            {"template", "m" + get(a, "uid")}, {"name", get(a, "name")}, {"level", integer(a, "level", 1)},
            {"hp", clamp(number(a, "hp", 100), 1, 100000)}, {"damage", clamp((low + high) / 2, 1, 100000)}, {"damage_range", Ordered::array({low, high})},
            {"attack", std::clamp(number(a, "attack"), 0.0, 100000.0)}, {"defense", std::clamp(number(a, "defense"), 0.0, 100000.0)},
            {"crit", Ordered::array({std::clamp(number(a, "criticalchance"), 0.0, 100.0), std::clamp(number(a, "criticalscale", 2), 1.0, 20.0)})},
            {"dmgred", {{"K", fraction("dmgredslash")}, {"D", fraction("dmgredpierce")}, {"E", fraction("dmgredbludge")}}}, {"resist", std::move(resist)},
            {"attack_cooldown", clamp(number(a, "attackrate", 1000) / 50, 1, 100000)}, {"attack_range", clamp(number(a, "attackrange", 3) * 1000, 1000, 60000)},
            {"aggro", aggro}, {"leash", std::min(100000, aggro * 3)},
            {"speed", clip_speed.contains(get(a, "classid")) ? clip_speed.at(get(a, "classid")) : 110},
            {"respawn", 600}, {"xp", clamp(number(a, "experience", 1), 1, 100000)}, {"currency", clamp(number(a, "dropmoney", 1), 1, 100000)}};
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
        Ordered ordered = {{"id", n.at("id")}}; ordered.update(n); npcs.push_back(std::move(ordered));
    };
    // Models the client cannot assemble (original biped/binding mismatches) are left out of the population.
    ikv::Scene probe; std::map<unsigned, bool> assembles;
    const auto renders = [&](unsigned uid) {
        if (const auto found = assembles.find(uid); found != assembles.end()) return found->second;
        bool ok = true;
        try { ikv::attach_source_npc(probe, base, uid, 1); } catch (const std::exception& error) { ok = false; std::cout << "excluded actor " << uid << ": " << error.what() << '\n'; }
        ikv::detach_source_actor(probe, 1);
        return assembles[uid] = ok;
    };
    // map_x/map_y: the NPC's shop mark in the original ui/zoneNN.dds map art (0 = none). A map pixel is 2048 minus the
    // support cell (the client minimap convention): zone00 "Bankacı" (938,718); zone01 "Aktar" (1098,1007),
    // "Demirci" (1119,1044), "Terzi" (993,1109). The VOD 18:30 Bankacı stands at that bank's colonnade.
    struct Town { const char* zone; unsigned uid; const char* role; const char* kind; int lo, hi, map_x, map_y; };
    // NONCANONICAL town population: roles follow the original merchant/banker/questGiver flags and the NPC trades
    // (aktar/demirci/terzi models at the matching Mısır Çarşısı marks); unmarked positions and all stock are server data.
    // Agah Efendi stands in the Yeni Cami courtyard (VOD).
    constexpr Town town[] = {
        {"eminonu", 1001, "quest", "", 0, 0, 0, 0}, {"eminonu", 1002, "merchant", "weapon", 1, 5, 0, 0},
        {"eminonu", 1012, "banker", "", 0, 0, 938, 718}, {"eminonu", 1017, "merchant", "potion", 1, 5, 0, 0},
        {"eminonu", 1057, "merchant", "potion", 1, 10, 0, 0}, {"eminonu", 1041, "merchant", "armour", 1, 4, 0, 0},
        {"eminonu", 1000, "talker", "", 0, 0, 0, 0},
        {"misir", 1003, "merchant", "potion", 1, 25, 1098, 1007}, {"misir", 1025, "merchant", "weapon", 6, 15, 1119, 1044},
        {"misir", 1006, "merchant", "armour", 5, 12, 993, 1109}, {"misir", 1024, "merchant", "potion", 26, 55, 0, 0},
        {"topkapi", 131527, "merchant", "armour", 13, 20, 0, 0}, {"topkapi", 131561, "merchant", "weapon", 16, 30, 0, 0},
        {"karakoy", 131810, "merchant", "armour", 21, 30, 0, 0},
        // Klan: Arz/Lodos Muhafızı (claninviter) guard their clan's Büyük Hol gate signs in Meteor Bölgesi (<loc> 131839
        // ID_ARZ_TABELA, 131840 ID_LODOS_TABELA); Tüccar (Arz/Lodos) stand at the Maden Ocağı hub of their half of Büyük Hol
        // (zone12.dds: Arz exit 131842 in the north-west half, Lodos exit 131841 in the south-east one). NONCANONICAL spots.
        {"meteor", 1033, "recruiter", "", 1, 0, 1345, 700}, {"meteor", 1034, "recruiter", "", 2, 0, 1334, 700},
        {"buyukhol", 1058, "clanshop", "Arzın Çocukları", 1, 0, 755, 1035}, {"buyukhol", 1046, "clanshop", "Lodos", 2, 0, 987, 1280}};
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
            // NONCANONICAL: the Eminönü terzi also sells the Akçe (İKV Dükkan) goods, the Topkapı terzi the lonca goods, the Topkapı
            // silahçı the first-tier Tılsım (all of each, by id).
            if (const char* extra = t.uid == 1041 ? "akce" : t.uid == 131527 ? "lonca" : t.uid == 131561 ? "glyph" : nullptr)
                for (const auto& [id, k] : item_kind) if (std::get<0>(k) == extra) n["shop"].push_back(id);
        }
        else if (role == "quest") { n["kind"] = "questgiver"; n["dialogue"] = "Hoş geldin evladım. Avludaki fareler hastalık saçıyor; yardımına ihtiyacım var."; } // quests: see the quest section
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
        const auto anchor = t.map_x ? std::optional{std::pair{units(2048 - t.map_x), units(2048 - t.map_y)}} : std::nullopt;
        const bool first = placed[t.zone].empty();
        const auto at = anchor ? place(t.zone, 0, 30, 1, 10, 10, 0, anchor) : place(t.zone, first ? 8 : 14, 150, 1, 10, 10, first ? 200 : 40.0 * double(placed[t.zone].size()));
        if (!at) throw std::runtime_error(std::string("no free walkable placement for NPC in ") + t.zone);
        add_npc(std::move(n), t.zone, *at);
    }
    for (const auto& [zone, lo, hi] : monster_bands) {
        // Up to three monster families per zone (distinct models), one group of three each, on the zone's main walkable component.
        std::vector<const Attributes*> chosen; std::set<std::string> models;
        std::vector<std::pair<int, const Attributes*>> pool;
        for (const auto& [uid, a] : actors) {
            const auto path = lower(get(a, "path")); const auto name = get(a, "name");
            if (get(a, "allignment") != "monster" || integer(a, "level") < lo || integer(a, "level") > hi || name.empty() || name == "Boş" ||
                name.find("Reserved") != std::string::npos || name.find('[') != std::string::npos || number(a, "hp") > 100000 ||
                number(a, "dmgmax") <= 0 || number(a, "attackrange") <= 1 || path.find("tuzak") != std::string::npos || path.find("dummy") != std::string::npos) continue;
            pool.push_back({uid == 16001 ? -1 : integer(a, "level"), &a}); // Fare first: the Eminönü quest target
        }
        std::sort(pool.begin(), pool.end(), [](const auto& l, const auto& r) { return l.first != r.first ? l.first < r.first : integer(*l.second, "uid") < integer(*r.second, "uid"); });
        for (const auto& [level, a] : pool) if (chosen.size() < 3 && !models.contains(lower(get(*a, "path"))) && renders(unsigned(integer(*a, "uid")))) { models.insert(lower(get(*a, "path"))); chosen.push_back(a); }
        for (size_t group = 0; group < chosen.size(); ++group) {
            const auto found = place(zone, 70 + 50 * double(group), 450, 2, 40, 25, 120 * double(group));
            if (!found) { std::cerr << "skipped monster group " << get(*chosen[group], "name") << " in " << zone << ": spawn area too small\n"; continue; }
            const auto anchor = *found; const Grid& g = grids.at(zone);
            for (int member = 0; member < 3; ++member) {
                const double t = (120.0 * member + 30) * M_PI / 180, ax = units(anchor.first), ay = units(anchor.second);
                auto at = std::pair{cell(ax + 3 * std::cos(t)), cell(ay + 3 * std::sin(t))};
                if (!g.block(at.first, at.second, 1) || g.component(at.first, at.second) != g.component(anchor.first, anchor.second)) at = anchor;
                Ordered n = enemy(*chosen[group]); n["loot"] = Ordered::array(); n["drops"] = drops(n.at("level").get<int>());
                // Short straight patrol (every sample keeps 3x3 support, so the world's segment trace stays on support).
                Ordered patrol = Ordered::array();
                for (int direction = 0; direction < 8 && patrol.empty(); ++direction) {
                    const double d = (45.0 * direction + 90 * member) * M_PI / 180; bool clear = true;
                    for (double s = 0; s <= 6 && clear; s += 0.5) clear = g.block(cell(units(at.first) + s * std::cos(d)), cell(units(at.second) + s * std::sin(d)), 1);
                    if (clear) patrol = {{wire(at.first), wire(at.second)}, {wire(cell(units(at.first) + 6 * std::cos(d))), wire(cell(units(at.second) + 6 * std::sin(d)))}};
                }
                n["patrol"] = std::move(patrol); add_npc(std::move(n), zone, at);
            }
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
    for (const auto& [uid, a] : locs) {
        const auto type = get(a, "locationtypename");
        if (!type.starts_with("maden") || !triggers_by_uid.contains(uid)) continue;
        const auto key = fold(type.substr(5)); const Attributes* raw = nullptr; int raw_uid = 0;
        for (const auto& [cuid, c] : objects)
            if (get(*c, "objecttype") == "component" && integer(*c, "rarity") == 500 && fold(get(*c, "name")).find(key) != std::string::npos &&
                (!raw || get(*c, "name").size() < get(*raw, "name").size())) { raw = c; raw_uid = cuid; }
        const auto& t = triggers_by_uid.at(uid); const Grid& g = *t.grid;
        const auto at = ring_search(t.x, t.y, 0, 6, 0, [&](int x, int y) { return g.walk(x, y); });
        if (!raw || integer(*raw, "crafttype") < 3 || integer(*raw, "crafttype") > 5 || (t.x == 0 && t.y == 0) || !at || !items.contains("o" + std::to_string(raw_uid)) || !renders(uid)) {
            std::cerr << "skipped gather node " << uid << ' ' << type << '\n'; continue;
        }
        Ordered gather = {{"craft", professions[integer(*raw, "crafttype")]}, {"level", integer(*raw, "itemlevel")}, {"item", "o" + std::to_string(raw_uid)}, {"respawn", 600}};
        if (const auto pair = objects.find(raw_uid + 1); pair != objects.end() && get(*pair->second, "objecttype") == "component" && integer(*pair->second, "rarity") == 0 &&
            get(*pair->second, "crafttype") == get(*raw, "crafttype") && get(*pair->second, "itemlevel") == get(*raw, "itemlevel") && get(*pair->second, "components").empty() &&
            items.contains("o" + std::to_string(raw_uid + 1))) gather["rare"] = "o" + std::to_string(raw_uid + 1);
        add_npc({{"source_uid", uid}, {"source_class", std::max(1, integer(a, "classid"))}, {"name", get(*raw, "name")}, {"hp", 100}, {"kind", "node"},
                 {"dialogue", get(a, "detail")}, {"gather", std::move(gather)}}, g.name, *at);
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
    // NONCANONICAL recipe sellers (scroll sources were server data): Eminönü merchants stock the scrolls by profession.
    for (auto& n : npcs) {
        const unsigned source = n.at("source_uid");
        for (const auto& [craft, scrolls] : recipe_scrolls)
            if ((source == 1017 && craft >= 3) || (source == 1002 && (craft == 0 || craft == 2)) || (source == 1041 && craft == 1))
                for (const auto& id : scrolls) n["shop"].push_back(id);
    }

    // ---- quests (rows 7, 18) ----
    // Every interactable classes.dat questGiver NPC gets a NONCANONICAL chain (objectives were server data). VOD names and
    // completion texts where the VOD shows them (chat 09:25-50:25); reward scale from the VOD tecrübe/para floaters: a level-L
    // 10-kill quest gives 77·(10+L) tecrübe and 100·L para (847/+100 at [1] ... 1078/+400 at [4]), a talk quest half that
    // tecrübe (424 at [1], 462 at [2]). Home zones of questgivers the town table does not place are inferred from their
    // names/models (spawns were server data); interactable=0 questGivers (fans, robots, the crystal, the clock) cannot be
    // talked to in the original and get none. GB-ANI (exe 0x592f88 "Mısır Çarşısındaki GB-Anı") re-offers auto quests.
    constexpr std::pair<unsigned, const char*> quest_homes[] = {
        {1005, "eminonu"}, {1007, "eminonu"}, {1008, "eminonu"}, {1009, "eminonu"}, {1010, "eminonu"}, {1011, "eminonu"}, {1013, "eminonu"},
        {1018, "eminonu"}, {1019, "eminonu"}, {1020, "eminonu"}, {1021, "eminonu"}, {1022, "eminonu"}, {1023, "eminonu"}, {1035, "eminonu"},
        {1038, "eminonu"}, {1039, "eminonu"}, {1040, "eminonu"}, {1042, "eminonu"}, {1043, "eminonu"}, {1048, "eminonu"}, {1054, "eminonu"},
        {1056, "eminonu"}, {1004, "misir"}, {1026, "misir"}, {1027, "misir"}, {1028, "misir"}, {17112, "misir"},
        {1016, "antrepo"}, {17068, "antrepo"}, {17069, "antrepo"}, {17110, "labirent"}, {17046, "labirent"}, {17047, "labirent"}, {17048, "labirent"},
        {1029, "meteor"}, {1030, "meteor"}, {131366, "meteor"}, {131342, "ada"}, {131490, "ada"}, {131833, "yeralti"}, {131835, "yeralti"},
        {131500, "migrat"}, {131108, "siginak"}, {131109, "siginak"}, {131112, "siginak"}, {131113, "siginak"}, {131114, "siginak"},
        {131115, "siginak"}, {131116, "siginak"}, {131117, "siginak"}, {131301, "cemberli"}, {131302, "cemberli"}, {131309, "cemberli"},
        {131310, "cemberli"}, {131311, "cemberli"}, {131312, "cemberli"}, {131313, "cemberli"}, {131314, "cemberli"}, {131315, "cemberli"},
        {131316, "cemberli"}, {131317, "cemberli"}, {131318, "cemberli"}, {131323, "cemberli"}, {1036, "topkapi"}, {1045, "topkapi"},
        {1053, "topkapi"}, {1055, "topkapi"}, {1069, "topkapi"}, {1070, "topkapi"}, {1031, "buyukhol"}, {1032, "buyukhol"},
        {131811, "karakoy"}, {131812, "karakoy"}, {131813, "karakoy"}, {131814, "karakoy"}, {131830, "karakoy"}, {131877, "karakoy"},
        {131878, "karakoy"}, {131879, "karakoy"}, {131880, "karakoy"}, {131881, "karakoy"}, {131882, "karakoy"}, {131883, "karakoy"},
        {131884, "karakoy"}, {131885, "karakoy"}, {131886, "karakoy"}, {131888, "karakoy"}, {131889, "karakoy"}, {131890, "karakoy"},
        {131891, "karakoy"}, {131892, "karakoy"}, {131893, "karakoy"}, {131894, "karakoy"}, {131895, "karakoy"}, {132054, "karakoy"},
        {132055, "karakoy"}, {132056, "karakoy"}, {131896, "karakoyic"}, {131897, "karakoyic"}, {131898, "karakoyic"}, {131899, "karakoyic"},
        {131995, "karakoyic"}, {132041, "karakoyic"}, {132042, "karakoyic"}, {132048, "karakoyic"}, {132075, "karakoyic"}, {132076, "karakoyic"},
        {132078, "karakoyic"}, {132081, "karakoyic"}, {132082, "karakoyic"}, {132085, "karakoyic"}, {132191, "karakoyic"}};
    const std::map<std::string, std::string> zone_titles{{"eminonu", "Eminönü"}, {"misir", "Mısır Çarşısı"}, {"antrepo", "Antrepo"}, {"labirent", "Labirent"},
        {"meteor", "Meteor Bölgesi"}, {"ada", "Sivri Ada"}, {"savas", "Er Meydanı"}, {"yeralti", "Yeraltı"}, {"migrat", "Migrat"}, {"siginak", "Sığınaklar"},
        {"cemberli", "Çemberlitaş"}, {"topkapi", "Topkapı"}, {"buyukhol", "Büyük Hol"}, {"karakoy", "Karaköy"}, {"karakoyic", "Karaköy İç"}}; // exe 0x58f310
    std::map<unsigned, size_t> giver_index; // classes.dat uid -> npcs index
    for (size_t i = 0; i < npcs.size(); ++i) if (npcs[i].at("kind") == "questgiver" || npcs[i].at("kind") == "merchant") giver_index.emplace(npcs[i].at("source_uid").get<unsigned>(), i);
    for (const auto& [uid, zone] : quest_homes) {
        if (giver_index.contains(uid)) continue;
        const auto& a = actors.at(uid);
        if (!renders(uid)) { std::cerr << "skipped questgiver " << uid << ": model does not assemble\n"; continue; }
        const auto at = place(zone, 14, 150, 1, 10, 10, 40.0 * double(placed[zone].size()));
        if (!at) { std::cerr << "skipped questgiver " << uid << ": no walkable placement in " << zone << '\n'; continue; }
        Ordered n = {{"source_uid", uid}, {"source_class", std::max(1, integer(a, "classid"))}, {"name", get(a, "name")}, {"hp", 100}, {"kind", "questgiver"},
                     {"dialogue", "Selam yolcu. Sana verecek bir işim var."}};
        if (uid == 17112) { n["auto_quests"] = true; n["dialogue"] = "Otomatik gelen görevlerini sildiysen onları yalnızca benden yeniden alabilirsin."; }
        giver_index.emplace(uid, npcs.size()); add_npc(std::move(n), zone, *at);
    }
    for (const auto& [uid, a] : actors)
        if (integer(a, "questGiver") == 1 && integer(a, "interactable") == 1 && !giver_index.contains(uid) &&
            std::none_of(std::begin(quest_homes), std::end(quest_homes), [&](const auto& h) { return h.first == uid; }))
            throw std::runtime_error("questGiver without a home zone: " + std::to_string(uid));
    struct Family { std::string tmpl, name, zone; int level = 1; Ordered drops; };
    std::map<std::string, std::vector<Family>> families; // zone -> enemy families by level
    for (const auto& n : npcs) if (n.at("kind") == "enemy") {
        auto& f = families[n.at("zone").get<std::string>()];
        if (std::none_of(f.begin(), f.end(), [&](const Family& x) { return n.at("template") == x.tmpl; }))
            f.push_back({n.at("template").get<std::string>(), n.at("name").get<std::string>(), n.at("zone").get<std::string>(), n.at("level").get<int>(), n.at("drops")});
    }
    for (auto& [zone, f] : families) std::stable_sort(f.begin(), f.end(), [](const Family& l, const Family& r) { return l.level < r.level; });
    const std::map<std::string, std::string> hunting{{"misir", "eminonu"}, {"topkapi", "meteor"}, {"savas", "eminonu"}}; // town zones without monsters
    const auto potion = [&](int level) {
        std::string best = "p253"; int found = 0;
        for (const auto& [id, k] : item_kind) if (std::get<0>(k) == "potion" && items.at(id).contains("heal") && std::get<1>(k) <= level && std::get<1>(k) >= found) { best = id; found = std::get<1>(k); }
        return best;
    };
    const auto words = [&](const std::string& s) { std::vector<std::string> v; std::istringstream in(s); std::string w; while (in >> w) v.push_back(fold(w)); return v; };
    // Collect objective: the rarity-10000 hide/cloth component named after the monster (Yılan -> Yılan Derisi, Kurt -> Kurt
    // Kürkü), else the highest such component at or below the monster level.
    const auto quest_item = [&](const Family& f) {
        std::string best = "o13564"; int found = -1; const auto monster = words(f.name);
        for (const auto& [uid, o] : objects) {
            if (get(*o, "objecttype") != "component" || integer(*o, "rarity") != 10000 || !items.contains("o" + std::to_string(uid))) continue;
            if (const auto first = words(get(*o, "name")); !first.empty() && std::find(monster.begin(), monster.end(), first.front()) != monster.end()) return "o" + std::to_string(uid);
            if (const int l = integer(*o, "itemlevel"); l <= f.level && l > found) { best = "o" + std::to_string(uid); found = l; }
        }
        return best;
    };
    const auto npc_id = [&](unsigned uid) { return npcs.at(giver_index.at(uid)).at("id").get<uint32_t>(); };
    const auto name_of = [&](unsigned uid) { return npcs.at(giver_index.at(uid)).at("name").get<std::string>(); };
    const auto kill_xp = [](int level) { return 77 * (10 + level); };
    const auto reward = [](int xp, int currency, Ordered list = Ordered::array()) { return Ordered{{"xp", xp}, {"currency", currency}, {"items", std::move(list)}}; };
    const auto stack = [](const std::string& item, int n) { return Ordered::array({Ordered{{"item", item}, {"quantity", n}}}); };
    struct Step { std::string name; int level; std::string kind, target, item; int count; unsigned turn_in; int time_limit; std::string goal, description, complete; Ordered reward; };
    Ordered quests = Ordered::object(); std::map<unsigned, std::string> chain_end;
    // Quest "<npc uid>.<n>": the first is accepted at the giver (once `after` is done), later ones arrive automatically; each is
    // handed in at turn_in (default the giver), whose catalog lists it.
    const auto chain = [&](unsigned giver, const std::vector<Step>& steps, const std::string& after = "") {
        std::string previous = after;
        for (size_t i = 0; i < steps.size(); ++i) {
            const auto& s = steps[i]; const auto id = std::to_string(giver) + "." + std::to_string(i + 1);
            Ordered q = {{"name", s.name}, {"giver", npc_id(giver)}};
            if (s.turn_in) q["turn_in"] = npc_id(s.turn_in);
            q["level"] = s.level;
            if (i) q["auto"] = true;
            if (!previous.empty()) q["requires"] = previous;
            if (s.time_limit) q["time_limit"] = s.time_limit;
            Ordered objective = {{"kind", s.kind}};
            if (s.kind != "talk") { objective["target"] = s.target; objective["count"] = s.count; }
            if (s.kind == "collect") objective["item"] = s.item;
            q.update({{"objective", std::move(objective)}, {"objective_text", s.goal}, {"description", s.description}, {"complete_text", s.complete}, {"reward", s.reward}});
            if (i == 0 || !s.turn_in) npcs[giver_index.at(giver)]["quests"].push_back(id);
            if (s.turn_in) npcs[giver_index.at(s.turn_in)]["quests"].push_back(id);
            quests[id] = std::move(q); previous = id;
        }
        chain_end[giver] = previous;
    };
    const auto eminonu_family = [&](const char* tmpl) {
        for (const auto& f : families["eminonu"]) if (f.tmpl == tmpl) return f;
        throw std::runtime_error(std::string("VOD quest monster missing in Eminönü: ") + tmpl);
    };
    const auto fare = eminonu_family("m16001"), kertenkele = eminonu_family("m17015"), orumcek = eminonu_family("m17020");
    // VOD quests (names, levels, completion texts and order as seen in chat; objectives of the talk steps follow those texts).
    chain(1000, {{"Teşkilat'a Katıl", 1, "talk", "", "", 0, 1048, 0, "Teşkilat Hattı ile konuş.",
                  "Teşkilat yeni gönüllüler arıyor. Meydandaki Teşkilat Hattı'ndan kaydını yaptır.",
                  "Kaydın tamam. Al bakalım kardeşim. Bunlara ihtiyacın olabilir. Şimdi git Agah Bey'le konuş.", reward(424, 0, stack(potion(1), 3))}}); // VOD 08:56/09:25
    chain(1001, {{"Salgın hastalık", 1, "kill", fare.tmpl, "", 10, 0, 0, "10 adet Fare öldür.",
                  "Avludaki fareler hastalık taşıyor; salgın yayılmadan onları temizlemeliyiz.\nOn fare öldür, sonra bana gel.",
                  "Tebrik ederim. İlk gerçek görevini başardın. [Eşyalarınız hasar gördüyse Mısır Çarşısı girişindeki örse tıklayarak tamir edebilirsiniz.]",
                  reward(kill_xp(1), 100, stack(potion(1), 2))}, // VOD 16:12-16:30
                 {"Bankacıya Ulak", 1, "talk", "", "", 0, 1012, 0, "Bankacı ile konuş.", "Bankacı birine bir emanet göndermek istiyor. Git ona uğra.",
                  "Ali'nin cüzdanı işte burada. Bunu ona götürün lütfen. İyi günler dilerim.", reward(kill_xp(1) / 2, 0)}, // VOD 18:30
                 {"Hesap Cüzdan", 1, "talk", "", "", 0, 1039, 0, "Cüzdanı Jandarma Ali'ye götür.", "Bankacının verdiği cüzdan Jandarma Ali'nin. Ona götür.",
                  "Çok sağol. Burada anacımın topladığı ceviz yapraklarından var. Al kaynatıp içersin. Soğuk iyi gelir.", reward(360, 0, stack("o13545", 3))}}, // VOD 19:17-20:40
          "1000.1");
    chain(1039, {{"Anacının İlaçları", 3, "talk", "", "", 0, 1021, 300, "İlaçları beş dakika içinde Teyze'ye götür.",
                  "Anacım hasta, ilaçlarını bekliyor. Acele et; geç kalırsan iş işten geçer.", "Allah razı olsun evladım, tam vaktinde geldin.",
                  reward(kill_xp(3) / 2, 0, stack(potion(3), 2))}}, "1001.3"); // VOD 44:35 "görevinin süresi doldu!"
    chain(1010, {{"Sahil Temizliği", 2, "kill", kertenkele.tmpl, "", 10, 0, 0, "10 adet " + kertenkele.name + " öldür.",
                  "Kıyıya kertenkeleler doluştu, ağlarımı kemiriyorlar. On tanesini temizle.", "Sağ ol, kıyı biraz nefes aldı.", reward(kill_xp(2), 200, stack(potion(2), 2))}, // VOD 24:30-26:13
                 {"Sahil Temizliği", 3, "kill", kertenkele.tmpl, "", 10, 0, 0, "10 adet " + kertenkele.name + " öldür.",
                  "Kertenkeleler yine çoğaldı. Kıyıyı bir kez daha temizle.", "Kıyı tertemiz oldu. Eline sağlık.", reward(kill_xp(3), 300, stack(potion(3), 2))}}); // VOD 28:45-31:30
    chain(1011, {{"Fiş Hesapları", 4, "collect", orumcek.tmpl, quest_item(orumcek), 5, 0, 0, "5 adet " + items.at(quest_item(orumcek)).at("name").get<std::string>() + " topla (" + orumcek.name + ").",
                  "Dilekçelerimi yazacak malzemem kalmadı. Örümcek yuvalarının oradan bana malzeme getir.", "İşte şimdi hesapları kapatabilirim. Sağ ol.",
                  reward(kill_xp(4), 400, orumcek.drops.empty() ? Ordered::array() : stack(orumcek.drops[0], 1))}}); // VOD 40:18 +1078/+400
    chain(1043, {{"Pis Haşereler", 4, "kill", orumcek.tmpl, "", 10, 0, 0, "10 adet " + orumcek.name + " öldür.",
                  "Pazarın arkasını örümcekler bastı. On tanesini temizle de nefes alalım.",
                  "Hmmm fena değil. Sayıları azaldı baya. Lütfen ara sıra bana uğrayın. Yardımınıza ihtiyaç duyabilirim.", reward(kill_xp(4), 400, stack(potion(4), 2))}}); // VOD 41:00
    chain(1042, {{"Savaş'ın Niyeti", 3, "kill", kertenkele.tmpl, "", 10, 0, 0, "10 adet " + kertenkele.name + " öldür.",
                  "Ne yapmak istediğimi anlatmadan önce sana güvenebileceğimi görmeliyim. Kertenkelelerden on tanesini öldür.",
                  "[Hayran bakışlarla Savaş sizi dinlemektedir.] [Savaş'la tekrar konuşun]", reward(164, 0)}, // VOD 49:44-50:25
                 {"Savaş'la Konuş", 3, "talk", "", "", 0, 0, 0, "Savaş ile tekrar konuş.", "Savaş anlatacaklarını bitirmedi.",
                  "Sana güvenebileceğimi biliyordum. Gerisini sonra konuşuruz.", reward(kill_xp(3) / 2, 0)}});
    // Generated chains: kill 10 of one zone family, collect 5 quest drops of the next, then carry word to the zone's next questgiver.
    std::map<std::string, std::vector<unsigned>> zone_givers;
    for (const auto& [uid, index] : giver_index)
        if (integer(actors.at(uid), "questGiver") == 1 && integer(actors.at(uid), "interactable") == 1 && !npcs[index].value("auto_quests", false))
            zone_givers[npcs[index].at("zone").get<std::string>()].push_back(uid);
    for (const auto& [zone, givers] : zone_givers)
        for (size_t k = 0; k < givers.size(); ++k) {
            const unsigned g = givers[k]; if (chain_end.contains(g)) continue;
            const auto& fams = families[hunting.contains(zone) ? hunting.at(zone) : zone]; std::vector<Step> steps;
            if (!fams.empty()) {
                const size_t first = k % fams.size(); const auto& a = fams[first]; const auto& b = fams[std::min(first + 1, fams.size() - 1)]; // levels never fall
                steps.push_back({a.name + " Avı", a.level, "kill", a.tmpl, "", 10, 0, 0, "10 adet " + a.name + " öldür.",
                                 zone_titles.at(a.zone) + " çevresinde " + a.name + " sürüleri dolaşıyor, kimseye rahat vermiyorlar.\n10 tanesini öldür, sonra bana gel.",
                                 "Aferin, sayıları epey azaldı.", reward(kill_xp(a.level), 100 * a.level, stack(potion(a.level), 2))});
                const auto item = quest_item(b); const auto item_name = items.at(item).at("name").get<std::string>();
                steps.push_back({item_name + " Toplama", b.level, "collect", b.tmpl, item, 5, 0, 0, "5 adet " + item_name + " topla (" + b.name + ").",
                                 b.name + " avlarken üzerlerinden " + item_name + " düşer. Bana 5 tane getir; işime yarayacak.",
                                 "Tam ihtiyacım olan şey. Eline sağlık.", reward(kill_xp(b.level), 100 * b.level, b.drops.empty() ? Ordered::array() : stack(b.drops[0], 1))});
            }
            if (givers.size() > 1) {
                const unsigned h = givers[(k + 1) % givers.size()]; const int level = steps.empty() ? 1 : steps.back().level;
                steps.push_back({name_of(h) + " ile Görüş", level, "talk", "", "", 0, h, 0, name_of(h) + " ile konuş.",
                                 name_of(h) + " senin gibi birini arıyor. Git onunla konuş; selamımı da ilet.",
                                 name_of(g) + " mi gönderdi seni? Hoş geldin, sana da bir işim olabilir.", reward(kill_xp(level) / 2, 0)});
            }
            if (!steps.empty()) chain(g, steps);
        }
    // <loc> requiredquestid gates (World checks them when a door is used): each id marks the last quest of a chain in the door's
    // zone, round-robin over that zone's questgivers in uid order (the original chains were server data).
    std::map<std::string, size_t> next_gate; std::set<int> gated;
    for (const auto& [uid, t] : triggers_by_uid) {
        if (t.quest < 0 || gated.contains(t.quest)) continue;
        const auto found = zone_givers.find(t.grid->name);
        for (size_t n = 0; found != zone_givers.end() && n < found->second.size(); ++n) {
            auto& at = next_gate[t.grid->name]; const unsigned g = found->second[(at + n) % found->second.size()];
            if (!chain_end.contains(g) || quests.at(chain_end.at(g)).contains("source_quest_id")) continue;
            quests[chain_end.at(g)]["source_quest_id"] = t.quest; gated.insert(t.quest); at = (at + n + 1) % found->second.size(); break;
        }
        if (!gated.contains(t.quest)) std::cerr << "gate quest " << t.quest << " (loc " << uid << ") has no questgiver chain in " << t.grid->name << '\n';
    }
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
        {17066, "karakoy", 0},        // Yol Savaşçısı (ID_ZICAN_DIS, level 49)
        {17071, "cemberli", 0},       // Junon (level 49): shares skill group 33 with the GBM 70/71 machines of the Çemberlitaş complex
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
    std::map<std::string, std::set<int32_t>> entries; // components players arrive on: the zone spawn and every door arrival
    for (const auto& [name, s] : spawns) entries[name].insert(grids.at(name).component(s.first, s.second));
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
    // NONCANONICAL: later-tier Tılsım without a recipe scroll (drop tables were server data) join the Şef tables in turn
    // (guide: Şefs drop important items).
    std::set<std::string> crafted; for (const auto& [id, r] : recipes.items()) crafted.insert(r.at("product").get<std::string>());
    std::vector<Ordered*> chiefs_in_order; for (auto& n : npcs) if (n.value("boss", false)) chiefs_in_order.push_back(&n);
    size_t turn = 0;
    for (const auto& id : later_glyphs) if (!crafted.contains(id) && !chiefs_in_order.empty()) {
        for (size_t tries = 0; tries < chiefs_in_order.size(); ++tries, ++turn) if (auto& d = (*chiefs_in_order[turn % chiefs_in_order.size()])["drops"]; d.size() < 16) { d.push_back(id); ++turn; break; }
    }
    Ordered defaults = config.at("defaults"); defaults["zone"] = "eminonu"; defaults["x"] = wire(spawns.at("eminonu").first); defaults["y"] = wire(spawns.at("eminonu").second);
    defaults["inventory"] = Ordered::array({Ordered{{"item", "p253"}, {"quantity", 3}}});
    Ordered used = Ordered::object();
    const auto use = [&](const std::string& id) { if (!items.contains(id)) throw std::runtime_error("unknown generated item " + id); used[id] = items.at(id); };
    for (const auto& n : npcs) { for (const auto& id : n.at("shop")) use(id); if (n.contains("drops")) for (const auto& id : n.at("drops")) use(id); }
    use("p253");
    for (const auto& n : npcs) if (n.contains("gather")) for (const char* key : {"item", "rare"}) if (n.at("gather").contains(key)) use(n.at("gather").at(key).get<std::string>());
    for (const auto& [id, r] : recipes.items()) { use(id); use(r.at("product").get<std::string>()); for (const auto& c : r.at("components")) use(c.at("item").get<std::string>()); }
    for (const auto& [id, q] : quests.items()) { for (const auto& v : q.at("reward").at("items")) use(v.at("item").get<std::string>()); if (q.at("objective").contains("item")) use(q.at("objective").at("item").get<std::string>()); }
    // Every prefix any catalog item can carry: fixed classes.dat prefix1/prefix2 plus the random drop pools (rarity > 0).
    Ordered used_prefixes = Ordered::object();
    for (const auto& [id, p] : prefix_table.items()) if (p.at("rarity").get<int>() > 0) used_prefixes[id] = p;
    for (const auto& [id, item] : used.items()) if (item.contains("prefixes")) for (const auto& p : item.at("prefixes")) used_prefixes[std::to_string(p.get<int>())] = prefix_table.at(std::to_string(p.get<int>()));
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
