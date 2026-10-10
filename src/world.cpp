#include "ikv/world.hpp"
#include "ikv/scene.hpp"
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <ctime>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <queue>
#include <unordered_map>

namespace ikv {
namespace {
struct Rejected { std::string code, message; };
struct TradePersistenceFailure { uint32_t peer; uint64_t revision; };
bool critical_event(const Json& value) {
    const auto& type=value.at("type");
    return type=="trade" || type=="trade_invite" || type=="trade_complete" || type=="trade_cancel" || type=="kick" ||
           (type=="error" && value.value("code","")=="trade_persistence");
}
void require(bool ok, const char* code, const char* message) { if (!ok) throw Rejected{code, message}; }
int integer(const Json& value, int minimum, int maximum) {
    require(value.is_number_integer(), "request", "Tam sayı gerekli.");
    const auto n = value.get<int64_t>();
    require(n >= minimum && n <= maximum, "quantity", "Sayı izin verilen aralığın dışında.");
    return static_cast<int>(n);
}
uint32_t target_id(const Json& value) {
    require(value.is_number_integer(),"request","Hedef belirtilmeli.");
    const auto number=value.get<int64_t>();
    require(number>0 && number<=UINT32_MAX,"target","Geçersiz hedef.");
    return static_cast<uint32_t>(number);
}
std::string text(const Json& value, size_t maximum = 64) {
    require(value.is_string(), "request", "Metin gerekli.");
    auto s = value.get<std::string>();
    require(!s.empty() && s.size() <= maximum && s.find('\0') == std::string::npos, "request", "Metin uzunluğu geçersiz.");
    return s;
}
int32_t coordinate(const Json& state, const char* key) { return state.at(key).get<int32_t>(); }
int64_t distance(int32_t x, int32_t y, int32_t ox, int32_t oy) {
    int64_t dx = int64_t(x) - ox, dy = int64_t(y) - oy; return dx * dx + dy * dy;
}
std::string new_uid() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1) throw std::runtime_error("item identifier generation failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string value(32, '0');
    for (size_t i = 0; i < bytes.size(); ++i) { value[2*i] = hex[bytes[i] >> 4]; value[2*i+1] = hex[bytes[i] & 15]; }
    return value;
}
uint32_t random_below(uint32_t n) { // n > 0
    uint32_t value; if (RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value)) != 1) throw std::runtime_error("random roll failed");
    return value % n;
}
// NONCANONICAL item tuning (gameplay.json item_rules); absent disables drops, wear and Efsun scaling.
int item_rule(const Json& config, const char* key) { return config.contains("item_rules") ? config.at("item_rules").at(key).get<int>() : 0; }
int sign(int64_t value) { return (value > 0) - (value < 0); }
std::pair<int32_t,int32_t> toward(int32_t x,int32_t y,int32_t gx,int32_t gy,int step,bool diagonal) {
    const int64_t dx=int64_t(gx)-x,dy=int64_t(gy)-y;
    if(!diagonal) {
        if(std::abs(dx)>=std::abs(dy))x+=sign(dx)*static_cast<int>(std::min<int64_t>(step,std::abs(dx)));
        else y+=sign(dy)*static_cast<int>(std::min<int64_t>(step,std::abs(dy)));
        return {x,y};
    }
    const double length=std::hypot(double(dx),double(dy));
    if(length<=step)return {gx,gy};
    // Truncation keeps each independently chosen authority step within its speed budget.
    return {x+int32_t(double(dx*step)/length),y+int32_t(double(dy*step)/length)};
}
int buff_sum(const Json& buffs, const char* key) { int sum = 0; for (const auto& buff : buffs) sum += buff.value(key, 0); return sum; }
// Effect icons as the original frames draw them (0x479840 list, oldest first): skill id, expiry tick and the skills.dat
// [classindex, skillno] that picks its icon (the caster's class, which may differ from the bearer's catalog).
Json shown_effects(const Json& list) {
    Json shown = Json::array();
    for (const auto& b : list) { Json e{{"id",b.at("id")},{"expires",b.at("expires")}}; if (b.contains("skill_source")) e["skill_source"] = b.at("skill_source"); shown.push_back(std::move(e)); }
    return shown;
}
bool buff_flag(const Json& buffs, const char* key) { return std::any_of(buffs.begin(), buffs.end(), [&](const Json& buff) { return buff.value(key, false); }); }
// Skill pace: Depar/Konsantrasyon/Sarsılmaz on players; Dikkat Dağıtma slow and Durdurma-style stuns on NPCs.
int move_percent(const Json& buffs) { return buff_flag(buffs, "stun") ? 0 : std::clamp(100 + buff_sum(buffs, "speed"), 0, 1000); }
// NONCANONICAL skill point budget: base + per_level*(level-1), minus invested ranks.
int unspent_skill_points(const Json& config, const Json& s) {
    int spent = 0; for (const auto& [skill,points] : s.at("skills").items()) { (void)skill; spent += points.get<int>(); }
    return config.at("skill_points_base").get<int>() + config.at("skill_points_per_level").get<int>()*(s.at("level").get<int>()-1) - spent;
}
// Hızlı Erişim Çubuğu (original 0044b330 'tdbp' upload / hcpe 0044b400 download): ten slots of null, {"skill":id} or
// {"item":uid} (the original 28-byte slot kept a bag item's identity). Unknown skills clear their slot.
Json quickbar_slots(const Json& config, const Json& slots) {
    require(slots.is_array() && slots.size() == 10, "quickbar", "Hızlı Erişim Çubuğu on hücreden oluşmalı.");
    Json result = Json::array();
    for (const auto& slot : slots) {
        if (slot.is_null()) { result.push_back(nullptr); continue; }
        require(slot.is_object() && slot.size() == 1, "quickbar", "Hızlı Erişim Çubuğu hücresi geçersiz.");
        if (slot.contains("skill")) { const auto skill = text(slot.at("skill"), 32); result.push_back(config.at("skills").contains(skill) ? slot : Json()); continue; }
        const auto uid = text(slot.at("item"), 32);
        require(uid.size() == 32 && std::all_of(uid.begin(), uid.end(), [](char c){ return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }), "quickbar", "Hızlı Erişim Çubuğundaki eşya geçersiz.");
        result.push_back(slot);
    }
    return result;
}
// skills.dat detail-text chances ("5 puanda %%10 Bedelsiz Yetenek şansı"): the last [rank, percent(, value)] row the rank
// reaches is rolled; nullptr when the skill has no such proc, the rank reaches no row or the roll fails.
const Json* proc(const Json& definition, const char* effect, int rank) {
    if (!definition.contains("procs") || !definition.at("procs").contains(effect)) return nullptr;
    const Json* row = nullptr; for (const auto& r : definition.at("procs").at(effect)) if (r[0].get<int>() <= rank) row = &r;
    return row && random_below(100) < (*row)[1].get<uint32_t>() ? row : nullptr;
}
// Karakter Bilgisi Dirençler (resistances) and Üretim/Toplama (crafts) point pools; order matches the original window rows.
constexpr std::array<const char*,5> resistance_stats{"ates","buz","elektrik","asit","zehir"};
constexpr std::array<const char*,6> craft_stats{"silahtar","zirhci","kimyaci","madenci","sarraf","lokman"};
// Original crafttype index -> name table at 0x5d6150 (strings 0x58d424..0x58d3d4); classes.dat crafttype indexes it.
constexpr std::array<const char*,6> craft_names{"Silahtar","Zırhçı","Kimyacı","Madenci","Sarraf","Lokman"};
bool craft_stat(const std::string& stat) { return std::find(craft_stats.begin(),craft_stats.end(),std::string_view(stat)) != craft_stats.end(); }
constexpr size_t quest_active_max = 20; // NONCANONICAL cap on accepted, unfinished quests (catalog bound and Görev al)
int quest_count(const Json& q) { return q.at("objective").at("kind") == "talk" ? 1 : q.at("objective").at("count").get<int>(); }
bool quest_finished(const Json& s, const std::string& quest) { const auto& d = s.at("quests_done"); return std::binary_search(d.begin(), d.end(), Json(quest)); }
int carried(const Json& s, const Json& item) { int n = 0; for (const auto& e : s.at("inventory")) if (e.at("item") == item) n += e.at("quantity").get<int>(); return n; }
// A clan's Sancak (banner NPC with "clan") can only be struck by a member of another clan.
bool shielded(const Json& npc, const Json& s) { return npc.contains("clan") && (!s.value("clan",0) || s.value("clan",0) == npc.at("clan").get<int>()); }
// Guide "Klan OOK'leri": clan NPCs (recruiters, merchants, arena guardians; non-enemy kinds with combat stats "aggro") attack the
// other clan's players and may be killed by them. An NPC this player may strike: an enemy, or such a guard, outside its shield.
bool guard(const Json& npc) { return npc.at("kind") != "enemy" && npc.contains("aggro") && npc.contains("clan"); }
bool strikable(const Json& npc, const Json& s) { return (npc.at("kind") == "enemy" || guard(npc)) && !shielded(npc, s); }
// NONCANONICAL pool: per_level*(level-1), minus invested points (VOD level 2: Dirençler Puan 5, Üretim Puan 2).
int unspent_points(const Json& config, const Json& s, const char* group) {
    int spent = 0; for (const auto& [stat,points] : s.at(group).items()) { (void)stat; spent += points.get<int>(); }
    return config.at("character_points").at(group == std::string_view("resistances") ? "resistance_per_level" : "craft_per_level").get<int>()*(s.at("level").get<int>()-1) - spent;
}
// GBM-0 / GBM 0-B (guide): "seviyenize göre değişen belli bir miktar para", Plus Paket holders pay 1/3. NONCANONICAL: the
// point_reset Lira per level (the official price table was server data).
int64_t point_reset_price(const Json& config, const Json& machine, const Json& s, bool plus) {
    const int64_t full = config.at("point_reset").at(machine.at("reset").get<std::string>()).get<int64_t>()*s.at("level").get<int64_t>();
    return plus ? full/3 : full;
}
constexpr const char* gbm0b_refusal = "GBM 0-B'yi kullanabilmek için Plus Paket sahibi olmanız gerekir."; // NONCANONICAL wording
// Original attribute ids (names 004338f0) per element in resistance-row order: Hasar(Ateş..Asit) 48-52, Direnç(Ateş..Asit) 12-16;
// skill schools in the same order. classes.dat monster resistances are emitted in this order too.
constexpr std::array<int,5> element_damage{48,49,50,52,51}, element_resist{12,13,14,16,15};
constexpr std::array<std::string_view,5> element_schools{"fire","ice","electric","acid","poison"};
// gameplay.json "formulas": VOD-fitted level curves and attributes over the original client formulas (00402f60).
const Json& formulas(const Json& config) { return config.at("formulas"); }
// Güç/Beceri at a level (server-sent attributes 8/9 read by 00402f60): the VOD samples, then the fitted line a + b·L.
double attribute_curve(const Json& curve, int level) {
    if (const auto& v = curve.at("levels"); level <= int(v.size())) return v[size_t(level-1)].get<double>();
    const auto& l = curve.at("line"); return l[0].get<double>() + l[1].get<double>()*level;
}
// Tılsım (classes.dat glyph; the client only shows "[Tılsım]" and the detail, the effect was server-only). NONCANONICAL: the best
// carried Tılsım of each kind (and proc field) modifies the skill definition used for this cast (see tools/generate_content.cpp
// glyph_rules); character-state kinds are read through glyph() where their stat is computed.
Json glyphed(const Json& config, const Json& s, const std::string& skill) {
    Json d = config.at("skills").at(skill); std::map<std::pair<std::string,std::string>,int> best;
    for (const auto& entry : s.at("inventory")) {
        const auto& item = config.at("items").at(entry.at("item").get<std::string>());
        if (!item.contains("glyph")) continue;
        const auto& g = item.at("glyph");
        if (std::find(g.at("skills").begin(),g.at("skills").end(),Json(skill)) == g.at("skills").end()) continue;
        auto& b = best[{g.at("kind").get<std::string>(),g.value("field","")}]; b = std::max(b,g.at("value").get<int>());
    }
    const auto scale = [&](const char* key, int percent) { if (d.contains(key)) d[key] = std::max(1,d.at(key).get<int>()*(100+percent)/100); };
    const auto grow = [](Json& e, const char* key, int percent, int cap) { if (e.contains(key)) e[key] = std::min(cap,e.at(key).get<int>()*(100+percent)/100); };
    const auto rows = [&](const char* rider, const auto& change) { if (d.contains(rider)) for (auto& row : d[rider]) change(row[1]); };
    for (const auto& [key,v] : best) {
        const auto& [kind,field] = key;
        if (kind == "damage") for (const char* k : {"damage","dot","heal","hot"}) scale(k,v);
        else if (kind == "attack") scale("attack",v);
        else if (kind == "duration") { scale("duration",v); rows("on_hit",[&](Json& e){ grow(e,"duration",v,72000); }); } // Buz Oku 2: its slow lives in on_hit rows
        else if (kind == "mana") d["mana"] = d.at("mana").get<int>()*(100-std::min(v,100))/100;
        else if (kind == "cooldown") d["cooldown"] = std::max(1,d.at("cooldown").get<int>()-v*20); // seconds at 20 Hz
        // Riders: proc chances +V points; slow/vulnerable/defense_break ×(100+V)%; crit/hit rider timings (V seconds) and amounts.
        else if (kind == "proc") { if (d.contains("procs") && d.at("procs").contains(field)) for (auto& r : d["procs"][field]) r[1] = std::min(100,r[1].get<int>()+v); }
        else if (kind == "slow") { grow(d,"slow",v,100); rows("on_hit",[&](Json& e){ grow(e,"slow",v,100); }); }
        else if (kind == "vulnerable") grow(d,"vulnerable",v,1000);
        else if (kind == "defense_break") rows("on_hit",[&](Json& e){ grow(e,"defense_break",v,1000); });
        else if (kind == "crit_duration") rows("on_crit",[&](Json& e){ grow(e,"duration",v,72000); });
        else if (kind == "crit_seconds") rows("on_crit",[&](Json& e){ e["duration"] = std::max(1,v*20); });
        else if (kind == "crit_armor_break") rows("on_crit",[&](Json& e){ e["armor_break"] = std::min(v,100); });
        else if (kind == "hit_seconds") rows("on_hit",[&](Json& e){ e["duration"] = std::max(1,v*20); });
        else if (kind == "speed") { if (d.contains("speed")) d["speed"] = std::min(0,d.at("speed").get<int>()+v); } // Konsantrasyon: move V % again
        else if (kind == "wrath_heal") { // healing allowed under Gazap at V % of its amounts
            d["wrath_heal"] = v;
            if (buff_flag(s.at("buffs"),"wrath")) for (const char* k : {"heal","hot","absorb"}) if (d.contains(k)) d[k] = std::max(1,d.at(k).get<int>()*v/100);
        }
        else if (kind == "redirect") d["redirect_boost"] = v; // Hedef Saptırma moves ×(100+V)% threat (`redirect` is the skill's flag)
        else if (kind == "self_stun" || kind == "chain" || kind == "silenced_damage" || kind == "crit_refund" || kind == "electric_damage" || kind == "heal_cut" || kind == "circle_heal") d[kind] = v; // read by the cast
    }
    return d;
}
// Best value of a carried Tılsım of `kind` for `skill`, −1 when none (character-state riders: armor_energy, armor_dodge,
// stance_damage, crit_armor, stance_crit, efsun_boost).
int glyph(const Json& config, const Json& s, const char* skill, const char* kind) {
    int best = -1;
    for (const auto& entry : s.at("inventory")) {
        const auto& item = config.at("items").at(entry.at("item").get<std::string>());
        if (!item.contains("glyph") || item.at("glyph").at("kind") != kind) continue;
        const auto& skills = item.at("glyph").at("skills");
        if (std::find(skills.begin(),skills.end(),Json(skill)) != skills.end()) best = std::max(best,item.at("glyph").at("value").get<int>());
    }
    return best;
}
// Enerji/Kudret maximum at a level: the VOD samples, then the fitted quadratic a + b·L + c·L².
int resource_curve(const Json& curve, int level) {
    if (const auto& v = curve.at("levels"); level <= int(v.size())) return v[size_t(level-1)].get<int>();
    const auto& q = curve.at("quadratic"); return int(std::lround(q[0].get<double>()+q[1].get<double>()*level+q[2].get<double>()*level*level));
}
// XP from a level to the next: xp_first·xp_growth^(level−1) (VOD Puan 424/1500 at level 1, 1811/3183 at level 2), capped at
// the 10,000,000 per-level state bound.
int xp_needed(const Json& config, int level) {
    const auto& f = formulas(config);
    return int(std::min(1e7, std::round(f.at("xp_first").get<double>()*std::pow(f.at("xp_growth").get<double>(), level-1))));
}
int64_t xp_before(const Json& config, int level) { int64_t total = 0; for (int l = 1; l < level; ++l) total += xp_needed(config,l); return total; }
double uniform(double low, double high) { return low + (high-low)*random_below(1000001)/1e6; }
bool active(const Json& buffs, const char* skill) { return std::any_of(buffs.begin(), buffs.end(), [&](const Json& b) { return b.value("id","") == skill; }); }
}

World::World(Json config, std::function<void(std::span<const CharacterSave>)> persist)
    : config_(std::move(config)), persist_(std::move(persist)) {
    try {
        require(bool(persist_), "config", "Persistence callback required");
        require(config_.at("provenance").get<std::string>().find("NONCANONICAL") != std::string::npos, "config", "Provenance required");
        require(config_.at("tick_hz") == 20, "config", "World tick rate must be 20Hz");
        integer(config_.at("movement_step"), 1, 10000);
        { // Combat/progression formulas (see formulas()): numbers within sane bounds, curve tables 1..100 levels.
            const auto& f = formulas(config_);
            const auto real = [](const Json& v, double lo, double hi) { require(v.is_number() && v.get<double>() >= lo && v.get<double>() <= hi, "config", "Formula value outside allowed range"); };
            real(f.at("xp_first"), 1, 1e6); real(f.at("xp_growth"), 1, 2);
            for (const char* key : {"guc","beceri"}) {
                const auto& c = f.at(key); require(c.at("levels").is_array() && c.at("levels").size() <= 100 && c.at("line").is_array() && c.at("line").size() == 2, "config", "Attribute curve invalid");
                for (const auto& v : c.at("levels")) real(v, 0, 1000);
                for (const auto& v : c.at("line")) real(v, 0, 1000);
            }
            for (const char* key : {"class_attack","class_defense"}) { require(f.at(key).is_array() && f.at(key).size() == 3, "config", "Class factors invalid"); for (const auto& v : f.at(key)) real(v, 0, 100); }
            for (const char* key : {"hp","mana"}) {
                const auto& c = f.at(key); require(c.at("levels").is_array() && !c.at("levels").empty() && c.at("levels").size() <= 100 && c.at("quadratic").is_array() && c.at("quadratic").size() == 3, "config", "Resource curve invalid");
                for (const auto& v : c.at("levels")) integer(v, 1, config_.at("resource_cap"));
                for (const auto& v : c.at("quadratic")) real(v, -1e5, 1e5);
                for (int level = 1; level <= 100; ++level) require(resource_curve(c, level) >= 1, "config", "Resource curve not positive");
            }
            const auto& hit = f.at("hit_percent"); require(hit.is_array() && hit.size() == 2 && integer(hit[0], 0, 100) <= integer(hit[1], 0, 100), "config", "Hit chance bounds invalid");
        }
        // Wire distances: 1000 per source scene unit, and a source actor stands ~7500 tall.
        for (const char* key : {"cell_size","aoi","interaction_range","combat_range","trade_range"}) integer(config_.at(key), 1, 200000);
        integer(config_.at("inventory_slots"), 1, 32); integer(config_.at("currency_cap"), 1, 999000000); // "En fazla 999 milyon paranız olabilir." (0x5927f8)
        integer(config_.at("resource_cap"),1,1000000);
        integer(config_.at("death_currency_percent"), 0, 100);
        integer(config_.at("death_resurrect_delay"), 0, 72000);
        if (config_.contains("auto_retaliate")) require(config_.at("auto_retaliate").is_boolean(), "config", "auto_retaliate must be a boolean");
        if (config_.contains("regeneration")) { // NONCANONICAL passive out-of-combat regeneration (row 31)
            const auto& r = config_.at("regeneration"); integer(r.at("interval"), 1, 72000); integer(r.at("hp"), 0, 10000); integer(r.at("mana"), 0, 10000);
        }
        // NONCANONICAL day clock + environment records (original server 'thgf' 0x004a4c90: hour + byte record).
        if (config_.contains("clock")) {
            const auto& clock = config_.at("clock"); integer(clock.at("start_hour"), 0, 23); integer(clock.at("rate"), 0, 1000);
            const auto& records = clock.at("records"); require(records.is_array() && !records.empty() && records.size() <= 24, "config", "Clock records required");
            for (std::size_t i = 0; i < records.size(); ++i) {
                require(records[i].is_array() && records[i].size() == 12, "config", "Clock record is [hour, ambient rgb, sun rgb, horizon rgb, shadow, glow]");
                integer(records[i][0], i ? records[i-1][0].get<int>() + 1 : 0, 23);
                for (std::size_t k = 1; k < 12; ++k) integer(records[i][k], 0, 255);
            }
            // NONCANONICAL fog start/end distances (original thgf 004a4680 params 6/7 = client record +0xec/+0xf0).
            const auto& fog = clock.at("fog"); require(fog.is_array() && fog.size() == 2, "config", "Clock fog is [start, end]");
            integer(fog[1], integer(fog[0], 0, 1000000), 1000000);
        }
        require(config_.at("vendor_return_denominator") == 5 && config_.at("vendor_plus_return_denominator") == 3, "config", "Guide vendor ratios required");
        require(config_.at("zones").is_object() && config_.at("zones").size() > 0 && config_.at("zones").size() <= 32, "config", "Zone count invalid");
        for (const auto& [name, zone] : config_.at("zones").items()) {
            text(name, 32); const auto& b = zone.at("bounds"); require(b.is_array() && b.size() == 4, "config", "Bounds required");
            for (const auto& n : b) integer(n, -coordinate_limit, coordinate_limit);
            require(b[0] < b[2] && b[1] < b[3], "config", "Ordered bounds required");
            if (zone.contains("source_registry_id")) {
                integer(zone.at("source_registry_id"),0,70);
                require(b==Json::array({-coordinate_limit,-coordinate_limit,coordinate_limit-1500,coordinate_limit-1500}),"config","Source bounds must match grid-center domain");
            }
            require(zone.at("collision").is_array() && zone.at("collision").size() <= 64, "config", "Collision limit");
            for (const auto& box : zone.at("collision")) {
                require(box.is_array() && box.size() == 4, "config", "Collision rectangle required");
                for (const auto& n : box) integer(n, -coordinate_limit, coordinate_limit);
                require(box[0] <= box[2] && box[1] <= box[3], "config", "Collision ordering");
            }
            require(zone.at("spawn").is_array() && zone.at("spawn").size() == 2, "config", "Spawn required");
            require(passable(name, integer(zone.at("spawn")[0], -coordinate_limit, coordinate_limit), integer(zone.at("spawn")[1], -coordinate_limit, coordinate_limit)), "config", "Spawn blocked");
            // Instanced copies (original registry rows "migrat1".."migrat9" etc. share the zone's scene and support grid):
            // NONCANONICAL per-group instances, cloned below as "<zone><n>" with their own NPC population.
            if (zone.contains("instances")) {
                require(zone.contains("source_registry_id") && zone.at("instances").is_array() && zone.at("instances").size() <= 20, "config", "Instance list invalid");
                for (const auto& registry : zone.at("instances")) integer(registry, 0, 70);
            }
        }
        require(config_.at("items").is_object() && config_.at("items").size() <= 1024 && config_.at("quests").is_object() && config_.at("quests").size() <= 1024, "config", "Catalog limit");
        require(config_.at("skills").is_object() && config_.at("skills").size() <= 64, "config", "Catalog limit");
        for (const char* key : {"skill_points_base","skill_points_per_level","skill_rank_max","skill_rank_percent","passive_rank_percent","skill_global_cooldown"}) integer(config_.at(key), 0, 1000);
        for (const char* key : {"resistance_per_level","craft_per_level","charisma"}) integer(config_.at("character_points").at(key), 0, 1000);
        // Efsun prefixes (generated from prefixes.dat): attribute ids are the client's character attribute ids
        // (names 004338f0, Karakter Bilgisi getters 00402f60). item_rules holds the NONCANONICAL drop/wear/repair/Efsun tuning.
        if (!config_.contains("prefixes")) config_["prefixes"] = Json::object();
        require(config_.at("prefixes").is_object() && config_.at("prefixes").size() <= 2048, "config", "Prefix catalog invalid");
        for (const auto& [id, p] : config_.at("prefixes").items()) {
            require(!id.empty() && id.size() <= 6 && std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; }), "config", "Prefix id invalid");
            text(p.at("name"), 128); integer(p.at("index"), 0, 3); integer(p.at("level"), 1, 100); integer(p.at("rarity"), 0, 1000000); integer(p.at("flag"), 0, 255);
            if (p.contains("class")) integer(p.at("class"), 0, 2);
            const auto& attributes = p.at("attributes"); require(attributes.is_array() && !attributes.empty() && attributes.size() <= 8, "config", "Prefix attributes invalid");
            for (const auto& a : attributes) { require(a.is_array() && a.size() == 2 && a[1].is_number() && std::abs(a[1].get<double>()) <= 100000, "config", "Prefix attribute invalid"); integer(a[0], 0, 127); }
        }
        if (config_.contains("item_rules")) for (const char* key : {"drop_percent","prefix_percent","second_prefix_percent","hit_wear_permille","death_wear_permille","repair_denominator","efsun_percent"}) integer(config_.at("item_rules").at(key), 0, 1000);
        for (const auto& [id, item] : config_.at("items").items()) {
            // classes.dat price is Lira; pvpprice ("Fiyat: %s Lira / %s Klan Parası", tooltip 0x438422) is clan_price; akceprice
            // (item +0x88, "Fiyat: %s Akçe" 0x438326) akce_price; minloncalevel (item +0x38) guild_level.
            if (item.contains("clan_price")) integer(item.at("clan_price"), 1, 999000000);
            if (item.contains("akce_price")) integer(item.at("akce_price"), 1, 999000000);
            if (item.contains("guild_level")) integer(item.at("guild_level"), 1, 100);
            if (item.contains("detail")) text(item.at("detail"), 256); // classes.dat detail (Tılsım tooltip text)
            if (item.contains("flags")) integer(item.at("flags"),0,255); // classes.dat itemflag (bits 2/4: unsellable / bound)
            if (item.contains("glyph")) {
                static constexpr std::string_view kinds[]{"damage","attack","duration","mana","cooldown","proc","slow","vulnerable","defense_break","crit_duration","crit_seconds",
                    "crit_armor_break","hit_seconds","speed","self_stun","chain","silenced_damage","crit_refund","electric_damage","heal_cut","circle_heal","wrath_heal","redirect",
                    "armor_energy","armor_dodge","stance_damage","crit_armor","stance_crit","efsun_boost"};
                const auto& g = item.at("glyph"); const auto kind = text(g.at("kind"), 16);
                require(std::find(std::begin(kinds),std::end(kinds),kind) != std::end(kinds), "config", "Glyph kind invalid");
                require(g.at("skills").is_array() && !g.at("skills").empty() && g.at("skills").size() <= 8, "config", "Glyph skills invalid");
                for (const auto& s : g.at("skills")) require(config_.at("skills").contains(text(s, 32)), "config", "Glyph skill missing");
                integer(g.at("value"), 0, 1000);
                if (kind == "proc") { const auto field = text(g.at("field"), 16); require(field == "free" || field == "silence" || field == "multiply" || field == "cleanse_all", "config", "Glyph proc field invalid"); }
            }
            text(id, 32); text(item.at("name"), 128); integer(item.at("stack"), 1, 1000); integer(item.at("price"), item.contains("clan_price") || item.contains("akce_price") ? 0 : 1, 999000000); integer(item.at("level"), 1, 100);
            require(Json(id).dump().size()<=34 && item.at("name").dump().size()<=64,"config","Item identifier/name exceeds trade schema envelope");
            for (const char* key : {"damage", "heal", "mana", "armor"}) if (item.contains(key)) integer(item.at(key), 0, 10000);
            if (item.contains("attack_range")) integer(item.at("attack_range"), 1, 200000);
            if (item.contains("damage_range")) { const auto& r=item.at("damage_range"); require(r.is_array() && r.size()==2 && r[0].is_number() && r[1].is_number() && r[0].get<double>() >= 0 && r[0].get<double>() <= r[1].get<double>() && r[1].get<double>() <= 10000,"config","Item damage range invalid"); }
            if (item.contains("attack_type")) { const auto t = text(item.at("attack_type"), 3); require(t.find_first_not_of("KDE") == std::string::npos, "config", "Item attack type invalid"); } // Kesici/Delici/Ezici
            if (item.contains("attack_cooldown")) integer(item.at("attack_cooldown"), 1, 10000);
            if (item.contains("slot")) text(item.at("slot"), 32);
            // Original classes.dat classlimit (0-2 = class_id-1) and sexlimit (appearance sex) gate equipping.
            if (item.contains("class")) integer(item.at("class"), 0, 2);
            if (item.contains("sex")) integer(item.at("sex"), 0, 1);
            // source_uid alone selects the original icon; source_slot additionally dresses the original model.
            if(item.contains("source_uid")) target_id(item.at("source_uid"));
            if(item.contains("source_slot")) { require(item.contains("source_uid") && item.contains("slot"),"config","Visual equipment needs UID and slot");integer(item.at("source_slot"),0,15); }
            if (item.contains("durability")) { require(item.contains("slot"), "config", "Durability needs equipment"); integer(item.at("durability"), 1, 10000); }
            if (item.contains("crit")) { const auto& c = item.at("crit"); require(c.is_array() && c.size() == 2, "config", "Item crit invalid"); integer(c[0], 0, 100); integer(c[1], 1, 10); }
            if (item.contains("prefixes")) { const auto& list = item.at("prefixes"); require(item.contains("slot") && list.is_array() && list.size() <= 2, "config", "Item prefixes invalid"); for (const auto& p : list) require(config_.at("prefixes").contains(std::to_string(integer(p, 1, 999999))), "config", "Item prefix missing"); }
        }
        // Skills mirror original data/skills.dat identities (source = classindex/skillno, class_id = classindex+1);
        // target/level/cooldown/duration come from its detail text, magnitudes are NONCANONICAL.
        for (const auto& [id, skill] : config_.at("skills").items()) {
            text(id, 32); text(skill.at("name"), 128); integer(skill.at("level"), 1, 100);
            const auto& source = skill.at("source"); require(source.is_array() && source.size() == 2, "config", "Skill source identity required");
            integer(source[0], 0, 2); integer(source[1], 0, 15);
            // Boz Ayı (skillno 15) is not a slot of its own: exe 0044ccf0/004a6720 swap it into the Savaşçı's slot 8 under a
            // per-character flag, so it shares that slot's rank ("replaces").
            if (skill.contains("replaces")) { const auto& other = config_.at("skills").at(text(skill.at("replaces"), 32)); require(other.at("source")[0] == source[0] && !other.contains("replaces"), "config", "Skill variant must replace a skill of its class"); }
            else require(source[1].get<int>() < 15, "config", "Skill slot 15 needs a replaced skill");
            // Detail-text chances ("5 puanda %%10 ..."): effect -> rows [rank, percent(, value)].
            if (skill.contains("procs")) for (const auto& [effect, rows] : skill.at("procs").items()) {
                require((effect == "free" || effect == "stun" || effect == "multiply" || effect == "dot" || effect == "silence" || effect == "drain" || effect == "cleanse_all") && rows.is_array() && !rows.empty() && rows.size() <= 8, "config", "Skill proc invalid");
                for (const auto& row : rows) { require(row.is_array() && row.size() >= 2 && row.size() <= 3, "config", "Skill proc row invalid"); integer(row[0], 1, 15); integer(row[1], 0, 100); if (row.size() == 3) integer(row[2], 1, 10000); }
            }
            // Skill crits: `crit` [from rank, percent per point], `crit_vulnerable` percent; riders `on_hit`/`on_crit`: rows
            // [rank, {duration, slow?, stun?, armor_break?, defense_break?}] (detail-text effects of a damaging hit / a crit;
            // defense_break: Savunma points off, Kışkırtma 15 puan "rakibin savunmasını 20 düşürür").
            if (skill.contains("crit")) { const auto& c = skill.at("crit"); require(c.is_array() && c.size() == 2, "config", "Skill crit invalid"); integer(c[0], 1, 15); integer(c[1], 0, 100); }
            if (skill.contains("crit_vulnerable")) integer(skill.at("crit_vulnerable"), 0, 100);
            for (const char* key : {"on_hit","on_crit"}) if (skill.contains(key)) {
                require(skill.at(key).is_array() && !skill.at(key).empty() && skill.at(key).size() <= 8, "config", "Skill rider invalid");
                for (const auto& row : skill.at(key)) {
                    require(row.is_array() && row.size() == 2 && row[1].is_object(), "config", "Skill rider row invalid"); integer(row[0], 1, 15);
                    const auto& e = row[1]; integer(e.at("duration"), 1, 72000);
                    for (const auto& [k, v] : e.items()) require(k == "duration" || (k == "stun" && v.is_boolean()) || ((k == "slow" || k == "armor_break") && integer(v, 0, 100) >= 0) || (k == "defense_break" && integer(v, 0, 1000) >= 0), "config", "Skill rider effect invalid");
                }
            }
            if (skill.contains("cleanse")) integer(skill.at("cleanse"), 0, 2); // the skills.dat class whose harmful effects it removes
            // Kutup Rüzgarı "Kendisi dahil ... buz içinde hapis kalır": the caster is stunned for self_stun % of the duration.
            if (skill.contains("self_stun")) { integer(skill.at("self_stun"), 0, 100); require(skill.contains("duration"), "config", "Self stun needs duration"); }
            const auto target = text(skill.at("target"), 16);
            require(target == "self" || target == "enemy" || target == "ally" || target == "enemies" || target == "allies" || target == "passive" || target == "summon", "config", "Skill target invalid");
            if (target == "passive") { text(skill.at("boosts"), 16); continue; }
            if (target == "summon") {
                const auto& s = skill.at("summon"); require(s.at("models").is_array() && !s.at("models").empty() && s.at("models").size() <= 8, "config", "Summon models invalid");
                for (const auto& m : s.at("models")) { require(m.is_array() && m.size() == 3, "config", "Summon model invalid"); target_id(m[0]); integer(m[1], 1, 65535); text(m[2], 32); } // [uid, classid, name]
                for (const char* key : {"attack_cooldown","attack_range","aggro","speed","hp_percent"}) integer(s.at(key), 1, 100000);
                integer(s.at("per_rank"), 0, 1000); if (s.contains("bleed")) integer(s.at("bleed"), 1, 72000);
                if (s.contains("hp_bonus")) for (const auto& row : s.at("hp_bonus")) { integer(row.at(0), 1, 15); integer(row.at(1), 0, 1000); }
                require(skill.contains("damage") && skill.contains("duration"), "config", "Summon needs damage and duration");
            }
            integer(skill.at("mana"), 0, 10000); integer(skill.at("cooldown"), 1, 100000);
            if (target == "enemy" || target == "ally") integer(skill.at("range"), 1, 200000);
            if (target == "enemies" || target == "allies") integer(skill.at("radius"), 1, 200000);
            for (const char* key : {"damage","heal","armor","attack","dot","hot","regen","duration"}) if (skill.contains(key)) integer(skill.at(key), 1, 100000);
            for (const char* key : {"speed","slow","power","absorb","vulnerable"}) if (skill.contains(key)) integer(skill.at(key), -100, 1000);
            if (skill.contains("school")) text(skill.at("school"), 16);
            if (skill.contains("stance")) text(skill.at("stance"), 16);
            if (skill.contains("requires")) require(config_.at("skills").contains(text(skill.at("requires"), 32)), "config", "Required stance skill missing");
            const bool hostile = target == "enemy" || target == "enemies";
            const bool effect = hostile
                ? skill.contains("damage") || skill.contains("dot") || skill.contains("slow") || skill.contains("vulnerable") || skill.value("stun", false) || skill.value("taunt", false)
                : skill.contains("heal") || skill.contains("hot") || skill.contains("armor") || skill.contains("attack") || skill.contains("speed") || skill.contains("regen") ||
                  skill.contains("power") || skill.contains("absorb") || skill.value("revive", false) || skill.contains("cleanse") || skill.value("redirect", false) || skill.value("immune", false);
            require(effect || target == "summon", "config", "Skill effect required");
            require(!(skill.contains("dot") || skill.contains("slow") || skill.contains("vulnerable") || skill.contains("hot") || skill.value("stun", false)) || skill.contains("duration"), "config", "Timed skill effect needs duration");
        }
        const auto validate_items = [&](const Json& values) {
            require(values.is_array() && values.size() <= 16, "config", "Item list invalid");
            for (const auto& v : values) { require(config_.at("items").contains(text(v.at("item"), 32)), "config", "Unknown item"); integer(v.at("quantity"), 1, 1000); }
        };
        require(config_.at("npcs").is_array() && config_.at("npcs").size() <= 1024, "config", "NPC limit");
        for (const auto& definition : config_.at("npcs")) {
            auto id = target_id(definition.at("id")); require(id > INT32_MAX && !npcs_.contains(id), "config", "NPC IDs must be unique high uint32 identifiers");
            // node: a classes.dat <loc> gathering node (Toplama); anvil: a <loc> örs; door: a <loc> door/crystal/barrier/elevator
            // that moves its user to "door.to"/"door.arrive" (original: entity type 3, cursor_interact, click sends 'nchr');
            // reset: a <loc> GBM-0 ("reset": "skills", resetleyici) or GBM 0-B ("reset": "crafts", uretimsifirlayici) machine.
            const auto kind = text(definition.at("kind")); require(kind == "enemy" || kind == "merchant" || kind == "questgiver" || kind == "node" || kind == "anvil" || kind == "door" || kind == "trap" || kind == "reset", "config", "NPC kind invalid");
            if (kind == "reset") { require(definition.at("reset") == "skills" || definition.at("reset") == "crafts", "config", "Reset machine kind invalid");
                integer(config_.at("point_reset").at(definition.at("reset").get<std::string>()), 1, 1000000); } // NONCANONICAL Lira per level
            if (kind == "door") {
                const auto& door = definition.at("door"); require(config_.at("zones").contains(text(door.at("to"), 32)), "config", "Door zone missing");
                require(door.at("arrive").is_array() && door.at("arrive").size() == 2 && passable(door.at("to"), integer(door.at("arrive")[0], -coordinate_limit, coordinate_limit), integer(door.at("arrive")[1], -coordinate_limit, coordinate_limit)), "config", "Door arrival blocked");
                if (door.contains("required_quest")) integer(door.at("required_quest"), 0, 100000);
                if (door.contains("gmonly")) require(door.at("gmonly").is_boolean(), "config", "Door gmonly flag invalid");
                if (door.contains("required_death")) integer(door.at("required_death"), 1, 65535); // classes.dat requiredentitydeathid (a classid)
            }
            if (kind == "trap") { // fixed classes.dat trap (Labirent Tuzağı): range, damage and rate of its strikes
                const auto& trap = definition.at("trap"); integer(trap.at("range"), 1000, 200000); integer(trap.at("damage"), 1, 100000); integer(trap.at("cooldown"), 1, 100000);
            }
            text(definition.at("name"), 128); const auto zone = text(definition.at("zone"), 32);
            auto x = integer(definition.at("x"), -coordinate_limit, coordinate_limit), y = integer(definition.at("y"), -coordinate_limit, coordinate_limit);
            require(config_.at("zones").contains(zone) && passable(zone, x, y), "config", "NPC position invalid");
            auto hp = integer(definition.at("hp"), 1, 100000); require(definition.at("quests").is_array() && definition.at("shop").is_array(), "config", "NPC catalogs required");
            if(definition.contains("source_uid"))target_id(definition.at("source_uid"));
            if(definition.contains("source_class"))integer(definition.at("source_class"),1,65535); // classes.dat classid reaches 527
            if(definition.contains("appearance"))require(valid_appearance(definition.at("appearance")),"config","NPC appearance invalid");
            if(definition.contains("idlewalktend") && integer(definition.at("idlewalktend"),0,100)) integer(definition.at("walk_speed"),1,100000); // classes.dat idle walk percent, animation.dat walk speed
            for (const auto& item : definition.at("shop")) require(config_.at("items").contains(text(item)), "config", "Shop item missing");
            for (const auto& quest : definition.at("quests")) require(config_.at("quests").contains(text(quest)), "config", "Quest missing");
            // Klan OOK guards (recruiter/merchant/arena guardian with "aggro") carry the same classes.dat combat stats as enemies.
            if (kind == "enemy" || definition.contains("aggro")) {
                if (kind == "enemy") text(definition.at("template"), 32); else require(definition.contains("clan") && kind != "door" && kind != "node" && kind != "trap" && kind != "anvil" && kind != "reset", "config", "Only clan NPCs guard");
                for (const char* key : {"damage","attack_cooldown","aggro","leash","speed","respawn","xp","currency"}) integer(definition.at(key), 1, 100000);
                if (definition.contains("attack_range")) integer(definition.at("attack_range"), 1, 200000);
                // classes.dat monster combat stats (land_npc / NPC bites): attack/defense, dmgmin-dmgmax, criticalchance/scale,
                // dmgredslash/pierce/bludge per attack type and fire/cold/electric/acid/poison resistance points.
                const auto real = [](const Json& v, double lo, double hi) { require(v.is_number() && v.get<double>() >= lo && v.get<double>() <= hi, "config", "NPC combat stat outside allowed range"); };
                for (const char* key : {"attack","defense"}) if (definition.contains(key)) real(definition.at(key), 0, 100000);
                if (definition.contains("damage_range")) { const auto& r = definition.at("damage_range"); require(r.is_array() && r.size() == 2, "config", "NPC damage range invalid"); real(r[0], 0, 100000); real(r[1], r[0].get<double>(), 100000); }
                if (definition.contains("crit")) { const auto& c = definition.at("crit"); require(c.is_array() && c.size() == 2, "config", "NPC crit invalid"); real(c[0], 0, 100); real(c[1], 1, 20); }
                if (definition.contains("dmgred")) { const auto& d = definition.at("dmgred"); require(d.is_object() && d.size() == 3, "config", "NPC damage reduction invalid"); for (const char* t : {"K","D","E"}) real(d.at(t), 0, 1); }
                if (definition.contains("resist")) { const auto& r = definition.at("resist"); require(r.is_array() && r.size() == 5, "config", "NPC resistances invalid"); for (const auto& v : r) real(v, 0, 1000); }
                require(definition.at("leash") >= definition.at("aggro"), "config", "Leash shorter than aggro");
                require(definition.at("patrol").is_array() && definition.at("patrol").size() <= 16, "config", "Patrol invalid");
                for (const auto& point : definition.at("patrol")) require(point.size() == 2 && passable(zone, integer(point[0], -coordinate_limit,coordinate_limit), integer(point[1],-coordinate_limit,coordinate_limit)), "config", "Patrol blocked");
                validate_items(definition.at("loot"));
                // Drops are equipment, plus Tılsım on a Şef (the later-tier glyphs with no recipe scroll, NONCANONICAL).
                if (definition.contains("drops")) { require(definition.at("drops").is_array() && definition.at("drops").size() <= 16, "config", "Drop table invalid"); for (const auto& item : definition.at("drops")) { require(config_.at("items").contains(text(item, 32)), "config", "Drop item missing"); const auto& d = config_.at("items").at(item.get<std::string>()); require(d.contains("slot") || (d.contains("glyph") && definition.value("boss",false)), "config", "Drop item must be equipment"); } }
                // Şef (boss): certain Efsun drop and group-zone lockout; skills = NONCANONICAL casts ("cast") and bite riders ("hit")
                // standing in for its classes.dat normalskillgroup/onhitskillgroup (server skill tables), each a configured skill's effect.
                if (definition.contains("boss")) require(definition.at("boss").is_boolean(), "config", "Boss flag invalid");
                if (definition.contains("skills")) { const auto& list = definition.at("skills");
                    require(definition.value("boss", false) && list.is_array() && list.size() <= 8, "config", "Boss skills invalid");
                    for (const auto& e : list) {
                        require(e.is_object() && config_.at("skills").contains(text(e.at("skill"), 32)) && (e.at("on") == "cast" || e.at("on") == "hit"), "config", "Boss skill invalid");
                        integer(e.at("group"), 0, 1000); integer(e.at("damage"), 0, 100000); integer(e.at("radius"), 0, 200000); integer(e.at("cooldown"), 1, 100000);
                        if (e.contains("duration")) integer(e.at("duration"), 1, 72000);
                    } }
                // Sancak (classes.dat clan banner): stands still, only another clan may strike it, its clan resurrects beside it.
                if (definition.contains("banner")) require(definition.at("banner").is_boolean() && definition.contains("clan"), "config", "Banner needs a clan");
            }
            if (kind != "enemy") text(definition.at("dialogue"), 512);
            if (definition.contains("recruiter")) require(definition.at("recruiter").is_boolean() && definition.contains("clan") && kind != "enemy", "config", "Recruiter needs a clan"); // classes.dat claninviter
            if (definition.contains("no_buy")) require(definition.at("no_buy").is_boolean(), "config", "no_buy flag invalid"); // classes.dat merchant=2
            if (kind == "node") {
                const auto& g = definition.at("gather"); require(craft_stat(text(g.at("craft"),16)),"config","Gather profession invalid");
                integer(g.at("level"),0,1000); integer(g.at("respawn"),1,72000);
                require(config_.at("items").contains(text(g.at("item"),32)) && (!g.contains("rare") || config_.at("items").contains(text(g.at("rare"),32))),"config","Gather item missing");
            }
            Npc n; n.definition = definition; n.x = x; n.y = y; n.hp = hp;
            // Original classes.dat defaults per kind: zıçan rat 16001/9, Agah 1001/16, Agop 1002/17.
            const auto source=kind=="enemy"?std::pair{16001u,9u}:kind=="merchant"?std::pair{1002u,17u}:std::pair{1001u,16u};
            if(!n.definition.contains("source_uid"))n.definition["source_uid"]=source.first;
            if(!n.definition.contains("source_class"))n.definition["source_class"]=source.second;
            npcs_.emplace(id, std::move(n)); index(id, zone, x, y, true);
        }
        // NONCANONICAL per-group instances: instance n of a zone is a copy "<zone><n>" (registry row zone.instances[n-1]) with its
        // own NPC population (ids + n*65536); doors into the zone send a group to its copy (instance()), solo players stay shared.
        const Json zones = config_.at("zones"); // copies are inserted while iterating
        for (const auto& [name, zone] : zones.items()) {
            if (!zone.contains("instances")) continue;
            require(name.size() <= 30, "config", "Instanced zone name too long");
            std::vector<std::pair<uint32_t, const Npc*>> base;
            for (const auto& [id, n] : npcs_) if (n.definition.at("zone") == name) base.push_back({id, &n});
            for (uint32_t copy = 1; copy <= zone.at("instances").size(); ++copy) {
                const auto copy_name = name + std::to_string(copy);
                require(!config_.at("zones").contains(copy_name), "config", "Instance zone name collides");
                auto z = zone; z.erase("instances"); z["instance_of"] = name; z["source_registry_id"] = zone.at("instances")[copy - 1];
                config_["zones"][copy_name] = std::move(z);
                for (const auto& [id, original] : base) {
                    const uint32_t clone_id = id + copy * 65536u; require(!npcs_.contains(clone_id), "config", "Instance NPC id collides");
                    Npc n = *original; n.definition["id"] = clone_id; n.definition["zone"] = copy_name;
                    if (n.definition.contains("door") && n.definition.at("door").at("to") == name) n.definition["door"]["to"] = copy_name;
                    index(clone_id, copy_name, n.x, n.y, true); npcs_.emplace(clone_id, std::move(n));
                }
            }
        }
        // Quests: giver (accept; not for auto quests) and turn_in (completion, default giver) must list the quest; objectives
        // kill/collect name an enemy template (collect: its quest item drops on each kill), talk completes at turn_in.
        std::set<int> gate_ids;
        for (const auto& [id, quest] : config_.at("quests").items()) {
            text(id,32); text(quest.at("name"),128); integer(quest.at("level"),1,100);
            const bool automatic = quest.value("auto",false);
            const auto lists = [&](const char* key) {
                require(npcs_.contains(target_id(quest.at(key))), "config", "Quest NPC missing");
                const auto& n = npcs_.at(target_id(quest.at(key))).definition;
                require(n.at("kind") != "enemy" && std::find(n.at("quests").begin(),n.at("quests").end(),Json(id)) != n.at("quests").end(),"config","Quest NPC catalog mismatch");
            };
            if (!automatic) lists("giver"); else require(npcs_.contains(target_id(quest.at("giver"))),"config","Quest giver missing");
            lists(quest.contains("turn_in") ? "turn_in" : "giver");
            const auto& objective = quest.at("objective"); const auto kind = text(objective.at("kind"),16);
            require(kind == "kill" || kind == "collect" || kind == "talk", "config", "Unknown quest objective");
            if (kind != "talk") {
                text(objective.at("target"),32); integer(objective.at("count"),1,1000);
                bool enemy_found=false;
                for(const auto& [npc_id,n]:npcs_){(void)npc_id;if(n.definition.at("kind")=="enemy" && n.definition.at("template")==objective.at("target"))enemy_found=true;}
                require(enemy_found,"config","Quest objective enemy missing");
            }
            if (kind == "collect") require(config_.at("items").contains(text(objective.at("item"),32)),"config","Quest item missing");
            if(quest.contains("requires") || automatic)require(config_.at("quests").contains(text(quest.at("requires"),32)) && quest.at("requires")!=id,"config","Quest prerequisite missing");
            if(quest.contains("time_limit"))integer(quest.at("time_limit"),1,86400);
            if(quest.contains("source_quest_id"))require(gate_ids.insert(integer(quest.at("source_quest_id"),0,100000)).second,"config","Duplicate source quest id");
            integer(quest.at("reward").at("xp"),0,100000); integer(quest.at("reward").at("currency"),0,100000); validate_items(quest.at("reward").at("items"));
            // NONCANONICAL quest window/chat texts (Yapman gerekenler / Açıklama / completion line); the original server sent them.
            if(quest.contains("objective_text"))text(quest.at("objective_text"),256);
            if(quest.contains("description"))text(quest.at("description"),1024);
            if(quest.contains("complete_text"))text(quest.at("complete_text"),1024);
        }
        // Üretim recipes (classes.dat objecttype recipe): the scroll item id keys its profession, level, product and inputs.
        const auto& economy=config_.at("economy");
        integer(economy.at("bank_slots"),1,128); integer(economy.at("recipe_limit"),1,200); integer(economy.at("gather_ticks"),1,1200); integer(economy.at("gather_rare_percent"),0,100);
        integer(economy.value("guild_level",Json(1)),1,100); // NONCANONICAL level of a new lonca (lonca progression is server-only)
        integer(economy.at("guild_level_cost"),1,999000000); // NONCANONICAL Lonca Parası (donated Klan Parası) per level: level × cost
        require(config_.at("recipes").is_object() && config_.at("recipes").size() <= 1024,"config","Recipe limit");
        for (const auto& [id, recipe] : config_.at("recipes").items()) {
            require(config_.at("items").contains(id) && craft_stat(text(recipe.at("craft"),16)) && config_.at("items").contains(text(recipe.at("product"),32)),"config","Recipe items missing");
            integer(recipe.at("level"),0,1000); integer(recipe.at("quantity"),1,1000);
            require(!recipe.at("components").empty(),"config","Recipe without components"); validate_items(recipe.at("components"));
        }
        validate_items(config_.at("defaults").at("inventory"));
        const auto& defaults=config_.at("defaults");
        require(passable(text(defaults.at("zone"),32),integer(defaults.at("x"),-coordinate_limit,coordinate_limit),integer(defaults.at("y"),-coordinate_limit,coordinate_limit)),"config","Default character spawn blocked");
        integer(defaults.at("max_hp"),1,config_.at("resource_cap"));integer(defaults.at("max_mana"),1,config_.at("resource_cap"));
        integer(defaults.at("hp"),1,defaults.at("max_hp"));integer(defaults.at("mana"),0,defaults.at("max_mana"));
        integer(defaults.at("level"),1,100);integer(defaults.at("xp"),0,10000000);integer(defaults.at("currency"),0,config_.at("currency_cap"));
        integer(defaults.value("akce",Json(0)),0,999000000); // Akçe comes from the İKV Dükkan (0x5926e0); no native Dükkan
        // Klan/Kabadayı/contacts/emotes (config "social"): names, clans and clips are original; amounts and zones NONCANONICAL.
        const auto& rules=config_.at("social");
        integer(rules.at("contacts_max"),1,20);
        require(rules.at("clans").is_object() && rules.at("clans").size()<=15,"config","Clan table invalid");
        for(const auto& [clan,name]:rules.at("clans").items()){integer(Json::parse(clan),1,15);text(name,32);}
        require(rules.at("safe_zones").is_array(),"config","Safe zone list invalid");for(const auto& zone:rules.at("safe_zones"))text(zone,32);
        require(rules.at("emotes").is_object() && rules.at("emotes").size()<=64,"config","Emote table invalid");
        for(const auto& [name,clip]:rules.at("emotes").items()){require(!name.empty() && name.size()<=16,"config","Emote name invalid");integer(clip,0,255);}
        // Held postures: an RP clip whose animation.dat jumptoid loops (78 Sit, 80 Lay, 82 Çok; 79/83 settle into 78/82).
        if (rules.contains("postures")) { require(rules.at("postures").is_object() && rules.at("postures").size()<=32,"config","Posture table invalid"); for (const auto& [clip,held]:rules.at("postures").items()) { require(!clip.empty() && clip.size()<=3 && std::all_of(clip.begin(),clip.end(),[](unsigned char c){return std::isdigit(c);}),"config","Posture clip invalid"); integer(held,1,255); } }
        for(const auto& [id,n]:npcs_){(void)id;if(n.definition.contains("clan"))require(rules.at("clans").contains(std::to_string(integer(n.definition.at("clan"),1,15))),"config","NPC clan unknown");}
        // NONCANONICAL: Şan level falloff (full gain up to `free` levels above the victim, none from `zero`) and clan ranks by Şan
        // (guide: the personal panel shows "klan rütbeniz"; the client has no rank strings).
        integer(rules.at("san_falloff").at("free"),0,99); require(integer(rules.at("san_falloff").at("zero"),1,100)>rules.at("san_falloff").at("free").get<int>(),"config","Şan falloff invalid");
        require(rules.at("clan_ranks").is_array() && !rules.at("clan_ranks").empty() && rules.at("clan_ranks").size()<=16 && rules.at("clan_ranks")[0][0]==0,"config","Clan ranks invalid");
        for(size_t r=0;r<rules.at("clan_ranks").size();++r){const auto& row=rules.at("clan_ranks")[r];require(row.is_array() && row.size()==2,"config","Clan rank invalid");integer(row[0],0,1000000000);text(row[1],32);
            require(!r || row[0].get<int64_t>()>rules.at("clan_ranks")[r-1][0].get<int64_t>(),"config","Clan ranks must ascend");}
        // Group zones and Er Meydanı (config "activities"): rules from the original guide (ikv_oyun_rehberi.pdf); amounts NONCANONICAL.
        const auto& activities=config_.at("activities"); integer(activities.at("lockout"),0,604800);
        const auto& match=activities.at("arena"); const auto& match_zone=config_.at("zones").at(text(match.at("zone"),30));
        require(!match_zone.contains("instance_of"),"config","Arena must be a base zone");
        integer(match.at("duration"),20,72000); integer(match.at("kill_points"),0,1000); integer(match.at("banner_points"),0,1000); integer(match.at("stake"),1,999000000);
        // NONCANONICAL capture areas (guide: "haritada bazı bölgeleri ele geçirdikleri takdirde yeteneklerine bonuslar kazanırlar"):
        // a clan alone inside an area for capture_ticks takes it; its members' skill amounts gain `power` percent while it holds.
        integer(match.at("guardian_points"),0,1000); integer(match.at("capture_ticks"),20,72000);
        require(match.at("areas").is_array() && match.at("areas").size()<=8,"config","Arena areas invalid");
        for(const auto& area:match.at("areas")){text(area.at("name"),32);integer(area.at("radius"),1,200000);integer(area.at("power"),1,100);
            integer(area.at("x"),-coordinate_limit,coordinate_limit);integer(area.at("y"),-coordinate_limit,coordinate_limit);} // support grid: set_source_zones()
        for(const auto& [clan,name]:rules.at("clans").items())require(config_.at("zones").contains(text(match.at("homes").at(clan),32)),"config","Clan home zone missing");
        // Validate the full supported schema once, not a runtime special case for legal 32+32-item exchanges.
        Json appearance_bound={{"p",std::string(376,'x')}};
        Json player_bound={{"id",INT32_MAX},{"kind","player"},{"name",std::string(32,'"')},{"appearance",appearance_bound},
                           {"class_id",3},{"x",-coordinate_limit},{"y",-coordinate_limit},{"hp",config_.at("resource_cap")},
                           {"max_hp",config_.at("resource_cap")},{"level",100},{"alive",false},{"equipment",Json::array()},
                           {"clan",15},{"kabadayi",false},{"guild",std::string(32,'"')},{"posture",255},
                           {"debuffs",Json(std::vector<Json>(8,{{"id",std::string(32,'x')},{"expires",UINT32_MAX},{"skill_source",Json::array({2,15})}}))}};
        std::map<std::string,Json> visual_slots;
        std::map<int,std::string> source_slots;
        int largest_prefix=0;for(const auto& [pid,p]:config_.at("prefixes").items()){(void)p;largest_prefix=std::max(largest_prefix,std::stoi(pid));}
        for(const auto& [id,item]:config_.at("items").items()) {
            (void)id;if(!item.contains("source_slot"))continue;
            const auto semantic=item.at("slot").get<std::string>();const int source_slot=item.at("source_slot");
            const auto [previous,inserted]=source_slots.emplace(source_slot,semantic);
            require(inserted || previous->second==semantic,"config","Distinct equipment slots cannot occupy one source slot");
            // The worn weapon's prefixes travel with its visual for the client's prefixes.dat wp_* glow (weapon_gfx_psystem).
            auto visual=Json{{"slot",item.at("source_slot")},{"source_uid",item.at("source_uid")}};
            if(semantic=="weapon" && largest_prefix)visual["prefixes"]=Json::array({largest_prefix,largest_prefix});
            auto& bound=visual_slots[item.at("slot").get<std::string>()];
            if(bound.is_null() || visual.dump().size()>bound.dump().size())bound=std::move(visual);
        }
        std::vector<Json> visual_bounds;
        for(const auto& [slot,bound]:visual_slots){(void)slot;visual_bounds.push_back(bound);}
        std::sort(visual_bounds.begin(),visual_bounds.end(),[](const Json& a,const Json& b){return a.dump().size()>b.dump().size();});
        for(size_t slot=0;slot<std::min<size_t>(8,visual_bounds.size());++slot)player_bound["equipment"].push_back(visual_bounds[slot]);
        // Other players and NPC entities (with the shop/loot items they introduce) are admitted in state() within the remaining
        // frame budget: group members first, then nearest-first. The static bound covers what is never budgeted: own state,
        // stats, catalogs, the critical-event reserve and the up to five other group members (groups hold six).
        Json entity_bounds=Json::array();
        for(int player=0;player<5;++player)entity_bounds.push_back(player_bound);
        std::vector<std::pair<size_t,std::string>> item_sizes;
        for(const auto& [id,item]:config_.at("items").items())item_sizes.push_back({Json({{id,item}}).dump().size(),id});
        std::sort(item_sizes.rbegin(),item_sizes.rend());
        Json item_bound=Json::object();
        for(size_t n=0;n<std::min<size_t>(item_sizes.size(),config_.at("inventory_slots").get<size_t>());++n)item_bound[item_sizes[n].second]=config_.at("items").at(item_sizes[n].second);
        // State catalogs carry the active quests (at most quest_active_max) plus budgeted NPC offers: bound by the largest ones.
        std::vector<std::pair<size_t,std::string>> quest_sizes;
        for(const auto& [id,quest]:config_.at("quests").items()){size_t bytes=Json({{id,quest}}).dump().size();for(const auto& v:quest.at("reward").at("items"))bytes+=Json({{v.at("item").get<std::string>(),config_.at("items").at(v.at("item").get<std::string>())}}).dump().size();quest_sizes.push_back({bytes,id});}
        std::sort(quest_sizes.rbegin(),quest_sizes.rend());
        Json quest_bound=Json::object();
        for(size_t n=0;n<std::min(quest_sizes.size(),quest_active_max);++n){const auto& quest=config_.at("quests").at(quest_sizes[n].second);quest_bound[quest_sizes[n].second]=quest;for(const auto& v:quest.at("reward").at("items"))item_bound[v.at("item").get<std::string>()]=config_.at("items").at(v.at("item").get<std::string>());}
        Json skill_bound=Json::object();
        for(int source_class=0;source_class<3;++source_class) {
            Json skills=Json::object();
            for(const auto& [id,skill]:config_.at("skills").items())if(skill.at("source")[0]==source_class)skills[id]=skill;
            if(skills.dump().size()>skill_bound.dump().size())skill_bound=std::move(skills);
        }
        Json full_offer={{"items",Json::array()},{"currency",1000000000},{"accepted",false}};
        for(int item=0;item<32;++item)full_offer["items"].push_back({{"uid",std::string(32,'f')},
            {"item",std::string(32,'x')},{"name",std::string(31,'"')},{"quantity",1000}}); // prefixed names fall back to the base name beyond 64 dumped bytes
        Json trade_bound={{"type","trade"},{"peer",INT32_MAX},{"peer_name",std::string(32,'"')},{"initiator",INT32_MAX},
                          {"revision",UINT64_MAX},{"phase","invited"},{"own",full_offer},{"other",std::move(full_offer)}};
        Json event_bounds=Json::array({trade_bound});
        for(int notice=0;notice<31;++notice)event_bounds.push_back({{"type","error"},{"code","trade_persistence"},
            {"peer",INT32_MAX},{"revision",UINT64_MAX},{"message",std::string(160,'x')}});
        Json wire_bound={{"self",INT32_MAX},{"tick",UINT32_MAX},{"admin",true},{"zone",std::string(32,'"')},{"source_registry_id",70},{"xp_interval",10000000},{"xp_total",INT32_MAX},{"skill_points",1000000},
            {"contacts_online",Json(std::vector<int64_t>(20,INT64_MAX))},
            {"stats",{{"attack",INT32_MAX},{"defense",INT32_MAX},{"attack_cooldown",10000},{"weapon_damage",Json::array({INT32_MAX,INT32_MAX})},{"armor",Json::array({1000,100})},{"charisma",1000},{"resistance_points",1000000},{"craft_points",1000000},
                      {"clan_rank",std::string(32,'"')},{"guild_rank",std::string(32,'"')},{"arena_power",1000},
                      {"crit",Json::array({INT32_MIN,10})},{"efsun",[&]{Json e=Json::object();for(const auto& [pid,p]:config_.at("prefixes").items()){(void)pid;for(const auto& a:p.at("attributes"))e[std::to_string(a[0].get<int>())]=INT32_MIN;}return e;}()}}},
            {"clock",{{"hour",23.999999999999996},{"environment",Json::array({255,255,255,255,255,255,255,255,255,255,255})},{"fog",Json::array({1000000,1000000})}}},
            {"bounds",Json::array({-coordinate_limit,-coordinate_limit,coordinate_limit,coordinate_limit})},
            {"character",{{"name",std::string(32,'"')},{"class_id",3},{"equipment",player_bound.at("equipment")},
            {"appearance",std::move(appearance_bound)},{"state",{{"p",std::string(8184,'x')}}}}},
            {"entities",std::move(entity_bounds)},{"events",std::move(event_bounds)},
            {"catalogs",{{"items",std::move(item_bound)},{"quests",std::move(quest_bound)},{"skills",std::move(skill_bound)}}}};
        require(trade_bound.dump().size()<=32768 && wire_bound.dump().size()<=65533,"config","Full supported trade/state schema exceeds protocol envelope");
        initializing_=false;
    } catch (const Rejected& e) { throw std::invalid_argument("gameplay config: " + e.message); }
      catch (const Json::exception& e) { throw std::invalid_argument(std::string("gameplay config: ") + e.what()); }
}

World::Actor& World::actor(uint32_t id) { auto it = actors_.find(id); require(it != actors_.end(), "target", "Oyuncu oyunda değil."); return it->second; }
void World::emit(uint32_t id, Json value) {
    auto found = actors_.find(id); if (found == actors_.end()) return;
    auto& a=found->second;
    if(a.bot) { // bots read world state directly; only player chat addressed to them (/dismiss) is kept
        if(value.at("type")=="chat" && (value.at("channel")=="group" || value.at("channel")=="private") && actors_.contains(value.at("from").get<uint32_t>()) &&
           !actors_.at(value.at("from").get<uint32_t>()).bot && bots_.at(id).inbox.size()<8) bots_.at(id).inbox.push_back(std::move(value));
        return;
    }
    if(a.kicked && value.at("type")!="kick")return;
    const auto bytes=value.dump().size();
    require(bytes<=32768,"event","Sunucu olayı çok büyük.");
    auto& events=a.events;
    const auto erase_event=[&](size_t index) {
        a.event_bytes-=a.event_sizes[index];events.erase(events.begin()+index);
        std::move(a.event_sizes.begin()+index+1,a.event_sizes.end(),a.event_sizes.begin()+index);
    };
    if(value.at("type")=="trade" || value.at("type")=="trade_complete" || value.at("type")=="trade_cancel") {
        for(size_t index=0;index<events.size();) {
            const auto& type=events[index].at("type");
            if(type=="trade" || type=="trade_invite")erase_event(index);else ++index;
        }
    }
    while(!events.empty() && (events.size()==32 || a.event_bytes+bytes>32768)) {
        size_t oldest=0;
        while(oldest<events.size() && critical_event(events[oldest]))++oldest;
        erase_event(oldest==events.size()?0:oldest);
    }
    a.event_sizes[events.size()]=bytes;a.event_bytes+=bytes;events.push_back(std::move(value));
}
void World::project_event(uint32_t entity,Json value) {
    std::string zone;int32_t x,y;
    if(const auto p=actors_.find(entity);p!=actors_.end()) {
        const auto& s=p->second.character.at("state");zone=s.at("zone");x=s.at("x");y=s.at("y");
    } else { const auto& n=npcs_.at(entity);zone=n.definition.at("zone");x=n.x;y=n.y; }
    const int range=config_.at("aoi");
    for(const auto& [observer,a]:actors_) {
        const auto& s=a.character.at("state");
        if(s.at("zone")==zone && distance(x,y,s.at("x"),s.at("y"))<=int64_t(range)*range)emit(observer,value);
    }
}
Json World::visible_equipment(const Json& s) const {
    Json result=Json::array();
    for(const auto& [slot,uid]:s.at("equipment").items()) {
        (void)slot;const auto& entry=owned(s,uid.get_ref<const std::string&>());const auto& item=config_.at("items").at(entry.at("item").get_ref<const std::string&>());
        if(!item.contains("source_slot"))continue;
        Json visual={{"slot",item.at("source_slot")},{"source_uid",item.at("source_uid")}};
        // 004315c0 (WEAR 0x4a94d0): prefix effect_name glows only while the item's Eskime is above zero.
        const Json& prefixes=entry.contains("prefixes")?entry.at("prefixes"):item.contains("prefixes")?item.at("prefixes"):visual.at("slot");
        if(slot=="weapon" && prefixes.is_array() && !prefixes.empty() && entry.value("durability",item.value("durability",1))>0)visual["prefixes"]=prefixes;
        result.push_back(std::move(visual));
    }
    return result;
}
void World::reject(uint32_t id, std::string code, std::string message) {
    if(logs_.size()==64)logs_.erase(logs_.begin());
    logs_.push_back({{"tick",tick_},{"player",id},{"result","rejected"},{"code",code}});
    emit(id, {{"type","error"},{"code",std::move(code)},{"message",std::move(message)}});
}
World::Cell World::cell(const std::string& zone, int32_t x, int32_t y) const {
    int size = config_.at("cell_size");
    const auto floor_div = [size](int n) { return n >= 0 ? n / size : (n-size+1)/size; };
    return {zone, floor_div(x), floor_div(y)};
}
void World::index(uint32_t id, const std::string& zone, int32_t x, int32_t y, bool insert) {
    auto key = cell(zone,x,y); if (insert) cells_[key].insert(id);
    else if (auto it = cells_.find(key); it != cells_.end()) { it->second.erase(id); if (it->second.empty()) cells_.erase(it); }
}
bool World::passable(const std::string& zone, int32_t x, int32_t y) const {
    if (!config_.at("zones").contains(zone)) return false;
    const auto& z = config_.at("zones").at(zone); const auto& b = z.at("bounds");
    if (x < b[0].get<int>() || y < b[1].get<int>() || x > b[2].get<int>() || y > b[3].get<int>()) return false;
    for (const auto& box : z.at("collision")) if (x >= box[0].get<int>() && y >= box[1].get<int>() && x <= box[2].get<int>() && y <= box[3].get<int>()) return false;
    if (!z.contains("source_registry_id")) return true;
    const auto grid=source_zones_.find(zone);
    return grid==source_zones_.end() ? initializing_ : zone_height(*grid->second,float(x)/1000.0f,float(y)/1000.0f).has_value();
}
std::string World::instance(uint32_t id, const std::string& zone) const {
    const auto leader = group(id);
    if (!leader || !config_.at("zones").at(zone).contains("instances")) return zone;
    const auto copies = config_.at("zones").at(zone).at("instances").size();
    const auto occupant = [&](const std::string& name, bool member) {
        for (const auto& [other, b] : actors_) if (other != id && b.character.at("state").at("zone") == name && (!member || group(other) == leader)) return true;
        return false;
    };
    for (size_t n = 1; n <= copies; ++n) if (occupant(zone + std::to_string(n), true)) return zone + std::to_string(n); // the group's copy
    for (size_t n = 1; n <= copies; ++n) if (!occupant(zone + std::to_string(n), false)) return zone + std::to_string(n); // a free one
    return zone; // every copy is held by another group: the shared zone
}
void World::transfer(uint32_t id, const std::string& zone, int32_t x, int32_t y) {
    auto& a=actor(id); const auto& current=a.character.at("state");
    const auto& target_zone=config_.at("zones").at(zone);
    require(a.admin || !disabled_zones_.contains(target_zone.value("instance_of",zone)),"zone_disabled","Bu bölge yönetici tarafından geçici olarak kapatıldı.");
    const auto old=current.at("zone").get<std::string>(); const auto ox=coordinate(current,"x"),oy=coordinate(current,"y");
    auto s=current; s["zone"]=zone; s["x"]=x; s["y"]=y; commit({{id,s}});
    index(id,old,ox,oy,false); index(id,zone,x,y,true); cancel_trade(id); a.moving=false; a.route.clear(); a.dx=a.dy=0; a.engaged=0;
    vacated(old);
}
void World::set_source_zones(std::map<std::string,std::shared_ptr<const Zone>> zones) {
    if(!actors_.empty())throw std::invalid_argument("support grids must be installed before admission");
    for(const auto& [name,grid]:zones) {
        if(!config_.at("zones").contains(name) || !config_.at("zones").at(name).contains("source_registry_id") ||
           !grid || grid->width!=2048 || grid->height!=2048 || grid->cell_size!=1.5f ||
           grid->cells.size()!=2048u*2048u || grid->heights.size()!=grid->cells.size())
            throw std::invalid_argument("source support grid provider mismatch");
    }
    for (const auto& [name, z] : config_.at("zones").items()) // instance copies share their zone's original support grid
        if (z.contains("instance_of") && zones.contains(z.at("instance_of").get<std::string>())) zones[name] = zones.at(z.at("instance_of").get<std::string>());
    auto previous=std::move(source_zones_);source_zones_=std::move(zones);
    try {
        const auto supported=[&](const std::string& name,const Json& point) {
            if(!passable(name,point[0],point[1]))throw std::invalid_argument("configured point is outside supported grid cells");
        };
        for(const auto& [name,z]:config_.at("zones").items()) {
            if(z.contains("source_registry_id") && !source_zones_.contains(name))throw std::invalid_argument("declared source zone requires original support grid");
            supported(name,z.at("spawn"));
        }
        const auto& defaults=config_.at("defaults");supported(defaults.at("zone"),Json::array({defaults.at("x"),defaults.at("y")}));
        const auto& match=config_.at("activities").at("arena");for(const auto& area:match.at("areas"))supported(match.at("zone"),Json::array({area.at("x"),area.at("y")}));
        for(const auto& [id,n]:npcs_) {
            (void)id;const std::string name=n.definition.at("zone");supported(name,Json::array({n.x,n.y}));
            if(n.definition.contains("door"))supported(n.definition.at("door").at("to"),n.definition.at("door").at("arrive"));
            if(n.definition.contains("patrol") && !config_.at("zones").at(name).contains("instance_of")) { // copies repeat their zone's patrols
                int32_t x=n.x,y=n.y;
                for(const auto& point:n.definition.at("patrol")) {
                    supported(name,point);
                    if(route(name,x,y,point[0],point[1]).empty())throw std::invalid_argument("configured NPC patrol has no bounded supported route");
                    x=point[0];y=point[1];
                }
                if(route(name,x,y,n.x,n.y).empty())throw std::invalid_argument("configured NPC patrol cannot return to spawn");
            }
        }
    } catch (...) { source_zones_=std::move(previous);throw; }
}
bool World::clear_path(const std::string& zone,int32_t x,int32_t y,int32_t nx,int32_t ny) const {
    if (!passable(zone,nx,ny)) return false;
    for (const auto& box : config_.at("zones").at(zone).at("collision")) {
        if (std::max(x,nx) >= box[0].get<int>() && std::min(x,nx) <= box[2].get<int>() &&
            std::max(y,ny) >= box[1].get<int>() && std::min(y,ny) <= box[3].get<int>()) return false;
    }
    if (source_zones_.contains(zone)) {
        // Trace the actual segment through source-rounded cells at half-grid-pitch intervals (<=4096).
        const auto cell=[](int32_t n){return int(std::round((float(n)/1000.0f)*0x1.555556p-1f));};
        int cx=cell(x),cy=cell(y);
        const int64_t dx=int64_t(nx)-x,dy=int64_t(ny)-y;
        const int count=int((std::max(std::abs(dx),std::abs(dy))+749)/750);
        for(int k=1;k<=count;++k) {
            const int32_t sample_x=int32_t(int64_t(x)+dx*k/count),sample_y=int32_t(int64_t(y)+dy*k/count);
            const int px=cell(sample_x),py=cell(sample_y);
            if(!passable(zone,sample_x,sample_y) ||
               (px!=cx && py!=cy && (!passable(zone,px*1500,cy*1500) || !passable(zone,cx*1500,py*1500))))return false;
            cx=px;cy=py;
        }
    }
    return true;
}
std::vector<std::pair<int32_t,int32_t>> World::route(const std::string& zone,int32_t x,int32_t y,int32_t nx,int32_t ny,int limit) const {
    if(!passable(zone,x,y) || !passable(zone,nx,ny))return {};
    if(clear_path(zone,x,y,nx,ny))return {{nx,ny}};
    if(!source_zones_.contains(zone))return {{nx,ny}}; // Independent fixture movement retains its collision correction.
    const auto grid_cell=[](int32_t n){return int(std::round((float(n)/1000.0f)*0x1.555556p-1f))+1024;};
    const int sx=grid_cell(x),sy=grid_cell(y),gx=grid_cell(nx),gy=grid_cell(ny);
    const auto key=[](int a,int b){return b*2048+a;};
    const auto heuristic=[&](int a,int b){const int dx=std::abs(gx-a),dy=std::abs(gy-b);return 10*std::max(dx,dy)+4*std::min(dx,dy);};
    struct Node { int cost,parent; };
    struct Pending { int score,cost,key;bool operator<(const Pending& rhs)const{return score>rhs.score;} };
    std::unordered_map<int,Node> visited;visited.reserve(8001);
    std::priority_queue<Pending> open;
    const int start=key(sx,sy),goal=key(gx,gy);visited.emplace(start,Node{0,start});open.push({heuristic(sx,sy),0,start});
    bool found=false;
    // Source-compatible 1000 popped-node ceiling for clicks; bots plan an errand's long walk once with a larger one.
    for(int popped=0;!open.empty() && popped<limit;++popped) {
        const auto current=open.top();open.pop();
        if(visited.at(current.key).cost!=current.cost)continue;
        if(current.key==goal){found=true;break;}
        const int cx=current.key%2048,cy=current.key/2048;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx) {
            if(!dx && !dy)continue;
            const int px=cx+dx,py=cy+dy;
            if(px<0 || py<0 || px>=2048 || py>=2048 ||
               !clear_path(zone,(cx-1024)*1500,(cy-1024)*1500,(px-1024)*1500,(py-1024)*1500))continue;
            const int next=key(px,py),cost=current.cost+(dx && dy?14:10);
            const auto old=visited.find(next);if(old!=visited.end() && old->second.cost<=cost)continue;
            visited.insert_or_assign(next,Node{cost,current.key});open.push({cost+heuristic(px,py),cost,next});
        }
    }
    if(!found)return {};
    std::vector<std::pair<int32_t,int32_t>> points;
    for(int at=goal;at!=start;at=visited.at(at).parent)points.emplace_back((at%2048-1024)*1500,(at/2048-1024)*1500);
    // Leave via the start cell centre: the segment from an off-centre position straight to a neighbour centre can clip a
    // blocked corner cell, which made every route from such a stop point fail (players stuck, "Hedefe ulaşılamıyor.").
    points.emplace_back((sx-1024)*1500,(sy-1024)*1500);
    std::reverse(points.begin(),points.end());points.emplace_back(nx,ny);
    std::vector<std::pair<int32_t,int32_t>> smoothed;
    size_t at=0;
    while(at<points.size()) {
        if(!clear_path(zone,x,y,points[at].first,points[at].second))return {};
        size_t end=at;
        while(end+1<points.size() && clear_path(zone,x,y,points[end+1].first,points[end+1].second))++end;
        smoothed.push_back(points[end]);x=points[end].first;y=points[end].second;at=end+1;
    }
    return smoothed;
}
bool World::close(uint32_t a, uint32_t b, int range) const {
    const auto& s = actors_.at(a).character.at("state"); std::string zone; int32_t x,y;
    if (auto it = actors_.find(b); it != actors_.end()) { const auto& other = it->second.character.at("state"); zone = other.at("zone"); x = coordinate(other,"x"); y = coordinate(other,"y"); }
    else if (auto npc = npcs_.find(b); npc != npcs_.end()) { zone = npc->second.definition.at("zone"); x = npc->second.x; y = npc->second.y; }
    else return false;
    return s.at("zone") == zone && distance(coordinate(s,"x"),coordinate(s,"y"),x,y) <= int64_t(range)*range;
}
std::vector<uint32_t> World::nearby(uint32_t id, int range) const {
    std::vector<uint32_t> result; const auto& s = actors_.at(id).character.at("state");
    const auto [zone,cx,cy] = cell(s.at("zone"),coordinate(s,"x"),coordinate(s,"y"));
    const int cells = (range + config_.at("cell_size").get<int>() - 1) / config_.at("cell_size").get<int>();
    for (auto it=cells_.lower_bound({zone,cx-cells,INT32_MIN});it!=cells_.end();++it) {
        const auto& [z,x,y]=it->first;
        if(z!=zone || x>cx+cells)break;
        if(y<cy-cells || y>cy+cells)continue;
        for (auto other : it->second) if (close(id,other,range)) result.push_back(other);
    }
    std::sort(result.begin(),result.end()); return result;
}
World::Npc& World::npc(uint32_t player, uint32_t target, const char* kind) {
    auto found = npcs_.find(target); require(found != npcs_.end(), "target", "Hedef bulunamadı.");
    require(!guard(found->second.definition) || !strikable(found->second.definition,actor(player).character.at("state")), "target", "Düşman bir hedefle etkileşime geçemezsiniz.");
    require(close(player,target,config_.at("interaction_range")), "range", "Hedef çok uzakta.");
    require(found->second.hp > 0, "dead", "Hedef ölü! Etkileşime geçemezsiniz!"); // a felled Klan OOK serves no one until it rises (guide)
    require(!*kind || found->second.definition.at("kind") == kind, "target", "Bu kişi bu hizmeti vermiyor.");
    found->second.wandering = false; found->second.idle_at = tick_ + 1200; // an idlewalktend stroller stays a minute for its customer
    return found->second;
}
void World::commit(const std::map<uint32_t, Json>& proposed, const Json* bank) {
    require(!bank || proposed.size() == 1, "state", "Banka işlemi tek bir karaktere ait olmalı.");
    std::vector<CharacterSave> saves; saves.reserve(proposed.size());
    for (const auto& [id,s] : proposed) {
        require(s.dump().size()<=8192,"state","Karakter verisi sınırı aşıldı.");
        require(s.at("equipment").is_object() && s.at("equipment").size()<=8,"equipment","Bütün giyim yerleri dolu.");
        const auto& a = actors_.at(id); saves.push_back({a.account_id,a.character_id,s,bank ? *bank : Json()});
        resources(saves.back().state); // every committed level/equipment change re-derives max Enerji/Kudret
        persisted_visual(saves.back().state);
    }
    // Persistence is the commit point. No cache or acknowledgement changes precede it. Bot-only changes are saved by save_all()
    // and remove() instead (NONCANONICAL: a crash loses at most one periodic-save interval of bot progress); any commit that
    // touches a player persists every participant at once.
    if (std::any_of(proposed.begin(),proposed.end(),[&](const auto& p){return !actors_.at(p.first).bot;}) || bank) persist_(saves);
    size_t saved=0;
    for (const auto& [id,s] : proposed) { (void)s; auto& a=actors_.at(id); a.character["state"] = std::move(saves[saved].state); a.character["state"].erase("visual"); if (bank) a.bank = std::move(saves[saved].bank); ++saved; }
}
Json& World::owned(Json& s, const std::string& uid) {
    for (auto& entry : s.at("inventory")) if (entry.at("uid") == uid) return entry;
    throw Rejected{"ownership","Bu eşya size ait değil."};
}
const Json& World::owned(const Json& s, const std::string& uid) const {
    for (const auto& entry : s.at("inventory")) if (entry.at("uid") == uid) return entry;
    throw Rejected{"ownership","Bu eşya size ait değil."};
}
void World::give(Json& s, const std::string& item, int quantity, const Json& instance) {
    require(config_.at("items").contains(item) && quantity > 0 && quantity <= 1000, "item", "Geçersiz eşya miktarı.");
    auto& inventory = s.at("inventory"); const int limit = config_.at("items").at(item).at("stack");
    // Prefixed or worn (durability) instances never merge into a stack.
    if (!instance.contains("prefixes") && !instance.contains("durability")) for (auto& entry : inventory) if (entry.at("item") == item && !entry.contains("prefixes") && !entry.contains("durability")) {
        int existing = entry.at("quantity"); int add = std::min(limit-existing,quantity); entry["quantity"] = existing+add; quantity -= add;
        if (!quantity) return;
    }
    while (quantity) {
        require(inventory.size() < config_.at("inventory_slots").get<size_t>(), "inventory", "Çantanızda yer yok!!"); // 0x590ca0
        int add = std::min(limit,quantity); Json entry{{"uid",new_uid()},{"item",item},{"quantity",add}};
        for (const char* key : {"prefixes","durability"}) if (instance.contains(key)) entry[key] = instance.at(key);
        inventory.push_back(std::move(entry)); quantity -= add;
    }
}
void World::take(Json& s, const std::string& uid, int quantity) {
    require(quantity > 0 && quantity <= 1000, "quantity", "Geçersiz miktar.");
    for (const auto& [slot,equipped] : s.at("equipment").items()) { (void)slot; require(equipped != uid,"equipped","Giyilen bir eşyayı önce üzerinizden çıkarın."); }
    auto& inv = s.at("inventory");
    for (auto it=inv.begin();it!=inv.end();++it) if (it->at("uid") == uid) { int count = it->at("quantity"); require(count >= quantity,"quantity","Yeterli eşyanız yok."); if (count == quantity) inv.erase(it); else (*it)["quantity"] = count-quantity; return; }
    throw Rejected{"ownership","Bu eşya size ait değil."};
}
void World::currency(Json& s, int64_t change) const { int64_t next = s.at("currency").get<int64_t>() + change; require(next >= 0,"currency","Yeterli paranız yok."); require(next <= config_.at("currency_cap").get<int64_t>(),"currency","En fazla 999 milyon paranız olabilir."); s["currency"] = next; }
void World::xp(Json& s, int amount) const {
    amount = int(int64_t(amount) * xp_rate_ / 100); // administrator XP rate (NONCANONICAL), 100 = unchanged
    int value = s.at("xp").get<int>() + amount, level = s.at("level"); const int before = level;
    while (level < 100 && value >= xp_needed(config_,level)) { value -= xp_needed(config_,level); ++level; }
    s["xp"] = std::min(value,10000000); s["level"] = level;
    if (level != before) { resources(s); s["hp"] = s.at("max_hp"); s["mana"] = s.at("max_mana"); } // a level-up refills both
}
void World::die(uint32_t id, Json& s) const {
    (void)id; s["hp"] = 0; s["buffs"] = Json::array(); s["currency"] = s.at("currency").get<int64_t>() * (100-config_.at("death_currency_percent").get<int>())/100;
    s.erase("kabadayi"); // NONCANONICAL: death ends Kabadayı mode (the original menu has no way back)
    // helps.xml Para: every death damages the worn equipment (repair at an örs); the amount is NONCANONICAL.
    wear(s,true,item_rule(config_,"death_wear_permille")); wear(s,false,item_rule(config_,"death_wear_permille"));
}
World::Gear World::gear(const Json& s) const {
    Gear g; const double scale = item_rule(config_,"efsun_percent")/100.0;
    for (const auto& [slot,uid] : s.at("equipment").items()) {
        const auto& entry = owned(s,uid.get_ref<const std::string&>()); const auto& item = config_.at("items").at(entry.at("item").get_ref<const std::string&>());
        if (slot == "weapon") g.weapon = &item;
        if (entry.value("durability",item.value("durability",1)) <= 0) continue; // Eskime exhausted: the item gives nothing
        if (slot == "weapon") g.intact = true;
        g.armor += item.value("armor",0);
        const Json& prefixes = entry.contains("prefixes") ? entry.at("prefixes") : item.contains("prefixes") ? item.at("prefixes") : entry.at("quantity");
        if (prefixes.is_array()) for (const auto& p : prefixes) for (const auto& a : config_.at("prefixes").at(std::to_string(p.get<int>())).at("attributes")) g.efsun[a[0].get<int>()] += a[1].get<double>();
    }
    // Gazap 1 Tılsım (NONCANONICAL): while Gazap is open, Büyü Zehir (56), Büyü Asit (57) and Maksimum Hasar (39) Efsun ×(100+V)%.
    if (active(s.at("buffs"),"gazap")) if (const int v = glyph(config_,s,"gazap","efsun_boost"); v >= 0) for (int id : {56,57,39}) if (g.efsun.contains(id)) g.efsun[id] *= (100+v)/100.0;
    // Büyü Hasarı (31) and İyileştirme Büyüleri (62) add to skill amounts at the NONCANONICAL efsun_percent; fighter() reads the rest.
    const auto sum = [&](int id) { const auto f = g.efsun.find(id); return f == g.efsun.end() ? 0.0 : f->second; };
    g.magic = int(std::lround(sum(31)*scale)); g.heal = int(std::lround(sum(62)*scale));
    return g;
}
// Client 00402f60 (Karakter Bilgisi), attribute ids 004338f0, item fields from the classes.dat loader 0043a54a (+0x4c damagelow,
// +0x50 damagehigh, +0x54 attackspeed, +0x58 attackrange, +0x60/+0x64 criticalchance/multiplier, +0x70 dmgreduction, +0x74 Eskime):
//   Saldırı = Saldırı(27) + Yakın/Menzilli(28/29) + Seviye·class_attack + Beceri; Savunma = Savunma(30) + Seviye·class_defense + Beceri
//   (class factors 0x58d438/0x58d444 by (class−2)>>1); Saldırı Hızı = max(0.5, ms·(1+haste/3000)/1000), bare hands (haste+1000)/1000;
//   Silah Hasarı = (Güç·damagelow)^0.75 – (Güç·damagehigh)^0.75 + (Maks(39)+Yakın/Menzilli Maks)·hız/3 (ranged ×0.5) + Maks(39) + elements,
//   bare hands damagelow 1/high 2; Kritik = 42+43/44 + criticalchance (bare 1) / ×(45+46/47 + multiplier, bare 2);
//   Zırh % = (Σ worn dmgreduction + Zırh(17))·100, cap 60 (Ruh Kalkanı 15 puan 70, Defansif Dövüşme 15 puan 75), Defansif +Zırh·25 and
//   damage ×0.75 (00402f60 local_50; skills.dat "Hasarını %%25 düşürür"), Ofansif ×0.75 and cap 30 ("maksimum zırhını 30 yapar").
// The client gates the stance terms on learned ranks and three unrecovered buff uids; native gates them on the open stance.
// Güç(8)/Beceri(9) themselves are server-sent: formulas.guc/beceri hold the VOD level 1-5 Büyücü samples (Beceri =
// Saldırı − 1.5·L), then a fitted line. v6 has no Güç/Zeka/Bünye/Beceri points: the Karakter Bilgisi constructor (00403e8b..00403ee3)
// builds only the Dirençler (12-16, pool 22) and Üretim/Toplama (pool 23) rows, and no client code reads Zeka(10)/Bünye(11).
World::Fighter World::fighter(const Json& s, int class_id) const {
    const auto& f = formulas(config_); const auto g = gear(s); const auto& buffs = s.at("buffs"); Fighter r;
    const auto e = [&](int id) { const auto it = g.efsun.find(id); return it == g.efsun.end() ? 0.0 : it->second; };
    const int level = s.at("level"); const auto c = size_t(std::clamp(class_id-1,0,2));
    const double guc = attribute_curve(f.at("guc"),level), beceri = attribute_curve(f.at("beceri"),level);
    const Json* w = g.weapon; const bool ranged = w && w->value("attack_range",0) > 10000; // classes.dat attackrange > 10
    const double haste = e(33)+e(ranged ? 35 : 34), ms = w ? w->value("attack_cooldown",20)*50.0 : 0;
    r.speed = std::max(0.5,(w ? ms+ms*haste/3000 : haste+1000)/1000);
    const auto weapon_damage = [&](size_t i, double bare) { return !w || !g.intact ? bare : w->contains("damage_range") ? w->at("damage_range")[i].get<double>() : w->value("damage",bare); };
    double bonus = (e(39)+e(ranged ? 41 : 40))*r.speed/3; if (ranged) bonus *= 0.5;
    r.min = std::pow(guc*weapon_damage(0,1),0.75); r.max = std::pow(guc*weapon_damage(1,2),0.75)+bonus+e(39);
    const bool crit = w && w->contains("crit");
    r.crit = e(42)+e(ranged ? 44 : 43)+(crit ? w->at("crit")[0].get<double>() : 1);
    r.crit_multiplier = e(45)+e(ranged ? 47 : 46)+(crit ? w->at("crit")[1].get<double>() : 2);
    // Skill buffs: "attack"/"armor" are Saldırı/Savunma points (Ofansif "1 puanda 12 saldırı", Defansif "savunmayı 12 arttırır").
    r.attack = e(27)+e(ranged ? 29 : 28)+level*f.at("class_attack")[c].get<double>()+beceri+buff_sum(buffs,"attack");
    // Kışkırtma 15 puan (detail text) / its Tılsım: harmful defense_break effects take Savunma points.
    r.defense = e(30)+level*f.at("class_defense")[c].get<double>()+beceri+buff_sum(buffs,"armor")-buff_sum(buffs,"defense_break");
    r.armor = (g.armor/10000.0+e(17))*100; // generator armor = classes.dat dmgreduction in basis points
    if (s.at("skills").value("ruh_kalkani",0) >= 15) r.armor_cap = 70;
    if (s.at("skills").value("defansif_dovusme",0) >= 15) r.armor_cap = 75;
    if (active(buffs,"defansif_dovusme")) { // Defansif Dövüşme 2 Tılsım (NONCANONICAL): damage ×(100+V)% instead of ×0.75
        const int v = glyph(config_,s,"defansif_dovusme","stance_damage"); r.armor += e(17)*25; r.multiplier = v < 0 ? 0.75 : (100+v)/100.0;
    }
    if (active(buffs,"ofansif_dovus")) { r.armor *= 0.75; r.armor_cap = 30; }
    r.armor = std::min(r.armor,r.armor_cap)*std::max(0,100-buff_sum(buffs,"armor_break"))/100; // Meteorit 15-point crit: Zırh −20 %
    for (size_t i = 0; i < 5; ++i) { r.element[i] = e(element_damage[i]); r.resist[i] = s.at("resistances").value(resistance_stats[i],0)+e(element_resist[i]); }
    r.crit_armor = e(32); // Kritik Zırhı: NONCANONICAL percentage points off the attacker's crit chance
    // Sakınma Tılsım (NONCANONICAL): while Sakınma is active, Kritik Zırhı + Zırh Efsun (17, a fraction) × V points.
    if (active(buffs,"sakinma")) if (const int v = glyph(config_,s,"sakinma","crit_armor"); v >= 0) r.crit_armor += e(17)*v;
    if (w && g.intact && w->contains("attack_type")) r.types = w->at("attack_type"); // bare hands strike blunt (Ezici)
    return r;
}
// One weapon roll: physical uniform over Silah Hasarı, each element uniform 0..its Hasar Efsun (the window adds them to the maximum).
World::Blow World::swing(const Fighter& f, const Json& buffs) const {
    const double scale = f.multiplier*(100+buff_sum(buffs,"power"))/100.0;
    Blow b{f.attack, uniform(f.min,f.max)*scale, f.crit, f.crit_multiplier, {}, f.types};
    for (size_t i = 0; i < 5; ++i) b.element[i] = uniform(0,f.element[i])*scale;
    return b;
}
World::Blow World::spell(int damage, const Json& definition) const {
    Blow b; const auto school = definition.value("school",std::string());
    if (const auto it = std::find(element_schools.begin(),element_schools.end(),school); it != element_schools.end()) b.element[size_t(it-element_schools.begin())] = damage;
    else b.physical = damage;
    return b;
}
// helps.xml Savunma: "saldırı ve savunma puanları eşit olan iki karakter birbirine yüzde 50 ihtimalle vurabilir" -> hit chance
// Saldırı/(Saldırı+Savunma) within formulas.hit_percent (a victim without Savunma is always hit); Zırh % cuts physical damage and
// each resistance point 0.1% of its element (helps.xml Dirençler "binde birlik hasar indirgemesi"); a crit multiplies the total.
World::Landed World::land(const Blow& b, double defense, double armor, double crit_armor, const std::array<double,5>& resist) const {
    if (b.attack >= 0 && defense > 0) {
        const auto& bounds = formulas(config_).at("hit_percent");
        const double chance = std::clamp(b.attack/(b.attack+defense), bounds[0].get<double>()/100, bounds[1].get<double>()/100);
        if (random_below(1000000) >= chance*1000000) return {};
    }
    double damage = b.physical*(1-std::clamp(armor,0.0,1.0));
    for (size_t i = 0; i < 5; ++i) damage += b.element[i]*(1-std::clamp(resist[i],0.0,1000.0)/1000);
    Landed r{0,true,false};
    if (b.crit > crit_armor && random_below(1000) < uint32_t((b.crit-crit_armor)*10)) { r.critical = true; damage *= b.multiplier; }
    r.damage = damage > 0 ? std::max(1,int(std::lround(damage))) : 0;
    return r;
}
// Physical damage against a monster loses its classes.dat dmgred for the weapon's best attack type (K slash, D pierce, E bludgeon),
// less a Meteorit crit's armor_break percent; Kışkırtma's defense_break takes classes.dat defense points.
World::Landed World::land_npc(const Blow& b, const Json& d, const Json& debuffs) const {
    double armor = 0;
    if (d.contains("dmgred") && !b.types.empty()) { armor = 1; for (char t : b.types) armor = std::min(armor,d.at("dmgred").at(std::string(1,t)).get<double>()); }
    armor *= std::max(0,100-buff_sum(debuffs,"armor_break"))/100.0;
    std::array<double,5> resist{}; if (d.contains("resist")) for (size_t i = 0; i < 5; ++i) resist[i] = d.at("resist")[i].get<double>();
    return land(b,std::max(0.0,d.value("defense",0.0)-buff_sum(debuffs,"defense_break")),armor,0,resist);
}
void World::resources(Json& s) const {
    const auto g = gear(s); const auto e = [&](int id) { const auto it = g.efsun.find(id); return it == g.efsun.end() ? 0.0 : it->second; };
    const int level = s.at("level"), cap = config_.at("resource_cap");
    // Defansif Dövüşme 1 Tılsım (NONCANONICAL, value = "katı" ×100): the open stance adds worn Maksimum Enerji (6) × V/100.
    const int energy = active(s.at("buffs"),"defansif_dovusme") ? glyph(config_,s,"defansif_dovusme","armor_energy") : -1;
    s["max_hp"] = std::clamp(resource_curve(formulas(config_).at("hp"),level)+int(std::lround(e(6)*(100+std::max(0,energy))/100)),1,cap);
    s["max_mana"] = std::clamp(resource_curve(formulas(config_).at("mana"),level)+int(std::lround(e(7))),1,cap);
    s["hp"] = std::min(s.at("hp").get<int>(),s.at("max_hp").get<int>()); s["mana"] = std::min(s.at("mana").get<int>(),s.at("max_mana").get<int>());
}
void World::wear(Json& s, bool weapon, int permille) const {
    if (permille <= 0) return;
    for (const auto& [slot,uid] : s.at("equipment").items()) {
        if ((slot == "weapon") != weapon) continue;
        for (auto& entry : s.at("inventory")) if (entry.at("uid") == uid) {
            const auto& item = config_.at("items").at(entry.at("item").get_ref<const std::string&>());
            if (!item.contains("durability")) continue;
            const int maximum = item.at("durability"), current = entry.value("durability",maximum);
            entry["durability"] = std::max(0,current-std::max(1,maximum*permille/1000));
        }
    }
}
// NONCANONICAL drop roll (drop tables were server data): one item of the monster's table with drop_percent chance; base items
// take a random prefixes.dat prefix (rarity > 0, flag 1, item/prefix class compatible, prefix level <= max(item, monster) level),
// index 1/3 for the first and index 2 for the second, each weighted by rarity (VOD 34:00 "Savunma Artırıcı Kanvaslı Deri Eldiven").
Json World::roll_drop(const Json& n, bool certain) const {
    const auto drop_chance = std::min<int64_t>(100, int64_t(item_rule(config_,"drop_percent")) * drop_rate_ / 100); // administrator drop rate (NONCANONICAL)
    if (!n.contains("drops") || n.at("drops").empty() || (!certain && random_below(100) >= uint32_t(drop_chance))) return nullptr;
    const auto& id = n.at("drops")[random_below(uint32_t(n.at("drops").size()))].get_ref<const std::string&>();
    const auto& item = config_.at("items").at(id);
    Json drop{{"item",id}}; if (item.contains("prefixes") || !item.contains("slot")) return drop; // fixed prefixes; a Tılsım takes none
    const int level = std::max(item.at("level").get<int>(), n.value("level",1));
    Json prefixes = Json::array();
    for (const auto& [chance,second] : {std::pair{item_rule(config_,"prefix_percent"),false},std::pair{item_rule(config_,"second_prefix_percent"),true}}) {
        if (((!certain || second) && random_below(100) >= uint32_t(chance)) || (second && prefixes.empty())) continue;
        std::vector<std::pair<int64_t,int>> pool; int64_t total = 0;
        for (const auto& [pid,p] : config_.at("prefixes").items()) {
            const int index = p.at("index");
            if (p.at("rarity").get<int>() <= 0 || p.at("flag") != 1 || p.at("level").get<int>() > level || (second ? index != 2 : index != 1 && index != 3) ||
                (p.contains("class") && item.contains("class") && p.at("class") != item.at("class")) || (p.contains("class") && !item.contains("class"))) continue;
            total += p.at("rarity").get<int>(); pool.push_back({total,std::stoi(pid)});
        }
        if (!total) continue;
        const int64_t pick = int64_t(random_below(uint32_t(total)));
        prefixes.push_back(std::upper_bound(pool.begin(),pool.end(),std::pair{pick,INT32_MAX})->second);
    }
    if (!prefixes.empty()) drop["prefixes"] = std::move(prefixes);
    return drop;
}

void World::add(uint32_t id, int64_t account, int64_t character, const Json& record, bool admin) { admit(id,account,character,record,admin,false); }
void World::admit(uint32_t id, int64_t account, int64_t character, const Json& record, bool admin, bool bot) {
    try {
        const auto players = std::count_if(actors_.begin(),actors_.end(),[](const auto& p){return !p.second.bot;});
        require(id != 0 && id <= INT32_MAX && !actors_.contains(id) && !npcs_.contains(id) && (bot || players < 32),"player","Invalid player slot");
        require(account > 0 && character > 0,"identity","Invalid identity");
        for (const auto& [other,a] : actors_) { (void)other; require(a.character_id != character,"identity","Character already connected"); }
        Actor a; a.account_id = account; a.character_id = character; a.admin = admin; a.bot = bot;
        a.externally_muted = record.value("moderation",Json::object()).value("muted",false);
        const auto appearance=record.value("appearance",Json::object());
        require(valid_appearance(appearance),"appearance","Character appearance invalid");
        require(record.at("name").dump().size()<=66,"name","Character name envelope exceeded");
        a.character = {{"name",text(record.at("name"),32)},{"class_id",integer(record.at("class_id"),1,3)},{"appearance",appearance}};
        Json s = record.value("state",Json::object()); require(s.is_object(),"state","Character state must be object");
        s.erase("visual"); // persisted_visual: rebuilt on every save
        for (const auto& [key,v] : config_.at("defaults").items()) if (key != "inventory" && !s.contains(key)) s[key] = v;
        if (!s.contains("inventory")) { s["inventory"] = Json::array(); for (const auto& item : config_.at("defaults").at("inventory")) give(s,item.at("item"),item.at("quantity")); }
        if (!s.contains("equipment")) {
            s["equipment"] = Json::object();
            for (const auto& entry : s.at("inventory")) {
                const auto& definition = config_.at("items").at(entry.at("item").get<std::string>());
                if (definition.contains("slot")) s["equipment"][definition.at("slot").get<std::string>()] = entry.at("uid");
            }
        }
        // Content regeneration (tools/generate_content.cpp) can retire zones and items: persisted state falls back to
        // the default spawn and drops retired items instead of locking the character out.
        if (s.at("zone").is_string() && !config_.at("zones").contains(s.at("zone").get<std::string>())) { const auto& d=config_.at("defaults"); s["zone"]=d.at("zone"); s["x"]=d.at("x"); s["y"]=d.at("y"); }
        // A character saved inside a group's instance copy resumes at the same point of the shared zone (copies are per session).
        if (s.at("zone").is_string()) if (const auto& z = config_.at("zones").at(s.at("zone").get<std::string>()); z.contains("instance_of")) s["zone"] = z.at("instance_of");
        // Er Meydanı matches are per session: a character saved in an arena resumes at its clan headquarters (guide "Klan Karargahları").
        if (const auto& match = config_.at("activities").at("arena"); s.at("zone").is_string() && s.at("zone") == match.at("zone")) {
            const auto home = match.at("homes").value(s.value("clan",Json(0)).dump(), config_.at("defaults").at("zone").get<std::string>());
            s["zone"] = home; s["x"] = config_.at("zones").at(home).at("spawn")[0]; s["y"] = config_.at("zones").at(home).at("spawn")[1];
        }
        if (s.at("inventory").is_array() && s.at("equipment").is_object()) {
            auto& inventory=s["inventory"]; auto& equipment=s["equipment"];
            for (auto it=inventory.begin(); it!=inventory.end();) {
                if (!it->is_object() || !it->value("item",Json()).is_string() || config_.at("items").contains(it->at("item").get<std::string>())) { ++it; continue; }
                for (auto slot=equipment.begin(); slot!=equipment.end();) if (*slot==it->value("uid",Json())) slot=equipment.erase(slot); else ++slot;
                it=inventory.erase(it);
            }
        }
        // Quests: legacy {progress, completed:true} entries move to the sorted "quests_done"; regenerated content may retire quest
        // ids (dropped like retired items); quest timers restart at admission (no server tick survives a restart).
        { Json active = Json::object(), done = Json::array();
          if (s.contains("quests_done") && s.at("quests_done").is_array()) for (const auto& q : s.at("quests_done")) if (q.is_string() && config_.at("quests").contains(q.get<std::string>())) done.push_back(q);
          if (s.contains("quests") && s.at("quests").is_object()) for (const auto& [quest, p] : s.at("quests").items()) {
              if (!config_.at("quests").contains(quest) || !p.is_object()) continue;
              if (p.contains("completed") && p.at("completed") == true) { done.push_back(quest); continue; }
              Json kept{{"progress", p.contains("progress") ? p.at("progress") : Json(0)}};
              if (const auto& q = config_.at("quests").at(quest); q.contains("time_limit")) kept["deadline"] = tick_ + 20 * q.at("time_limit").get<uint32_t>();
              active[quest] = std::move(kept);
          }
          std::sort(done.begin(), done.end()); done.erase(std::unique(done.begin(), done.end()), done.end());
          s["quests"] = std::move(active); s["quests_done"] = std::move(done); }
        // Invested skill points (rank per skill id); drop ids no longer configured for this class.
        if (!s.contains("skills") || !s.at("skills").is_object()) s["skills"] = Json::object();
        for (auto it = s["skills"].begin(); it != s["skills"].end();) {
            const auto& skills = config_.at("skills");
            if (!skills.contains(it.key()) || skills.at(it.key()).at("source")[0].get<int>() + 1 != a.character.at("class_id").get<int>() ||
                !it->is_number_integer() || it->get<int64_t>() < 1) it = s["skills"].erase(it);
            else { *it = std::min(it->get<int64_t>(), config_.at("skill_rank_max").get<int64_t>()); ++it; }
        }
        // Invested resistance/craft points; a pool overspent after a config change is refunded whole.
        for (const auto& [group,stats] : {std::pair{"resistances",std::span<const char* const>(resistance_stats)},std::pair{"crafts",std::span<const char* const>(craft_stats)}}) {
            Json kept = Json::object();
            if (s.contains(group) && s.at(group).is_object()) for (const char* stat : stats)
                if (const auto& v = s.at(group).value(stat,Json()); v.is_number_integer() && v.get<int64_t>() >= 1 && v.get<int64_t>() <= 1000) kept[stat] = v;
            s[group] = std::move(kept);
            if (s.at("level").is_number_integer() && unspent_points(config_,s,group) < 0) s[group] = Json::object();
        }
        // Learned Üretim recipes (scroll item ids); retired or duplicate ids drop, memory capped.
        { Json kept = Json::array();
          if (s.contains("recipes") && s.at("recipes").is_array()) for (const auto& r : s.at("recipes"))
              if (r.is_string() && config_.at("recipes").contains(r.get<std::string>()) && std::find(kept.begin(),kept.end(),r) == kept.end() && kept.size() < config_.at("economy").at("recipe_limit").get<size_t>()) kept.push_back(r);
          s["recipes"] = std::move(kept); }
        // Banka: retired items drop (as in the bag); everything else must be a valid stored stack.
        a.bank = record.value("bank",Json{{"items",Json::array()},{"currency",0}});
        require(a.bank.is_object() && a.bank.value("items",Json()).is_array(),"state","Bank invalid");
        for (auto it=a.bank["items"].begin(); it!=a.bank["items"].end();) { if (it->is_object() && it->value("item",Json()).is_string() && !config_.at("items").contains(it->at("item").get<std::string>())) it=a.bank["items"].erase(it); else ++it; }
        require(a.bank.at("items").size() <= config_.at("economy").at("bank_slots").get<size_t>(),"state","Bank limit");
        integer(a.bank.at("currency"),0,config_.at("currency_cap"));
        { std::set<std::string> bank_uids;
          for (const auto& entry : a.bank.at("items")) {
              const auto uid = text(entry.at("uid"),32);
              require(uid.size()==32 && std::all_of(uid.begin(),uid.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');}) && bank_uids.insert(uid).second,"state","Invalid bank uid");
              integer(entry.at("quantity"),1,config_.at("items").at(text(entry.at("item"),32)).at("stack"));
          } }
        // Live buffs/cooldowns expire at disconnect; no server tick is reused across restarts.
        s["buffs"] = Json::array();
        const auto zone = text(s.at("zone"),32); require(config_.at("zones").contains(zone),"state","Unknown character zone");
        require(!config_.at("zones").at(zone).contains("source_registry_id") || source_zones_.contains(zone),"state","Original support grid required before admission");
        integer(s.at("x"),-coordinate_limit,coordinate_limit); integer(s.at("y"),-coordinate_limit,coordinate_limit);
        if (!passable(zone,s.at("x"),s.at("y"))) { s["x"] = config_.at("zones").at(zone).at("spawn")[0]; s["y"] = config_.at("zones").at(zone).at("spawn")[1]; }
        require(passable(zone,s.at("x"),s.at("y")),"state","Character spawn unsupported");
        integer(s.at("max_hp"),1,config_.at("resource_cap")); integer(s.at("max_mana"),1,config_.at("resource_cap"));
        integer(s.at("hp"),0,config_.at("resource_cap")); integer(s.at("mana"),0,config_.at("resource_cap")); integer(s.at("level"),1,100); integer(s.at("xp"),0,10000000); integer(s.at("currency"),0,config_.at("currency_cap")); // resources() clamps to the derived maxima
        require(s.at("inventory").is_array() && s.at("inventory").size() <= config_.at("inventory_slots").get<size_t>(),"state","Inventory limit");
        std::set<std::string> uids;
        for (auto& entry : s.at("inventory")) {
            const auto uid = text(entry.at("uid"),32);
            require(uid.size()==32 && std::all_of(uid.begin(),uid.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');}),"state","Invalid item UID");
            require(uids.insert(uid).second,"state","Duplicate inventory uid");const auto item=text(entry.at("item"),32);
            require(config_.at("items").contains(item),"state","Unknown inventory item");integer(entry.at("quantity"),1,config_.at("items").at(item).at("stack"));
            const auto& definition=config_.at("items").at(item);
            // Instance Efsun prefixes and Eskime; regenerated content may retire prefixes or lower the maximum.
            if (entry.contains("prefixes")) {
                require(definition.contains("slot") && entry.at("prefixes").is_array() && entry.at("prefixes").size()<=2,"state","Item prefixes invalid");
                Json kept=Json::array(); for (const auto& p : entry.at("prefixes")) if (p.is_number_integer() && p.get<int64_t>()>0 && config_.at("prefixes").contains(std::to_string(p.get<int64_t>()))) kept.push_back(p);
                if (kept.empty()) entry.erase("prefixes"); else entry["prefixes"]=std::move(kept);
            }
            if (entry.contains("durability")) {
                require(entry.at("durability").is_number_integer(),"state","Item durability invalid");
                if (definition.contains("durability")) entry["durability"]=std::clamp<int64_t>(entry.at("durability").get<int64_t>(),0,definition.at("durability").get<int64_t>()); else entry.erase("durability");
            }
        }
        require(s.at("equipment").is_object() && s.at("equipment").size() <= 8 && s.at("quests").is_object() && s.at("quests").size() <= 64,"state","Character state limits"); // = quests catalog cap; keys are validated against it below
        for (const auto& [slot,uid] : s.at("equipment").items()) { auto& entry = owned(s,text(uid,32)); require(config_.at("items").at(entry.at("item").get<std::string>()).value("slot","") == slot,"state","Equipment slot mismatch"); }
        resources(s); // stored maxima follow the current level curves and worn Maksimum Enerji/Kudret
        for (const auto& [quest,progress] : s.at("quests").items()) { (void)quest; integer(progress.at("progress"),0,1000); }
        if (s.contains("guild") && !s.at("guild").is_null()) {
            const auto& g=s.at("guild"); text(g.at("id"),128);text(g.at("name"),32);
            require(g.at("members").is_array() && g.at("members").size() <= 64,"guild","Guild roster limit");
            require(std::find(g.at("members").begin(),g.at("members").end(),Json(character)) != g.at("members").end(),"guild","Character is not a guild member");
            require(g.at("revision").is_number_integer() && g.at("revision").get<int64_t>() > 0,"guild","Guild revision invalid");
            integer(g.value("level",Json(1)),1,100); integer(g.value("funds",Json(0)),0,999000000); // Lonca Parası (guild_donate)
        }
        s.erase("kabadayi"); // Kabadayı consent is per session
        if (s.contains("clan")) require(config_.at("social").at("clans").contains(std::to_string(integer(s.at("clan"),1,15))),"state","Unknown clan");
        integer(s.value("san",Json(0)),0,1000000000); integer(s.value("clan_currency",Json(0)),0,999000000); integer(s.value("akce",Json(0)),0,999000000);
        const auto stamp = [](const Json& v) { require(v.is_number_integer() && v.get<int64_t>() >= 0, "state", "Time stamp invalid"); };
        integer(s.value("karizma",Json(100)),0,100); stamp(s.value("karizma_reset",Json(0)));
        if (s.contains("lockouts")) { require(s.at("lockouts").is_object() && s.at("lockouts").size()<=32,"state","Lockout list invalid"); for (const auto& [locked,until] : s.at("lockouts").items()) { text(locked,32); stamp(until); } }
        if (s.contains("contacts")) for (const char* list : {"friends","ignored"}) {
            const auto& entries=s.at("contacts").at(list);
            require(entries.is_array() && entries.size()<=config_.at("social").at("contacts_max").get<size_t>(),"state","Contact list limit");
            for (const auto& entry : entries) { require(entry.is_array() && entry.size()==2 && entry[0].is_number_integer() && entry[0].get<int64_t>()>0,"state","Contact entry invalid"); text(entry[1],32); }
        }
        if (s.contains("quickbar")) s["quickbar"] = quickbar_slots(config_,s.at("quickbar"));
        if (s.contains("boz_ayi")) require(s.at("boz_ayi").is_boolean(),"state","Skill variant flag invalid");
        require(s.dump().size()<=8192,"state","Character state envelope exceeded");
        a.character["state"] = std::move(s); actors_.emplace(id,std::move(a)); index(id,zone,actors_.at(id).character.at("state").at("x"),actors_.at(id).character.at("state").at("y"),true);
    } catch (const Rejected& e) { throw std::invalid_argument("character: " + e.message); }
}
void World::remove(uint32_t id) {
    auto found = actors_.find(id); if (found == actors_.end()) return;
    const auto& a = found->second; CharacterSave save{a.account_id,a.character_id,a.character.at("state")}; persisted_visual(save.state);
    persist_(std::span<const CharacterSave>(&save,1));
    cancel_trade(id); leave_group(id); invites_.erase(id); guild_invites_.erase(id);
    if (bots_.erase(id)) released_bots_.emplace_back(save.account_id,save.character_id);
    const auto& s = a.character.at("state"); const auto zone = s.at("zone").get<std::string>(); index(id,zone,s.at("x"),s.at("y"),false); actors_.erase(found);
    for (auto& [npc_id,n] : npcs_) { (void)npc_id; n.threat.erase(id); if (n.owner == id) n.owner = 0; }
    for (auto& [name,match] : arenas_) if (const auto stake = match.stakes.extract(id)) match.stakes[0] += stake.mapped(); // a quitter's stake stays in the pot
    vacated(zone);
}
void World::input(uint32_t id, int8_t dx, int8_t dy, uint32_t sequence) {
    if (!actors_.contains(id)) return;
    auto& a = actors_.at(id);
    if (a.kicked || sequence <= a.sequence) return;
    if (std::abs(int(dx))+std::abs(int(dy)) > 1) { reject(id,"movement","Geçersiz hareket."); return; }
    a.sequence = sequence; a.dx = dx; a.dy = dy; a.moving = false;a.route.clear();
    if (dx || dy) a.engaged = 0; // moving away ends auto-attack
}
void World::destination(uint32_t id, int32_t x, int32_t y, uint32_t sequence) {
    if (!actors_.contains(id)) return;
    auto& a = actors_.at(id); if (a.kicked || sequence <= a.sequence) return;
    a.sequence = sequence; const auto& s = a.character.at("state");
    auto points=route(s.at("zone"),s.at("x"),s.at("y"),x,y);
    if (points.empty()) { a.moving = false; a.route.clear();a.dx = a.dy = 0; emit(id,{{"type","correction"},{"x",s.at("x")},{"y",s.at("y")},{"reason","blocked_destination"}}); return; }
    a.route=std::move(points);a.waypoint=0; a.moving = true; a.dx = a.dy = 0; a.engaged = 0;
}

// ponytail: linear social-session lookup is capped at 32 actors; index by actor if the cap grows.
World::Trade* World::trade(uint32_t id) { for (auto& [key,value] : trades_) { (void)key; if (value.a == id || value.b == id) return &value; } return nullptr; }
void World::cancel_trade(uint32_t id) {
    for (auto it=trades_.begin();it!=trades_.end();++it) if (it->second.a == id || it->second.b == id) { emit(it->second.a,{{"type","trade_cancel"}}); emit(it->second.b,{{"type","trade_cancel"}}); trades_.erase(it); return; }
}
std::pair<Json,Json> World::trade_events(const Trade& t) const {
    const auto resolve=[&](uint32_t owner,const Json& offer,bool accepted) {
        Json result{{"items",Json::array()},{"currency",offer.at("currency")},{"accepted",accepted}};
        const auto& state=actors_.at(owner).character.at("state");
        for(const auto& entry:offer.at("items")) {
            const auto& owned_entry=owned(state,entry.at("uid").get_ref<const std::string&>());const auto& item=owned_entry.at("item");
            const auto& definition=config_.at("items").at(item.get_ref<const std::string&>());
            // 0x437590 item title: "<prefix1> <prefix2> <name>" (VOD 34:00 "Savunma Artırıcı Kanvaslı Deri Eldiven").
            std::string name;const Json& prefixes=owned_entry.contains("prefixes")?owned_entry.at("prefixes"):definition.contains("prefixes")?definition.at("prefixes"):item;
            if(prefixes.is_array())for(const auto& p:prefixes)name+=config_.at("prefixes").at(std::to_string(p.get<int>())).at("name").get<std::string>()+" ";
            name+=definition.at("name").get<std::string>();
            if(Json(name).dump().size()>64)name=definition.at("name").get<std::string>(); // trade schema envelope
            result["items"].push_back({{"uid",entry.at("uid")},{"item",item},{"name",std::move(name)},{"quantity",entry.at("quantity")}});
        }
        return result;
    };
    auto first=resolve(t.a,t.offer_a,t.accept_a),second=resolve(t.b,t.offer_b,t.accept_b);
    Json a{{"type","trade"},{"peer",t.b},{"peer_name",actors_.at(t.b).character.at("name")},{"initiator",t.a},
           {"phase",t.open?"open":"invited"},{"revision",t.revision},{"own",first},{"other",second}};
    Json b{{"type","trade"},{"peer",t.a},{"peer_name",actors_.at(t.a).character.at("name")},{"initiator",t.a},
           {"phase",t.open?"open":"invited"},{"revision",t.revision},{"own",std::move(second)},{"other",std::move(first)}};
    require(a.dump().size()<=32768 && b.dump().size()<=32768,"config","Alışveriş şartları çok büyük.");
    return {std::move(a),std::move(b)};
}
void World::trade_event(const Trade& t) {
    auto [a,b]=trade_events(t);emit(t.a,std::move(a));emit(t.b,std::move(b));
}
void World::validate_offer(uint32_t id, const Json& offer) {
    require(offer.at("items").is_array() && offer.at("items").size() <= 32,"trade","Geçersiz alışveriş teklifi.");
    int amount = integer(offer.at("currency"),0,config_.at("currency_cap"));const auto& s=actor(id).character.at("state");
    require(s.at("currency").get<int64_t>() >= amount,"currency","Yeterli paranız yok.");
    std::array<std::string_view,32> seen{};size_t count=0;
    for (const auto& item : offer.at("items")) {
        const auto& value=item.at("uid");require(value.is_string(),"request","Eşya belirtilmeli.");
        const auto& uid=value.get_ref<const std::string&>();
        require(!uid.empty() && uid.size()<=32 && uid.find('\0')==std::string::npos,"request","Geçersiz eşya kimliği.");
        seen[count++]=uid;
        const auto& entry=owned(s,uid); require(entry.at("quantity").get<int>() >= integer(item.at("quantity"),1,1000),"quantity","Teklif ettiğiniz kadar eşyanız yok.");
        for(const auto& [slot,equipped]:s.at("equipment").items()){(void)slot;require(equipped!=uid,"equipped","Giyilmekte olan nesnelerle alışveriş yapılamaz; giysiyi önce üzerinizden çıkarın.");} // 0x591bf0
        // 00431220: classes.dat itemflag bits 2|4 bind an item (uid 14000 excepted); 0x591bbc.
        const auto& d=config_.at("items").at(entry.at("item").get<std::string>());
        require(!(d.value("flags",0)&6) || d.value("source_uid",0)==14000,"bound","Bu eşya size bağlanmıştır. Kimseye veremezsiniz.");
    }
    std::sort(seen.begin(),seen.begin()+count);
    require(std::adjacent_find(seen.begin(),seen.begin()+count)==seen.begin()+count,"trade","Aynı eşya iki kez teklif edilemez.");
}
uint32_t World::group(uint32_t id) const { for (const auto& [leader,members] : groups_) if (std::find(members.begin(),members.end(),id) != members.end()) return leader; return 0; }
void World::leave_group(uint32_t id) {
    auto leader = group(id); if (!leader) return; auto members = groups_.at(leader); groups_.erase(leader); std::erase(members,id);
    emit(id,{{"type","group"},{"leader",0},{"members",Json::array()}});
    if (members.size() > 1) { auto next = members.front(); groups_[next] = members; for (auto member : members) emit(member,{{"type","group"},{"leader",next},{"members",members}}); }
    else for (auto member : members) emit(member,{{"type","group"},{"leader",0},{"members",Json::array()}});
}

// Authoritative hostile-NPC damage shared by attacks, skills and skill damage over time:
// kill rewards, quest progress, loot ownership and projected hit/death events. Callers validate range/cooldowns.
bool World::combat(uint32_t id, uint32_t target, int damage, Json caster, Json event, double crit, double multiplier) {
    auto& n = npcs_.at(target);
    // A skill's element meets the monster's classes.dat resistance (spells always land; physical skills ignore dmgred) and
    // rolls the cast's crit chance (×multiplier).
    bool critical = false;
    if (const auto skill = event.find("skill"); skill != event.end() && config_.at("skills").contains(skill->get<std::string>())) {
        auto blow = spell(damage,config_.at("skills").at(skill->get<std::string>())); blow.crit = crit; blow.multiplier = multiplier;
        const auto landed = land_npc(blow,n.definition,n.debuffs); damage = landed.damage; critical = landed.critical;
        if (critical) event["critical"] = true;
    }
    damage = damage * (100 + buff_sum(n.debuffs, "vulnerable")) / 100;
    uint32_t owner = n.owner;
    if (!owner || !actors_.contains(owner) || actors_.at(owner).character.at("state").at("hp").get<int>() == 0) owner = id;
    const int hp = std::max(0,n.hp-damage); std::map<uint32_t,Json> proposed{{id,std::move(caster)}}, quest_events;
    if (!hp) {
        std::vector<uint32_t> recipients{owner}; auto leader = group(owner);
        if (leader) { recipients.clear(); for (auto member : groups_.at(leader)) if (close(owner,member,config_.at("aoi")) && actors_.at(member).character.at("state").at("hp").get<int>() > 0) recipients.push_back(member); }
        if (recipients.empty()) recipients.push_back(owner);
        for (auto member : recipients) {
            if (!proposed.contains(member)) proposed[member] = actors_.at(member).character.at("state");
            auto& reward = proposed.at(member);
            int amount = n.definition.at("xp"); if (reward.at("level").get<int>() > n.definition.value("level",1)+5) amount /= 2;
            xp(reward,amount/static_cast<int>(recipients.size()));
            quest_kill(reward, n.definition, quest_events[member] = Json::array());
            // NONCANONICAL lockout: a Şef's kill credit in a group-zone copy bars re-entry to that group zone (handle "interact").
            if (const auto& z = config_.at("zones").at(n.definition.at("zone").get<std::string>()); n.definition.value("boss",false) && z.contains("instance_of"))
                reward["lockouts"][z.at("instance_of").get<std::string>()] = int64_t(std::time(nullptr)) + config_.at("activities").at("lockout").get<int64_t>();
        }
        // Guide groups randomly assign loot among nearby members; this RNG is not an original-server formula.
        if (recipients.size() > 1) { unsigned int value; if (RAND_bytes(reinterpret_cast<unsigned char*>(&value),sizeof(value)) != 1) throw std::runtime_error("loot assignment failed"); owner = recipients[value % recipients.size()]; }
    }
    // A summon's bite carries its own NPC id as "source": it draws the threat and animates; kill credit stays with `id`.
    const uint32_t source = event.value("source",id);
    commit(proposed); n.owner = owner; n.hp = hp; n.threat[source] += damage;
    for (auto& [member, events] : quest_events) for (auto& e : events) emit(member, std::move(e));
    if (!hp) {
        n.respawn_at = tick_ + n.definition.at("respawn").get<uint32_t>(); n.looted = false; n.debuffs = Json::array();
        // Ganimet slots (psba window, 3x3): currency first (granted when the box opens), then the loot table and the rolled drop.
        // A felled Klan OOK leaves no Ganimet (the client draws sacks for monsters only).
        if (guard(n.definition)) n.drop = nullptr;
        else {
            n.drop = Json::array(); if (n.definition.at("currency").get<int64_t>()) n.drop.push_back({{"currency",n.definition.at("currency")}});
            for (const auto& item : n.definition.at("loot")) n.drop.push_back(item);
            if (auto rolled = roll_drop(n.definition,n.definition.value("boss",false)); rolled.is_object()) { rolled["quantity"] = 1; n.drop.push_back(std::move(rolled)); } // guide: Şefs drop important items
        }
    }
    event["entity"]=source; event["source"]=source; event["target"]=target; event["damage"]=damage; event["hp"]=hp; event["owner"]=owner;
    project_event(source,std::move(event));
    if(!hp)project_event(target,{{"type","death"},{"entity",target},{"source",source},{"target",target}});
    // Guide "Er Meydanı": "Karşı klandan oyuncu veya muhafızları öldürmek ve bayrakları ele geçirmek puan kazandırır."
    if(const auto zone=n.definition.at("zone").get<std::string>();!hp && n.definition.contains("clan") && arenas_.contains(zone) && actors_.contains(id)) {
        const bool banner=n.definition.value("banner",false);
        arenas_.at(zone).score[actors_.at(id).character.at("state").value("clan",0)]+=config_.at("activities").at("arena").at(banner?"banner_points":"guardian_points").get<int64_t>();
        arena_say(zone,actors_.at(id).character.at("name").get<std::string>()+(banner?" sancak düşürdü: ":" muhafız alt etti: ")+n.definition.at("name").get<std::string>()+".");
    }
    return critical;
}
// Timed skill effects on a hostile NPC or player (Durdurma, Dikkat Dağıtma, Kanatma, Zehirleme, Direnç Kırma Alanı...) and taunts.
void World::hostile_skill(uint32_t id, uint32_t target, const Json& definition, const std::string& skill, int dot) {
    Json debuff{{"id",skill},{"source",id},{"skill_source",config_.at("skills").at(skill).at("source")},{"expires",tick_+definition.value("duration",0u)}};
    if (definition.value("stun",false)) debuff["stun"] = true;
    if (definition.value("silence",false)) debuff["silence"] = true;
    if (definition.contains("slow")) debuff["speed"] = -definition.at("slow").get<int>();
    if (definition.contains("vulnerable")) debuff["vulnerable"] = definition.at("vulnerable");
    // Meteorit armor_break, Kışkırtma defense_break, Asit Saldırısı Tılsım heal_cut and Yıldırım's electric mark count (NONCANONICAL).
    for (const char* key : {"armor_break","defense_break","heal_cut","electric"}) if (definition.contains(key)) debuff[key] = definition.at(key);
    // Detail text: damage every 3 seconds; Kanatma's drain (10/15 puan roll, "hedefin kudretini azaltan bir etki") also takes Kudret.
    if (definition.contains("dot") || definition.value("drain",false)) debuff["next"] = tick_+60;
    if (definition.contains("dot")) debuff["dot"] = dot;
    if (definition.value("drain",false)) debuff["drain"] = dot;
    // One effect per skill and kind: a Buz Oku crit stun (1 s) and its slow (3 s), or Boz Ayı's bleed and its Kudret drain, coexist.
    const auto same = [&](const Json& b) { return b.at("id") == skill && b.value("stun",false) == debuff.value("stun",false) && b.contains("dot") == debuff.contains("dot"); };
    // A refreshed effect keeps its pending damage tick (re-applying every bite or cast must not postpone the 3 s tick forever).
    const auto replace = [&](Json& list) { for (auto it=list.begin();it!=list.end();) if (same(*it)) { if (it->contains("next") && debuff.contains("next")) debuff["next"] = std::min(debuff.at("next").get<uint32_t>(),it->at("next").get<uint32_t>()); it=list.erase(it); } else ++it;
                                           if (list.size() < 8) list.push_back(std::move(debuff)); };
    if (const auto p = actors_.find(target); p != actors_.end()) {
        // PvP: the effect lands as a harmful buff (Zihin Toplama/Büyü Bozma cleanse it); Sarsılmaz ignores it; no threat.
        const auto& s = p->second.character.at("state");
        if (!definition.contains("duration") || s.at("hp").get<int>() == 0 || buff_flag(s.at("buffs"),"immune")) return;
        // armor 0: every own-state buff carries the client-validated Savunma field (client_session buffs schema).
        auto next = s; debuff["harmful"] = true; debuff["armor"] = 0; replace(next.at("buffs")); commit({{target,next}}); return;
    }
    auto& n = npcs_.at(target); if (!n.hp) return;
    int64_t top = 0; for (const auto& [player,threat] : n.threat) { (void)player; top = std::max(top,threat); }
    n.threat[id] = definition.value("taunt",false) ? top+1 : n.threat[id]+1;
    if (definition.contains("duration")) replace(n.debuffs);
}
// NONCANONICAL Şef skills (generator: classes.dat normalskillgroup/onhitskillgroup ids mapped onto configured skills). A ready
// "cast" entry fires while the Şef fights: radius 0 at its victim, else at every living player within the radius (always lands,
// the skill's school against resistances); a "hit" entry rides a bite that drew blood. Either then lays the skill's effect
// (stun/slow/silence/vulnerable/DoT; the entry's duration overrides) like a player's cast, and all observers see the cast.
void World::boss_skills(uint32_t id, Npc& n, const char* on, uint32_t victim) {
    const auto& list = n.definition.at("skills"); n.skill_ready.resize(list.size());
    const auto zone = n.definition.at("zone");
    for (size_t i = 0; i < list.size() && n.hp > 0; ++i) {
        const auto& e = list[i]; if (e.at("on") != on || tick_ < n.skill_ready[i]) continue;
        const auto skill = e.at("skill").get<std::string>(); auto effect = config_.at("skills").at(skill);
        if (e.contains("duration")) effect["duration"] = e.at("duration");
        const int damage = e.at("damage"); const int64_t r = e.at("radius");
        std::vector<uint32_t> hit;
        if (r) { for (const auto& [pid,p] : actors_) { const auto& s = p.character.at("state"); if (!p.kicked && s.at("hp").get<int>() > 0 && s.at("zone") == zone && distance(n.x,n.y,s.at("x"),s.at("y")) <= r*r) hit.push_back(pid); } }
        else if (actors_.contains(victim) && actors_.at(victim).character.at("state").at("hp").get<int>() > 0) hit.push_back(victim);
        if (hit.empty()) continue;
        n.skill_ready[i] = tick_ + e.at("cooldown").get<uint32_t>();
        project_event(id,{{"type","skill"},{"entity",id},{"source",id},{"target",r ? id : hit[0]},{"skill",skill},{"kind",r ? "enemies" : "enemy"},{"skill_source",effect.at("source")}});
        for (const auto pid : hit) {
            // A rider whose skill lays no effect (Ağır Vuruş, Meteorit, Süpürme Vuruşu) adds half its damage to the bite instead.
            if (const int blow = std::string_view(on) == "cast" ? damage : effect.contains("duration") ? 0 : damage/2; blow) strike(id,pid,spell(blow,effect));
            try { hostile_skill(id,pid,effect,skill,effect.contains("dot") ? std::max(1,damage/4) : 0); }
            catch (const std::exception&) { reject(pid,"persistence","Etki kaydedilemedi."); }
        }
    }
}
// Boz Ayı / Tesla Küresi / Çağrı (skills.dat detail text): a classes.dat summon template (hp/xp/currency 1) appears at the
// caster and "çağrıldığı alanda kalır ve orada savaşır" (stays and fights where it was called) until its Süre ends.
// NONCANONICAL: energy = caster maximum × hp_percent (+hp_bonus rows by rank), bite = the skill's ranked damage,
// Boz Ayı's 60 s bleed ticks a quarter of it. One summon per skill: recasting replaces it (Boz Ayı redirects instead).
void World::summon(uint32_t id, const std::string& skill, const Json& definition, int rank, int damage) {
    const auto& s = actor(id).character.at("state"); const auto& spec = definition.at("summon");
    for (auto& [other,n] : npcs_) { (void)other; if (n.master == id && n.definition.at("skill") == skill) n.expires = tick_; }
    int bonus = 0; if (spec.contains("hp_bonus")) for (const auto& row : spec.at("hp_bonus")) if (row[0].get<int>() <= rank) bonus = row[1];
    const auto& model = spec.at("models")[random_below(uint32_t(spec.at("models").size()))]; // Çağrı: one of the Evcil/Kangal templates
    if (next_summon_ < 0xf0000000u) next_summon_ = 0xf0000000u;
    while (npcs_.contains(next_summon_)) ++next_summon_;
    const auto sid = next_summon_++;
    Npc n; n.master = id; n.x = s.at("x"); n.y = s.at("y");
    n.expires = tick_ + definition.at("duration").get<uint32_t>() + spec.at("per_rank").get<uint32_t>()*uint32_t(rank-1);
    n.hp = std::max(1, int(int64_t(s.at("max_hp").get<int>())*spec.at("hp_percent").get<int>()*(100+bonus)/10000));
    n.definition = {{"kind","summon"},{"skill",skill},{"name",model[2]},{"zone",s.at("zone")},{"x",n.x},{"y",n.y},{"hp",n.hp},
        {"source_uid",model[0]},{"source_class",model[1]},{"quests",Json::array()},{"shop",Json::array()},{"damage",std::max(1,damage)},
        {"attack_cooldown",spec.at("attack_cooldown")},{"attack_range",spec.at("attack_range")},{"aggro",spec.at("aggro")},{"speed",spec.at("speed")}};
    if (spec.contains("bleed")) n.definition["bleed"] = spec.at("bleed");
    index(sid,s.at("zone"),n.x,n.y,true); npcs_.emplace(sid,std::move(n));
}
// Summons fight their commanded target or the nearest living hostile NPC or player hostile to their caster (relation
// 00478090: Kabadayı / enemy clan, via hostile()) within the template's aggro, otherwise return to where they stay; they leave
// at expiry or when the caster dies, leaves the zone or disconnects. Kills credit the caster.
void World::summons() {
    for (auto it = npcs_.begin(); it != npcs_.end();) {
        const auto sid = it->first; auto& n = it->second;
        if (!n.master) { ++it; continue; }
        const auto zone = n.definition.at("zone").get<std::string>(); const auto m = actors_.find(n.master);
        if (n.hp && (m == actors_.end() || m->second.character.at("state").at("hp").get<int>() == 0 || m->second.character.at("state").at("zone") != zone)) n.expires = tick_;
        if (tick_ >= n.expires) { index(sid,zone,n.x,n.y,false); it = npcs_.erase(it); continue; }
        ++it;
        if (!n.hp) continue; // a killed summon lies until it vanishes
        const auto spot = [&](uint32_t v) { if (const auto p = actors_.find(v); p != actors_.end()) return std::pair<int32_t,int32_t>{coordinate(p->second.character.at("state"),"x"),coordinate(p->second.character.at("state"),"y")}; const auto& e = npcs_.at(v); return std::pair<int32_t,int32_t>{e.x,e.y}; };
        uint32_t victim = 0;
        if (n.command) {
            const auto t = npcs_.find(n.command);
            if (actors_.contains(n.command) ? hostile(n.master,n.command) : t != npcs_.end() && t->second.hp > 0 && t->second.definition.at("zone") == zone) victim = n.command; else n.command = 0;
        }
        // ponytail: linear NPC/player scan per summon (≤32 summons × ≤1024 NPCs); use the cell index if populations grow.
        int64_t nearest = int64_t(n.definition.at("aggro").get<int>())*n.definition.at("aggro").get<int>();
        if (!victim) {
            for (const auto& [eid,e] : npcs_) if (e.hp > 0 && !e.master && e.definition.at("zone") == zone &&
                                                  (actors_.contains(n.master) ? strikable(e.definition,actors_.at(n.master).character.at("state")) : e.definition.at("kind") == "enemy" && !e.definition.contains("clan")))
                if (const auto d = distance(n.x,n.y,e.x,e.y); d <= nearest) { nearest = d; victim = eid; }
            for (const auto& [pid,p] : actors_) if (!p.kicked && hostile(n.master,pid))
                if (const auto [px,py] = spot(pid); distance(n.x,n.y,px,py) <= nearest) { nearest = distance(n.x,n.y,px,py); victim = pid; }
        }
        if (!victim) { walk(sid,n,n.definition.at("x"),n.definition.at("y"),100); continue; }
        const auto [fx,fy] = spot(victim); const int64_t reach = n.definition.at("attack_range").get<int>();
        if (distance(n.x,n.y,fx,fy) > reach*reach) { walk(sid,n,fx,fy,100); continue; }
        if (tick_ < n.attack_ready) continue;
        n.attack_ready = tick_ + n.definition.at("attack_cooldown").get<uint32_t>();
        const int damage = n.definition.at("damage");
        const bool player = actors_.contains(victim);
        try {
            // NONCANONICAL: on a player the bite is a physical blow that always lands (Zırh, Sarsılmaz, Can Kurtaran apply).
            if (player) strike(sid,victim,Blow{-1,double(damage),0,2,{},""},n.master);
            else combat(n.master,victim,damage,m->second.character.at("state"),{{"type","combat"},{"source",sid}});
            if (n.definition.contains("bleed")) hostile_skill(n.master,victim,{{"duration",n.definition.at("bleed")},{"dot",true}},n.definition.at("skill"),std::max(1,damage/4));
            // Boz Ayı: "Defansif mod açıksa 10 sn boyunca hedefin kudretini azaltan bir etki bırakır" (monsters have no Kudret).
            if (player && n.definition.contains("bleed") && active(m->second.character.at("state").at("buffs"),"defansif_dovusme"))
                hostile_skill(n.master,victim,{{"duration",200},{"drain",true}},n.definition.at("skill"),std::max(1,damage/4));
        } catch (const Rejected& e) { reject(n.master,e.code,e.message); } catch (const std::exception&) { reject(n.master,"persistence","Dövüş sonucu kaydedilemedi."); }
    }
}
// A hostile NPC's bite on the summon that drew its threat; a killed summon's body lies 2 s for the death clip.
void World::hit_summon(uint32_t source, uint32_t summon, int damage) {
    auto& n = npcs_.at(summon); n.hp = std::max(0,n.hp-damage);
    project_event(source,{{"type","combat"},{"entity",source},{"source",source},{"target",summon},{"damage",damage},{"hp",n.hp}});
    if (!n.hp) { n.expires = tick_+40; project_event(summon,{{"type","death"},{"entity",summon},{"source",source},{"target",summon}}); }
}
void World::walk(uint32_t id, Npc& n, int32_t gx, int32_t gy, int pace, int speed) {
    const auto zone = n.definition.at("zone").get<std::string>(); const bool source = source_zones_.contains(zone);
    if(source && (!n.route_planned || n.goal_x!=gx || n.goal_y!=gy)) {
        n.route=route(zone,n.x,n.y,gx,gy);n.route_waypoint=0;n.goal_x=gx;n.goal_y=gy;n.route_planned=true;
    }
    if(source && n.route.empty())return;
    const auto [tx,ty]=source?n.route.at(n.route_waypoint):std::pair<int32_t,int32_t>{gx,gy};
    const auto [nx,ny]=toward(n.x,n.y,tx,ty,std::max(1,(speed?speed:n.definition.at("speed").get<int>())*pace/100),source);
    if(source && nx==tx && ny==ty && ++n.route_waypoint==n.route.size())n.route.clear();
    bool clear=source ? passable(zone,nx,ny) : clear_path(zone,n.x,n.y,nx,ny); // source steps follow a validated route()
    if(clear && (nx!=n.x || ny!=n.y)) { index(id,zone,n.x,n.y,false);n.x=nx;n.y=ny;index(id,zone,n.x,n.y,true); }
}
// classes.dat idlewalktend (0-99; 0 on most records, 50 on townsfolk, 99 on roaming bosses) is a server field: the client loader
// 00439bd0 never reads it. NONCANONICAL reading: every 5 s a resting NPC takes a walk with that percent chance, at its
// animation.dat walk speed (state 2 forwardspeed, `walk_speed`), to a reachable point within 12 units of its spawn (VOD 04:45:
// a Yeni Cami Dayı stands a few seconds, then walks off past the players beside him). It stands for a minute after serving a
// player (npc()) and while it is someone's open bank, so shops, banks and quest dialogs stay in reach.
void World::idle_walk(uint32_t id, Npc& n, int pace) {
    const auto& d=n.definition; const int tend=d.value("idlewalktend",0);
    if(!tend || !pace) return;
    const auto zone=d.at("zone").get<std::string>();
    if(n.wandering) {
        const auto [x,y]=std::pair{n.x,n.y}; walk(id,n,n.goal_x,n.goal_y,pace,d.at("walk_speed").get<int>());
        if((n.x==n.goal_x && n.y==n.goal_y) || (n.x==x && n.y==y)) { n.wandering=false; n.idle_at=tick_+100; } // arrived or blocked
        return;
    }
    if(tick_<n.idle_at) return;
    n.idle_at=tick_+100;
    if(random_below(100)>=uint32_t(tend)) return;
    for(const auto& player:actors_) if(player.second.banker==id) return;
    const double angle=uniform(0,2*M_PI),reach=std::min(12000,d.value("leash",24000)/2)*std::sqrt(uniform(0,1)); // inside half any leash
    const int32_t gx=d.at("x").get<int32_t>()+int32_t(std::lround(reach*std::cos(angle))),gy=d.at("y").get<int32_t>()+int32_t(std::lround(reach*std::sin(angle)));
    if(!passable(zone,gx,gy)) return;
    if(source_zones_.contains(zone)) { // plan here so an unreachable pick costs one search and walk() reuses the route
        n.route=route(zone,n.x,n.y,gx,gy);n.route_waypoint=0;n.route_planned=!n.route.empty();
        if(n.route.empty()) return;
    }
    n.goal_x=gx;n.goal_y=gy;n.wandering=true;
}
// Auto-attack: run into weapon range, then swing every Saldırı Hızı (00402f60: weapon attackspeed, bare hands 1 s) until the
// target dies or leaves, the player moves or picks another target. Weapon "attack_range" carries classes.dat attackrange;
// bare hands fall back to combat_range (NONCANONICAL).
void World::engage(uint32_t id, Actor& a) {
    const auto& s = a.character.at("state");
    const auto found = npcs_.find(a.engaged);
    const auto player = actors_.find(a.engaged); // Kabadayı/enemy-clan auto-attack (hostile() re-checked every swing)
    const bool pvp = player != actors_.end();
    // Attacking needs a living target; looting (walk over, then claim the corpse's Ganimet) a dead one.
    if ((pvp ? a.looting || !hostile(id,a.engaged) : found == npcs_.end() || (found->second.hp > 0) == a.looting || found->second.definition.at("zone") != s.at("zone")) || !close(id,a.engaged,config_.at("aoi"))) {
        a.engaged = 0; a.moving = false; a.route.clear(); return;
    }
    const int32_t tx = pvp ? coordinate(player->second.character.at("state"),"x") : found->second.x;
    const int32_t ty = pvp ? coordinate(player->second.character.at("state"),"y") : found->second.y;
    const Json* weapon = s.at("equipment").contains("weapon") ? &config_.at("items").at(owned(s,s.at("equipment").at("weapon")).at("item").get<std::string>()) : nullptr;
    const bool talking = !pvp && found->second.definition.at("kind") != "enemy" && !strikable(found->second.definition,s); // "interact" engages a friendly NPC
    const int range = a.looting || talking ? config_.at("interaction_range").get<int>() : weapon ? weapon->value("attack_range",config_.at("combat_range").get<int>()) : config_.at("combat_range").get<int>();
    if (!close(id,a.engaged,range)) {
        // Re-plan only when the target left the planned end point's reach (A* per tick is too costly).
        if (a.moving && !a.route.empty() && distance(a.route.back().first,a.route.back().second,tx,ty) <= int64_t(range/2)*(range/2)) return;
        a.route = route(s.at("zone"),s.at("x"),s.at("y"),tx,ty); a.waypoint = 0; a.moving = !a.route.empty(); a.dx = a.dy = 0;
        if (!a.moving) { a.engaged = 0; reject(id,"range","Hedefe ulaşılamıyor."); }
        return;
    }
    a.moving = false; a.route.clear(); a.dx = a.dy = 0;
    if (a.looting || talking) {
        const Json request{{"action",talking?"interact":"loot"},{"target",a.engaged}}; a.engaged = 0;
        try { handle(id,request); } catch (const Rejected& e) { reject(id,e.code,e.message); } catch (const std::exception&) { reject(id,"persistence","Ganimet kaydedilemedi."); }
        return;
    }
    if (tick_ < a.attack_ready || buff_flag(s.at("buffs"),"immune")) return;
    const auto f = fighter(s,a.character.at("class_id")); a.attack_ready = tick_ + uint32_t(std::max(1L,std::lround(f.speed*20)));
    const auto blow = swing(f,s.at("buffs"));
    if (pvp) { strike(id,a.engaged,blow); return; }
    // A missed swing (Saldırı vs classes.dat defense) still draws threat and plays the target's Dodge clip; only hits wear the weapon.
    const auto landed = land_npc(blow,found->second.definition,found->second.debuffs);
    Json caster = s, event{{"type","combat"}};
    if (!landed.hit) event["miss"] = true; else wear(caster,true,item_rule(config_,"hit_wear_permille"));
    if (landed.critical) event["critical"] = true;
    try { combat(id,a.engaged,landed.damage,std::move(caster),std::move(event)); }
    catch (const Rejected& e) { reject(id,e.code,e.message); } catch (const std::exception&) { reject(id,"persistence","Dövüş sonucu kaydedilemedi."); }
}
// Original relation 00478090 between two PCs returns 1 (hostile: red map dot 004b40ba, purple name 00471e41, attackable)
// for different nonzero clans or two Kabadayı players. NONCANONICAL: both alive, same zone, outside social.safe_zones.
bool World::hostile(uint32_t a, uint32_t b) const {
    const auto x=actors_.find(a), y=actors_.find(b);
    if (a==b || x==actors_.end() || y==actors_.end()) return false;
    const auto& s=x->second.character.at("state"); const auto& t=y->second.character.at("state");
    if (s.at("zone")!=t.at("zone") || s.at("hp").get<int>()<=0 || t.at("hp").get<int>()<=0) return false;
    const auto& zone=s.at("zone"); const auto& base=config_.at("zones").at(zone.get<std::string>()).value("instance_of",zone);
    // Guide: no PvP in Mısır Çarşısı, Antrepo, Labirent, Yeraltı and the group zones (their copies included).
    for (const auto& safe : config_.at("social").at("safe_zones")) if (safe==zone || safe==base) return false;
    const int c=s.value("clan",0), d=t.value("clan",0);
    return (c && d && c!=d) || (s.value("kabadayi",false) && t.value("kabadayi",false));
}
bool World::ignores(uint32_t listener, int64_t speaker) const {
    const auto& s=actors_.at(listener).character.at("state");
    if (s.contains("contacts")) for (const auto& entry : s.at("contacts").at("ignored")) if (entry[0]==speaker) return true;
    return false;
}
// A blow on a player from an NPC, a hostile player or a skill: Saldırı vs Savunma, Zırh %, resistances, Kritik Zırhı, Direnç Kırma
// vulnerability, Sarsılmaz/Can Kurtaran, death and, for a kill across clans, the killer's Şan and Klan Parası (amounts NONCANONICAL).
// A miss projects a "miss" combat event (the victim's Dodge clip). False when the commit failed; `critical` reports a crit.
bool World::strike(uint32_t source, uint32_t victim, const Blow& blow, uint32_t credit, bool* critical) {
    // NONCANONICAL `auto_retaliate` (off by default): an idle player swung at by an enemy NPC or a hostile player takes it as the
    // auto-attack target. The original client sends nothing on being hit (its only command-sending inbound handlers are hcpe
    // 004a6720, vldw 004a4900 and tzrt 004a8940), so any retaliation was the official server's and is unknown.
    if(config_.value("auto_retaliate",false)) if(auto& v=actors_.at(victim); !v.engaged && !v.moving && !v.dx && !v.dy && !v.gathering && v.character.at("state").at("hp").get<int>()>0) {
        const auto n=npcs_.find(source);
        if(actors_.contains(source) ? hostile(victim,source) : n!=npcs_.end() && !n->second.master && n->second.hp>0 && n->second.definition.at("kind")=="enemy" && !n->second.definition.value("banner",false)) { v.engaged=source; v.looting=false; }
    }
    const auto& defender=actors_.at(victim); const auto& s=defender.character.at("state");
    const auto f=fighter(s,defender.character.at("class_id")); const auto landed=land(blow,f.defense,f.armor/100,f.crit_armor,f.resist);
    // Defansif Dövüşme 1 Tılsım (NONCANONICAL "zırh bonusunun üçte biri"): in the open stance a physical blow misses with chance Zırh % × V/100 %.
    const int dodge=blow.physical>0 && active(s.at("buffs"),"defansif_dovusme") ? glyph(config_,s,"defansif_dovusme","armor_dodge") : -1;
    if(!landed.hit || (dodge>0 && random_below(10000)<uint32_t(f.armor*dodge))) { project_event(source,{{"type","combat"},{"entity",source},{"source",source},{"target",victim},{"damage",0},{"hp",s.at("hp")},{"miss",true}}); return true; }
    auto next=s; wear(next,false,item_rule(config_,"hit_wear_permille")); // NONCANONICAL: every landed hit wears the worn armour
    int damage=landed.damage*(100+buff_sum(s.at("buffs"),"vulnerable"))/100;
    // Sarsılmaz ignores attacks; Can Kurtaran absorbs one hit above 5% of maximum health (skills.dat detail text).
    if(buff_flag(s.at("buffs"),"immune"))damage=0;
    else if(damage*20>s.at("max_hp").get<int>()) { auto& list=next.at("buffs"); for(auto it=list.begin();it!=list.end();++it) if(it->contains("absorb")) { damage=damage*(100-it->at("absorb").get<int>())/100;list.erase(it);break; } }
    next["hp"]=std::max(0,s.at("hp").get<int>()-damage);const bool dead=next.at("hp").get<int>()==0;if(dead)die(victim,next);
    const int hp=next.at("hp");std::map<uint32_t,Json> proposed{{victim,std::move(next)}}; uint32_t scorer=0;
    if(const auto killer=actors_.find(credit ? credit : source);dead && killer!=actors_.end()) {
        const auto& k=killer->second.character.at("state");
        if(k.value("clan",0) && s.value("clan",0) && k.value("clan",0)!=s.value("clan",0)) {
            // Guide "Klan savaşlarının kuralları": 100 Şan between two 3000+ Şan players, 10 when one has 3000, else 1, scaled by both
            // Karizmas (0% earns/gives none; at least 1); Klan Parası equals the Şan; the victim loses 20% Karizma. "Şan puanı kazanımı
            // iki karakter arasındaki seviye farkına göre azalır ve bir noktadan sonra şan kazanamamaya başlarsınız": NONCANONICAL
            // numbers social.san_falloff {free, zero}: a killer up to `free` levels above the victim gains in full, then linearly
            // less (still at least 1), nothing from `zero` levels above.
            const int64_t ks=k.value("san",int64_t(0)), vs=s.value("san",int64_t(0)); const int kk=karizma(k), vk=karizma(s);
            const auto& falloff=config_.at("social").at("san_falloff"); const int64_t full=falloff.at("free"), zero=falloff.at("zero");
            const int64_t above=k.at("level").get<int64_t>()-s.at("level").get<int64_t>();
            const int64_t base=ks>=3000 && vs>=3000 ? 100 : ks>=3000 || vs>=3000 ? 10 : 1;
            const int64_t gain=kk && vk && above<zero ? std::max<int64_t>(1,base*kk*vk/10000*std::min(zero-full,zero-above)/(zero-full)) : 0;
            // Guide: "Plus aboneler, normalin 3 katı şan puanı kazanırlar" (Klan Parası follows the Şan) and lose 10 % Karizma, not 20 %.
            const int64_t earned=gain*(killer->second.plus ? 3 : 1);
            auto reward=k;reward["san"]=std::min<int64_t>(1000000000,ks+earned);
            reward["clan_currency"]=std::min<int64_t>(999000000,k.value("clan_currency",int64_t(0))+earned); // "En fazla 999 milyon klan paranız olabilir."
            reward.erase("karizma_reset"); reward["karizma"]=kk;
            proposed[killer->first]=std::move(reward);
            auto& v=proposed.at(victim); v["karizma"]=std::max(0,vk-(defender.plus ? 10 : 20));
            if(!v.at("karizma").get<int>()) v["karizma_reset"]=int64_t(std::time(nullptr))+7200; else v.erase("karizma_reset"); // "2 saat sonra yenilenerek"
            if(arenas_.contains(s.at("zone").get<std::string>())) scorer=killer->first; // Er Meydanı: scores once the death commits
        }
    }
    try { commit(proposed); }
    catch(const Rejected&) { reject(victim,"persistence","Dövüş sonucu kaydedilemedi.");return false; }
    catch(const std::exception&) { reject(victim,"persistence","Dövüş sonucu kaydedilemedi.");return false; }
    Json event{{"type","combat"},{"entity",source},{"source",source},{"target",victim},{"damage",damage},{"hp",hp}}; if(landed.critical)event["critical"]=true;
    if(critical)*critical=landed.critical;
    project_event(source,std::move(event));
    if(dead) { auto& v=actors_.at(victim);v.resurrect_ready=tick_+config_.at("death_resurrect_delay").get<uint32_t>();v.engaged=0;
        project_event(victim,{{"type","death"},{"entity",victim},{"source",source},{"target",victim}});cancel_trade(victim); }
    if(scorer) { // killing an enemy-clan player in Er Meydanı scores for the killer's clan (guide)
        const auto zone=actors_.at(victim).character.at("state").at("zone").get<std::string>(); const auto& k=actors_.at(scorer).character;
        arenas_.at(zone).score[k.at("state").value("clan",0)]+=config_.at("activities").at("arena").at("kill_points").get<int64_t>();
        arena_say(zone,k.at("name").get<std::string>()+" bir düşmanı alt etti.");
    }
    return true;
}
bool World::social(uint32_t id, const std::string& action, const Json& request) {
    auto& a=actor(id); const auto& current=a.character.at("state");
    if (action=="contact_add" || action=="contact_remove") {
        // Bağlantılar: "Arkadaşlara Ekle"/"Yasaklılara Ekle" on a player, "Listeden sil" on an entry. A character is on one list at most.
        const auto list=text(request.at("list"),8); require(list=="friends" || list=="ignored","request","Böyle bir liste yok.");
        auto s=current; if (!s.contains("contacts")) s["contacts"]={{"friends",Json::array()},{"ignored",Json::array()}};
        int64_t who=0; Json entry;
        if (action=="contact_add") { const auto t=target_id(request.at("target")); require(t!=id,"target","Kendinizi listeye ekleyemezsiniz."); const auto& other=actor(t); who=other.character_id; entry={who,other.character.at("name")}; }
        else { require(request.at("character_id").is_number_integer(),"request","Karakter belirtilmeli."); who=request.at("character_id").get<int64_t>(); }
        bool listed=false;
        for (const char* name : {"friends","ignored"}) { auto& entries=s["contacts"][name]; for (auto it=entries.begin();it!=entries.end();) if ((*it)[0]==who) { listed|=list==name; it=entries.erase(it); } else ++it; }
        if (action=="contact_add") { auto& entries=s["contacts"][list]; require(entries.size()<config_.at("social").at("contacts_max").get<size_t>(),"contacts","Listeniz dolu."); entries.push_back(std::move(entry)); }
        else require(listed,"contacts","Bu kişi listenizde değil.");
        commit({{id,s}}); emit(id,{{"type","ack"},{"action",action}}); return true;
    }
    if (action=="clan_join" || action=="clan_leave") {
        // Original NPC menu 0041a230: "Klana Katıl" on a clan-1/2 recruiter (flag 0x20000, classes.dat claninviter) while clanless,
        // "Klandan Ayrıl" on the own clan's recruiter; leaving costs Şan and Klan Parası (confirm text 0x58e948).
        auto& n=npc(id,target_id(request.at("target"))); require(n.definition.value("recruiter",false),"target","Bu kişi klana üye almıyor.");
        const int clan=n.definition.at("clan"); auto s=current;
        if (action=="clan_join") { require(!s.contains("clan"),"clan","Zaten bir klandasınız.");
            require(s.at("level").get<int>()>=5,"level","Klana katılmak için en az 5. seviye olmalısınız."); s["clan"]=clan; } // guide "Klan savaşları"
        else { require(s.value("clan",0)==clan,"clan","Bu klanda değilsiniz."); for (const char* key : {"clan","san","clan_currency"}) s.erase(key); }
        commit({{id,s}}); emit(id,{{"type","ack"},{"action",action}}); return true;
    }
    if (action=="kabadayi") { // "Kabadayı Moduna Geç" (self menu 0041a404) after "Kabadayı moduna geçmek istediğinize emin misiniz?"
        require(!current.value("kabadayi",false),"kabadayi","Zaten Kabadayı modundasınız.");
        auto s=current; s["kabadayi"]=true; commit({{id,s}}); emit(id,{{"type","ack"},{"action",action}}); return true;
    }
    return false;
}

void World::action(uint32_t id, const Json& request) {
    if (!actors_.contains(id)) return;
    try {
        handle(id,request);
        if (logs_.size() == 64) logs_.erase(logs_.begin());
        logs_.push_back({{"tick",tick_},{"player",id},{"action",request.value("action","")},{"result","accepted"}});
    } catch (const Rejected& e) { reject(id,e.code,e.message); }
      catch (const TradePersistenceFailure& e) {
        if(auto* current=trade(id))trade_event(*current);
        if(logs_.size()==64)logs_.erase(logs_.begin());
        logs_.push_back({{"tick",tick_},{"player",id},{"result","rejected"},{"code","trade_persistence"}});
        emit(id,{{"type","error"},{"code","trade_persistence"},{"peer",e.peer},{"revision",e.revision},{"message","Alışveriş kaydedilemedi."}});
      }
      catch (const Json::exception&) { reject(id,"request","Geçersiz istek."); }
      catch (const std::exception&) { reject(id,"persistence","İşlem kaydedilemedi."); }
}
void World::moderation(uint32_t id, bool muted) { if (actors_.contains(id)) actors_.at(id).externally_muted = muted; }
void World::entitlement(uint32_t id, bool plus) { if (actors_.contains(id)) actors_.at(id).plus = plus; }
void World::handle(uint32_t id, const Json& request) {
    require(request.is_object() && request.dump().size() <= 8192,"request","İstek çok büyük.");
    auto& a = actor(id); require(!a.kicked,"permission","Yönetici tarafından oyundan atıldınız.");
    const auto action = text(request.at("action"),32); auto& current = a.character.at("state");
    require(current.at("hp").get<int>() > 0 || action == "resurrect" || action == "chat" || action == "trade_cancel" || action == "group_leave" || action == "quickbar", "dead", "Ölüyken bunu yapamazsınız!"); // 0x58ece4
    const auto target = [&]() { return target_id(request.at("target")); };
    const auto ack = [&]() { emit(id,{{"type","ack"},{"action",action}}); };
    if (action == "craft" || action == "gather" || action == "recipe_forget" || action.starts_with("bank_") ||
        (action == "use" && config_.at("recipes").contains(owned(current,text(request.at("uid"),32)).at("item").get<std::string>()))) { economy(id,action,request); return; }
    if (social(id,action,request)) return;
    if (action.starts_with("arena_")) { arena(id,action,request); return; }
    if (action == "interact") {
        auto t = target();
        // Click-to-talk like click-to-attack: an NPC beyond interaction_range is walked to (engage()), then interacted with.
        if (const auto f = npcs_.find(t); f != npcs_.end() && f->second.hp > 0 && f->second.definition.at("kind") != "enemy" && !strikable(f->second.definition,current) && !close(id,t,config_.at("interaction_range"))) {
            require(close(id,t,config_.at("aoi")),"range","Hedef çok uzakta."); a.engaged = t; a.looting = false; a.dx = a.dy = 0; ack(); return;
        }
        auto& n = npc(id,t); require(n.definition.at("kind") != "enemy","target","Düşman bir hedefle etkileşime geçemezsiniz.");
        if (n.definition.at("kind") == "anvil") { handle(id,{{"action","repair"},{"target",t}}); return; } // clicking an örs repairs (helps.xml Para)
        if (n.definition.at("kind") == "reset") { // GBM machines: right-click offers the level price; the Dikkat ✓ sends point_reset
            if (n.definition.at("reset") == "crafts") require(a.plus,"plus",gbm0b_refusal);
            emit(id,{{"type","reset_offer"},{"target",t},{"reset",n.definition.at("reset")},{"price",point_reset_price(config_,n.definition,current,a.plus)}}); return;
        }
        if (n.definition.at("kind") == "door") {
            // classes.dat <loc> gmonly / requiredquestid. NONCANONICAL: the quest (config "source_quest_id") must be taken or done;
            // ids no configured quest carries stay open (their chains were server data).
            const auto& door = n.definition.at("door");
            require(!door.value("gmonly",false) || a.admin,"permission","Bu geçit yalnızca yöneticilere açıktır.");
            if (door.contains("required_quest")) {
                const int q = door.at("required_quest"); bool open = true;
                for (const auto& [key,quest] : config_.at("quests").items())
                    if (quest.value("source_quest_id",-1) == q && !(open = current.at("quests").contains(key) || quest_done(current,q))) break;
                require(open,"quest","Bu geçidi kullanmak için gerekli görevi almış olmalısınız.");
            }
            // classes.dat requiredentitydeathid (Yaşam Merkezi doors 518/523, İstihbarat crystal 384). NONCANONICAL: the door stays
            // shut while an NPC of that classid lives in the door's zone copy (the original kill rule was server data).
            if (door.contains("required_death"))
                for (const auto& [other_id,other] : npcs_)
                    if (other.hp > 0 && other.definition.at("source_class") == door.at("required_death") && other.definition.at("zone") == n.definition.at("zone"))
                        throw Rejected{"door", other.definition.at("name").get<std::string>() + " yenilmeden bu geçit açılmaz."};
            const std::string to = door.at("to");
            // NONCANONICAL group-zone lockout (stamped by a Şef kill in a copy): the zone stays shut until it expires.
            if (const auto until = current.value("lockouts",Json::object()).value(to,int64_t(0)); until > std::time(nullptr) && config_.at("zones").at(to).contains("instances"))
                throw Rejected{"lockout", "Bu grup bölgesinin şefini yakın zamanda yendiniz; " + std::to_string((until - std::time(nullptr) + 59) / 60) + " dakika sonra tekrar girebilirsiniz."};
            transfer(id,current.at("zone") == to ? to : instance(id,to),door.at("arrive")[0],door.at("arrive")[1]); ack(); return;
        }
        // banker: the original Bankacı dialogue offers "Kasamı göster" (0x58ebfc), which opens the Banka window.
        emit(id,{{"type","dialogue"},{"target",t},{"text",n.definition.at("dialogue")},{"quests",quests_at(n,current)},{"shop",n.definition.at("shop")},{"bank",n.definition.value("banker",false)}}); return;
    }
    if (action == "quest_accept" || action == "quest_complete") {
        auto t = target(); auto& n = npc(id,t); auto quest = text(request.at("quest"),32); const auto offered = quests_at(n,current);
        require(config_.at("quests").contains(quest) && std::find(offered.begin(),offered.end(),Json(quest)) != offered.end(),"quest","Bu görev burada verilmiyor.");
        const auto& q = config_.at("quests").at(quest); auto s = current; Json events = Json::array();
        // Instanced zone copies clone their NPCs at id + n*65536 (same source_uid): the copy serves the configured NPC's quests.
        const auto is = [&](const Json& configured) { const uint32_t c = configured; return t == c || (t > c && (t-c) % 65536 == 0 && npcs_.contains(c) && n.definition.at("source_uid") == npcs_.at(c).definition.at("source_uid")); };
        if (action == "quest_accept") {
            require(q.value("auto",false) ? n.definition.value("auto_quests",false) : is(q.at("giver")),"quest","Bu görevi bu kişi vermiyor.");
            require(!s.at("quests").contains(quest) && !quest_finished(s,quest),"quest","Bu görevi zaten aldınız."); require(s.at("level").get<int>() >= q.at("level").get<int>(),"level","Bu görev için seviyeniz yetersiz.");
            if (q.contains("requires")) require(quest_finished(s,q.at("requires").get<std::string>()),"quest","Önce bir önceki görevi tamamlamalısınız.");
            require(s.at("quests").size() < quest_active_max,"quest","Görev listeniz dolu.");
            Json p{{"progress",q.at("objective").at("kind") == "talk" ? 1 : 0}};
            if (q.contains("time_limit")) p["deadline"] = tick_ + 20 * q.at("time_limit").get<uint32_t>();
            s["quests"][quest] = std::move(p); events.push_back(quest_event(quest,"accepted"));
        } else {
            require(is(q.contains("turn_in") ? q.at("turn_in") : q.at("giver")),"quest","Bu görev bu kişiye teslim edilmez.");
            require(s.at("quests").contains(quest) && s.at("quests").at(quest).at("progress").get<int>() >= quest_count(q),"quest","Görev henüz tamamlanmadı.");
            quest_finish(s,quest,events);
        }
        commit({{id,s}}); for (auto& e : events) emit(id,std::move(e)); ack(); return;
    }
    if (action == "quest_abandon") { // Görevler 'Seçili görevi sil': drop an unfinished quest and its progress.
        auto quest = text(request.at("quest"),32); auto s = current;
        require(s.at("quests").contains(quest),"quest","Silinecek tamamlanmamış bir görev yok.");
        s["quests"].erase(quest); commit({{id,s}}); ack(); return;
    }
    if (action == "stat_points") {
        // Karakter Bilgisi Dirençler/Üretim/Toplama ✓ (00402a80): −/+ only move window-pending points (+0x3b4..+0x4cc); the ✓
        // sends each group's new totals ('targ' resistances, 'cnch' crafts) and clears them. Invested points are permanent:
        // only the GBM 0 / GBM 0-B machines (point_reset) give them back.
        const auto& points = request.at("points"); require(points.is_object() && !points.empty() && points.size() <= 11,"request","Geçersiz özellik puanları.");
        auto s = current;
        for (const auto& [stat,count] : points.items()) {
            const char* group = std::find(resistance_stats.begin(),resistance_stats.end(),std::string_view(stat)) != resistance_stats.end() ? "resistances"
                : std::find(craft_stats.begin(),craft_stats.end(),std::string_view(stat)) != craft_stats.end() ? "crafts" : nullptr;
            require(group,"stat","Bilinmeyen özellik.");
            s[group][stat] = s.at(group).value(stat,0) + integer(count,1,1000);
        }
        for (const char* group : {"resistances","crafts"}) require(unspent_points(config_,s,group) >= 0,"points","Daha fazla puan yatıramazsınız!");
        commit({{id,s}}); ack(); return;
    }
    if (action == "attack") {
        // Original click-to-attack: select the hostile target, then advance() approaches and swings until it ends.
        require(!buff_flag(current.at("buffs"),"immune"),"state","Sarsılmaz etkisindeyken bunu yapamazsınız.");
        const auto t = target();
        if (actors_.contains(t)) require(hostile(id,t),"target","Bu oyuncu düşmanınız değil."); // Kabadayı or enemy clan
        else { auto found = npcs_.find(t); require(found != npcs_.end() && strikable(found->second.definition,current),"target","Bu hedefe saldıramazsınız."); require(found->second.hp > 0,"dead","Hedef ölü! Etkileşime geçemezsiniz!"); }
        require(close(id,t,config_.at("aoi")),"range","Hedef çok uzakta.");
        a.engaged = t; a.looting = false; a.dx = a.dy = 0; ack(); return;
    }
    if (action == "quickbar") { // Hızlı Erişim Çubuğu drag-and-drop layout (original 'tdbp' 0044b330), stored per character
        auto s = current; s["quickbar"] = quickbar_slots(config_,request.at("slots")); commit({{id,s}}); ack(); return;
    }
    if (action == "skill_variant") { // Yetenekler slot 8 of a Savaşçı: Kanatma or Boz Ayı (exe 0044ccf0 / hcpe flag byte, 004a6720)
        // NONCANONICAL: the official server set the flag; its grant rule is unrecovered, so the player toggles it.
        auto skill = text(request.at("skill"),32); require(config_.at("skills").contains(skill) && config_.at("skills").at(skill).contains("replaces"),"skill","Bilinmeyen yetenek.");
        const auto& definition = config_.at("skills").at(skill);
        require(definition.at("source")[0].get<int>()+1 == a.character.at("class_id").get<int>(),"class","Bu yetenek sınıfınıza ait değil.");
        require(current.at("level").get<int>() >= definition.at("level").get<int>(),"level","Bu yetenek için seviyeniz yetersiz!");
        auto s = current; if (current.value(skill,false)) s.erase(skill); else s[skill] = true; commit({{id,s}}); ack(); return;
    }
    // A class skill this character may rank or cast; returns its rank key. A variant (Boz Ayı) occupies its replaced skill's
    // slot and rank while the character flag named after it is set.
    const auto skill_slot = [&](const std::string& skill) {
        require(config_.at("skills").contains(skill),"skill","Bilinmeyen yetenek."); const auto& definition = config_.at("skills").at(skill);
        require(definition.at("source")[0].get<int>()+1 == a.character.at("class_id").get<int>(),"class","Bu yetenek sınıfınıza ait değil.");
        require(current.at("level").get<int>() >= definition.at("level").get<int>(),"level","Bu yetenek için seviyeniz yetersiz!");
        bool slotted = definition.contains("replaces") ? current.value(skill,false) : true;
        for (const auto& [other,o] : config_.at("skills").items()) if (o.value("replaces","") == skill && current.value(other,false)) slotted = false;
        require(slotted,"variant","Bu yeteneğin diğer biçimi kullanımda.");
        return definition.value("replaces",skill);
    };
    if (action == "skill_learn") {
        // Yetenekler ✓ (0044d4e0 sends every new rank at once in 'clxs'; the ABILITY answer 004a4030 clears the window's pending
        // points via 0044ce90): the points pending from +/− (0044d2a0/0044d480) per skill, committed together within rank 15
        // ("Bir yetenekteki toplam puan 15'i geçemez!") and the unspent budget.
        const auto& points = request.at("skills"); require(points.is_object() && !points.empty() && points.size() <= 16,"request","Geçersiz yetenek puanları.");
        auto s = current; int total = 0;
        for (const auto& [skill,count] : points.items()) {
            const auto key = skill_slot(text(Json(skill),32)); const int add = integer(count,1,15); total += add;
            const int rank = s.at("skills").value(key,0)+add; require(rank <= config_.at("skill_rank_max").get<int>(),"rank","Bir yetenekteki toplam puan 15'i geçemez!");
            s["skills"][key] = rank;
        }
        require(total <= unspent_skill_points(config_,current),"points","Daha fazla puan yatıramazsınız!");
        commit({{id,s}}); ack(); return;
    }
    if (action == "skill") {
        auto skill = text(request.at("skill"),32); const auto rank_key = skill_slot(skill); const Json definition = glyphed(config_,current,skill); // carried Tılsım
        const int rank = current.at("skills").value(rank_key,0);
        require(rank > 0,"rank","Bu yeteneğe puan vermediniz.");
        const auto kind = definition.at("target").get<std::string>(); require(kind != "passive","passive","Pasif yetenekler her zaman etkindir.");
        const auto& buffs = current.at("buffs");
        require(!buff_flag(buffs,"immune"),"state","Sarsılmaz etkisindeyken bunu yapamazsınız.");
        require(!buff_flag(buffs,"silence"),"state","Zihniniz tutulmuşken yetenek kullanamazsınız."); // Zihin tutulması (Durdurma, Zihin Saldırısı)
        if (definition.contains("requires")) require(std::any_of(buffs.begin(),buffs.end(),[&](const Json& b){return b.at("id")==definition.at("requires");}),"requires","Bu yetenek için gereken dövüş duruşu etkin değil.");
        const bool healing = definition.contains("heal") || definition.contains("hot") || definition.contains("absorb") || definition.value("revive",false);
        require(!healing || !buff_flag(buffs,"wrath") || definition.contains("wrath_heal"),"wrath","Gazap etkinken iyileştirme yapılamaz."); // Gazap Tılsım: at V % (glyphed)
        if (kind == "summon" && definition.at("summon").value("command",false)) for (auto& [sid,n] : npcs_) if (n.master == id && n.hp > 0 && n.definition.at("skill") == skill) {
            // Boz Ayı recast while it lives (detail text): no mana; sent at the selected hostile NPC or player, otherwise beside its warrior.
            require(tick_ >= a.skill_ready,"cooldown","Yetenek henüz hazır değil.");
            const auto t = request.contains("target") ? target() : 0; const auto e = npcs_.find(t);
            n.command = (actors_.contains(t) ? hostile(id,t) : e != npcs_.end() && e->second.hp > 0 && e->second.definition.at("zone") == n.definition.at("zone") && strikable(e->second.definition,current)) ? t : 0;
            if (!n.command) { n.definition["x"] = current.at("x"); n.definition["y"] = current.at("y"); }
            a.skill_ready = tick_+config_.at("skill_global_cooldown").get<uint32_t>();
            project_event(id,{{"type","skill"},{"entity",id},{"source",id},{"target",n.command ? n.command : sid},{"skill",skill},{"kind",kind},{"skill_source",definition.at("source")}});
            ack(); return;
        }
        require(tick_ >= a.cooldowns[skill] && tick_ >= a.skill_ready,"cooldown","Yetenek henüz hazır değil.");
        require(current.at("mana").get<int>() >= definition.at("mana").get<int>(),"resource","Yeterli kudretiniz yok.");
        // NONCANONICAL rank scaling; learned passive Bilgi skills boost spells of their school.
        // Worn Efsun (NONCANONICAL weights, see gear()): Büyü Hasarı (31 + school 53-58) adds to spell damage, İyileştirme (62) to heals.
        const auto worn = gear(current);
        const auto amount = [&](const char* key) {
            int value = definition.at(key).get<int>()*(100+config_.at("skill_rank_percent").get<int>()*(rank-1))/100*(100+arena_power(current))/100; // + Er Meydanı areas held
            if (definition.contains("school")) for (const auto& [other,points] : current.at("skills").items())
                if (Json(config_.at("skills").at(other).value("boosts","")) == definition.at("school")) value = value*(100+config_.at("passive_rank_percent").get<int>()*points.get<int>())/100;
            const std::string_view k = key;
            // Asit Saldırısı 1 Tılsım (NONCANONICAL): harmful heal_cut effects on the caster cut the heals it casts.
            if (k == "heal" || k == "hot") value = (value+worn.heal)*(100-std::min(100,buff_sum(buffs,"heal_cut")))/100;
            else if ((k == "damage" || k == "dot") && definition.contains("school")) {
                static constexpr std::pair<std::string_view,int> schools[]{{"fire",53},{"ice",54},{"electric",55},{"poison",56},{"acid",57},{"physical",58}};
                value += worn.magic;
                for (const auto& [school,attribute] : schools) if (definition.at("school") == school)
                    if (const auto f = worn.efsun.find(attribute); f != worn.efsun.end()) value += int(std::lround(f->second*item_rule(config_,"efsun_percent")/100.0));
            }
            return value;
        };
        // Bedelsiz Yetenek (Sert Vuruş detail text): a rolled free cast spends no mana.
        auto caster = current; caster["mana"] = current.at("mana").get<int>()-(proc(definition,"free",rank) ? 0 : definition.at("mana").get<int>());
        // Summons: "Her bir puanda yenilenme ve etki süresi 3 sn artar" (per_rank ticks on cooldown and duration).
        const uint32_t cooldown = definition.at("cooldown").get<uint32_t>() + (kind == "summon" ? definition.at("summon").at("per_rank").get<uint32_t>()*uint32_t(rank-1) : 0u);
        const auto charge = [&] { a.cooldowns[skill] = tick_+cooldown; a.skill_ready = tick_+config_.at("skill_global_cooldown").get<uint32_t>(); };
        if (kind == "summon") {
            commit({{id,caster}}); charge(); summon(id,skill,definition,rank,amount("damage"));
            project_event(id,{{"type","skill"},{"entity",id},{"source",id},{"target",id},{"skill",skill},{"kind",kind},{"skill_source",definition.at("source")}});
            ack(); return;
        }
        if (kind == "enemy" || kind == "enemies") {
            // Hostile NPCs, and players hostile under the original relation 00478090 (Kabadayı / enemy clans).
            const auto foe = [&](uint32_t t) { const auto found = npcs_.find(t); return actors_.contains(t) ? hostile(id,t) : found != npcs_.end() && found->second.hp > 0 && strikable(found->second.definition,current); };
            std::vector<uint32_t> targets;
            if (kind == "enemy") {
                const auto t = target(); require(foe(t),"target","Bir düşman seçmelisiniz."); require(close(id,t,definition.at("range")),"range","Hedef çok uzakta."); targets.push_back(t);
            } else for (auto other : nearby(id,definition.at("radius"))) if (foe(other)) targets.push_back(other);
            const auto position = [&](uint32_t t) { const auto p = actors_.find(t); return p != actors_.end() ? std::pair{coordinate(p->second.character.at("state"),"x"),coordinate(p->second.character.at("state"),"y")} : std::pair{npcs_.at(t).x,npcs_.at(t).y}; };
            // Yıldırım 2 Tılsım (NONCANONICAL): every other foe within chain×100 wire units of the primary target takes the same hit.
            if (kind == "enemy" && definition.contains("chain")) {
                const auto [cx,cy] = position(targets[0]); const int reach = definition.at("chain").get<int>()*100;
                for (auto other : nearby(id,definition.at("range").get<int>()+reach)) if (other != targets[0] && foe(other))
                    if (const auto [ox,oy] = position(other); distance(cx,cy,ox,oy) <= int64_t(reach)*reach) targets.push_back(other);
            }
            const int damage = definition.contains("damage") ? (amount("damage")+buff_sum(buffs,"attack"))*(100+buff_sum(buffs,"power"))/100 : 0;
            const int dot = definition.contains("dot") ? amount("dot") : 0;
            // Detail-text chances: Zihin Saldırısı imprisons (stun + no skills) only on its roll; Durdurma may lock the mind.
            Json effect = definition;
            if (definition.contains("procs") && definition.at("procs").contains("stun")) { if (proc(definition,"stun",rank)) effect["silence"] = true; else effect.erase("stun"); }
            if (proc(definition,"silence",rank)) effect["silence"] = true;
            if (proc(definition,"drain",rank)) effect["drain"] = true; // Kanatma 10/15 puan: 25/50 % Kudret drain for its duration
            // Skill crits: spells (a `school`) roll Büyü Kritik (00402f60: 1 % + Büyü Kritik Şansı 37, ×2 + Büyü Kritik Çarpanı 38)
            // plus detail-text `crit` [from rank, % per point] ("5 puandan itibaren kritik şansı her bir puan için %1 artar") and Buz
            // Oku's `crit_vulnerable` ("Direnci sıfırın altında olanlara %25 daha fazla kritik"; below-zero resistance = Direnç Kırma's
            // vulnerable effect, as for Ateş Çemberi). NONCANONICAL: Savaşçı techniques never crit (no detail text names one).
            const auto efsun = [&](int key) { const auto f = worn.efsun.find(key); return f == worn.efsun.end() ? 0.0 : f->second; };
            const bool magic = definition.contains("school");
            double crit = magic ? 1+efsun(37) : 0, multiplier = 2+efsun(38);
            if (magic && definition.contains("crit")) crit += std::max(0,rank-definition.at("crit")[0].get<int>()+1)*definition.at("crit")[1].get<double>();
            // NONCANONICAL stance crit bases (skills.dat), ×(100+V)% with a stance_crit Tılsım: Konsantrasyon adds Büyü Kritik Şansı (37)
            // again ("kritik efsunları kadar fazladan"), Meditasyon 15 puan 5 points, Gazap 10 points only with its Tılsım.
            const auto stance_crit = [&](const char* stance, double base, bool glyph_only) {
                if (!active(buffs,stance)) return 0.0;
                const int v = glyph(config_,current,stance,"stance_crit"); return v < 0 && glyph_only ? 0.0 : base*(100+std::max(0,v))/100; };
            const auto meditation = [&](const char* stance) { return stance_crit(stance,current.at("skills").value(stance,0) >= 15 ? 5 : 0,false); };
            if (magic) crit += stance_crit("konsantrasyon",efsun(37),false)+meditation("meditasyon_buyucu")+meditation("meditasyon_sifaci")+stance_crit("gazap",10,true);
            // Ofansif Dövüşme 1 Tılsım (NONCANONICAL): Ağır Vuruş rolls the weapon's Kritik and a crit refunds V % of its Kudret.
            if (!magic && definition.contains("crit_refund")) { const auto f = fighter(current,a.character.at("class_id")); crit = f.crit; multiplier = f.crit_multiplier; }
            const auto effects = [&](uint32_t t) -> const Json& { const auto n = npcs_.find(t); return n != npcs_.end() ? n->second.debuffs : actors_.at(t).character.at("state").at("buffs"); };
            // Yıldırım: "her bir elektrik etkisi başına fazladan %%5 kritik şansı" (electric marks, see the target loop).
            const bool electric = magic && definition.at("school") == "electric";
            const auto chance = [&](uint32_t t) { const auto& d = effects(t);
                return crit + (buff_sum(d,"vulnerable") > 0 ? definition.value("crit_vulnerable",0) : 0) + (electric ? 5*buff_sum(d,"electric") : 0); };
            // Detail-text riders by rank (last row reached): `on_hit` (Buz Oku 5/10/15 puan: 3 s 10/15/20 %% slow) and `on_crit`
            // (Buz Oku "Kritik vurursa rakibi 1 sn durdurur", Meteorit 15 puan "rakibin zırhını 4,5 sn'liğine %%20 azaltır").
            const auto rider = [&](const char* key) -> const Json* { const Json* row = nullptr; if (definition.contains(key)) for (const auto& r : definition.at(key)) if (r[0].get<int>() <= rank) row = &r[1]; return row; };
            const auto* on_hit = rider("on_hit"); const auto* on_crit = rider("on_crit");
            // Ateş Çemberi: 50% double/triple damage to targets whose resistances Direnç Kırma Alanı drove below zero.
            const auto* multiply = proc(definition,"multiply",rank);
            // Sert Vuruş 2 / Yıldırım 1 Tılsım (NONCANONICAL): ×(100+V)% against a mind-locked target / one carrying 3 electric marks.
            const auto scaled = [&](uint32_t t) { const auto& d = effects(t);
                int value = multiply && npcs_.contains(t) && buff_sum(d,"vulnerable") > 0 ? damage*(*multiply)[2].get<int>() : damage;
                if (definition.contains("silenced_damage") && buff_flag(d,"silence")) value = value*(100+definition.at("silenced_damage").get<int>())/100;
                if (definition.contains("electric_damage") && buff_sum(d,"electric") >= 3) value = value*(100+definition.at("electric_damage").get<int>())/100;
                return value; };
            // Passive Bilgi chances on spells of their school (Asit Bilgisi: extra acid damage over time).
            std::vector<std::pair<std::string,uint32_t>> passive_dots;
            if (definition.contains("school")) for (const auto& [other,points] : current.at("skills").items())
                if (Json(config_.at("skills").at(other).value("boosts","")) == definition.at("school")) if (const auto* row = proc(config_.at("skills").at(other),"dot",points.get<int>())) passive_dots.emplace_back(other,(*row)[2].get<uint32_t>());
            // kind + skills.dat source pair let every observer animate and voice casts of skills outside its own class catalog.
            const Json cast{{"type","skill"},{"skill",skill},{"kind",kind},{"skill_source",definition.at("source")}};
            const bool npc_first = kind == "enemy" && !actors_.contains(targets[0]);
            const int spent = current.at("mana").get<int>()-caster.at("mana").get<int>();
            // Kutup Rüzgarı "Kendisi dahil ... buz içinde hapis kalır" (NONCANONICAL): the caster is stunned for self_stun % of the duration.
            if (definition.value("self_stun",0) > 0) {
                auto& own = caster.at("buffs"); for (auto it=own.begin();it!=own.end();) if (it->at("id") == skill) it=own.erase(it); else ++it;
                if (own.size() < 8) own.push_back({{"id",skill},{"skill_source",definition.at("source")},{"expires",tick_+definition.at("duration").get<uint32_t>()*definition.at("self_stun").get<uint32_t>()/100},{"armor",0},{"stun",true}});
            }
            bool first_critical = false, refund = false;
            if (npc_first && damage) first_critical = combat(id,targets[0],scaled(targets[0]),std::move(caster),cast,chance(targets[0]),multiplier);
            else { commit({{id,caster}}); auto event = cast; event.update({{"entity",id},{"source",id},{"target",kind == "enemy" ? targets[0] : id}}); project_event(id,std::move(event)); }
            charge();
            for (auto t : targets) {
                bool critical = npc_first && t == targets[0] && first_critical;
                if (actors_.contains(t)) { if (damage) { auto blow = spell(scaled(t),definition); blow.crit = chance(t); blow.multiplier = multiplier; strike(id,t,blow,0,&critical); } }
                else if ((kind == "enemies" || t != targets[0]) && damage && npcs_.at(t).hp > 0) critical = combat(id,t,scaled(t),actor(id).character.at("state"),{{"type","hit"},{"skill",skill}},chance(t),multiplier);
                if (t == targets[0]) refund = critical;
                hostile_skill(id,t,effect,skill,dot);
                if (on_hit) hostile_skill(id,t,*on_hit,skill,0);
                if (on_crit && critical) hostile_skill(id,t,*on_crit,skill,0);
                for (const auto& [passive,ticks] : passive_dots) hostile_skill(id,t,{{"duration",ticks},{"dot",true}},passive,std::max(1,dot ? dot : damage/4));
                // Yıldırım's electric mark (NONCANONICAL reading of "Süre: 0 sn veya 5(+) sn"): 5 s, refreshed by each hit, up to 3 marks.
                if (electric && damage) hostile_skill(id,t,{{"duration",100},{"electric",std::min(3,buff_sum(effects(t),"electric")+1)}},skill,0);
            }
            if (refund && spent > 0 && definition.contains("crit_refund")) {
                auto s = actor(id).character.at("state"); s["mana"] = std::min(s.at("max_mana").get<int>(),s.at("mana").get<int>()+spent*definition.at("crit_refund").get<int>()/100); commit({{id,s}});
            }
            if (kind == "enemy") { a.engaged = targets[0]; a.looting = false; } // the skill's target becomes the auto-attack target
            ack(); return;
        }
        std::vector<uint32_t> targets;
        if (kind == "self") targets.push_back(id);
        else if (kind == "ally") {
            const auto t = target(); actor(t); require(close(id,t,definition.at("range")),"range","Hedef çok uzakta.");
            require(t == id || (group(id) && group(id) == group(t)),"target","Bu yetenek yalnızca size veya grubunuzdan birine kullanılabilir."); targets.push_back(t);
        } else for (auto other : nearby(id,definition.at("radius"))) if (actors_.contains(other) && (other == id || (group(id) && group(id) == group(other)))) targets.push_back(other);
        std::map<uint32_t,Json> proposed{{id,std::move(caster)}};
        const bool revive = definition.value("revive",false);
        for (auto t : targets) {
            if (!proposed.contains(t)) proposed[t] = actor(t).character.at("state");
            auto& result = proposed.at(t);
            if (revive) { require(result.at("hp").get<int>() == 0,"state","Hedef ölü değil."); result["hp"] = std::max(1,result.at("max_hp").get<int>()/2); continue; }
            if (result.at("hp").get<int>() == 0) { require(kind == "allies","dead","Hedef ölü! Etkileşime geçemezsiniz!"); continue; }
            if (definition.contains("heal")) { // İyileştirme Çemberi 1 Tılsım (NONCANONICAL): ×(100+V)% on a target inside the circle
                const int heal = amount("heal")*(definition.contains("circle_heal") && active(result.at("buffs"),"iyilestirme_cemberi") ? 100+definition.at("circle_heal").get<int>() : 100)/100;
                result["hp"] = std::min(result.at("max_hp").get<int>(),result.at("hp").get<int>()+heal);
            }
            auto& list = result.at("buffs");
            // skills.dat: Zihin Toplama "Savaşsal", Büyücü Büyü Bozma "Büyüsel", Şifacı Büyü Bozma "Ruhsal tüm kötü etkileri kaldırır";
            // at 5/10/15 puan a 10/20/25 %% roll (`cleanse_all`) also removes the other two kinds. A harmful effect's kind is its
            // skill's class (`cleanse` = classindex: 0 Savaşçı Savaşsal, 1 Büyücü Büyüsel, 2 Şifacı Ruhsal) [INFERENCE from the class
            // names: neither skills.dat nor the client holds a category table; the official server decided].
            if (definition.contains("cleanse")) {
                const bool all = proc(definition,"cleanse_all",rank); const int category = definition.at("cleanse");
                const auto removes = [&](const Json& b) { const auto s = config_.at("skills").find(b.at("id").get<std::string>());
                    return b.value("harmful",false) && (all || (s != config_.at("skills").end() && s->at("source")[0] == category)); };
                for (auto it=list.begin();it!=list.end();) if (removes(*it)) it=list.erase(it); else ++it;
            }
            const bool stance = definition.contains("stance"), active = std::any_of(list.begin(),list.end(),[&](const Json& b){return b.at("id")==skill;});
            for (auto it=list.begin();it!=list.end();) if (it->at("id") == skill || (stance && it->contains("stance") && it->at("stance") == definition.at("stance"))) it=list.erase(it); else ++it;
            if (stance && active) continue; // recasting an open stance closes it
            Json buff{{"id",skill},{"skill_source",definition.at("source")},{"expires",definition.contains("duration") ? tick_+definition.at("duration").get<uint32_t>() : UINT32_MAX},{"armor",definition.contains("armor") ? amount("armor") : 0}};
            for (const char* key : {"attack","regen","hot"}) if (definition.contains(key)) buff[key] = amount(key);
            for (const char* key : {"speed","power","absorb"}) if (definition.contains(key)) buff[key] = definition.at(key);
            for (const char* key : {"immune","wrath"}) if (definition.value(key,false)) buff[key] = true;
            if (stance) buff["stance"] = definition.at("stance");
            if (buff.size() > 4 || buff.at("armor") != 0) { require(list.size() < 8,"buff","Hedefte daha fazla etki olamaz."); list.push_back(std::move(buff)); } // beyond id/skill_source/expires/armor
        }
        commit(proposed); charge();
        for (auto t : targets) for (auto& [npcid,n] : npcs_) {
            (void)npcid; if (n.hp == 0) continue;
            if (definition.contains("heal") && n.threat.contains(t)) n.threat[id] += amount("heal");
            if (definition.value("redirect",false) && t != id) if (auto it = n.threat.find(t); it != n.threat.end()) { n.threat[id] += it->second*(100+definition.value("redirect_boost",0))/100; n.threat.erase(t); } // Hedef Saptırma Tılsım
        }
        project_event(id,{{"type","skill"},{"entity",id},{"source",id},{"target",kind == "ally" ? targets[0] : id},{"skill",skill},{"kind",kind},{"skill_source",definition.at("source")}});
        if (revive) project_event(targets[0],{{"type","resurrect"},{"entity",targets[0]},{"source",id},{"target",targets[0]}});
        ack(); return;
    }
    if (action == "resurrect") {
        require(current.at("hp").get<int>() == 0,"state","Ölü değilsiniz."); auto s = current; Json spawn = config_.at("zones").at(s.at("zone").get<std::string>()).at("spawn");
        if (tick_ < a.resurrect_ready) throw Rejected{"cooldown",std::to_string((a.resurrect_ready-tick_+19)/20)+" saniye geçmeden sadece şifacılar sizi canlandırabilir."}; // 20 Hz ticks
        // Guide "Klan sancakları": clan members resurrect at their clan's standing banner; NONCANONICAL: the nearest, else the zone spawn.
        int64_t nearest = INT64_MAX;
        for (const auto& [banner_id,b] : npcs_) if (b.hp > 0 && b.definition.value("banner",false) && b.definition.at("zone") == s.at("zone") && b.definition.at("clan") == s.value("clan",0))
            if (const auto d = distance(b.x,b.y,s.at("x"),s.at("y")); d < nearest) { nearest = d; spawn = {b.x,b.y}; }
        require(passable(s.at("zone"),spawn[0],spawn[1]),"collision","Canlanma noktası kullanılamıyor.");
        s["x"] = spawn[0]; s["y"] = spawn[1]; s["hp"] = s.at("max_hp"); s["mana"] = s.at("max_mana");
        auto zone = current.at("zone").get<std::string>(); auto x = coordinate(current,"x"), y = coordinate(current,"y"); commit({{id,s}}); index(id,zone,x,y,false); index(id,zone,s.at("x"),s.at("y"),true); a.moving = false;a.route.clear(); a.dx = a.dy = 0; a.engaged = 0;
        project_event(id,{{"type","resurrect"},{"entity",id},{"source",id},{"target",id}});ack(); return;
    }
    // Original Ganimet box: a click sends 'btlr' (box id), the server answers 'sdbt' with up to 9 slots and the client opens
    // the 3x3 window (00436140); taking a slot is answered by 'fatl' (item to bag, slot cleared, 0043f560). The box money
    // (classes.dat dropmoney) is granted when the box opens: NONCANONICAL timing, the original server rule is unknown.
    if (action == "loot" || action == "loot_take") {
        auto t = target(); auto found = npcs_.find(t); require(found != npcs_.end(),"target","Ganimet bulunamadı."); auto& n = found->second;
        require(n.hp == 0 && !n.looted && n.owner == id && n.drop.is_array(),"ownership","Bu ganimet size ait değil ya da alınmış.");
        if (!close(id,t,config_.at("interaction_range"))) { require(action == "loot" && close(id,t,config_.at("aoi")),"range","Ganimet çok uzakta."); a.engaged = t; a.looting = true; a.dx = a.dy = 0; ack(); return; }
        auto s = current; Json event{{"type","loot"},{"target",t}};
        if (action == "loot") {
            for (auto& slot : n.drop) if (slot.contains("currency")) { currency(s,slot.at("currency")); slot = nullptr; }
            event["type"] = "loot_open"; event["items"] = n.drop;
        } else {
            const auto index = request.at("slot"); require(index.is_number_unsigned() && index.get<size_t>() < n.drop.size() && n.drop[index.get<size_t>()].is_object(),"slot","Ganimet hücresi boş.");
            const auto& slot = n.drop[index.get<size_t>()]; give(s,slot.at("item"),slot.at("quantity"),slot);
            event["slot"] = index; event["item"] = slot.at("item"); if (slot.contains("prefixes")) event["prefixes"] = slot.at("prefixes");
            n.drop[index.get<size_t>()] = nullptr;
        }
        commit({{id,s}}); n.looted = std::all_of(n.drop.begin(),n.drop.end(),[](const Json& slot){return slot.is_null();});
        emit(id,std::move(event)); ack(); return;
    }
    // Guide "GBM-0 kullanarak ...": yetenek ve direnç puanları; "GBM-0B ...": üretim ve toplama puanları, usable once a Çemberlitaş or
    // Derindeki Sır Plus Paket code was redeemed. NONCANONICAL: no redemption history exists here, so GBM 0-B needs current Plus Paket.
    // The price the player confirmed must still hold (level or Plus may change between offer and ✓).
    if (action == "point_reset") {
        require(!trade(id),"trade","Alışveriş halindeyken bunu yapamazsınız!"); const auto& n = npc(id,target(),"reset"); auto s = current;
        const bool crafts = n.definition.at("reset") == "crafts"; if (crafts) require(a.plus,"plus",gbm0b_refusal);
        const int64_t price = point_reset_price(config_,n.definition,current,a.plus);
        require(request.at("price").is_number_integer() && request.at("price").get<int64_t>() == price,"price","Fiyat değişti; makineyi yeniden kullanın.");
        require(crafts ? !s.at("crafts").empty() : !s.at("skills").empty() || !s.at("resistances").empty(),"reset","Sıfırlanacak puanınız yok.");
        if (crafts) s["crafts"] = Json::object(); else { s["skills"] = Json::object(); s["resistances"] = Json::object(); }
        currency(s,-price); commit({{id,s}}); emit(id,{{"type","point_reset"},{"reset",n.definition.at("reset")},{"price",price}}); ack(); return;
    }
    // helps.xml Para: right-clicking an örs (classes.dat <loc> id_ors) repairs the equipment (ui\repair.raw). The cost is
    // NONCANONICAL: price x missing/maximum Eskime / repair_denominator per item, at least 1 for each worn-down item.
    if (action == "repair") {
        require(!trade(id),"trade","Alışveriş halindeyken bunu yapamazsınız!"); npc(id,target(),"anvil"); auto s = current; int64_t cost = 0; int repaired = 0;
        for (auto& entry : s.at("inventory")) {
            const auto& item = config_.at("items").at(entry.at("item").get_ref<const std::string&>());
            if (!entry.contains("durability") || !item.contains("durability")) continue;
            const int64_t maximum = item.at("durability").get<int>(), missing = maximum - entry.at("durability").get<int>();
            if (missing > 0) { cost += std::max<int64_t>(1, item.at("price").get<int64_t>()*missing/maximum/std::max(1,item_rule(config_,"repair_denominator"))); ++repaired; }
            entry.erase("durability");
        }
        require(repaired > 0,"repair","Tamir edilecek bir şey yok.");
        currency(s,-cost); commit({{id,s}}); emit(id,{{"type","repair"},{"cost",cost},{"items",repaired}}); ack(); return;
    }
    if (action == "equip" || action == "unequip" || action == "use") {
        require(!trade(id),"trade","Alışveriş halindeyken bunu yapamazsınız!"); auto s = current;
        if (action == "unequip") { auto slot = text(request.at("slot"),32); require(s.at("equipment").contains(slot),"equipment","Bu yer boş."); s["equipment"].erase(slot); }
        else {
            auto uid = text(request.at("uid"),32); const auto entry = owned(s,uid); const auto& item = config_.at("items").at(entry.at("item").get<std::string>());
            // 0042dff0 refusal "Bu eşyayı giyemezsiniz." (0x591964) for every wear gate.
            const char* unwearable = action == "equip" ? "Bu eşyayı giyemezsiniz." : "Bu eşyayı kullanmak için seviyeniz yetersiz.";
            require(s.at("level").get<int>() >= item.at("level").get<int>(),"level",unwearable);
            if (action == "equip") {
                require(item.contains("slot"),"equipment",unwearable);const auto slot=item.at("slot").get<std::string>();
                require(!item.contains("class") || item.at("class").get<int>()+1==a.character.at("class_id").get<int>(),"class",unwearable);
                require(!item.contains("sex") || item.at("sex")==a.character.at("appearance").value("sex",1),"sex",unwearable);
                require(s.at("equipment").contains(slot) || s.at("equipment").size()<8,"equipment","Bütün giyim yerleri dolu.");
                s["equipment"][slot]=uid;
            }
            else {
                require(item.contains("heal") || item.contains("mana"),"item","Bu eşya kullanılamaz.");
                const bool heal=item.contains("heal") && s.at("hp").get<int>() < s.at("max_hp").get<int>(), mana=item.contains("mana") && s.at("mana").get<int>() < s.at("max_mana").get<int>();
                require(heal || mana,"resource",item.contains("heal") ? "Enerjiniz zaten dolu." : "Kudretiniz zaten dolu."); take(s,uid,1);
                if (heal) s["hp"] = std::min(s.at("max_hp").get<int>(),s.at("hp").get<int>()+item.at("heal").get<int>());
                if (mana) s["mana"] = std::min(s.at("max_mana").get<int>(),s.at("mana").get<int>()+item.at("mana").get<int>());
            }
        }
        commit({{id,s}});
        if(action=="equip" || action=="unequip")project_event(id,{{"type",action},{"entity",id},{"source",id},{"target",id},{"equipment",visible_equipment(s)}});
        ack(); return;
    }
    if (action == "buy" || action == "sell") {
        require(!trade(id),"trade","Alışveriş halindeyken bunu yapamazsınız!"); auto& n = npc(id,target(),"merchant"); int count = integer(request.at("quantity"),1,1000); auto s = current;
        // Client buy check 00431700, in its order and with its messages: Lira (attribute 3, 0x591248), Klan Parası (pvpprice vs 64,
        // 0x5911d8), lonca membership (item minloncalevel > 0, 0x591178) and level (0x591108), then Akçe: trunc(akceprice·0.2) vs
        // attribute 66 (0x5910a8). The server-side charge is server-only: native deducts exactly the amounts the client checked.
        if (action == "buy") { auto item = text(request.at("item"),32); require(config_.at("items").contains(item) && std::find(n.definition.at("shop").begin(),n.definition.at("shop").end(),Json(item)) != n.definition.at("shop").end(),"item","Bu eşya burada satılmıyor.");
            const auto& d = config_.at("items").at(item);
            const int64_t lira = d.at("price").get<int64_t>()*count, clan = d.value("clan_price",int64_t(0))*count, akce = d.value("akce_price",int64_t(0))/5*count; // ×0.2 (0x590be0), truncated
            require(s.at("currency").get<int64_t>() >= lira,"currency","Bu eşyayı satın almak için yeterli paranız yok.");
            require(s.value("clan_currency",int64_t(0)) >= clan,"clan_currency","Bu eşyayı satın almak için yeterli KLAN paranız yok.");
            if (d.contains("guild_level")) {
                require(s.contains("guild") && s.at("guild").is_object(),"guild","Bu eşyayı satın almak için bir loncanız olmalı.");
                require(s.at("guild").value("level",1) >= d.at("guild_level").get<int>(),"guild","Bu eşyayı satın almak için lonca seviyeniz yetersiz.");
            }
            require(s.value("akce",int64_t(0)) >= akce,"akce","Bu eşyayı satın almak için yeterli Akçeniz yok.");
            currency(s,-lira); if (clan) s["clan_currency"] = s.value("clan_currency",int64_t(0))-clan; if (akce) s["akce"] = s.value("akce",int64_t(0))-akce;
            give(s,item,count); }
        else { auto uid = text(request.at("uid"),32); auto item = owned(s,uid).at("item").get<std::string>(); const auto& d = config_.at("items").at(item);
            // Client vendor drop 004344b0 checks, in order: itemflag bit 2 (0x591f40), no Lira and no Klan Parası price (0x591eb8),
            // Klan Parası goods need itemflag bit 4 without bits 1|2 (004311b0, 0x591e08), worn items (0x591c98). Plus Paket
            // holders get 1/3 instead of 1/5 (confirm 0x591d60, guide). NONCANONICAL: Lira-less Klan Parası goods pay that ratio of
            // their pvpprice in Klan Parası (the client only says such goods sell "klan parasıyla"; the payout was server-only).
            const int flags = d.value("flags",0); const int64_t lira = d.at("price").get<int64_t>(), clan = d.value("clan_price",int64_t(0));
            require(!n.definition.value("no_buy",false),"sell","Bu hedef, eşya satın almaz."); // entity flag +0x82 & 0x40 (004344b0 @434603, 0x591f54), before the item checks
            require(!(flags & 2),"sell","Bu eşya satılamaz.");
            require(lira > 0 || clan > 0,"sell","Bu eşya satılamaz, çöpe atmayı veya başka oyuncuya satmayı deneyin.");
            require(clan <= 0 || ((flags & 3) == 0 && (flags & 4)),"sell","Bu eşya klan parasıyla da satılamaz, çöpe atmayı veya başka oyuncuya satmayı deneyin.");
            for (const auto& [slot,worn] : s.at("equipment").items()) { (void)slot; require(worn != uid,"equipped","Giyilmekte olan nesneler satılamaz; giysiyi önce üzerinizden çıkarın."); }
            const int denominator = config_.at(a.plus ? "vendor_plus_return_denominator" : "vendor_return_denominator");
            if (lira > 0) currency(s,lira*count/denominator);
            else { const int64_t next = s.value("clan_currency",int64_t(0))+clan*count/denominator; require(next <= 999000000,"clan_currency","En fazla 999 milyon klan paranız olabilir."); s["clan_currency"] = next; }
            take(s,uid,count); }
        commit({{id,s}}); ack(); return;
    }
    if (action == "trade_request") {
        auto t = target(); auto& other = actor(t); require(id != t && !other.kicked && other.character.at("state").at("hp").get<int>() > 0,"trade","Bu kişiyle alışveriş yapılamaz."); require(close(id,t,config_.at("trade_range")),"range","Alışveriş yapılacak kişi çok uzakta."); require(!trade(id),"trade","Alışveriş halindeyken başka alışveriş başlatamazsınız."); require(!trade(t),"trade","Diğer taraf şu anda meşgul.");
        require(trade_revision_<UINT64_MAX,"trade_revision","Alışveriş başlatılamadı.");
        Trade value;value.a=id;value.b=t;value.revision=trade_revision_+1;
        auto [first,second]=trade_events(value);trade_revision_=value.revision;trades_[id]=std::move(value);
        emit(id,std::move(first));emit(t,std::move(second));
        emit(t,{{"type","trade_invite"},{"from",id},{"phase","invited"},{"revision",trade_revision_}});ack();return;
    }
    if (action == "trade_cancel") { require(trade(id),"trade","Süren bir alışveriş yok."); cancel_trade(id); ack(); return; }
    if (action == "trade_offer" || action == "trade_accept") {
        auto* t = trade(id); require(t,"trade","Süren bir alışveriş yok."); require(close(t->a,t->b,config_.at("trade_range")) && actor(t->a).character.at("state").at("hp").get<int>() > 0 && actor(t->b).character.at("state").at("hp").get<int>() > 0,"trade","Diğer taraf çok uzakta ya da ölü.");
        if(action=="trade_accept") {
            require(request.contains("revision"),"trade_revision","Alışveriş şartları belirtilmeli.");
            const auto& revision=request.at("revision");
            require(revision.is_number_unsigned() || (revision.is_number_integer() && revision.get<int64_t>()>0),"trade_revision","Geçersiz alışveriş şartları.");
            require(revision.get<uint64_t>()==t->revision,"trade_revision","Alışveriş şartları değişti; onaylamadan önce yeni şartları inceleyin.");
        }
        if (!t->open) { require(action == "trade_accept" && id == t->b,"trade","Önce diğer taraf alışverişi kabul etmeli."); t->open = true; trade_event(*t); return; }
        if (action == "trade_offer") {
            Json offer{{"items",request.at("items")},{"currency",request.at("currency")}};validate_offer(id,offer);
            require(trade_revision_<UINT64_MAX,"trade_revision","Alışveriş başlatılamadı.");
            auto proposed=*t;if(id==t->a)proposed.offer_a=std::move(offer);else proposed.offer_b=std::move(offer);
            proposed.accept_a=proposed.accept_b=false;proposed.revision=trade_revision_+1;
            auto [first,second]=trade_events(proposed);trade_revision_=proposed.revision;*t=std::move(proposed);
            emit(t->a,std::move(first));emit(t->b,std::move(second));return;
        }
        bool both = id == t->a ? t->accept_b : t->accept_a;
        if (!both) { if (id == t->a) t->accept_a = true; else t->accept_b = true; trade_event(*t); return; }
        validate_offer(t->a,t->offer_a); validate_offer(t->b,t->offer_b);
        auto sa = actor(t->a).character.at("state"), sb = actor(t->b).character.at("state");
        // Remove both sets first, allowing a full bag to swap an outgoing slot for an incoming slot.
        // Moved entries keep their instance fields (Efsun prefixes, Eskime); give() assigns fresh uids.
        Json outgoing_a=Json::array(),outgoing_b=Json::array();
        const auto moving=[&](Json& s,const Json& item){ auto entry=owned(s,item.at("uid")); entry["quantity"]=item.at("quantity"); return entry; };
        for (const auto& item : t->offer_a.at("items")) { outgoing_a.push_back(moving(sa,item)); take(sa,item.at("uid"),item.at("quantity")); }
        for (const auto& item : t->offer_b.at("items")) { outgoing_b.push_back(moving(sb,item)); take(sb,item.at("uid"),item.at("quantity")); }
        for (const auto& item : outgoing_a) give(sb,item.at("item"),item.at("quantity"),item);
        for (const auto& item : outgoing_b) give(sa,item.at("item"),item.at("quantity"),item);
        int64_t delta = t->offer_b.at("currency").get<int64_t>()-t->offer_a.at("currency").get<int64_t>(); currency(sa,delta); currency(sb,-delta);
        const auto first=t->a,second=t->b;
        try { commit({{first,sa},{second,sb}}); }
        catch(const std::exception&) { throw TradePersistenceFailure{id==first?second:first,t->revision}; }
        trades_.erase(first);
        emit(first,{{"type","trade_complete"},{"peer",second}}); emit(second,{{"type","trade_complete"},{"peer",first}}); return;
    }
    // Shift-click on a stack (0042f690): "%d nesneden kaç tanesini ayırmak istersiniz?" then 'mtes' {uid, count} (004336c0);
    // refused while an exchange is open with 0x590db0.
    if (action == "split") {
        require(!trade(id),"trade","Ticaret esnasında bunu yapamazsınız.");
        auto s=current;auto uid=text(request.at("uid"),32);auto& entry=owned(s,uid);int count=integer(request.at("quantity"),1,1000);
        require(count < entry.at("quantity").get<int>(),"quantity","Ayırdıktan sonra hücrede en az bir eşya kalmalı.");
        require(s.at("inventory").size() < config_.at("inventory_slots").get<size_t>(),"inventory","Çantanızda yer yok!!");
        const auto item=entry.at("item");entry["quantity"]=entry.at("quantity").get<int>()-count;
        s["inventory"].push_back({{"uid",new_uid()},{"item",item},{"quantity",count}});
        commit({{id,s}});ack();return;
    }
    // A bag cell dropped outside every window (004344b0 param_1 0): worn items refuse (0x591890), otherwise the confirm
    // "Bu eşya kalıcı olarak yok edilecektir. Emin misiniz?" sends 'spro' {uid} (0042e0a0) and the whole cell goes.
    // NONCANONICAL: itemflag bit 2 refuses for everyone ("Bu eşya yok edilemez." 0x5918e0; the client gated it on an account range).
    if (action == "item_destroy") {
        require(!trade(id),"trade","Ticaret esnasında bunu yapamazsınız.");
        auto s=current;const auto uid=text(request.at("uid"),32);const auto& entry=owned(s,uid);
        for (const auto& [slot,worn] : s.at("equipment").items()) { (void)slot; require(worn != uid,"equipped","Giyilmekte olan nesneler yok edilemez."); }
        require(!(config_.at("items").at(entry.at("item").get<std::string>()).value("flags",0) & 2),"destroy","Bu eşya yok edilemez.");
        take(s,uid,entry.at("quantity"));commit({{id,s}});ack();return;
    }
    // A bag cell dropped on another grid cell (004344b0 bag → bag). NONCANONICAL: the native bag holds one stack per cell in
    // inventory order, so the dragged entry swaps with the one in grid cell `to` (unworn entries counted), or goes last.
    if (action == "item_move") {
        auto s=current;const auto uid=text(request.at("uid"),32);const auto to=size_t(integer(request.at("to"),0,1000));owned(s,uid);
        const auto worn=[&](const Json& e){for (const auto& [slot,w] : s.at("equipment").items()) { (void)slot; if (w == e.at("uid")) return true; } return false;};
        require(!worn(owned(s,uid)),"equipped","Giyilenler penceresi içerisinde taşıma yapamazsınız.");
        auto& inventory=s.at("inventory");std::vector<size_t> cells;size_t from=0;
        for (size_t i=0;i<inventory.size();++i) { if (inventory[i].at("uid") == uid) from=i; if (!worn(inventory[i])) cells.push_back(i); }
        if (to < cells.size()) std::swap(inventory[from],inventory[cells[to]]);
        else { Json moved=std::move(inventory[from]); inventory.erase(inventory.begin()+std::ptrdiff_t(from)); inventory.push_back(std::move(moved)); }
        commit({{id,s}});ack();return;
    }
    if (action == "guild_create" || action == "guild_invite" || action == "guild_accept" || action == "guild_leave" || action == "guild_kick" || action == "guild_donate") {
        const bool member=current.contains("guild") && !current.at("guild").is_null();
        if (action == "guild_invite") {
            require(member && current.at("guild").at("leader_character_id") == a.character_id,"guild","Bunu yalnızca Lonca Reisi yapabilir.");
            auto t=target();auto& other=actor(t);const auto& s=other.character.at("state");
            require(t!=id && (!s.contains("guild") || s.at("guild").is_null()),"guild","Bu kişi zaten bir loncada.");
            require(current.at("guild").at("members").size()<64,"guild","Lonca dolu.");
            guild_invites_[t]=current.at("guild");emit(t,{{"type","guild_invite"},{"from",id},{"name",a.character.at("name")},{"guild",current.at("guild")}});ack();return;
        }
        Json g;uint32_t departing=0;int64_t donated=0;
        if (action == "guild_create") {
            require(!member,"guild","Zaten bir loncadasınız.");
            auto name=text(request.at("name"),32);require(name.size()>=2,"guild","Lonca adı çok kısa.");
            g={{"id",new_uid()},{"name",name},{"leader_character_id",a.character_id},{"members",Json::array({a.character_id})},{"revision",1},{"level",config_.at("economy").value("guild_level",1)}};
        } else if (action == "guild_accept") {
            require(!member && guild_invites_.contains(id),"guild","Lonca davetiniz yok.");g=guild_invites_.at(id);
            bool leader_online=false;
            for (const auto& [other,b] : actors_) {
                (void)other;const auto& s=b.character.at("state");
                if (b.character_id==g.at("leader_character_id").get<int64_t>() && s.contains("guild") && !s.at("guild").is_null() && s.at("guild").at("id")==g.at("id")) {g=s.at("guild");leader_online=true;break;}
            }
            require(leader_online,"guild","Davet eden Lonca Reisi artık oyunda değil.");
            require(g.at("members").size()<64,"guild","Lonca dolu.");g["members"].push_back(a.character_id);g["revision"]=g.at("revision").get<int64_t>()+1;
        } else if (action == "guild_donate") {
            // NONCANONICAL lonca progression (minloncalevel gates purchases; the original rule is server-only): members donate
            // Klan Parası into the Lonca Parası pool, and level L rises to L+1 once it holds L × economy.guild_level_cost.
            require(member,"guild","Bir loncada değilsiniz.");g=current.at("guild");
            donated=integer(request.at("amount"),1,999000000);
            require(current.value("clan_currency",int64_t(0))>=donated,"clan_currency","Bağış için yeterli KLAN paranız yok.");
            int level=g.value("level",1);require(level<100,"guild","Lonca en yüksek seviyede.");
            int64_t funds=g.value("funds",int64_t(0))+donated;const int64_t cost=config_.at("economy").at("guild_level_cost");
            while (level<100 && funds>=level*cost) {funds-=level*cost;++level;}
            g["funds"]=std::min<int64_t>(funds,999000000);g["level"]=level;g["revision"]=g.at("revision").get<int64_t>()+1;
        } else {
            require(member,"guild","Bir loncada değilsiniz.");g=current.at("guild");departing=id;
            if (action == "guild_kick") {
                require(g.at("leader_character_id")==a.character_id,"guild","Bunu yalnızca Lonca Reisi yapabilir.");departing=target();auto& other=actor(departing);
                require(departing!=id && other.character.at("state").contains("guild") && !other.character.at("state").at("guild").is_null() && other.character.at("state").at("guild").at("id")==g.at("id"),"guild","Bu kişi loncanızda değil.");
            }
            auto character=actor(departing).character_id;auto& members=g.at("members");
            for (auto it=members.begin();it!=members.end();) if (*it==character)it=members.erase(it);else ++it;
            if (g.at("leader_character_id")==character)g["leader_character_id"]=members.empty()?Json(0):members.front();
            g["revision"]=g.at("revision").get<int64_t>()+1;
        }
        std::map<uint32_t,Json> proposed;
        for (const auto& [other,b] : actors_) {
            const auto& s=b.character.at("state");
            const bool same=s.contains("guild") && !s.at("guild").is_null() && s.at("guild").at("id")==g.at("id");
            if (same || other==id || other==departing) {proposed[other]=s;proposed[other]["guild"]=g;}
        }
        if (donated) proposed.at(id)["clan_currency"]=current.value("clan_currency",int64_t(0))-donated;
        commit(proposed);
        if (departing) {actor(departing).character["state"].erase("guild");guild_invites_.erase(departing);}
        if (action=="guild_accept")guild_invites_.erase(id);
        for (const auto& [other,s] : proposed) {(void)s;emit(other,{{"type","guild"},{"guild",other==departing?Json(nullptr):g}});}
        ack();return;
    }
    if (action == "chat") {
        require(!a.muted && !a.externally_muted,"muted","Konuşmanız yasaklandı."); require(tick_ >= a.chat_ready,"rate","Bu yazışma kanalını gereksiz yere kirletmemeye özen gösteriniz."); auto channel = text(request.at("channel"),16);
        // Ctrl-click item link (0042e8c0 → cdfc payload {-1, uid, prefix1, prefix2, 0} on any channel but Rol yap, read back by
        // 0040b170 as "[<prefixes> <name>]"): the server resolves the sender's own bag/worn entry, so a link cannot be forged.
        const bool linked = request.contains("link"); auto message = linked ? std::string{} : text(request.at("text"),512);
        for (unsigned char c : message) require(c >= 32 || c == '\t',"chat","Mesajda geçersiz karakter var.");
        Json value{{"type","chat"},{"channel",channel},{"from",id},{"name",a.character.at("name")},{"text",message}};
        if (linked) {
            require(channel != "roleplay","chat","Rol yap kanalında eşya bağlantısı gönderilemez.");
            const auto& entry = owned(current,text(request.at("link"),32)); const auto item = entry.at("item").get<std::string>();
            Json link{{"item",item},{"definition",config_.at("items").at(item)}}; if (entry.contains("prefixes")) link["prefixes"] = entry.at("prefixes");
            value["link"] = std::move(link);
        }
        // Guide "Er Meydanı": "/em" lists, "/savas 1" enters; "/kpem" and "/kpsavas 1" for the Klan Parası kind. NONCANONICAL:
        // an optional "/kpsavas N miktar" stake (default activities.arena.stake), confirmed in a Dikkat box (arena_stake event;
        // the original stake prompt and its strings are not in the client) before the client sends arena_join.
        if (std::istringstream words(message); message.starts_with("/em") || message.starts_with("/kpem") || message.starts_with("/savas") || message.starts_with("/kpsavas")) {
            std::string command; long long code = 0, stake = 0; words >> command >> code >> stake;
            a.chat_ready = tick_+10;
            if (command == "/em" || command == "/kpem") { arena(id,"arena_list",{{"stake",command == "/kpem"}}); return; }
            if (command == "/savas") { arena(id,"arena_join",{{"code",code},{"stake",0}}); return; }
            if (command == "/kpsavas") { arena(id,"arena_stake",{{"code",code},{"stake",stake > 0 ? stake : config_.at("activities").at("arena").at("stake").get<long long>()}}); return; }
        }
        // Rol yapma carries character gestures (help "Rol yapma Kanalı"); the original Z key sends "/otur" (0x59b728, 004b73ab).
        if ((channel == "roleplay" || channel == "local") && message.starts_with('/')) {
            const auto& emotes=config_.at("social").at("emotes"); const auto name=message.substr(1,message.find(' ')==std::string::npos?std::string::npos:message.find(' ')-1);
            require(emotes.contains(name),"emote","Bilinmeyen hareket."); require(current.at("hp").get<int>() > 0,"dead","Ölüyken bunu yapamazsınız!");
            const auto& clip=emotes.at(name); const auto& postures=config_.at("social").value("postures",Json::object());
            a.posture=postures.value(std::to_string(clip.get<int>()),uint8_t{0}); // late joiners see a held sit/lay (snapshot "posture")
            project_event(id,{{"type","emote"},{"entity",id},{"source",id},{"target",id},{"clip",clip}});
            a.chat_ready = tick_+10; return;
        }
        // Yasaklılar: a listener never receives chat from a character on its ignore list.
        const auto deliver=[&](uint32_t other) { if (other == id || !ignores(other,a.character_id)) emit(other,value); };
        // Original help: Söyle and Rol yapma reach the view range, Bölge the whole region (native zone). Guide (rehberi "Bölge kanalı",
        // "Lonca kanalı"): writing on Bölge needs Plus Paket (or a web-shop "Genel sohbet" product, not ported) and on Lonca Plus Paket.
        // NONCANONICAL refusal wording: the client holds no such string (the official server sent it).
        if (channel == "zone" || channel == "guild") require(a.plus,"plus",channel == "zone" ? "Bölge kanalına yazabilmek için Plus Paket sahibi olmanız gerekir." : "Lonca kanalını kullanabilmek için Plus Paket sahibi olmanız gerekir.");
        if (channel == "local" || channel == "roleplay") { for (auto other : nearby(id,config_.at("aoi"))) if (actors_.contains(other)) deliver(other); }
        else if (channel == "zone") { for (const auto& [other,b] : actors_) if (b.character.at("state").at("zone") == current.at("zone")) deliver(other); }
        else if (channel == "private") { auto to = target_id(request.at("to")); actor(to); deliver(to); if (to != id) emit(id,value); }
        else if (channel == "group") { auto leader = group(id); require(leader,"group","Bir grupta değilsiniz."); for (auto other : groups_.at(leader)) deliver(other); }
        else if (channel == "guild") {
            require(current.contains("guild") && !current.at("guild").is_null(),"guild","Bir loncada değilsiniz.");
            for (const auto& [other,b] : actors_) { const auto& s=b.character.at("state");if (s.contains("guild") && !s.at("guild").is_null() && s.at("guild").at("id")==current.at("guild").at("id"))deliver(other); }
        }
        else if (channel == "clan") { // help "Klan Kanalı": everyone bound to the same clan
            require(current.contains("clan"),"clan","Bir klanda değilsiniz.");
            for (const auto& [other,b] : actors_) if (b.character.at("state").value("clan",0) == current.at("clan")) deliver(other);
        }
        else throw Rejected{"chat","Bilinmeyen sohbet kanalı."};
        a.chat_ready = tick_+10; return;
    }
    if (action == "group_invite") {
        auto t = target(); actor(t); require(t != id && !group(t),"group","Bu kişi zaten bir grupta."); require(close(id,t,config_.at("aoi")),"range","Davet edilen kişi çok uzakta.");
        auto leader = group(id); require(!leader || leader == id,"group","Yalnızca grup lideri davet edebilir."); require(!leader || groups_.at(leader).size() < 6,"group","Grup dolu."); invites_[t] = id; emit(t,{{"type","group_invite"},{"from",id},{"name",a.character.at("name")}}); ack(); return;
    }
    if (action == "group_accept") {
        require(invites_.contains(id) && !group(id),"group","Grup davetiniz yok."); auto leader = invites_.at(id); actor(leader); require(close(id,leader,config_.at("aoi")),"range","Grup lideri çok uzakta."); require(!group(leader) || group(leader) == leader,"group","Davet eden artık grup lideri değil.");
        if (!groups_.contains(leader)) groups_[leader] = {leader};
        require(groups_.at(leader).size() < 6,"group","Grup dolu."); groups_[leader].push_back(id); invites_.erase(id); for (auto other : groups_.at(leader)) emit(other,{{"type","group"},{"leader",leader},{"members",groups_.at(leader)}}); return;
    }
    if (action == "group_leave") { leave_group(id); ack(); return; }
    if (action == "group_kick") { // original İşlemler "Gruptan Çıkar" (VMER, 0041a4f0): leader removes a member
        auto t = target(); require(t != id && group(id) == id && group(t) == id,"group","Yalnızca grup lideri kendi grubundan birini atabilir."); leave_group(t); ack(); return;
    }
    // Original "Şikayet Et" sends 'lpmc' with the player id (0040af4c/004192a8); its handling was server-side.
    // NONCANONICAL: online administrators receive a complaint event.
    if (action == "complain") {
        auto t = target(); require(t != id,"target","Kendinizi şikayet edemezsiniz."); const auto& name = actor(t).character.at("name");
        // Guide "Şikayet hakları ... Plus Paket Sahibi Oyuncular:10, Normal Kullanıcılar: 3". NONCANONICAL: counted per session.
        require(a.complaints < (a.plus ? 10 : 3),"complain","Şikayet hakkınız doldu."); ++a.complaints;
        for (const auto& [other,b] : actors_) if (b.admin) emit(other,{{"type","complaint"},{"from",id},{"name",a.character.at("name")},{"target",t},{"target_name",name}});
        ack(); return;
    }
    // Original GM "Yanına Işınla" (NISI, 00419435) chats "|tele <name> x y zone" with the caller's own position: the target
    // is moved next to the administrator. The privilege check was server-side; here only administrators may use it.
    if (action == "teleport_here") {
        require(a.admin,"permission","Bunun için yönetici yetkisi gerekir."); auto t = target(); actor(t); require(t != id,"target","Kendinizi yanınıza ışınlayamazsınız.");
        transfer(t,current.at("zone").get<std::string>(),current.at("x").get<int32_t>(),current.at("y").get<int32_t>()); ack(); return;
    }
    if (action == "zone") {
        // Players change zones by using <loc> door NPCs ("interact"); this administrator jump to a zone spawn is debug-only.
        require(a.admin,"permission","Bunun için yönetici yetkisi gerekir."); auto zone = text(request.at("zone"),32);
        require(config_.at("zones").contains(zone),"zone","Böyle bir bölge yok."); const auto& spawn=config_.at("zones").at(zone).at("spawn");
        require(passable(zone,spawn[0],spawn[1]),"collision","Bölgenin başlangıç noktası yürünebilir değil.");
        transfer(id,zone,spawn[0],spawn[1]); ack(); return;
    }
    if (action == "moderate") {
        require(a.admin,"permission","Bunun için yönetici yetkisi gerekir."); auto t = target(); auto& other = actor(t); require(t != id,"permission","Bu işlemi kendinize uygulayamazsınız."); auto operation=text(request.at("operation"),16);
        if (operation == "mute") { other.muted=true; emit(t,{{"type","moderation"},{"operation","mute"},{"by",id}}); }
        else if (operation == "unmute") { other.muted=false; emit(t,{{"type","moderation"},{"operation","unmute"},{"by",id}}); }
        else if (operation == "kick") kick(t,id);
        else throw Rejected{"request","Bilinmeyen yönetim işlemi."};
        ack(); return;
    }
    throw Rejected{"action","Bilinmeyen işlem."};
}

void World::advance() {
    ++tick_;
    for (auto& [id,a] : actors_) {
        auto& s = a.character.at("state"); auto& buffs = s.at("buffs"); for (auto it=buffs.begin();it!=buffs.end();) if (it->at("expires").get<uint32_t>() <= tick_) it=buffs.erase(it); else ++it;
        if (a.kicked || s.at("hp").get<int>() <= 0) { a.moving=false; a.dx=a.dy=0; a.gathering=0; a.posture=0; continue; }
        // Skill regeneration: Meditasyon mana each second, İyileştirme Çemberi health every 3 seconds.
        if (tick_ % 20 == 0) if (const int regen = buff_sum(buffs,"regen")) s["mana"] = std::min(s.at("max_mana").get<int>(),s.at("mana").get<int>()+regen);
        if (tick_ % 60 == 0) if (const int hot = buff_sum(buffs,"hot")) s["hp"] = std::min(s.at("max_hp").get<int>(),s.at("hp").get<int>()+hot);
        // Passive regeneration while not auto-attacking (VOD 14:20-14:55: "+3 enerji" every 5 s out of combat).
        if (const auto r = config_.find("regeneration"); r != config_.end() && !a.engaged && tick_ % r->at("interval").get<uint32_t>() == 0) {
            s["hp"] = std::min(s.at("max_hp").get<int>(), s.at("hp").get<int>() + r->at("hp").get<int>() * regen_rate_ / 100); // administrator regen rate
            s["mana"] = std::min(s.at("max_mana").get<int>(), s.at("mana").get<int>() + r->at("mana").get<int>() * regen_rate_ / 100);
        }
        if (a.engaged) { a.posture=0; engage(id,a); } // may commit state: re-read buffs below
        if (a.gathering) gather_tick(id,a); // Toplama completes or is cancelled (move/attack/range); may commit state
        auto x=coordinate(s,"x"),y=coordinate(s,"y"); int nx=x,ny=y,step=config_.at("movement_step").get<int>()*move_percent(s.at("buffs"))/100;
        // Hareket Hızı Efsun (61): NONCANONICAL percent on the step (one prefixes.dat entry carries 2).
        if (step && (a.moving || a.dx || a.dy)) if (const auto g = gear(s); g.efsun.contains(61)) step = int(step*(100+g.efsun.at(61))/100);
        if (tick_ % 20 == 0) quest_expiry(id,a); // "... görevinin süresi doldu!" (VOD 44:35); may commit state
        if (!step) continue;
        const bool source=source_zones_.contains(s.at("zone").get<std::string>()), routed=a.moving;
        if (a.moving) {
            const auto [gx,gy]=a.route.at(a.waypoint);
            std::tie(nx,ny)=toward(x,y,gx,gy,step,source);
            if(nx==gx && ny==gy && ++a.waypoint==a.route.size()){a.moving=false;a.route.clear();}
        }
        else { nx += step*a.dx; ny += step*a.dy; }
        if (nx==x && ny==y) continue;
        const auto zone=s.at("zone").get<std::string>();
        // route() validated each source segment whole; re-sampling 800-unit steps of it picks different cell corners and
        // falsely collided mid-route, so routed steps only need a supported endpoint.
        bool clear=routed && source ? passable(zone,nx,ny) : clear_path(zone,x,y,nx,ny);
        if (!clear) { a.moving=false;a.dx=a.dy=0; emit(id,{{"type","correction"},{"x",x},{"y",y},{"reason","collision"}}); continue; }
        index(id,zone,x,y,false); s["x"]=nx;s["y"]=ny;index(id,zone,nx,ny,true); a.posture=0;
    }
    for (auto it=trades_.begin();it!=trades_.end();) {
        const auto first=it->second.a,second=it->second.b;
        if (!close(first,second,config_.at("trade_range")) || actors_.at(first).character.at("state").at("hp").get<int>() == 0 || actors_.at(second).character.at("state").at("hp").get<int>() == 0) { ++it;cancel_trade(first); } else ++it;
    }
    // Skill damage over time on players (harmful buffs from hostile_skill), every 3 seconds while its caster stays connected (a
    // Şef's: while it lives); the effect's skill school meets the victim's resistance (Zehirleme poison, Asit acid), without crits.
    std::vector<std::tuple<uint32_t,uint32_t,int,std::string>> dots;
    const auto casting = [&](uint32_t source) { const auto n = npcs_.find(source); return actors_.contains(source) || (n != npcs_.end() && n->second.hp > 0); };
    // Kanatma / Boz Ayı drain takes its amount of Kudret per tick (NONCANONICAL amount: the effect's DoT value).
    for (auto& [id,a] : actors_) { auto& s = a.character.at("state"); for (auto& buff : s.at("buffs"))
        if ((buff.contains("dot") || buff.contains("drain")) && tick_ >= buff.at("next").get<uint32_t>()) {
            buff["next"] = tick_+60; const auto source = buff.at("source").get<uint32_t>();
            if (buff.contains("drain") && casting(source) && s.at("hp").get<int>() > 0) s["mana"] = std::max(0,s.at("mana").get<int>()-buff.at("drain").get<int>());
            if (buff.contains("dot")) dots.emplace_back(source,id,buff.at("dot").get<int>(),buff.value("id",std::string()));
        } }
    for (const auto& [source,victim,dot,skill] : dots) if (casting(source) && actors_.at(victim).character.at("state").at("hp").get<int>() > 0)
        strike(source,victim,spell(dot,config_.at("skills").contains(skill) ? config_.at("skills").at(skill) : Json::object()));
    summons();
    for (auto& [id,n] : npcs_) {
        if (n.master) continue; // summons() runs summoned NPCs
        const auto& kind = n.definition.at("kind");
        // Labirent Tuzağı (classes.dat 17109 ID_LAB_TUZAK: attack 9999, attackrange 10, attackrate 3000 ms, not interactable)
        // strikes every living player within its range at its rate. NONCANONICAL damage: its effect was server skill group 23.
        if (kind == "trap") {
            if (tick_ < n.attack_ready) continue;
            const auto& trap = n.definition.at("trap"); const int64_t reach = trap.at("range").get<int64_t>();
            for (const auto& [pid,a] : actors_) { const auto& s = a.character.at("state");
                if (!a.kicked && s.at("hp").get<int>() > 0 && s.at("zone") == n.definition.at("zone") && distance(n.x,n.y,s.at("x"),s.at("y")) <= reach*reach)
                    strike(id,pid,Blow{n.definition.value("attack",-1.0),trap.at("damage").get<double>(),0,2,{},""}); }
            n.attack_ready = tick_ + trap.at("cooldown").get<uint32_t>(); continue;
        }
        if (kind != "enemy" && !guard(n.definition)) { if (!n.hp && tick_ >= n.respawn_at) n.hp = n.definition.at("hp"); else if (n.hp) idle_walk(id,n,100); continue; } // exhausted Toplama nodes regrow
        const auto zone=n.definition.at("zone").get<std::string>(); const int sx=n.definition.at("x"),sy=n.definition.at("y");
        if (n.hp == 0) {
            if (tick_ < n.respawn_at) continue;
            index(id,zone,n.x,n.y,false);n.x=sx;n.y=sy;n.hp=n.definition.at("hp");n.owner=0;n.threat.clear();n.looted=false;n.returning=false;n.wandering=false;n.route.clear();n.route_planned=false;index(id,zone,n.x,n.y,true);
            project_event(id,{{"type","resurrect"},{"entity",id},{"source",id},{"target",id}});continue;
        }
        // Skill damage over time is credited to its living caster; expired effects drop afterwards.
        for (auto it=n.debuffs.begin();it!=n.debuffs.end();) {
            if (it->contains("dot") && tick_ >= it->at("next").get<uint32_t>()) {
                (*it)["next"] = it->at("next").get<uint32_t>()+60;
                const auto source = it->at("source").get<uint32_t>(); const auto skill = it->at("id").get<std::string>(); const int dot = it->at("dot");
                if (actors_.contains(source) && actors_.at(source).character.at("state").at("hp").get<int>() > 0 && actors_.at(source).character.at("state").at("zone") == zone) {
                    try { combat(source,id,dot,actors_.at(source).character.at("state"),{{"type","hit"},{"skill",skill}}); }
                    catch (const Rejected& e) { reject(source,e.code,e.message); } catch (const std::exception&) { reject(source,"persistence","Dövüş sonucu kaydedilemedi."); }
                    if (!n.hp) break; // death cleared the effect list
                }
            }
            if (tick_ >= it->at("expires").get<uint32_t>()) it=n.debuffs.erase(it); else ++it;
        }
        if (n.hp == 0) continue;
        if (n.definition.value("banner",false)) continue; // Sancak: stands still and never strikes
        const int pace = move_percent(n.debuffs);
        const bool was_engaged=!n.threat.empty() || n.owner || n.hp<n.definition.at("hp").get<int>();
        uint32_t victim=0;int64_t highest=-1;
        for (auto it=n.threat.begin();it!=n.threat.end();) {
            // Victims: players, or a summon that bit this NPC while it lives in the zone.
            const auto a=actors_.find(it->first); const auto m=npcs_.find(it->first);
            const bool alive=a!=actors_.end() ? !a->second.kicked && a->second.character.at("state").at("hp").get<int>()>0 && a->second.character.at("state").at("zone")==zone
                                              : m!=npcs_.end() && m->second.master && m->second.hp>0 && m->second.definition.at("zone")==zone;
            if (!alive) it=n.threat.erase(it);
            else { if (it->second > highest) { highest=it->second;victim=it->first; } ++it; }
        }
        const auto spot=[&](uint32_t v) { if(const auto p=actors_.find(v);p!=actors_.end())return std::pair<int32_t,int32_t>{coordinate(p->second.character.at("state"),"x"),coordinate(p->second.character.at("state"),"y")}; const auto& m=npcs_.at(v);return std::pair<int32_t,int32_t>{m.x,m.y}; };
        if (n.returning && n.x==sx && n.y==sy) n.returning=false;
        if (!victim && !n.returning) { // leashed NPCs walk home before aggroing again
            const auto [z,cx,cy] = cell(zone,n.x,n.y); const int radius=(n.definition.at("aggro").get<int>()+config_.at("cell_size").get<int>()-1)/config_.at("cell_size").get<int>();
            int64_t nearest=INT64_MAX;
            for (auto bucket=cells_.lower_bound({z,cx-radius,INT32_MIN});bucket!=cells_.end();++bucket) {
                const auto& [bucket_zone,x,y]=bucket->first;
                if(bucket_zone!=z || x>cx+radius)break;
                if(y<cy-radius || y>cy+radius)continue;
                for(auto candidate:bucket->second) if(actors_.contains(candidate)) { const auto& a=actors_.at(candidate);const auto& s=a.character.at("state");auto d=distance(n.x,n.y,s.at("x"),s.at("y"));
                    if(!a.kicked && s.at("hp").get<int>()>0 && !shielded(n.definition,s) && d<=int64_t(n.definition.at("aggro").get<int>())*n.definition.at("aggro").get<int>() && d<nearest) { victim=candidate;nearest=d; } } // Klan OOK: enemy-clan players only
            }
            if (victim) {
                n.threat[victim]=1;
                // Guide "Grup bölgeleri": creatures there call nearby allies within their alarm distance (NONCANONICAL: their aggro range).
                if (const auto& area=config_.at("zones").at(zone); area.contains("instances") || area.contains("instance_of"))
                    for (auto& [other_id,o] : npcs_) if (other_id!=id && !o.master && o.hp>0 && o.threat.empty() && !o.definition.value("banner",false) && o.definition.at("kind")=="enemy" &&
                                                         o.definition.at("zone")==zone && distance(n.x,n.y,o.x,o.y)<=int64_t(n.definition.at("aggro").get<int>())*n.definition.at("aggro").get<int>()) o.threat[victim]=1;
            }
        }
        int gx=sx,gy=sy;
        bool leash=(was_engaged && !victim) || distance(n.x,n.y,sx,sy)>int64_t(n.definition.at("leash").get<int>())*n.definition.at("leash").get<int>();
        if(victim) { const auto [vx,vy]=spot(victim); leash=leash || distance(vx,vy,sx,sy)>int64_t(n.definition.at("leash").get<int>())*n.definition.at("leash").get<int>(); }
        if(leash) { n.threat.clear();n.owner=0;n.hp=n.definition.at("hp");victim=0;n.returning=true; }
        if(victim || leash) n.wandering=false;
        if(!pace) continue; // stunned: neither attacks nor moves
        if(victim) {
            std::tie(gx,gy)=spot(victim);
            // NONCANONICAL Şef skills (classes.dat normalskillgroup = "cast", onhitskillgroup = "hit"; both were server skill tables).
            if(n.definition.contains("skills") && !buff_flag(n.debuffs,"silence")) boss_skills(id,n,"cast",victim);
            // Within classes.dat attackrange the NPC stands and bites at its attackrate; it never walks onto the player.
            const int64_t reach=n.definition.value("attack_range",config_.at("combat_range").get<int>());
            if(distance(n.x,n.y,gx,gy)<=reach*reach) {
                if(tick_<n.attack_ready)continue;
                // classes.dat dmgmin-dmgmax, attack (vs the victim's Savunma) and criticalchance x criticalscale; physical only.
                const auto& d=n.definition; const auto& range=d.contains("damage_range")?d.at("damage_range"):Json::array({d.at("damage"),d.at("damage")});
                Blow bite{d.value("attack",-1.0),uniform(range[0].get<double>(),range[1].get<double>()),d.contains("crit")?d.at("crit")[0].get<double>():0,d.contains("crit")?d.at("crit")[1].get<double>():2,{},""};
                if(!actors_.contains(victim)) hit_summon(id,victim,std::max(1,int(std::lround(bite.physical))));
                else { const int before=actors_.at(victim).character.at("state").at("hp"); if(!strike(id,victim,bite))continue;
                    if(n.definition.contains("skills") && actors_.at(victim).character.at("state").at("hp").get<int>()<before) boss_skills(id,n,"hit",victim); }
                n.attack_ready=tick_+n.definition.at("attack_cooldown").get<uint32_t>();
                if(n.owner==victim && actors_.contains(victim) && actors_.at(victim).character.at("state").at("hp").get<int>()==0)n.owner=0;
                continue;
            }
        } else if(!leash && !n.returning) {
            if(n.definition.contains("idlewalktend")) { idle_walk(id,n,pace); continue; } // classes.dat defines its idle behaviour
            if(!n.definition.at("patrol").empty()) {
                const auto& point=n.definition.at("patrol")[n.waypoint];gx=point[0];gy=point[1];if(n.x==gx && n.y==gy) { n.waypoint=(n.waypoint+1)%n.definition.at("patrol").size();continue; }
            }
        }
        walk(id,n,gx,gy,pace);
    }
    if (tick_ % 20 == 0) arena_tick();
    bots_tick();
}
uint32_t World::tick() const { return tick_; }
// Self plus the 31 players state() lists (group members, then nearest): the snapshot protocol bound now that bots may crowd an AOI.
std::vector<Player> World::snapshot(uint32_t id) const {
    std::vector<Player> result;if(!actors_.contains(id))return result;
    const auto& own=actors_.at(id).character.at("state");const auto leader=group(id);
    std::vector<std::tuple<bool,int64_t,uint32_t>> players;
    for(auto other:nearby(id,config_.at("aoi")))if(other!=id && actors_.contains(other)){const auto& s=actors_.at(other).character.at("state");players.push_back({!leader || group(other)!=leader,distance(coordinate(own,"x"),coordinate(own,"y"),s.at("x"),s.at("y")),other});}
    std::sort(players.begin(),players.end());if(players.size()>31)players.resize(31);
    result.push_back({id,own.at("x"),own.at("y")});
    for(const auto& [stranger,d,other]:players){(void)stranger;(void)d;const auto& s=actors_.at(other).character.at("state");result.push_back({other,s.at("x"),s.at("y")});}
    std::sort(result.begin(),result.end(),[](const Player& a,const Player& b){return a.id<b.id;});
    return result;
}
Json World::state(uint32_t id) {
    if(!actors_.contains(id))return Json::object();
    auto& a=actors_.at(id);
    // Per-tick catalogs carry only this character's class skills.
    Json skills=Json::object();
    for(const auto& [skill,definition]:config_.at("skills").items())if(definition.at("source")[0].get<int>()+1==a.character.at("class_id").get<int>())skills[skill]=definition;
    // Per-tick item catalogs carry the character's inventory and quest rewards; visible NPCs add their shop/loot below.
    const auto& own=a.character.at("state");Json items=Json::object();
    const auto catalog=[&](Json& into,const std::string& item){if(!into.contains(item))into[item]=config_.at("items").at(item);};
    for(const auto& entry:own.at("inventory"))catalog(items,entry.at("item").get<std::string>());
    // Quest catalog: the active quests here; quests offered by visible NPCs join with their entity below (budgeted).
    Json quests=Json::object();
    for(const auto& [quest_id,progress]:own.at("quests").items()){(void)progress;const auto& quest=config_.at("quests").at(quest_id);quests[quest_id]=quest;for(const auto& v:quest.at("reward").at("items"))catalog(items,v.at("item").get<std::string>());}
    Json result{{"self",id},{"tick",tick_},{"character",a.character},{"entities",Json::array()},{"events",Json::array()},{"catalogs",{{"items",std::move(items)},{"quests",std::move(quests)},{"skills",std::move(skills)}}}};
    if(a.admin)result["admin"]=true; // original privilege%100 gates İsim Yansıt / Yanına Işınla menu items (0041a230)
    const auto& zone=config_.at("zones").at(a.character.at("state").at("zone").get<std::string>());
    result["zone"]=a.character.at("state").at("zone");result["bounds"]=zone.at("bounds");
    result["source_registry_id"]=zone.value("source_registry_id",Json(nullptr));
    if(config_.contains("clock")) {
        // Cyclic linear blend between the configured hourly records; 20 ticks per real second.
        const auto& clock=config_.at("clock");const auto& records=clock.at("records");
        const double hour=clock_hour();
        std::size_t from_index=records.size()-1;for(std::size_t i=0;i<records.size();++i)if(records[i][0].get<double>()<=hour)from_index=i;
        const auto& from=records[from_index];const auto& to=records[(from_index+1)%records.size()];
        const double span=std::fmod(to[0].get<double>()-from[0].get<double>()+24.0,24.0),t=span>0?std::fmod(hour-from[0].get<double>()+24.0,24.0)/span:0;
        Json environment=Json::array();for(std::size_t k=1;k<12;++k)environment.push_back(int(std::lround(from[k].get<double>()+(to[k].get<double>()-from[k].get<double>())*t)));
        result["clock"]={{"hour",hour},{"environment",std::move(environment)},{"fog",clock.at("fog")}};
    }
    result["character"]["equipment"]=visible_equipment(a.character.at("state"));
    // Puan: the original shows cumulative XP over the next cumulative threshold (VOD 1811/3183); state xp is within the level.
    result["xp_interval"]=xp_needed(config_,a.character.at("state").at("level").get<int>());
    result["xp_total"]=xp_before(config_,a.character.at("state").at("level").get<int>());
    result["skill_points"]=std::max(0,unspent_skill_points(config_,a.character.at("state")));
    { // Bağlantılar online status: friends ("dışarıda" = offline in the original list) that are in this world now.
        Json online=Json::array();
        if(own.contains("contacts"))for(const auto& entry:own.at("contacts").at("friends"))
            for(const auto& [other,b]:actors_){(void)other;if(entry[0]==b.character_id){online.push_back(entry[0]);break;}}
        result["contacts_online"]=std::move(online);
    }
    {
        // Karakter Bilgisi values of client 00402f60 (fighter()): Saldırı/Savunma in tenths, Saldırı Hızı in ticks, Zırh tenths/cap.
        const auto& own_state=a.character.at("state");
        const auto g=gear(own_state);const auto f=fighter(own_state,a.character.at("class_id"));
        double elements=0;for(double e:f.element)elements+=e;
        Json efsun=Json::object();for(const auto& [attribute,value]:g.efsun)efsun[std::to_string(attribute)]=int(std::clamp(std::lround(value*100),long(INT32_MIN),long(INT32_MAX)));
        result["stats"]={{"attack",std::lround(f.attack*10)},{"defense",std::lround(f.defense*10)},{"attack_cooldown",std::max(1L,std::lround(f.speed*20))},
            {"weapon_damage",Json::array({std::lround(f.min*f.multiplier),std::lround((f.max+elements)*f.multiplier)})},{"armor",Json::array({std::lround(f.armor*10),std::lround(f.armor_cap)})},
            {"crit",Json::array({std::lround(f.crit*10),std::clamp(std::lround(f.crit_multiplier),1L,10L)})},{"efsun",std::move(efsun)},
            {"charisma",karizma(own_state)}, // guide Karizma (Klan savaşları)
            {"resistance_points",std::max(0,unspent_points(config_,own_state,"resistances"))},{"craft_points",std::max(0,unspent_points(config_,own_state,"crafts"))}};
        // NONCANONICAL ranks (no rank strings in the client): the clan rank by Şan (social.clan_ranks) for the personal panel, the
        // Lonca leader/member for Klan ve Lonca; arena_power = skill bonus % from Er Meydanı areas the own clan holds.
        if (own_state.contains("clan")) for (const auto& row : config_.at("social").at("clan_ranks")) if (own_state.value("san",int64_t(0)) >= row[0].get<int64_t>()) result["stats"]["clan_rank"]=row[1];
        if (own_state.contains("guild") && own_state.at("guild").is_object()) result["stats"]["guild_rank"]=own_state.at("guild").at("leader_character_id")==a.character_id?"Lonca Reisi":"Üye";
        if (const int power=arena_power(own_state)) result["stats"]["arena_power"]=power;
    }
    {
        // Üretim: learned recipes with their product/input names; Banka: contents only while a banker session is open.
        auto& listed=result["catalogs"]["items"]; Json recipes=Json::object();
        for(const auto& r:own.at("recipes")){const auto& recipe=config_.at("recipes").at(r.get<std::string>());recipes[r.get<std::string>()]=recipe;catalog(listed,recipe.at("product").get<std::string>());for(const auto& c:recipe.at("components"))catalog(listed,c.at("item").get<std::string>());}
        result["catalogs"]["recipes"]=std::move(recipes);
        if(a.banker && npcs_.contains(a.banker) && close(id,a.banker,config_.at("interaction_range"))) {
            for(const auto& entry:a.bank.at("items"))catalog(listed,entry.at("item").get<std::string>());
            result["bank"]={{"items",a.bank.at("items")},{"currency",a.bank.at("currency")},{"slots",config_.at("economy").at("bank_slots")},{"banker",a.banker}};
        }
    }
    std::vector<std::pair<int64_t,uint32_t>> visible;
    std::vector<std::tuple<bool,int64_t,uint32_t>> players; // (not a group member, distance, id)
    const auto leader=group(id);
    for(auto other:nearby(id,config_.at("aoi"))) {
        if(other==id)continue;
        if(auto p=actors_.find(other);p!=actors_.end()) { const auto& s=p->second.character.at("state");players.push_back({!leader || group(other)!=leader,distance(coordinate(own,"x"),coordinate(own,"y"),s.at("x"),s.at("y")),other}); }
        else { const auto& n=npcs_.at(other);visible.push_back({distance(coordinate(own,"x"),coordinate(own,"y"),n.x,n.y),other}); }
    }
    std::sort(players.begin(),players.end());
    if(players.size()>31)players.resize(31); // snapshot() lists the same 31 beside self
    std::sort(visible.begin(),visible.end());
    size_t used=result.dump().size();
    // Events are delivered once (the queue is cleared below) while entities are resent every frame, so every queued event
    // (≤32 events, ≤32768 bytes) is reserved before optional entities. Players first: group members always (the
    // constructor bounds five), then nearest-first while they fit. At extreme crowding the farthest players drop out of a
    // frame; NPCs then fill what remains.
    used+=a.event_bytes+a.events.size();
    for(const auto& [stranger,d,other]:players) {
        (void)d;const auto& p=actors_.at(other);const auto& s=p.character.at("state");
        Json e={{"id",other},{"kind","player"},{"name",p.character.at("name")},{"appearance",p.character.at("appearance")},{"class_id",p.character.at("class_id")},{"equipment",visible_equipment(s)},{"x",s.at("x")},{"y",s.at("y")},{"hp",s.at("hp")},{"max_hp",s.at("max_hp")},{"level",s.at("level")},{"alive",s.at("hp").get<int>()>0}};
        if (p.posture) e["posture"]=p.posture;
        // Name colour/menu inputs (original 00471e41, 0041a230): clan, Kabadayı flag and the Lonca line "<%s>".
        e["clan"]=s.value("clan",0);e["kabadayi"]=s.value("kabadayi",false);
        if(!s.at("buffs").empty())e["debuffs"]=shown_effects(s.at("buffs")); // the bearer's effect icons on its target/party frame
        if(s.contains("guild") && !s.at("guild").is_null())e["guild"]=s.at("guild").at("name");
        const size_t bytes=e.dump().size()+1;
        if(stranger && used+bytes>65533)continue;
        used+=bytes;result["entities"].push_back(std::move(e));
    }
    // Nearest NPCs first, each only while it and the catalog items it introduces still fit beside every queued event.
    auto& catalog_items=result["catalogs"]["items"];auto& catalog_quests=result["catalogs"]["quests"];
    for(const auto& [d,other]:visible) {
        (void)d;if(result["entities"].size()>=96)break; // client snapshot entity limit (dense <loc> door/node areas)
        const auto& n=npcs_.at(other);
        const Json offered=quests_at(n,own);
        Json entity={{"id",other},{"kind",n.definition.at("kind")},{"name",n.definition.at("name")},{"appearance",n.definition.value("appearance",Json::object())},{"equipment",Json::array()},{"source_uid",n.definition.at("source_uid")},{"source_class",n.definition.at("source_class")},{"x",n.x},{"y",n.y},{"hp",n.hp},{"max_hp",n.definition.at("hp")},{"alive",n.hp>0},{"quests",offered},{"shop",n.definition.at("shop")},{"owner",n.owner},{"looted",n.looted}};
        if(n.definition.contains("clan"))entity["clan"]=n.definition.at("clan"); // relation 00478090 clan bits (Klan OOK / Sancak)
        if(n.definition.value("recruiter",false))entity["recruiter"]=true; // entity flag 0x20000: Klana Katıl / Klandan Ayrıl
        if(n.definition.value("no_buy",false))entity["no_buy"]=true; // entity flag +0x82&0x40: "Bu hedef, eşya satın almaz." (004344b0)
        if(n.master)entity["master"]=n.master; // summon: the caster it fights for
        if(n.definition.value("banker",false))entity["banker"]=true; // original entity flag +0x81&0x10: İşlemler "Kasamı göster" (0041a62f)
        if(!n.debuffs.empty())entity["debuffs"]=shown_effects(n.debuffs); // target frame icons
        Json added=Json::object();size_t bytes=entity.dump().size()+1;
        const auto add=[&](const std::string& item){if(catalog_items.contains(item) || added.contains(item))return;catalog(added,item);bytes+=Json(item).dump().size()+added.at(item).dump().size()+2;};
        for(const auto& item:n.definition.at("shop"))add(item.get<std::string>());
        for(const auto& v:n.definition.value("loot",Json::array()))add(v.at("item").get<std::string>());
        if(n.drop.is_array())for(const auto& v:n.drop)if(v.is_object() && v.contains("item"))add(v.at("item").get<std::string>()); // rolled Ganimet slot names/icons
        if(const auto g=n.definition.find("gather");g!=n.definition.end())for(const char* key:{"item","rare"})if(g->contains(key))add(g->at(key).get<std::string>()); // Toplandı notice names
        Json added_quests=Json::object();
        for(const auto& quest:offered){const auto& q=quest.get_ref<const std::string&>();if(catalog_quests.contains(q) || added_quests.contains(q))continue;const auto& definition=config_.at("quests").at(q);added_quests[q]=definition;bytes+=Json(q).dump().size()+definition.dump().size()+2;for(const auto& v:definition.at("reward").at("items"))add(v.at("item").get<std::string>());}
        if(used+bytes>65533)continue;
        used+=bytes;result["entities"].push_back(std::move(entity));catalog_items.update(added);catalog_quests.update(added_quests);
    }
    const auto baseline=result.dump().size();
    if(baseline>65533)throw std::logic_error("validated state schema exceeds protocol envelope");
    size_t available=65533-baseline,selected=0;
    std::array<bool,32> keep{};
    // Superseded terms are coalesced on enqueue. Preserve every current consent/kick/terminal event;
    // only older noncritical notices may be omitted to meet the exact remaining frame budget.
    for(int pass=0;pass<2;++pass)for(size_t index=a.events.size();index-- > 0;) {
        if(critical_event(a.events[index])!=(pass==0))continue;
        const auto bytes=a.event_sizes[index]+(selected?1:0);
        if(bytes<=available){keep[index]=true;available-=bytes;++selected;}
        else if(pass==0)throw std::logic_error("validated critical event schema exceeds protocol envelope");
    }
    for(size_t index=0;index<a.events.size();++index)if(keep[index])result["events"].push_back(std::move(a.events[index]));
    a.events=Json::array();a.event_bytes=0;return result;
}
Json World::status() const {
    std::map<std::string,std::array<int,3>> counts; // zone: players, bots, living NPCs
    Json online=Json::array();
    for(const auto& [id,a]:actors_){const auto& s=a.character.at("state");++counts[s.at("zone").get<std::string>()][a.bot?1:0];
        if(a.bot)continue; // bots() lists them
        online.push_back({{"id",id},{"character_id",a.character_id},{"account_id",a.account_id},{"name",a.character.at("name")},{"zone",s.at("zone")},{"x",s.at("x")},{"y",s.at("y")},
                          {"level",s.at("level")},{"hp",s.at("hp")},{"max_hp",s.at("max_hp")},{"admin",a.admin},{"muted",a.muted||a.externally_muted}});}
    for(const auto& [id,n]:npcs_){(void)id;if(n.hp>0&&!n.master)++counts[n.definition.at("zone").get<std::string>()][2];}
    Json zones=Json::array();
    for(const auto& [name,z]:config_.at("zones").items()){const auto& c=counts[name];zones.push_back({{"zone",name},{"players",c[0]},{"bots",c[1]},{"npcs",c[2]},{"disabled",disabled_zones_.contains(z.value("instance_of",name))}});}
    Json result{{"tick",tick_},{"tick_hz",20},{"players",online.size()},{"npcs",npcs_.size()},{"zones",zones},{"online",online},{"bots",bots()},{"trades",trades_.size()},{"groups",groups_.size()},{"logs",logs_},{"provenance",config_.at("provenance")},
                {"rates",{{"xp_rate",xp_rate_},{"drop_rate",drop_rate_},{"regen_rate",regen_rate_}}}};
    if(config_.contains("clock"))result["clock_hour"]=clock_hour();
    return result;
}
double World::clock_hour() const {
    const auto& clock=config_.at("clock");
    return std::fmod(clock.at("start_hour").get<double>()+clock_offset_+tick_/72000.0*clock.at("rate").get<double>(),24.0);
}
void World::kick(uint32_t id, uint32_t by) { auto& a=actor(id); a.kicked=true; a.dx=a.dy=0; a.moving=false; cancel_trade(id); emit(id,{{"type","kick"},{"by",by}}); }
// Private web administration (NONCANONICAL: the original GM tools were in-game chat commands such as "|tele").
// Moves go through transfer(), so collision, zone switches and persistence match player-initiated travel.
Json World::admin(const std::string& command, const Json& arguments) {
    try {
        const auto online=[&](const char* key) {
            const auto character=arguments.at(key).get<int64_t>();
            for(const auto& [id,a]:actors_)if(a.character_id==character&&!a.kicked)return id;
            throw Rejected{"offline","Character is not online in this world"};
        };
        if(command=="announce") {
            const auto message=text(arguments.at("message"),256);
            for(const auto& [id,a]:actors_){(void)a;emit(id,{{"type","announce"},{"text",message}});}
            return {{"ok",true},{"recipients",actors_.size()}};
        }
        if(command=="kick"){const auto id=online("character_id");kick(id,0);return {{"ok",true},{"player",id}};}
        if(command=="teleport"||command=="summon") {
            const auto id=online("character_id");std::string zone;int32_t x=0,y=0;
            if(command=="summon") {
                const auto to=online("to_character_id");require(to!=id,"target","Cannot summon a character to itself");
                const auto& s=actors_.at(to).character.at("state");zone=s.at("zone");x=s.at("x");y=s.at("y");
            } else {
                zone=text(arguments.at("zone"),32);require(config_.at("zones").contains(zone),"zone","Unknown zone");
                const auto& spawn=config_.at("zones").at(zone).at("spawn");
                x=arguments.contains("x")?arguments.at("x").get<int32_t>():spawn[0].get<int32_t>();y=arguments.contains("y")?arguments.at("y").get<int32_t>():spawn[1].get<int32_t>();
                require(passable(zone,x,y),"collision","Destination is not walkable");
            }
            transfer(id,zone,x,y);return {{"ok",true},{"player",id},{"zone",zone},{"x",x},{"y",y}};
        }
        if(command=="clock") {
            require(config_.contains("clock"),"clock","This gameplay configuration has no day clock");
            clock_offset_=std::fmod(clock_offset_+integer(arguments.at("hour"),0,23)-clock_hour()+48.0,24.0);
            return {{"ok",true},{"clock_hour",clock_hour()}};
        }
        if(command=="respawn") {
            const auto zone=arguments.contains("zone")?text(arguments.at("zone"),32):std::string();int count=0;
            require(zone.empty()||config_.at("zones").contains(zone),"zone","Unknown zone");
            for(auto& [id,n]:npcs_){(void)id;if(!n.master&&n.hp==0&&(zone.empty()||n.definition.at("zone")==zone)){n.respawn_at=tick_;++count;}}
            return {{"ok",true},{"respawned",count}};
        }
        if(command=="settings") {
            const int xp=integer(arguments.at("xp_rate"),10,1000),drop=integer(arguments.at("drop_rate"),10,1000),regen=integer(arguments.at("regen_rate"),10,1000);
            require(arguments.at("disabled_zones").is_array()&&arguments.at("disabled_zones").size()<=128,"settings","disabled_zones must be an array");
            std::set<std::string> zones;Json unknown=Json::array();
            for(const auto& zone:arguments.at("disabled_zones")){auto name=text(zone,32);if(config_.at("zones").contains(name))zones.insert(name);else unknown.push_back(name);}
            xp_rate_=xp;drop_rate_=drop;regen_rate_=regen;disabled_zones_=std::move(zones);
            return {{"ok",true},{"unknown_zones",unknown}};
        }
        throw Rejected{"command","Unknown world command"};
    } catch (const Rejected& e) { throw std::invalid_argument(e.code+": "+e.message); }
}
// Store::save_characters takes at most 32 characters per transaction: players (ids below the bot range) fill the first batch.
void World::save_all() { std::vector<CharacterSave> saves;saves.reserve(actors_.size());for(const auto& [id,a]:actors_){(void)id;saves.push_back({a.account_id,a.character_id,a.character.at("state")});persisted_visual(saves.back().state);}for(size_t at=0;at<saves.size();at+=32)persist_(std::span(saves).subspan(at,std::min<size_t>(32,saves.size()-at))); }
}
namespace ikv {
// ---- Üretim (row 24), Toplama (row 25), Banka (row 35) ----
// Professions are the Karakter Bilgisi Üretim/Toplama points; a recipe or node needs that many points in its
// profession (VOD 06:30 "Buradan toplayabilmek için 7 seviye Lokman olmanız gerekir"). NONCANONICAL: crafting always
// succeeds and professions only rise by spending points (the original success/skill-up rules were server-side).
void World::economy(uint32_t id, const std::string& action, const Json& request) {
    auto& a = actor(id); auto s = a.character.at("state"); const auto& economy = config_.at("economy");
    const auto ack = [&]() { emit(id,{{"type","ack"},{"action",action}}); };
    const auto profession = [&](const std::string& craft, int level) {
        if (s.at("crafts").value(craft,0) >= level) return;
        const auto index = std::find(craft_stats.begin(),craft_stats.end(),std::string_view(craft)) - craft_stats.begin();
        throw Rejected{"level",(action == "gather" ? "Buradan toplayabilmek için " : "Bu reçeteyi üretebilmek için ")+std::to_string(level)+" seviye "+craft_names[size_t(index)]+" olmanız gerekir"};
    };
    if (action == "use") { // Kullan on a recipe scroll: the scroll is consumed and the recipe memorised.
        const auto uid = text(request.at("uid"),32); const auto recipe = owned(s,uid).at("item").get<std::string>(); auto& known = s["recipes"];
        require(std::find(known.begin(),known.end(),Json(recipe)) == known.end(),"recipe","Bu reçeteyi zaten biliyorsunuz");
        require(known.size() < economy.at("recipe_limit").get<size_t>(),"recipe","Reçete hafızanız dolu");
        take(s,uid,1); known.push_back(recipe); commit({{id,s}}); ack(); return;
    }
    if (action == "recipe_forget" || action == "craft") {
        const auto recipe = text(request.at("recipe"),32); auto& known = s["recipes"]; const auto found = std::find(known.begin(),known.end(),Json(recipe));
        require(found != known.end(),"recipe","Üretebileceğiniz birşey seçili değil"); // original 0x592914 text
        if (action == "recipe_forget") { known.erase(found); commit({{id,s}}); ack(); return; }
        const auto& r = config_.at("recipes").at(recipe); profession(r.at("craft").get<std::string>(),r.at("level").get<int>());
        for (const auto& c : r.at("components")) { // stacks of the input item, never equipped ones
            int need = c.at("quantity"); const auto item = c.at("item").get<std::string>();
            std::vector<std::pair<std::string,int>> stacks;
            for (const auto& entry : s.at("inventory")) if (entry.at("item") == item && need > 0) { const int n = std::min(need,entry.at("quantity").get<int>()); stacks.push_back({entry.at("uid").get<std::string>(),n}); need -= n; }
            require(need == 0,"components","Reçetedeki malzemeler eksik");
            for (const auto& [uid,n] : stacks) take(s,uid,n);
        }
        give(s,r.at("product").get<std::string>(),r.at("quantity").get<int>()); commit({{id,s}});
        emit(id,{{"type","craft"},{"entity",id},{"recipe",recipe},{"item",r.at("product")},{"quantity",r.at("quantity")}}); return;
    }
    if (action == "gather") {
        const auto t = target_id(request.at("target")); const auto found = npcs_.find(t);
        require(found != npcs_.end() && found->second.definition.at("kind") == "node","target","Toplanacak bir kaynak seçin");
        require(found->second.hp > 0,"exhausted","Bu kaynak şimdilik tükendi");
        const auto& g = npc(id,t,"node").definition.at("gather"); profession(g.at("craft").get<std::string>(),g.at("level").get<int>());
        a.gathering = t; a.gather_ready = tick_ + economy.at("gather_ticks").get<uint32_t>();
        a.moving = false; a.route.clear(); a.dx = a.dy = 0; a.engaged = 0; a.looting = false;
        project_event(id,{{"type","gather"},{"phase","start"},{"entity",id},{"source",id},{"target",t}}); return;
    }
    if (action == "bank_open") {
        const auto t = target_id(request.at("target")); require(npc(id,t).definition.value("banker",false),"target","Bu kişi bankacı değil");
        a.banker = t; ack(); return;
    }
    if (action == "bank_close") { a.banker = 0; ack(); return; }
    require(a.banker && npcs_.contains(a.banker) && close(id,a.banker,config_.at("interaction_range")),"bank","Banka kapalı: bir bankacıyla konuşun");
    // Every bank move builds the new state and bank together and commits them in one row update (Store::save_characters);
    // the cached copies change only after that succeeds, so a repeated or failed request can never duplicate anything.
    Json bank = a.bank; auto& vault = bank["items"];
    const auto plain = [](const Json& entry) { return entry.size() == 3; }; // {uid,item,quantity}: no durability/prefix instance data
    if (action == "bank_deposit") {
        const auto uid = text(request.at("uid"),32); const int quantity = integer(request.at("quantity"),1,1000);
        for (const auto& [slot,worn] : s.at("equipment").items()) { (void)slot; require(worn != uid,"equipped","Giyilen bir eşyayı çıkarmadan bankaya koyamazsınız!"); } // 0x591a8c
        Json entry = owned(s,uid); const auto item = entry.at("item").get<std::string>(); const int stack = config_.at("items").at(item).at("stack"), had = entry.at("quantity");
        take(s,uid,quantity); int left = quantity;
        if (plain(entry)) for (auto& slot : vault) if (plain(slot) && slot.at("item") == item && left) { const int add = std::min(left,stack-slot.at("quantity").get<int>()); slot["quantity"] = slot.at("quantity").get<int>()+add; left -= add; }
        if (left) { // a whole, unstacked entry keeps its uid; split remainders get a fresh one
            require(vault.size() < economy.at("bank_slots").get<size_t>(),"bank","Banka dolu");
            if (left != had) entry["uid"] = new_uid();
            entry["quantity"] = left; vault.push_back(std::move(entry));
        }
    } else if (action == "bank_withdraw") {
        const auto uid = text(request.at("uid"),32); const int quantity = integer(request.at("quantity"),1,1000);
        const auto found = std::find_if(vault.begin(),vault.end(),[&](const Json& entry){ return entry.at("uid") == uid; });
        require(found != vault.end(),"ownership","Eşya bankanızda değil"); const int count = found->at("quantity"); require(count >= quantity,"quantity","Yeterli eşyanız yok.");
        if (plain(*found)) give(s,found->at("item").get<std::string>(),quantity);
        else { require(quantity == count,"quantity","Bu eşya bölünemez; tamamını taşıyın."); require(s.at("inventory").size() < config_.at("inventory_slots").get<size_t>(),"inventory","Çantanızda yer yok!!"); s["inventory"].push_back(*found); }
        if (count == quantity) vault.erase(found); else (*found)["quantity"] = count-quantity;
    } else if (action == "bank_currency") { // NONCANONICAL: Help_Bank shows no money field; positive deposits, negative withdraws.
        const int amount = integer(request.at("amount"),-config_.at("currency_cap").get<int>(),config_.at("currency_cap").get<int>()); require(amount != 0,"request","Miktar girmelisiniz.");
        currency(s,-amount); const int64_t next = bank.at("currency").get<int64_t>()+amount;
        require(next >= 0,"currency","Bankada yeterli paranız yok."); require(next <= config_.at("currency_cap").get<int64_t>(),"currency","Bankada en fazla 999 milyon paranız olabilir."); bank["currency"] = next;
    } else if (action == "bank_destroy") { // a bank cell dropped outside every window: the 004344b0 destroy confirm, 'spro'
        const auto uid = text(request.at("uid"),32); const auto found = std::find_if(vault.begin(),vault.end(),[&](const Json& entry){ return entry.at("uid") == uid; });
        require(found != vault.end(),"ownership","Eşya bankanızda değil");
        require(!(config_.at("items").at(found->at("item").get<std::string>()).value("flags",0) & 2),"destroy","Bu eşya yok edilemez.");
        vault.erase(found);
    } else throw Rejected{"request","Bilinmeyen banka işlemi."};
    commit({{id,s}},&bank); ack();
}
void World::gather_tick(uint32_t id, Actor& a) {
    const auto t = a.gathering; const auto found = npcs_.find(t);
    const auto stop = [&](const char* phase) { a.gathering = 0; project_event(id,{{"type","gather"},{"phase",phase},{"entity",id},{"source",id},{"target",t}}); };
    if (a.moving || a.dx || a.dy || a.engaged || found == npcs_.end() || found->second.hp <= 0 || !close(id,t,config_.at("interaction_range"))) { stop("cancel"); return; }
    if (tick_ < a.gather_ready) return;
    const auto& g = found->second.definition.at("gather"); auto s = a.character.at("state"); Json gained = Json::array({g.at("item")});
    try {
        give(s,g.at("item").get<std::string>(),1);
        // NONCANONICAL: the paired rare material (e.g. Bakır -> Saf Bakır) at gather_rare_percent; the original odds were server-side.
        unsigned int roll = 0;
        if (g.contains("rare") && RAND_bytes(reinterpret_cast<unsigned char*>(&roll),sizeof(roll)) == 1 && roll % 100 < config_.at("economy").at("gather_rare_percent").get<unsigned>()) { give(s,g.at("rare").get<std::string>(),1); gained.push_back(g.at("rare")); }
        commit({{id,s}});
    } catch (const Rejected& e) { reject(id,e.code,e.message); stop("cancel"); return; }
      catch (const std::exception&) { reject(id,"persistence","Toplama kaydedilemedi."); stop("cancel"); return; }
    found->second.hp = 0; found->second.respawn_at = tick_ + g.at("respawn").get<uint32_t>(); // shared nodes exhaust for everyone
    a.gathering = 0; project_event(id,{{"type","gather"},{"phase","done"},{"entity",id},{"source",id},{"target",t},{"items",gained}});
}
}
namespace ikv {
// ---- Quests (rows 7, 18). VOD 08:16-52:30 chat (all yellow system lines): "Yeni bir görev aldınız.", "<ad> [<seviye>] : (n/10)"
// (also a red floater over the player, 24:30), "... görevini teslim edebilirsiniz!", "... görevini bitirdiniz!" followed by
// the NPC's completion text, "... görevinin süresi doldu!". Objectives, texts, timers and rewards were server data: the
// generated quests are NONCANONICAL. Auto quests ("otomatik gelen görevler", exe 0x592f88) follow their prerequisite at
// once; a deleted one is offered again only by the auto_quests NPC (GB-Anı in Mısır Çarşısı).
bool World::quest_done(const Json& s, int source_quest_id) const {
    for (const auto& [id, q] : config_.at("quests").items()) if (q.value("source_quest_id", -1) == source_quest_id && quest_finished(s, id)) return true;
    return false;
}
Json World::quests_at(const Npc& n, const Json& s) const {
    if (!n.definition.value("auto_quests", false)) return n.definition.at("quests");
    Json list = Json::array();
    for (const auto& [id, q] : config_.at("quests").items())
        if (q.value("auto", false) && !s.at("quests").contains(id) && !quest_finished(s, id) && quest_finished(s, q.at("requires").get<std::string>()) &&
            q.at("level").get<int>() <= s.at("level").get<int>() && list.size() < 16) list.push_back(id);
    return list;
}
Json World::quest_event(const std::string& quest, const char* stage, int progress) const {
    const auto& q = config_.at("quests").at(quest);
    Json e{{"type","quest"},{"quest",quest},{"stage",stage},{"name",q.at("name")},{"level",q.at("level")},{"progress",progress},{"count",quest_count(q)}};
    if (std::string_view(stage) == "completed" && q.contains("complete_text")) e["text"] = q.at("complete_text");
    return e;
}
void World::quest_kill(Json& s, const Json& enemy, Json& events) {
    for (auto& [quest, p] : s.at("quests").items()) {
        const auto& q = config_.at("quests").at(quest); const auto& objective = q.at("objective");
        if (objective.at("kind") == "talk" || objective.at("target") != enemy.value("template", Json())) continue;
        const int count = quest_count(q), before = p.at("progress");
        int now = std::min(before + 1, count);
        if (objective.at("kind") == "collect") { // the quest item drops straight into the bag while the quest is active
            try { give(s, objective.at("item").get<std::string>(), 1); } catch (const Rejected&) { continue; } // full bag: no drop
            now = std::min(carried(s, objective.at("item")), count);
        }
        if (now == before) continue;
        p["progress"] = now; events.push_back(quest_event(quest, "progress", now));
        if (now == count) events.push_back(quest_event(quest, "ready", now));
    }
}
void World::quest_finish(Json& s, const std::string& quest, Json& events) {
    const auto& q = config_.at("quests").at(quest); const auto& objective = q.at("objective");
    if (objective.at("kind") == "collect") { // hand the collected items over
        int left = quest_count(q);
        require(carried(s, objective.at("item")) >= left, "quest", "Görev eşyaları eksik.");
        for (size_t i = s.at("inventory").size(); i-- > 0 && left;) {
            const auto entry = s.at("inventory")[i];
            if (entry.at("item") != objective.at("item")) continue;
            const int n = std::min(left, entry.at("quantity").get<int>()); take(s, entry.at("uid"), n); left -= n;
        }
    }
    int reward_xp = q.at("reward").at("xp"); if (s.at("level").get<int>() > q.at("level").get<int>() + 5) reward_xp /= 2;
    xp(s, reward_xp); currency(s, q.at("reward").at("currency"));
    for (const auto& item : q.at("reward").at("items")) give(s, item.at("item"), item.at("quantity"));
    s["quests"].erase(quest);
    auto& done = s["quests_done"]; done.insert(std::lower_bound(done.begin(), done.end(), Json(quest)), quest);
    events.push_back(quest_event(quest, "completed", quest_count(q)));
    for (const auto& [id, next] : config_.at("quests").items()) // auto follow-ups arrive at once (VOD 50:25)
        if (next.value("auto", false) && next.at("requires") == quest && !s.at("quests").contains(id) && !quest_finished(s, id) && s.at("quests").size() < quest_active_max) {
            Json p{{"progress", next.at("objective").at("kind") == "talk" ? 1 : 0}};
            if (next.contains("time_limit")) p["deadline"] = tick_ + 20 * next.at("time_limit").get<uint32_t>();
            s["quests"][id] = std::move(p); events.push_back(quest_event(id, "accepted"));
        }
}
void World::quest_expiry(uint32_t id, Actor& a) {
    const auto& current = a.character.at("state"); Json s, events = Json::array();
    for (const auto& [quest, p] : current.at("quests").items())
        if (p.contains("deadline") && p.at("deadline").get<uint32_t>() <= tick_) {
            if (s.is_null()) s = current;
            s["quests"].erase(quest); events.push_back(quest_event(quest, "expired"));
        }
    if (s.is_null()) return;
    try { commit({{id, s}}); } catch (const std::exception&) { return; } // retried next second
    for (auto& e : events) emit(id, std::move(e));
}
// ---- Group zones (Grup bölgeleri) and Er Meydanı. Rules from the original guide (original/app/ikv_oyun_rehberi.pdf):
// "Er meydanı, karşılıklı klan gruplarının birbirleri ile çarpıştığı bir savaş alanıdır ... Karşı klandan oyuncu veya muhafızları
// öldürmek ve bayrakları ele geçirmek puan kazandırır. Süre bitiminde en çok puan yapan taraf Er Meydanını kazanır." /em + /savas N:
// no Klan Parası, one winner drawn by lot receives an Efsun item; /kpem + /kpsavas N: the losers' Klan Parası stakes go to the
// winners in proportion to their own stakes. Arena zones are registry rows savas1..savas20 ("Er Meydanı 1-20", exe 0x58ffc4..).
// NONCANONICAL: durations, points, the default stake, a draw refunding stakes, the prize pool and the Şef lockout.
namespace {
std::string scoreline(const Json& clans, const std::map<int,int64_t>& score) {
    std::string line;
    for (const auto& [clan,name] : clans.items()) { const auto found = score.find(std::stoi(clan)); line += (line.empty() ? "" : " - ") + name.get<std::string>() + " " + std::to_string(found == score.end() ? 0 : found->second); }
    return line;
}
}
int World::karizma(const Json& s) const {
    const int full = config_.at("character_points").at("charisma"), k = s.value("karizma", full);
    return k == 0 && std::time(nullptr) >= s.value("karizma_reset", int64_t(0)) ? full : k;
}
int World::arena_power(const Json& s) const {
    const auto found = arenas_.find(s.at("zone").get<std::string>()); if (found == arenas_.end() || !s.value("clan",0)) return 0;
    int power = 0; const auto& areas = config_.at("activities").at("arena").at("areas");
    for (size_t i = 0; i < found->second.holds.size(); ++i) if (found->second.holds[i].owner == s.value("clan",0)) power += areas[i].at("power").get<int>();
    return power;
}
void World::vacated(const std::string& zone, bool any) {
    if (!any && !config_.at("zones").at(zone).contains("instance_of")) return;
    for (const auto& [id,b] : actors_) if (b.character.at("state").at("zone") == zone) return;
    // The next group finds a fresh copy: every monster, Şef, banner and node back home at full health, corpses and loot gone.
    for (auto& [id,n] : npcs_) if (!n.master && n.definition.at("zone") == zone) {
        index(id,zone,n.x,n.y,false); n.x = n.definition.at("x"); n.y = n.definition.at("y"); index(id,zone,n.x,n.y,true);
        n.hp = n.definition.at("hp"); n.owner = n.respawn_at = n.attack_ready = 0; n.skill_ready.clear(); n.threat.clear(); n.route.clear();
        n.waypoint = n.route_waypoint = 0; n.route_planned = n.returning = n.looted = false; n.debuffs = Json::array(); n.drop = nullptr;
    }
}
std::string World::arena_zone(int code) const {
    const std::string base = config_.at("activities").at("arena").at("zone");
    if (code < 1 || size_t(code) > config_.at("zones").at(base).value("instances", Json::array()).size() + 1) return {};
    return code == 1 ? base : base + std::to_string(code - 1);
}
void World::arena_say(const std::string& zone, const std::string& line) {
    Json event{{"type","arena"},{"text",line}};
    if (const auto found = arenas_.find(zone); found != arenas_.end()) {
        const auto left = found->second.ends > tick_ ? (found->second.ends - tick_ + 1199) / 1200 : 0;
        event["text"] = line + " (" + scoreline(config_.at("social").at("clans"), found->second.score) + ", kalan " + std::to_string(left) + " dk)";
    }
    for (const auto& [id,b] : actors_) if (b.character.at("state").at("zone") == zone) emit(id, event);
}
void World::send_home(uint32_t id) {
    const auto& s = actor(id).character.at("state");
    const std::string home = config_.at("activities").at("arena").at("homes").value(s.value("clan",Json(0)).dump(), config_.at("defaults").at("zone").get<std::string>());
    const auto& spawn = config_.at("zones").at(home).at("spawn"); transfer(id, home, spawn[0], spawn[1]);
}
void World::arena(uint32_t id, const std::string& action, const Json& request) {
    auto& a = actor(id); const auto& s = a.character.at("state"); const auto& rules = config_.at("activities").at("arena");
    const std::string base = rules.at("zone"); const auto here = s.at("zone").get<std::string>();
    const bool inside = config_.at("zones").at(here).value("instance_of", here) == base;
    if (action == "arena_list") {
        require(request.at("stake").is_boolean(), "request", "Er Meydanı türü belirtilmeli."); const bool stake = request.at("stake");
        Json list = Json::array(); std::string line = stake ? "Klan Parası Er Meydanları:" : "Er Meydanları:";
        for (int code = 1; !arena_zone(code).empty(); ++code) {
            const auto name = arena_zone(code); const auto found = arenas_.find(name);
            if (found != arenas_.end() && found->second.stake != stake) continue; // the other kind is running there
            int players = 0; for (const auto& [other,b] : actors_) players += b.character.at("state").at("zone") == name;
            list.push_back({{"code",code},{"players",players}});
            line += " " + std::to_string(code) + (found == arenas_.end() ? " (boş)" : " (" + std::to_string(players) + " kişi)");
        }
        emit(id, {{"type","arena"},{"text",line},{"stake",stake},{"arenas",std::move(list)}}); return;
    }
    if (action == "arena_leave") { require(inside, "arena", "Bir Er Meydanında değilsiniz."); send_home(id); emit(id,{{"type","ack"},{"action",action}}); return; }
    require(action == "arena_join" || action == "arena_stake", "request", "Bilinmeyen Er Meydanı işlemi.");
    require(s.at("hp").get<int>() > 0, "dead", "Ölüyken bunu yapamazsınız!");
    require(s.value("clan",0) != 0, "clan", "Er Meydanına girmek için bir klana katılmalısınız.");
    require(!inside, "arena", "Zaten bir Er Meydanındasınız.");
    const int code = integer(request.at("code"), 1, 20); const auto name = arena_zone(code); require(!name.empty(), "arena", "Böyle bir Er Meydanı yok.");
    require(request.at("stake").is_number_integer() && request.at("stake").get<int64_t>() >= 0 && request.at("stake").get<int64_t>() <= 999000000, "request", "Geçersiz katkı.");
    const int64_t stake = request.at("stake");
    auto found = arenas_.find(name);
    require(found == arenas_.end() || found->second.stake == (stake > 0), "arena", stake ? "Bu Er Meydanında Klan Parasız savaş sürüyor (/em)." : "Bu Er Meydanında Klan Parası savaşı sürüyor (/kpem).");
    require(s.value("clan_currency", int64_t(0)) >= stake, "clan_currency", "Bu savaş için yeterli KLAN paranız yok.");
    if (action == "arena_stake") { // /kpsavas: the client confirms (Dikkat) and then sends arena_join with this code and stake
        require(stake > 0, "request", "Geçersiz katkı.");
        emit(id, {{"type","arena_stake"},{"code",code},{"stake",stake},{"text",std::to_string(stake) + " Klan Parası katkıyla Er Meydanı " + std::to_string(code) + " savaşına girilsin mi?"}});
        return;
    }
    // Arrival: the clan's first standing banner (guide: clan members rise at their banners), else the arena spawn.
    Json arrive = config_.at("zones").at(name).at("spawn");
    for (const auto& [bid,b] : npcs_) if (b.hp > 0 && b.definition.value("banner",false) && b.definition.at("zone") == name && b.definition.at("clan") == s.value("clan",0)) { arrive = {b.x,b.y}; break; }
    transfer(id, name, arrive[0], arrive[1]);
    if (found == arenas_.end()) found = arenas_.emplace(name, Arena{stake > 0, tick_ + rules.at("duration").get<uint32_t>(), {}, {}, std::vector<Hold>(rules.at("areas").size())}).first;
    if (stake) { auto next = s; next["clan_currency"] = s.value("clan_currency", int64_t(0)) - stake; commit({{id,next}}); found->second.stakes[id] += stake; }
    arena_say(name, a.character.at("name").get<std::string>() + " Er Meydanı " + std::to_string(code) + " savaşına katıldı.");
    emit(id,{{"type","ack"},{"action",action}});
}
void World::arena_tick() {
    std::vector<std::string> names; for (const auto& [name,match] : arenas_) names.push_back(name);
    for (const auto& name : names) {
        bool occupied = false; for (const auto& [id,b] : actors_) occupied = occupied || b.character.at("state").at("zone") == name;
        if (!occupied) { arenas_.erase(name); vacated(name, true); continue; } // everyone left: the stakes stay forfeited
        if (tick_ >= arenas_.at(name).ends) { arena_end(name); continue; }
        // Capture areas: the only clan with living members inside an area for capture_ticks takes it (a contest or an empty
        // area restarts the count; the holder keeps it until another clan stands there alone).
        const auto& areas = config_.at("activities").at("arena").at("areas");
        for (size_t i = 0; i < areas.size(); ++i) {
            auto& hold = arenas_.at(name).holds[i]; const auto& area = areas[i]; const int64_t r = area.at("radius");
            std::set<int> present;
            for (const auto& [id,b] : actors_) { const auto& s = b.character.at("state");
                if (!b.kicked && s.at("zone") == name && s.at("hp").get<int>() > 0 && distance(area.at("x"),area.at("y"),s.at("x"),s.at("y")) <= r*r) present.insert(s.value("clan",0)); }
            const int alone = present.size() == 1 ? *present.begin() : 0;
            if (!alone || alone == hold.owner) { hold.clan = 0; hold.ticks = 0; continue; }
            if (hold.clan != alone) { hold.clan = alone; hold.ticks = 0; }
            if ((hold.ticks += 20) < config_.at("activities").at("arena").at("capture_ticks").get<uint32_t>()) continue;
            hold.owner = alone; hold.clan = 0; hold.ticks = 0;
            arena_say(name, config_.at("social").at("clans").at(std::to_string(alone)).get<std::string>() + " " + area.at("name").get<std::string>() +
                            " bölgesini ele geçirdi: yetenekleri %" + std::to_string(area.at("power").get<int>()) + " güçlendi.");
        }
    }
}
void World::arena_end(const std::string& name) {
    auto match = std::move(arenas_.extract(name).mapped());
    int winner = 0; int64_t best = -1; bool draw = false;
    for (const auto& [clan,label] : config_.at("social").at("clans").items()) {
        const int c = std::stoi(clan); const int64_t v = match.score.contains(c) ? match.score.at(c) : 0;
        if (v > best) { best = v; winner = c; draw = false; } else if (v == best) draw = true;
    }
    if (draw) winner = 0;
    std::vector<uint32_t> present; for (const auto& [id,b] : actors_) if (b.character.at("state").at("zone") == name) present.push_back(id);
    const auto clan_of = [&](uint32_t id) { return actors_.at(id).character.at("state").value("clan",0); };
    std::map<uint32_t,Json> proposed; std::vector<std::pair<uint32_t,std::string>> notes;
    const auto state_of = [&](uint32_t id) -> Json& { if (!proposed.contains(id)) proposed[id] = actors_.at(id).character.at("state"); return proposed.at(id); };
    if (match.stake) {
        std::vector<uint32_t> winners; int64_t staked = 0, pot = 0;
        for (auto id : present) if (winner && clan_of(id) == winner && match.stakes.contains(id)) { winners.push_back(id); staked += match.stakes.at(id); }
        for (const auto& [id,v] : match.stakes) if (std::find(winners.begin(),winners.end(),id) == winners.end()) pot += v;
        const auto pay = [&](uint32_t id, int64_t amount) {
            auto& st = state_of(id); st["clan_currency"] = std::min<int64_t>(999000000, st.value("clan_currency",int64_t(0)) + amount);
            notes.push_back({id, std::to_string(amount) + " Klan Parası aldınız."});
        };
        if (winners.empty()) { for (auto id : present) if (match.stakes.contains(id)) pay(id, match.stakes.at(id)); } // a draw refunds whoever stayed
        else for (auto id : winners) pay(id, match.stakes.at(id) + pot * match.stakes.at(id) / staked);
    } else if (winner) {
        std::vector<uint32_t> pool; for (auto id : present) if (clan_of(id) == winner) pool.push_back(id);
        if (!pool.empty()) {
            // The lot's Efsun item: a monster-table piece within five levels below the winner (any lower one when none), always prefixed.
            const auto lucky = pool[random_below(uint32_t(pool.size()))]; const int level = actors_.at(lucky).character.at("state").at("level");
            std::set<std::string> near, lower;
            for (const auto& [nid,n] : npcs_) for (const auto& item : n.definition.value("drops", Json::array())) { if (!config_.at("items").at(item.get<std::string>()).contains("slot")) continue; // Efsun pieces only
                const int l = config_.at("items").at(item.get<std::string>()).at("level"); if (l <= level) (l >= level-5 ? near : lower).insert(item.get<std::string>()); }
            const auto& chosen = near.empty() ? lower : near;
            if (auto prize = roll_drop({{"level",level},{"drops",Json(chosen)}}, true); prize.is_object()) {
                auto& st = state_of(lucky); const auto item = prize.at("item").get<std::string>();
                try { give(st, item, 1, prize); notes.push_back({lucky, "Er Meydanı ödülü: " + config_.at("items").at(item).at("name").get<std::string>() + "."}); }
                catch (const Rejected&) { proposed.erase(lucky); notes.push_back({lucky, "Er Meydanı ödülü çantanız dolu olduğu için verilemedi."}); }
            }
        }
    }
    std::string result = "Er Meydanı savaşı bitti: " + (winner ? config_.at("social").at("clans").at(std::to_string(winner)).get<std::string>() + " kazandı" : std::string("berabere")) +
                         " (" + scoreline(config_.at("social").at("clans"), match.score) + ").";
    try { commit(proposed); } catch (const std::exception&) { notes.clear(); result += " Ödüller kaydedilemedi."; }
    for (auto id : present) {
        emit(id, {{"type","arena"},{"text",result},{"winner",winner}});
        for (const auto& [to,note] : notes) if (to == id) emit(id, {{"type","arena"},{"text",note}});
        try { send_home(id); } catch (const std::exception&) { reject(id, "arena", "Klan karargahına dönülemedi."); }
    }
    vacated(name, true);
}
}
