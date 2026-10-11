// Server-side bot players (NONCANONICAL: the original game had no bots). A bot is an ordinary World actor without a network
// peer: every move goes through destination() and every deed through handle(), exactly the requests a client sends, so range,
// cooldown, resource, ownership, door and quest rules bind bots like players. The brain follows mod-playerbots (AzerothCore,
// analysis/porting-wave11/Bots/design.md): a "new rpg" status (town/grind/quest/travel, re-rolled every few minutes by the
// behaviour's weights) selects strategies; each think runs the highest-relevance rule whose trigger fires and whose action
// could be sent (Engine::DoNextAction), falling through refused or useless ones. Social input (whispers, Söyle lines, group
// invitations, trades) becomes delayed reactions; sessions end and resume on a churn schedule. Positions of NPCs, monsters,
// doors and merchants are always read from the world, never hard-coded.
#include "ikv/world.hpp"
#include <openssl/rand.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <random>
#include <stdexcept>

namespace ikv {
namespace {
int64_t distance2(int32_t x, int32_t y, int32_t ox, int32_t oy) { const int64_t dx = int64_t(x)-ox, dy = int64_t(y)-oy; return dx*dx+dy*dy; }
uint32_t roll(uint32_t n) { thread_local std::mt19937 engine{std::random_device{}()}; return n ? std::uniform_int_distribution<uint32_t>(0,n-1)(engine) : 0; }
uint32_t span(uint32_t lo, uint32_t hi) { return lo + roll(hi >= lo ? hi-lo+1 : 1); }
const Json& settings(const Json& config) { static const Json none = Json::object(); return config.contains("bots") ? config.at("bots") : none; }
int setting(const Json& config, const char* key, int fallback) { return settings(config).value(key, fallback); }
// A configured [lo,hi] pair, or the fallback pair.
std::pair<uint32_t,uint32_t> pair_setting(const Json& config, const char* key, uint32_t lo, uint32_t hi) {
    const auto& s = settings(config); if (!s.contains(key)) return {lo,hi};
    return {s.at(key)[0].get<uint32_t>(), s.at(key)[1].get<uint32_t>()};
}
std::string base_zone(const Json& config, const std::string& zone) { const auto& z = config.at("zones").at(zone); return z.value("instance_of", zone); }
int32_t at(const Json& s, const char* key) { return s.at(key).get<int32_t>(); }
std::string uid() {
    std::array<unsigned char,16> bytes{}; if (RAND_bytes(bytes.data(), bytes.size()) != 1) throw std::runtime_error("bot item identifier failed");
    static constexpr char hex[] = "0123456789abcdef"; std::string value(32,'0');
    for (size_t i = 0; i < bytes.size(); ++i) { value[2*i] = hex[bytes[i]>>4]; value[2*i+1] = hex[bytes[i]&15]; }
    return value;
}
bool fits(const Json& item, int level, int class_id, int sex) {
    return item.at("level").get<int>() <= level && (!item.contains("class") || item.at("class").get<int>()+1 == class_id) && (!item.contains("sex") || item.at("sex").get<int>() == sex);
}
constexpr std::array<const char*,7> gear_slots{"weapon","head","body","legs","hands","feet","armor"};
// ponytail: item level first, then Zırh/Silah Hasarı; a stat-weighted score if Efsun prefixes ever matter to bot gear.
int score(const Json& item) { return item.at("level").get<int>()*1000 + item.value("armor",0) + item.value("damage",0)*20; }
std::string lower(std::string text) { for (auto& c : text) c = char(std::tolower(static_cast<unsigned char>(c))); return text; }
bool has_word(const std::string& text, const Json& words) {
    for (const auto& w : words) if (text.find(w.get<std::string>()) != std::string::npos) return true;
    return false;
}
std::string fill(std::string line, const std::string& speaker) {
    if (const auto at = line.find("{name}"); at != std::string::npos) line.replace(at, 6, speaker);
    return line;
}
// Weighted pick from {"name":weight}; exclude names the caller rules out.
std::string weighted(const Json& weights, const std::string& exclude = "") {
    int total = 0; for (const auto& [k,w] : weights.items()) if (k != exclude) total += w.get<int>();
    int r = int(roll(uint32_t(std::max(total,1))));
    for (const auto& [k,w] : weights.items()) if (k != exclude && (r -= w.get<int>()) < 0) return k;
    return weights.begin().key();
}
const Json& profile(const Json& config, const std::string& behaviour) {
    static const Json defaults = {{"town",{{"town",70},{"grind",15},{"quest",15}}},{"hunt",{{"grind",45},{"quest",45},{"travel",10}}},{"companion",{{"town",100}}}};
    const auto& modes = settings(config).contains("modes") ? settings(config).at("modes") : defaults;
    return modes.contains(behaviour) ? modes.at(behaviour) : defaults.at(behaviour);
}
}

// Kit (NONCANONICAL, mod-playerbots random gear by level): the best fitting priced item of every gear slot the bot neither
// wears nor carries, so each bot is dressed for its level at login; the bot itself equips it through `equip`.
static void kit(const Json& config, Json& state, int class_id, int sex) {
    if (!state.contains("inventory") || !state.at("inventory").is_array()) return;
    const int level = state.value("level", 1); auto& inventory = state["inventory"];
    for (const char* slot : gear_slots) {
        if (inventory.size() >= config.at("inventory_slots").get<size_t>()) return;
        if (std::any_of(inventory.begin(),inventory.end(),[&](const Json& e){const auto& items = config.at("items"); const auto key = e.at("item").get<std::string>(); return items.contains(key) && items.at(key).value("slot","") == slot;})) continue;
        std::string best;
        for (const auto& [key,item] : config.at("items").items())
            if (item.value("slot","") == slot && item.at("price").get<int64_t>() > 0 && fits(item,level,class_id,sex) && (best.empty() || score(item) > score(config.at("items").at(best)))) best = key;
        if (!best.empty()) inventory.push_back({{"uid",uid()},{"item",best},{"quantity",1}});
    }
}
// History (NONCANONICAL): a bot that never handed in a quest but is already level L has, like a player of that level, done the
// story below it: every quest of level <= L-3 counts as done, so the chains of its own level band open (Görev al prerequisites)
// instead of only the few chain heads. A bot with any real quest history keeps it.
static void history(const Json& config, Json& state) {
    if (!state.value("quests_done", Json::array()).empty() || !state.value("quests", Json::object()).empty()) return;
    const int level = state.value("level", 1); std::vector<std::string> done;
    for (const auto& [id,q] : config.at("quests").items()) if (q.at("level").get<int>() <= level-3) done.push_back(id);
    std::sort(done.begin(), done.end()); state["quests_done"] = done;
}

uint32_t World::add_bot(int64_t account, int64_t character, const Json& record, const Json& spec) {
    const auto behaviour = spec.value("behaviour", std::string("town"));
    if (behaviour != "town" && behaviour != "hunt" && behaviour != "companion") throw std::invalid_argument("bot: unknown behaviour");
    if (bots_.size() >= size_t(setting(config_,"max",256))) throw std::invalid_argument("bot: bot limit reached");
    // A bot new to its zone (freshly created, or saved elsewhere) enters at a random spot within the town wander radius that a
    // straight walk from the zone spawn reaches, like a player who walked there, so a spawned crowd never stands stacked on the
    // spawn point; a position saved in the zone is resumed. admit() still validates the spot.
    Json placed = record; auto& state = placed["state"]; if (state.is_null()) state = Json::object();
    const auto home = config_.at("defaults").at("zone").get<std::string>();
    std::string zone = state.is_object() ? state.value("zone", home) : home;
    const int level = state.is_object() ? state.value("level", 1) : 1;
    if (spec.contains("zone")) {
        zone = spec.at("zone").get<std::string>();
        if (zone == "auto") { // RandomPlayerbotMgr level brackets: a zone whose monsters fit the level, weighted by how many fit
            const auto suits = [&](const Json& d) { const int l = d.value("level",1); return d.at("kind") == "enemy" && l <= level+2 && l >= level-6 && !d.value("banner",false); };
            std::map<std::string,int> fit; for (const auto& [nid,n] : npcs_) { (void)nid; if (!n.master && suits(n.definition)) ++fit[n.definition.at("zone").get<std::string>()]; }
            std::erase_if(fit,[&](const auto& f){ return config_.at("zones").at(f.first).contains("instance_of"); });
            const std::string saved = state.is_object() ? state.value("zone", std::string()) : std::string();
            if (fit.contains(saved)) zone = saved;
            else if (fit.empty()) zone = home;
            else { Json weights = Json::object(); for (const auto& [z,n] : fit) weights[z] = n; zone = weighted(weights); }
        }
        if (!config_.at("zones").contains(zone) || config_.at("zones").at(zone).contains("instance_of")) throw std::invalid_argument("bot: unknown zone");
    }
    if (state.is_object() && config_.at("zones").contains(zone) && (state.value("zone", home) != zone || (!state.contains("x") && !state.contains("y")))) {
        const auto& spawn = config_.at("zones").at(zone).at("spawn"); const int32_t sx = spawn[0], sy = spawn[1], radius = config_.at("aoi").get<int32_t>()/4;
        state["zone"] = zone; state["x"] = sx; state["y"] = sy;
        for (int attempt = 0; attempt < 16; ++attempt) {
            const int32_t x = sx + int32_t(roll(uint32_t(2*radius+1))) - radius, y = sy + int32_t(roll(uint32_t(2*radius+1))) - radius;
            if (clear_path(zone,sx,sy,x,y)) { state["x"] = x; state["y"] = y; break; }
        }
    }
    if (state.is_object()) { kit(config_, state, placed.value("class_id",1), placed.value("appearance",Json::object()).value("sex",1)); history(config_, state); }
    while (actors_.contains(next_bot_) || npcs_.contains(next_bot_)) ++next_bot_;
    const auto id = next_bot_;
    admit(id, account, character, placed, false, true); ++next_bot_;
    const auto& s = actors_.at(id).character.at("state");
    Bot b; b.spec = spec; b.behaviour = behaviour; b.level_seen = s.at("level");
    const uint32_t think = setting(config_,"think_ticks",10);
    b.next_think = tick_ + 1 + id % think; b.chat_ready = tick_ + 20*roll(setting(config_,"chat_seconds",300)+1);
    b.mode = weighted(profile(config_,behaviour), "travel"); // a fresh session starts where it logged in
    const auto [mode_lo,mode_hi] = pair_setting(config_,"mode_seconds",180,720); b.mode_until = tick_ + 20*span(mode_lo,mode_hi);
    if (settings(config_).contains("session_minutes")) { const auto [lo,hi] = pair_setting(config_,"session_minutes",0,0); b.logout_at = tick_ + 1200*span(lo,hi); }
    bots_.emplace(id, std::move(b)); return id;
}

Json World::bots() const {
    Json rows = Json::array();
    for (const auto& [id,b] : bots_) {
        const auto& a = actors_.at(id); const auto& s = a.character.at("state");
        const char* activity = s.at("hp").get<int>() == 0 ? "dead" : a.engaged ? "fighting" : b.resting ? "resting" : b.errand ? "errand"
                             : b.lost ? "lost" : b.leader ? "following" : a.moving ? "walking" : a.posture ? "sitting" : "idle";
        // "m": kills, steals, quests handed in, vendor visits, zones visited, thinks, idle thinks, worn pieces (one array; status()
        // keeps the rows that fit the store's 64 KiB).
        rows.push_back({{"id",id},{"character_id",a.character_id},{"name",a.character.at("name")},{"class_id",a.character.at("class_id")},{"level",s.at("level")},
                        {"behaviour",b.behaviour},{"mode",b.mode},{"action",b.last},{"zone",s.at("zone")},{"x",s.at("x")},{"y",s.at("y")},{"state",activity},{"hp",s.at("hp")},{"leader",b.leader},
                        {"m",{b.kills,b.steals,b.quests,b.vendor,b.zones.size(),b.thinks,b.idle,s.at("equipment").size()}}});
    }
    return rows;
}
std::vector<std::pair<int64_t,int64_t>> World::released_bots() { return std::exchange(released_bots_, {}); }
Json World::bot_returns() {
    Json due = Json::array();
    for (size_t i = 0; i < bot_returns_.size();) {
        if (bot_returns_[i].first > tick_) { ++i; continue; }
        due.push_back(std::move(bot_returns_[i].second)); bot_returns_.erase(bot_returns_.begin()+long(i));
    }
    return due;
}

Json World::bot_plan(int total) const {
    Json plan = Json::array(); const auto& spawn = settings(config_).value("spawn", Json::array());
    if (total < 0) { for (const auto& entry : spawn) for (int n = 0; n < entry.value("count",0); ++n) plan.push_back(entry); }
    else if (!spawn.empty()) for (int n = 0; n < total; ++n) plan.push_back(spawn[size_t(n) % spawn.size()]);
    for (auto& entry : plan) entry.erase("count");
    return plan;
}

Json World::bot_character(int level) const {
    level = std::clamp(level, 1, 100);
    const auto& names = settings(config_).value("names", Json::object());
    const int sex = int(roll(2)); // protocol valid_appearance: 0 female, 1 male
    const auto pick = [&](const char* key, const char* fallback) { const auto list = names.value(key, Json::array()); return list.empty() ? std::string(fallback) : list[roll(uint32_t(list.size()))].get<std::string>(); };
    auto name = pick(sex ? "male" : "female", sex ? "Ahmet" : "Ayşe");
    if (roll(100) < 75) name = pick("epithets", "Koca") + " " + name;
    const int class_id = 1 + int(roll(3));
    // Original creator tables {female, male}: face, hair, hair colour, beard, beard colour upper bounds.
    static constexpr std::array<std::pair<const char*,std::array<int,2>>,5> cosmetics{{{"face",{9,9}},{"hair",{20,17}},{"hair_color",{9,4}},{"beard",{24,14}},{"beard_color",{1,4}}}};
    Json appearance{{"sex",sex}};
    for (const auto& [key,maximum] : cosmetics) appearance[key] = sex == 0 && std::string_view(key).starts_with("beard") ? 0 : int(roll(uint32_t(maximum[size_t(sex)])+1));
    // NONCANONICAL creation kit: the default bag and Lira scaled by level; add_bot's kit() adds the level's gear.
    const auto cap = config_.at("resource_cap").get<int>();
    Json state{{"level",level},{"hp",cap},{"mana",cap},{"currency",int64_t(100)*level},{"inventory",Json::array()}};
    for (const auto& item : config_.at("defaults").at("inventory")) state["inventory"].push_back({{"uid",uid()},{"item",item.at("item")},{"quantity",item.at("quantity")}});
    return {{"name",name},{"class_id",class_id},{"appearance",appearance},{"state",state}};
}

// ponytail: built once from the configured population; NPCs added at runtime other than summons need an index refresh.
const std::vector<uint32_t>& World::bot_npcs(const std::string& kind, const std::string& zone) {
    if (bot_index_.empty()) {
        bot_index_[""] = {};
        for (const auto& [nid,n] : npcs_) {
            if (n.master) continue;
            const auto& d = n.definition; const std::string z = d.at("zone");
            bot_index_[d.at("kind").get<std::string>() + ":" + z].push_back(nid);
            if (!d.at("quests").empty() || d.value("auto_quests",false)) bot_index_["quest:" + z].push_back(nid);
            if (d.contains("template")) bot_index_["tmpl:" + d.at("template").get<std::string>()].push_back(nid);
        }
    }
    const auto found = bot_index_.find(kind + ":" + zone); return found == bot_index_.end() ? bot_index_.at("") : found->second;
}
// Zones reachable through doors in breadth-first order with the door to take from `from` (0 for `from` itself).
std::vector<std::pair<std::string,uint32_t>> World::bot_zones(const Bot& b, const std::string& from) {
    std::vector<std::pair<std::string,uint32_t>> order{{from,0}}; std::set<std::string> seen{base_zone(config_,from)};
    for (size_t at = 0; at < order.size(); ++at) {
        const auto [zone,first] = order[at];
        for (auto door : bot_npcs("door",zone)) {
            if (b.blocked.contains(door)) continue;
            const auto to = base_zone(config_,npcs_.at(door).definition.at("door").at("to").get<std::string>());
            if (seen.insert(to).second) order.emplace_back(to, first ? first : door);
        }
    }
    return order;
}
const World::BotCensus& World::bot_census() {
    if (census_.tick == tick_) return census_;
    census_ = BotCensus{}; census_.tick = tick_;
    for (const auto& [id,a] : actors_) {
        if (a.bot) { if (const auto b = bots_.find(id); b != bots_.end() && !b->second.ground.empty()) ++census_.grinders[b->second.ground]; continue; }
        const auto& s = a.character.at("state"); const std::string zone = s.at("zone");
        census_.players[zone].push_back(id);
        for (const auto& [quest,p] : s.at("quests").items()) {
            (void)p; if (!config_.at("quests").contains(quest)) continue;
            if (const auto t = config_.at("quests").at(quest).at("objective").value("target",Json()); t.is_string()) census_.targets[zone].insert(t.get<std::string>());
        }
    }
    return census_;
}

bool World::bot_act(uint32_t id, const Json& request) {
    try { handle(id, request); return true; } catch (...) { return false; } // refused like a client request (rule, range, cooldown, store)
}
bool World::player_claim(uint32_t bot, uint32_t npc) {
    const auto n = npcs_.find(npc); if (n == npcs_.end()) return false;
    const auto team = group(bot);
    const auto rival = [&](uint32_t who) { const auto p = actors_.find(who); return who != bot && p != actors_.end() && !p->second.bot && (!team || group(who) != team); };
    if (n->second.owner && rival(n->second.owner)) return true;
    for (const auto& [who,threat] : n->second.threat) { (void)threat; if (rival(who)) return true; }
    const auto& players = bot_census().players; const auto here = players.find(n->second.definition.at("zone").get<std::string>());
    if (here != players.end()) for (auto who : here->second) if (rival(who) && actors_.at(who).engaged == npc) return true;
    return false;
}
// A configured line (bots.lines[key]) as the request's text, sent after a human reaction delay (typing time with its length).
void World::bot_say(Bot& b, const char* key, Json request, const std::string& speaker) {
    const auto& lines = settings(config_).value("lines", Json::object()); if (!lines.contains(key) || lines.at(key).empty() || b.pending.size() >= 6) return;
    const auto& list = lines.at(key); request["action"] = "chat"; request["text"] = fill(list[roll(uint32_t(list.size()))].get<std::string>(), speaker);
    const auto due = std::max(tick_, b.pending.empty() ? tick_ : b.pending.back().due) + span(20,80) + uint32_t(request.at("text").get<std::string>().size());
    b.pending.push_back({due, std::move(request), 0});
}
// Walk toward (x,y) in straight legs of at most aoi/4 that clear_path() accepts, fanning out ±30°/60°/90° around the direct
// heading, like a player clicking visible ground. Only after four thinks without progress (legs circling an obstacle) does the
// bot plan with A*: one bot search in flight world-wide, advanced bots.route_nodes cells per tick (bots_tick) up to 12 slices,
// collected on a later think. A search that runs out returns the stretch to the explored cell nearest the goal and the next
// stretch is planned on arrival (incremental planning); false when no stretch gains ground.
bool World::bot_walk(uint32_t id, Actor& a, Bot& b, int32_t x, int32_t y) {
    const auto& s = a.character.at("state"); const std::string zone = s.at("zone"); const int32_t sx = at(s,"x"), sy = at(s,"y");
    const double dist = std::sqrt(double(distance2(sx,sy,x,y))), leg = std::min(dist, config_.at("aoi").get<double>()/4);
    if (std::abs(int64_t(x)-b.goal_x)+std::abs(int64_t(y)-b.goal_y) > config_.at("aoi").get<int64_t>()/8) { b.goal_x = x; b.goal_y = y; b.goal_best = INT64_MAX; b.stalls = 0; b.routed = b.partial = false; }
    if (b.routed && a.moving) return true; // following the searched route
    b.routed = false;
    if (const auto d = distance2(sx,sy,x,y); d < b.goal_best - b.goal_best/10) { b.goal_best = d; b.stalls = 0; } else ++b.stalls;
    const double progress = std::max(dist - leg/2, config_.at("interaction_range").get<double>()/2);
    if (a.moving && !a.route.empty() && double(distance2(a.route.back().first,a.route.back().second,x,y)) <= progress*progress) return true;
    if (dist < 1) return false;
    const auto follow = [&](std::vector<std::pair<int32_t,int32_t>> points) {
        if (points.empty()) return false;
        b.partial = points.back() != std::pair{x,y};
        a.route = std::move(points); a.waypoint = 0; a.moving = true; a.dx = a.dy = 0; a.engaged = 0; b.routed = true; // as destination()
        return true;
    };
    if (bot_search_owner_ == id) { // this bot's search: wait while bots_tick advances it, then take its route
        const bool same = bot_search_.zone == zone && std::abs(int64_t(bot_search_.nx)-x)+std::abs(int64_t(bot_search_.ny)-y) <= config_.at("aoi").get<int64_t>()/8;
        if (same && !bot_search_.over) { bot_search_tick_ = tick_; return true; }
        bot_search_owner_ = 0;
        if (same) { auto points = route_finish(bot_search_,true,200); bot_search_ = RouteSearch{}; return follow(std::move(points)); } // 300-unit stretches
        bot_search_ = RouteSearch{};
    }
    if ((b.stalls >= 4 || b.partial) && !bot_search_owner_) { // else another bot's search is in flight: walk legs, ask again next think
        b.stalls = 0; b.partial = false;
        if (auto trivial = route_begin(bot_search_,zone,sx,sy,x,y)) return follow(std::move(*trivial));
        bot_search_owner_ = id; bot_search_tick_ = tick_;
        return true;
    }
    const double heading = std::atan2(double(y-sy), double(x-sx));
    for (double turn : {0.0, .52, -.52, 1.05, -1.05, 1.57, -1.57}) {
        const int32_t tx = turn == 0 && leg == dist ? x : int32_t(sx + leg*std::cos(heading+turn)), ty = turn == 0 && leg == dist ? y : int32_t(sy + leg*std::sin(heading+turn));
        if (passable(zone,tx,ty) && clear_path(zone,sx,sy,tx,ty)) { destination(id,tx,ty,a.sequence+1); if (a.moving) return true; }
    }
    return false;
}
bool World::bot_reach(uint32_t id, Actor& a, Bot& b, uint32_t target, Json request) {
    const auto found = npcs_.find(target); const auto& s = a.character.at("state");
    if (found == npcs_.end()) { b.errand = 0; return false; }
    const std::string zone = s.at("zone"), there = found->second.definition.at("zone");
    const auto give_up = [&] {
        b.errand = 0; b.stuck = 0; b.quest_ready = tick_ + 600; b.blocked.insert(target); b.unblock = tick_ + 1200; return false; };
    if (there != zone) return base_zone(config_,there) != base_zone(config_,zone) && bot_travel(id,a,b,there) ? true : give_up(); // the next door toward it
    if (close(id,target,config_.at("interaction_range"))) {
        b.errand = 0; b.stuck = 0; bool ok = true;
        const std::string action = request.at("action");
        if (action == "bot_shop") { // sell the junk, top up potions, buy a gear upgrade it can afford
            auto own = s; const auto& shop = found->second.definition.at("shop"); bool dealt = false;
            const int level = own.at("level"), class_id = a.character.at("class_id"), sex = a.character.at("appearance").value("sex",1);
            for (const auto& entry : own.at("inventory")) {
                const auto& item = config_.at("items").at(entry.at("item").get<std::string>()); bool keep = item.contains("heal") || item.contains("mana") || item.at("price").get<int64_t>() <= 0;
                for (const auto& [slot,worn] : own.at("equipment").items()) { (void)slot; keep = keep || worn == entry.at("uid"); }
                for (const auto& [quest,p] : own.at("quests").items()) { (void)p; keep = keep || (config_.at("quests").contains(quest) && config_.at("quests").at(quest).at("objective").value("item",Json()) == entry.at("item")); }
                if (!keep) dealt = bot_act(id,{{"action","sell"},{"target",target},{"uid",entry.at("uid")},{"quantity",entry.at("quantity")}}) || dealt;
            }
            for (const auto& [kind,want] : {std::pair<const char*,int>{"heal",5},{"mana",3}}) {
                const auto& now = a.character.at("state"); int carried = 0; std::string best;
                for (const auto& entry : now.at("inventory")) if (config_.at("items").at(entry.at("item").get<std::string>()).contains(kind)) carried += entry.at("quantity").get<int>();
                for (const auto& item : shop) { const auto& d = config_.at("items").at(item.get<std::string>());
                    if (d.contains(kind) && d.at("level").get<int>() <= level && (best.empty() || d.at(kind).get<int>() > config_.at("items").at(best).at(kind).get<int>())) best = item; }
                if (best.empty() || carried >= want) continue;
                const int64_t price = config_.at("items").at(best).at("price"), count = std::min<int64_t>(want-carried, now.at("currency").get<int64_t>()/2/std::max<int64_t>(1,price));
                if (count > 0) dealt = bot_act(id,{{"action","buy"},{"target",target},{"item",best},{"quantity",count}}) || dealt;
            }
            for (const char* slot : gear_slots) { // mod-playerbots autogear: an upgrade costing at most a third of the purse
                const auto& now = a.character.at("state"); int worn = -1;
                if (now.at("equipment").contains(slot)) worn = score(config_.at("items").at(owned(now,now.at("equipment").at(slot)).at("item").get<std::string>()));
                std::string best;
                for (const auto& item : shop) { const auto& d = config_.at("items").at(item.get<std::string>());
                    if (d.value("slot","") == slot && fits(d,level,class_id,sex) && score(d) > worn && d.at("price").get<int64_t>()*3 <= now.at("currency").get<int64_t>() &&
                        (best.empty() || score(d) > score(config_.at("items").at(best)))) best = item; }
                if (!best.empty()) dealt = bot_act(id,{{"action","buy"},{"target",target},{"item",best},{"quantity",1}}) || dealt;
            }
            if (dealt) ++b.vendor;
        } else ok = bot_act(id,request);
        if (ok && action == "repair") ++b.vendor;
        if (ok && action == "quest_complete") { ++b.quests; if (roll(2)) bot_say(b,"quest",{{"channel","local"}}); }
        if (!ok && action == "quest_accept") b.refused.insert(request.at("quest").get<std::string>());
        if (!ok && found->second.definition.at("kind") == "door") { b.blocked.insert(target); b.unblock = tick_ + 1200; }
        if (!ok) b.quest_ready = tick_ + 600; // a refused chore waits before it is retried
        return ok;
    }
    if (b.errand == target && !a.moving && ++b.stuck > 6) return give_up();
    if (b.errand != target) b.stuck = 0;
    b.errand = target; b.request = std::move(request);
    return bot_walk(id,a,b,found->second.x,found->second.y) || give_up(); // even the long route search found no way
}
bool World::bot_travel(uint32_t id, Actor& a, Bot& b, const std::string& zone) {
    const std::string here = a.character.at("state").at("zone");
    for (const auto& [reached,door] : bot_zones(b,here)) if (door && reached == base_zone(config_,zone)) return bot_reach(id,a,b,door,{{"action","interact"},{"target",door}});
    return false;
}

bool World::bot_fight(uint32_t id, Actor& a, Bot& b) {
    const auto& s = a.character.at("state"); const int class_id = a.character.at("class_id");
    const auto ratio = [&](const Json& v, const char* now = "hp", const char* most = "max_hp") { return v.at(now).get<int>()*100/std::max(1,v.at(most).get<int>()); };
    const auto potion = [&](const char* kind) {
        for (const auto& entry : s.at("inventory")) {
            const auto& item = config_.at("items").at(entry.at("item").get<std::string>());
            if (item.contains(kind) && item.at("level").get<int>() <= s.at("level").get<int>() && bot_act(id,{{"action","use"},{"uid",entry.at("uid")}})) return true;
        }
        return false;
    };
    if (ratio(s) < 35 && potion("heal")) return true;
    if (a.engaged && ratio(s,"mana","max_mana") < 15 && potion("mana")) return true;
    if (tick_ < a.skill_ready) return false;
    const auto ready = [&](const std::string& skill, const Json& d) {
        return !d.contains("replaces") && d.at("source")[0].get<int>()+1 == class_id && s.at("skills").value(skill,0) > 0 && d.at("target") != "passive" &&
               tick_ >= (a.cooldowns.contains(skill) ? a.cooldowns.at(skill) : 0u) && s.at("mana").get<int>() >= d.value("mana",0);
    };
    // Heals: the most hurt of self and groupmates in this zone below 60 %.
    uint32_t patient = 0; int worst = 60;
    std::vector<uint32_t> party{id}; if (const auto leader = group(id)) party = groups_.at(leader);
    for (auto member : party) if (const auto m = actors_.find(member); m != actors_.end()) {
        const auto& ms = m->second.character.at("state");
        if (ms.at("zone") == s.at("zone") && ms.at("hp").get<int>() > 0 && ratio(ms) < worst) { worst = ratio(ms); patient = member; }
    }
    if (patient) for (const auto& [skill,d] : config_.at("skills").items())
        if (d.contains("heal") && (d.at("target") == "ally" || d.at("target") == "allies") && ready(skill,d)) {
            Json request{{"action","skill"},{"skill",skill}}; if (d.at("target") == "ally") request["target"] = patient;
            if (bot_act(id,request)) return true;
        }
    const auto target = npcs_.find(a.engaged);
    if (target == npcs_.end() || target->second.hp <= 0) return false;
    for (const auto& [skill,d] : config_.at("skills").items()) {
        if ((d.at("target") != "enemy" && d.at("target") != "enemies") || !(d.contains("damage") || d.contains("dot")) || !ready(skill,d)) continue;
        if (d.contains("requires") && !std::any_of(s.at("buffs").begin(),s.at("buffs").end(),[&](const Json& f){return f.at("id") == d.at("requires");})) {
            const auto stance = d.at("requires").get<std::string>(); // open the stance the technique needs
            if (ready(stance,config_.at("skills").at(stance)) && bot_act(id,{{"action","skill"},{"skill",stance}})) return true;
            continue;
        }
        if (!close(id,a.engaged,d.value("range",d.value("radius",0)))) continue;
        const auto engaged = a.engaged; Json request{{"action","skill"},{"skill",skill}}; if (d.at("target") == "enemy") request["target"] = engaged;
        if (bot_act(id,request)) { b.target = engaged; return true; }
    }
    return false;
}

// Upkeep chores (mod-playerbots maintenance): a worn piece goes to an örs, a full bag or an empty potion belt to a merchant;
// when this zone has none, the nearest zone through doors that has one.
bool World::bot_chores(uint32_t id, Actor& a, Bot& b) {
    if (tick_ < b.quest_ready) return false;
    const auto& s = a.character.at("state"); const std::string zone = s.at("zone");
    const auto nearest = [&](const std::string& kind, bool potions) {
        for (const auto& [z,door] : bot_zones(b,zone)) {
            (void)door; uint32_t best = 0; int64_t d = INT64_MAX;
            for (auto n : bot_npcs(kind,z)) {
                if (b.blocked.contains(n)) continue;
                if (potions && !std::any_of(npcs_.at(n).definition.at("shop").begin(),npcs_.at(n).definition.at("shop").end(),[&](const Json& i){return config_.at("items").at(i.get<std::string>()).contains("heal");})) continue;
                if (const auto e = z == zone ? distance2(at(s,"x"),at(s,"y"),npcs_.at(n).x,npcs_.at(n).y) : 0; e < d) { d = e; best = n; }
            }
            if (best) return best;
        }
        return uint32_t(0);
    };
    bool worn_down = false;
    for (const auto& [slot,uid] : s.at("equipment").items()) {
        (void)slot; const auto& entry = owned(s,uid.get<std::string>()); const auto& item = config_.at("items").at(entry.at("item").get<std::string>());
        worn_down = worn_down || (entry.contains("durability") && item.contains("durability") && entry.at("durability").get<int>()*4 < item.at("durability").get<int>());
    }
    if (worn_down) if (const auto anvil = nearest("anvil",false)) return bot_reach(id,a,b,anvil,{{"action","repair"},{"target",anvil}});
    int potions = 0; for (const auto& entry : s.at("inventory")) if (config_.at("items").at(entry.at("item").get<std::string>()).contains("heal")) potions += entry.at("quantity").get<int>();
    const bool full = s.at("inventory").size()+3 >= config_.at("inventory_slots").get<size_t>(), dry = potions == 0 && s.at("currency").get<int64_t>() >= 100;
    if (full || dry) if (const auto merchant = nearest("merchant",dry && !full)) return bot_reach(id,a,b,merchant,{{"action","bot_shop"},{"target",merchant}});
    return false;
}

// Görevler from real quest data: hand in finished ones (in another zone too), walk to a visit spot, Kullan a use quest's prop,
// drop a quest that stalled or can no longer be done, accept a doable one (quest mode: from givers in other reachable zones
// too; at most three at once). Gathered-item collect quests (no monster target) stay with players.
bool World::bot_quests(uint32_t id, Actor& a, Bot& b) {
    if (tick_ < b.quest_ready) return false;
    const auto& s = a.character.at("state"); const std::string zone = s.at("zone"); const int level = s.at("level");
    const auto zones = bot_zones(b,zone); std::set<std::string> reachable; for (const auto& [z,door] : zones) { (void)door; reachable.insert(z); }
    const auto in_reach = [&](uint32_t npc) { return npcs_.contains(npc) && reachable.contains(base_zone(config_,npcs_.at(npc).definition.at("zone"))); };
    // A use objective's props: each configured target and every NPC sharing its classes.dat uid (copies, rescue groups), alive.
    const auto props = [&](const Json& o) {
        std::vector<uint32_t> out;
        for (const auto& t : o.at("targets")) {
            const auto c = npcs_.find(t.get<uint32_t>()); if (c == npcs_.end()) continue;
            const auto uid = c->second.definition.value("source_uid",Json());
            for (const auto& [nid,n] : npcs_) if (!n.master && n.hp > 0 && (nid == c->first || (!uid.is_null() && n.definition.value("source_uid",Json()) == uid)) && in_reach(nid)) out.push_back(nid);
        }
        return out;
    };
    const auto& done = s.at("quests_done");
    const auto doable = [&](const std::string& quest) {
        const auto& q = config_.at("quests").at(quest); const auto& o = q.at("objective");
        const auto finished = [&](const Json& r) { return !r.is_string() || r.get<std::string>().empty() || std::binary_search(done.begin(),done.end(),r); };
        const Json also = q.value("requires_also",Json::array());
        if (q.value("min_level",q.at("level").get<int>()) > level || (q.contains("class") && q.at("class") != a.character.at("class_id")) || !finished(q.value("requires",Json())) ||
            !std::all_of(also.begin(),also.end(),finished)) return false; // the Görev al rules (level, class, prerequisites)
        if (o.at("kind") == "talk") return in_reach(q.value("turn_in",q.at("giver")).get<uint32_t>());
        if (o.at("kind") == "visit") return reachable.contains(base_zone(config_,o.at("zone").get<std::string>()));
        if (o.at("kind") == "use") return !props(o).empty();
        if (!o.contains("target")) return false; // gathered items: left to players
        const auto& hosts = bot_npcs("tmpl",o.at("target").get<std::string>());
        return std::any_of(hosts.begin(),hosts.end(),[&](uint32_t e){ return in_reach(e) && npcs_.at(e).definition.value("level",1) <= level+2; });
    };
    for (const auto& [quest,p] : s.at("quests").items()) {
        if (!config_.at("quests").contains(quest)) return bot_act(id,{{"action","quest_abandon"},{"quest",quest}}); // content changed under a saved bot
        const auto& q = config_.at("quests").at(quest); const auto& o = q.at("objective");
        const int count = o.at("kind") == "talk" ? 1 : o.value("count",1), progress = p.value("progress",0);
        const uint32_t to = q.contains("turn_in") ? q.at("turn_in").get<uint32_t>() : q.at("giver").get<uint32_t>();
        if (progress >= count && in_reach(to) && !b.blocked.contains(to)) return bot_reach(id,a,b,to,{{"action","quest_complete"},{"target",to},{"quest",quest}});
        auto& seen = b.quest_seen[quest];
        if (!seen.second || seen.first != progress) seen = {progress, tick_};
        else if (tick_ - seen.second > uint32_t(20*60*setting(config_,"quest_patience_minutes",20)) || !doable(quest)) { // mod-playerbots "drop obsolete quests"
            b.quest_seen.erase(quest); b.refused.insert(quest);
            return bot_act(id,{{"action","quest_abandon"},{"quest",quest}});
        }
        if (o.at("kind") == "visit" && progress == 0) { // quest_tick credits the spot once the bot stands inside its radius
            const std::string there = o.at("zone");
            if (base_zone(config_,there) != base_zone(config_,zone) ? bot_travel(id,a,b,there) : bot_walk(id,a,b,o.at("x"),o.at("y"))) return true;
        }
        if (o.at("kind") == "use" && progress < count) { // nearest prop not yet credited (or past its cooldown); Kullan = interact
            const auto now = int64_t(std::time(nullptr)); const auto used = p.value("used",Json::object()); uint32_t pick = 0; int64_t near = INT64_MAX;
            for (auto t : props(o)) {
                const auto key = std::to_string(t); const auto& n = npcs_.at(t);
                if (count > 1 && used.contains(key) && (!o.contains("cooldown") || now - used.at(key).get<int64_t>() < o.at("cooldown").get<int64_t>())) continue;
                const int64_t d = n.definition.at("zone") == zone ? distance2(at(s,"x"),at(s,"y"),n.x,n.y) : INT64_MAX/2;
                if (d < near) { near = d; pick = t; }
            }
            if (pick && bot_reach(id,a,b,pick,{{"action","interact"},{"target",pick}})) return true;
        }
    }
    std::erase_if(b.quest_seen,[&](const auto& q){ return !s.at("quests").contains(q.first); });
    b.quest_ready = tick_ + 400;
    if (s.at("quests").size() >= 3 || (b.mode != "grind" && b.mode != "quest")) return false; // town/travel bots only hand in and run errands
    uint32_t pick = 0; std::string chosen; int64_t closest = INT64_MAX;
    for (const auto& [z,door] : zones) {
        (void)door; if (z != zone && b.mode != "quest") break;
        for (auto giver : bot_npcs("quest",z)) {
            const auto& g = npcs_.at(giver); const auto away = z == zone ? distance2(at(s,"x"),at(s,"y"),g.x,g.y) : 0;
            if (b.blocked.contains(giver) || away >= closest) continue;
            for (const auto& offered : quests_at(g,s)) {
                const std::string quest = offered;
                if (s.at("quests").contains(quest) || b.refused.contains(quest) || std::binary_search(done.begin(),done.end(),offered) || !doable(quest)) continue;
                pick = giver; chosen = quest; closest = away; break;
            }
        }
        if (pick) break;
    }
    return pick && bot_reach(id,a,b,pick,{{"action","quest_accept"},{"target",pick},{"quest",chosen}});
}

bool World::bot_town(uint32_t id, Actor& a, Bot& b) {
    const auto& s = a.character.at("state"); const std::string zone = s.at("zone");
    if (b.mode == "town" && b.behaviour != "companion" && bot_npcs("merchant",zone).empty() && bot_npcs("quest",zone).size() < 3) // go to town (mod-playerbots GoCamp); a companion waits where it was dismissed
        for (const auto& [z,door] : bot_zones(b,zone)) if (door && !bot_npcs("merchant",z).empty()) { if (bot_travel(id,a,b,z)) return true; break; }
    if (a.moving) return true;
    if (tick_ < b.idle_until) return false;
    if (b.home != zone) { // anchor beside a townsfolk NPC of this zone (merchant, quest giver, örs), not on the spawn courtyard
        std::vector<uint32_t> folk; for (const char* kind : {"merchant","questgiver","anvil"}) for (auto n : bot_npcs(kind,zone)) folk.push_back(n);
        const auto& spawn = config_.at("zones").at(zone).at("spawn"); b.home = zone; b.anchor_x = spawn[0]; b.anchor_y = spawn[1];
        if (!folk.empty()) { const auto& n = npcs_.at(folk[roll(uint32_t(folk.size()))]); b.anchor_x = n.x; b.anchor_y = n.y; }
    }
    const auto& chat = settings(config_).value("chat", Json::array());
    if (tick_ >= b.chat_ready && !chat.empty()) {
        const auto& lines = settings(config_).value("lines", Json::object());
        const bool offer = b.behaviour == "companion" && lines.contains("companion") && roll(2);
        bot_act(id,{{"action","chat"},{"channel","local"},{"text",offer ? lines.at("companion")[roll(uint32_t(lines.at("companion").size()))] : chat[roll(uint32_t(chat.size()))]}});
        const uint32_t period = setting(config_,"chat_seconds",300); b.chat_ready = tick_ + 20*(period/2 + roll(period+1));
    }
    const auto r = roll(100); const int radius = config_.at("aoi").get<int>()/6;
    if (r < 55) { // wander: a walkable spot around the anchor (or here, when idling away from it)
        const bool home = b.mode == "town";
        const int32_t cx = home ? b.anchor_x : at(s,"x"), cy = home ? b.anchor_y : at(s,"y");
        for (int attempt = 0; attempt < 3; ++attempt) {
            const int32_t x = cx + int32_t(roll(uint32_t(2*radius+1))) - radius, y = cy + int32_t(roll(uint32_t(2*radius+1))) - radius;
            if (passable(zone,x,y) && bot_walk(id,a,b,x,y)) break;
        }
        b.idle_until = tick_ + 20*(2+roll(6));
    } else if (r < 70) {
        static constexpr std::array<const char*,9> emotes{"selam","gerin","elsalla","alkış","gül","konuş","sor","eyvallah","yaşasın"};
        bot_act(id,{{"action","chat"},{"channel","local"},{"text",std::string("/")+emotes[roll(emotes.size())]}});
        b.idle_until = tick_ + 20*(3+roll(6));
    } else if (r < 82) {
        bot_act(id,{{"action","chat"},{"channel","local"},{"text",roll(2) ? "/otur" : "/bağdaş"}}); // held posture until the next walk
        b.idle_until = tick_ + 20*(15+roll(26));
    } else b.idle_until = tick_ + 20*(5+roll(11));
    return true;
}

bool World::bot_hunt(uint32_t id, Actor& a, Bot& b) {
    const auto& s = a.character.at("state"); const std::string zone = s.at("zone"); const int level = s.at("level");
    const auto& census = bot_census(); const auto aoi = config_.at("aoi").get<int64_t>();
    const auto suits = [&](const Json& d) { const int l = d.value("level",1); return l <= level+2 && l >= level-6 && !d.value("banner",false); };
    std::set<std::string> wanted; // this bot's quest targets
    for (const auto& [quest,p] : s.at("quests").items()) { (void)p; if (!config_.at("quests").contains(quest)) continue; if (const auto t = config_.at("quests").at(quest).at("objective").value("target",Json()); t.is_string()) wanted.insert(t.get<std::string>()); }
    const auto tmpl = [](const Json& d) { return d.value("template",std::string()); };
    // Hunting ground (mod-playerbots grind + RandomPlayerbotMgr level brackets): stay while this zone holds a quest target or
    // enough fitting monsters for the bots already on it, else a weighted pick among door-reachable zones by fitting monsters,
    // quest targets, bots already there and distance; a ground that stopped paying (no fair quarry) is re-picked.
    if (b.ground.empty() || b.dry > 40) {
        const std::string tired = b.dry > 40 ? b.ground : std::string(); b.dry = 0; b.ground.clear();
        Json weights = Json::object(); size_t hop = 0;
        for (const auto& [z,door] : bot_zones(b,zone)) {
            (void)door; int64_t fit = 0, quest = 0;
            for (auto e : bot_npcs("enemy",z)) if (const auto& d = npcs_.at(e).definition; suits(d)) { ++fit; quest += wanted.contains(tmpl(d)); }
            const int64_t crowd = census.grinders.contains(z) ? census.grinders.at(z) : 0;
            if (z == zone && z != tired && (quest || (fit >= 3 && crowd*2 < fit))) { weights = Json::object(); weights[z] = 1; break; }
            if (fit && z != tired) weights[z] = std::max<int64_t>(1, (fit*100 + quest*400) / (1 + crowd) / int64_t(1 + hop/3));
            ++hop;
        }
        if (weights.empty()) return false;
        b.ground = weighted(weights);
    }
    if (b.ground != base_zone(config_,zone)) { if (bot_travel(id,a,b,b.ground)) return true; b.ground.clear(); return false; }
    // Quarry (GrindTargetValue): never a monster a player has hit or is attacking, nor one a player's active quest needs while
    // that player is near it, nor a newbie monster by the zone spawn while a player is in the zone; a player's quest family
    // elsewhere counts four times farther, this bot's own quest targets four times closer.
    static const std::vector<uint32_t> nobody; const auto found_players = census.players.find(zone);
    const auto& players = found_players == census.players.end() ? nobody : found_players->second;
    const auto hot = census.targets.find(zone); const auto& spawn = config_.at("zones").at(zone).at("spawn");
    const auto fair = [&](uint32_t e) {
        const auto f = npcs_.find(e); if (f == npcs_.end()) return false; const auto& n = f->second;
        if (n.hp <= 0 || n.definition.at("zone") != zone || !suits(n.definition) || (n.owner && n.owner != id) || b.blocked.contains(e)) return false;
        for (const auto& [who,threat] : n.threat) { (void)threat; if (who != id && actors_.contains(who) && !actors_.at(who).bot) return false; }
        for (auto p : players) {
            const auto& pa = actors_.at(p); const auto& ps = pa.character.at("state");
            if (pa.engaged == e) return false;
            if (hot != census.targets.end() && hot->second.contains(tmpl(n.definition)) && distance2(n.x,n.y,at(ps,"x"),at(ps,"y")) < aoi*aoi/4) return false;
        }
        return players.empty() || n.definition.value("level",1)+2 > level || distance2(n.x,n.y,spawn[0],spawn[1]) >= aoi*aoi/9;
    };
    uint32_t quarry = b.quarry && fair(b.quarry) ? b.quarry : 0; int64_t best = INT64_MAX;
    if (!quarry) for (auto e : bot_npcs("enemy",zone)) {
        if (!fair(e)) continue;
        const auto& n = npcs_.at(e); int64_t d = distance2(at(s,"x"),at(s,"y"),n.x,n.y);
        if (wanted.contains(tmpl(n.definition))) d /= 4; else if (hot != census.targets.end() && hot->second.contains(tmpl(n.definition))) d *= 4;
        if (d < best) { best = d; quarry = e; }
    }
    if (!quarry) { ++b.dry; b.quarry = 0; return false; }
    b.dry = 0;
    // Approach in legs, attack from a quarter view; a quarry the engine keeps calling unreachable is skipped for a minute.
    if (b.quarry != quarry) { b.quarry = quarry; b.misses = 0; b.attacked = false; }
    if (b.attacked && !a.engaged) { b.attacked = false; ++b.misses; }
    if (b.misses >= 3) { b.blocked.insert(quarry); b.unblock = tick_ + 1200; b.quarry = 0; return false; }
    const auto& n = npcs_.at(quarry); const int64_t near = aoi/4;
    if (distance2(at(s,"x"),at(s,"y"),n.x,n.y) > near*near) { if (!bot_walk(id,a,b,n.x,n.y)) ++b.misses; return true; }
    b.attacked = bot_act(id,{{"action","attack"},{"target",quarry}});
    if (b.attacked) b.target = quarry;
    return true;
}

bool World::bot_follow(uint32_t id, Actor& a, Bot& b) {
    const auto& leader = actors_.at(b.leader); const auto& ls = leader.character.at("state"); const auto& s = a.character.at("state");
    if (ls.at("zone") != s.at("zone")) {
        if (b.stay) return false;
        if (bot_travel(id,a,b,ls.at("zone"))) { b.lost = false; return true; }
        if (!b.lost) { b.lost = true; bot_say(b,"lost",{{"channel","group"}}); }
        return false;
    }
    b.lost = false;
    // Assist: the leader's target, or whatever bites the leader.
    uint32_t foe = 0;
    if (const auto t = npcs_.find(leader.engaged); t != npcs_.end() && t->second.hp > 0 && t->second.definition.at("kind") == "enemy") foe = leader.engaged;
    if (!foe) for (auto e : bot_npcs("enemy",s.at("zone"))) if (npcs_.at(e).hp > 0 && npcs_.at(e).threat.contains(b.leader)) { foe = e; break; }
    if (foe && a.engaged != foe && bot_act(id,{{"action","attack"},{"target",foe}})) { b.target = foe; return true; }
    const int follow = config_.at("aoi").get<int>()/10;
    if (!b.stay && distance2(at(s,"x"),at(s,"y"),at(ls,"x"),at(ls,"y")) > int64_t(follow)*follow) {
        const int spread = follow/3;
        return bot_walk(id,a,b,at(ls,"x")+int32_t(roll(uint32_t(2*spread+1)))-spread,at(ls,"y")+int32_t(roll(uint32_t(2*spread+1)))-spread);
    }
    return false;
}

// Social input (mod-playerbots chat/group/trade handlers): leader commands act at once; replies, invitation answers and
// trade decisions are queued with a reaction delay. Söyle lines are answered when they name the bot, or (greetings) by the
// nearest bot once. Invitations: companions always, others usually when the levels are close. Trades: the bot never gives
// anything away, accepts what a player hands over, and cancels an empty exchange after a while.
void World::bot_social(uint32_t id, Actor& a, Bot& b) {
    const auto& s = a.character.at("state"); const auto& conf = settings(config_);
    const std::string own = a.character.at("name"), first = lower(own.substr(own.rfind(' ') == std::string::npos ? 0 : own.rfind(' ')+1));
    for (const auto& message : b.inbox) {
        const uint32_t from = message.at("from"); const std::string channel = message.at("channel"), text = lower(message.at("text").get<std::string>());
        if (!actors_.contains(from)) continue;
        const std::string speaker = actors_.at(from).character.at("name");
        if (b.leader == from && channel != "local" && text.starts_with("/")) {
            if (text.starts_with("/dismiss") || text.starts_with("/ayrıl")) { bot_act(id,{{"action","chat"},{"channel","group"},{"text","Allahaısmarladık!"}}); bot_act(id,{{"action","group_leave"}}); b.leader = 0; b.lost = b.stay = false; b.errand = 0; }
            else if (text.starts_with("/bekle")) { b.stay = true; bot_say(b,"stay",{{"channel","group"}},speaker); }
            else if (text.starts_with("/gel")) { b.stay = false; bot_say(b,"come",{{"channel","group"}},speaker); }
            continue;
        }
        const bool named = first.size() >= 3 && text.find(first) != std::string::npos;
        const Json& greetings = conf.contains("replies") && !conf.at("replies").empty() ? conf.at("replies")[0].at("match") : Json::array();
        if (channel == "local" && !named) { // one nearby voice answers a greeting to the crowd, at most every five seconds
            if (!has_word(text,greetings) || !close(id,from,config_.at("aoi").get<int>()/5) || (bot_replied_.contains(from) && bot_replied_.at(from)+100 > tick_) || roll(2)) continue;
        }
        bot_replied_[from] = tick_;
        Json request{{"channel",channel}}; if (channel == "private") request["to"] = from;
        const Json* lines = nullptr;
        if (conf.contains("replies")) for (const auto& reply : conf.at("replies")) if (has_word(text,reply.at("match"))) { lines = &reply.at("lines"); break; }
        if (!lines && conf.contains("lines") && conf.at("lines").contains("reply")) lines = &conf.at("lines").at("reply");
        if (!lines || lines->empty() || b.pending.size() >= 6) continue;
        request["action"] = "chat"; request["text"] = fill((*lines)[roll(uint32_t(lines->size()))].get<std::string>(), speaker);
        b.pending.push_back({tick_ + span(30,90) + uint32_t(request.at("text").get<std::string>().size()), std::move(request), 0});
    }
    b.inbox = Json::array();
    if (b.leader && (!actors_.contains(b.leader) || !group(id) || group(id) != group(b.leader))) { // kicked, or the leader logged out
        if (group(id)) bot_act(id,{{"action","group_leave"}});
        b.leader = 0; b.lost = b.stay = false; b.errand = 0;
    }
    if (const auto invite = invites_.find(id); invite != invites_.end() && !b.leader && invite->second != b.invite_from) {
        const uint32_t from = invite->second; b.invite_from = from;
        if (!actors_.contains(from) || actors_.at(from).bot) invites_.erase(invite);
        else {
            const std::string speaker = actors_.at(from).character.at("name");
            const int gap = std::abs(actors_.at(from).character.at("state").at("level").get<int>() - s.at("level").get<int>());
            if (b.behaviour == "companion" || (gap <= 10 && roll(100) < 75)) {
                b.pending.push_back({tick_ + span(20,60), {{"action","group_accept"}}, from});
                bot_say(b,"invite_accept",{{"channel","group"}},speaker);
            } else { invites_.erase(id); bot_say(b,"invite_decline",{{"channel","private"},{"to",from}},speaker); }
        }
    }
    if (!invites_.contains(id)) b.invite_from = 0;
    if (auto* t = trade(id)) {
        const uint32_t peer = t->a == id ? t->b : t->a; const std::string speaker = actors_.contains(peer) ? actors_.at(peer).character.at("name").get<std::string>() : "";
        if (!t->open) {
            if (t->b == id && b.trade_seen != t->revision) {
                b.trade_seen = t->revision; b.trade_since = tick_;
                if (peer == b.leader || roll(100) < 60) { b.pending.push_back({tick_ + span(20,60), {{"action","trade_accept"},{"revision",t->revision}}, 0}); bot_say(b,"trade_open",{{"channel","private"},{"to",peer}},speaker); }
                else { b.pending.push_back({tick_ + span(20,60), {{"action","trade_cancel"}}, 0}); bot_say(b,"trade_decline",{{"channel","private"},{"to",peer}},speaker); }
            }
        } else {
            const auto& theirs = t->a == id ? t->offer_b : t->offer_a; const bool accepted = t->a == id ? t->accept_a : t->accept_b;
            const bool gift = !theirs.at("items").empty() || theirs.at("currency").get<int64_t>() > 0;
            if (gift && !accepted && b.trade_seen != t->revision) { b.trade_seen = t->revision; b.pending.push_back({tick_ + span(30,80), {{"action","trade_accept"},{"revision",t->revision}}, 0}); bot_say(b,"trade_thanks",{{"channel","private"},{"to",peer}},speaker); }
            else if (!gift && tick_ > b.trade_since + 20*45) { b.trade_since = tick_ + 20*600; b.pending.push_back({tick_ + span(10,30), {{"action","trade_cancel"}}, 0}); }
        }
    }
    guild_invites_.erase(id); // Lonca davetleri: bots stay out of guilds (the invitation lapses unanswered)
}

void World::bot_think(uint32_t id, Actor& a, Bot& b) {
    using Run = bool (*)(World&, uint32_t, Actor&, Bot&);
    enum : unsigned { TOWN = 1, GRIND = 2, QUEST = 4, TRAVEL = 8, FOLLOW = 16, ROAM = TOWN|GRIND|QUEST|TRAVEL, ALL = ROAM|FOLLOW };
    struct Rule { const char* name; float relevance; unsigned modes; Run run; }; // relevance: mod-playerbots ACTION_* scale, sorted
    static const Rule rules[] = {
        {"react", 97, ALL, [](World& w, uint32_t id, Actor& a, Bot& b) { // the first due reaction (a chat line waits out the rate limit)
            for (size_t i = 0; i < b.pending.size(); ++i) {
                if (b.pending[i].due > w.tick_ || (b.pending[i].request.at("action") == "chat" && w.tick_ < a.chat_ready)) continue;
                auto r = std::move(b.pending[i]); b.pending.erase(b.pending.begin()+long(i));
                if (w.bot_act(id,r.request) && r.leader) { b.leader = b.ally = r.leader; b.errand = 0; b.stay = false; b.resting = false; }
                return true;
            }
            return false; }},
        {"revive", 95, ALL, [](World& w, uint32_t id, Actor& a, Bot& b) { // random revive delay (mod-playerbots revive time)
            if (a.character.at("state").at("hp").get<int>() > 0) { b.revive_at = 0; return false; }
            b.resting = false; b.errand = 0;
            if (!b.revive_at) { b.revive_at = std::max(w.tick_, a.resurrect_ready) + span(60,500); if (roll(3) == 0) w.bot_say(b,"death",{{"channel","local"}}); }
            if (w.tick_ >= b.revive_at) w.bot_act(id,{{"action","resurrect"}});
            return true; }},
        {"flee", 85, ALL, [](World& w, uint32_t id, Actor& a, Bot& b) { // low and without a potion against an even monster: run home
            if (w.tick_ < b.flee_until) return a.moving;
            const auto& s = a.character.at("state"); const auto foe = w.npcs_.find(a.engaged);
            if (foe == w.npcs_.end() || foe->second.hp <= 0 || s.at("hp").get<int>()*5 >= s.at("max_hp").get<int>() || foe->second.definition.value("level",1) < s.at("level").get<int>()) return false;
            for (const auto& e : s.at("inventory")) if (const auto& d = w.config_.at("items").at(e.at("item").get<std::string>()); d.contains("heal") && d.at("level").get<int>() <= s.at("level").get<int>()) return false;
            const auto& spawn = w.config_.at("zones").at(s.at("zone").get<std::string>()).at("spawn");
            b.flee_until = w.tick_ + 200; b.quarry = 0; return w.bot_walk(id,a,b,spawn[0],spawn[1]); }},
        {"fight", 80, ALL, [](World& w, uint32_t id, Actor& a, Bot& b) { return w.bot_fight(id,a,b); }},
        {"defend", 75, ALL, [](World& w, uint32_t id, Actor& a, Bot& b) { // answer any monster biting this bot
            if (a.engaged || w.tick_ < b.flee_until) return false;
            for (auto e : w.bot_npcs("enemy",a.character.at("state").at("zone"))) if (w.npcs_.at(e).hp > 0 && w.npcs_.at(e).threat.contains(id)) {
                b.resting = false; if (w.bot_act(id,{{"action","attack"},{"target",e}})) { b.target = e; return true; }
            }
            return false; }},
        {"engaged", 74, ALL, [](World&, uint32_t, Actor& a, Bot&) { return a.engaged != 0; }}, // auto-attack runs on
        {"follow", 60, FOLLOW, [](World& w, uint32_t id, Actor& a, Bot& b) { return w.bot_follow(id,a,b); }},
        {"rest", 55, ALL, [](World& w, uint32_t id, Actor& a, Bot& b) { // sit until 95 % Enerji / 80 % Kudret (food/drink strategy)
            const auto& s = a.character.at("state"); const int hp = s.at("hp").get<int>()*100/std::max(1,s.at("max_hp").get<int>()), mana = s.at("mana").get<int>()*100/std::max(1,s.at("max_mana").get<int>());
            if (b.resting) { if (hp < 95 || mana < 80) return true; b.resting = false; w.bot_act(id,{{"action","chat"},{"channel","local"},{"text","/kalk"}}); return false; }
            if (hp >= 55 && mana >= 20) return false;
            b.resting = true; b.errand = 0; w.bot_act(id,{{"action","chat"},{"channel","local"},{"text",roll(4) ? "/otur" : "/bağdaş"}}); return true; }},
        {"points", 50, ALL, [](World& w, uint32_t id, Actor& a, Bot&) { // Yetenekler: lowest-ranked damage/heal skill (and its stance); Dirençler evenly
            const auto& s = a.character.at("state"); const int level = s.at("level"), class_id = a.character.at("class_id");
            int points = w.config_.at("skill_points_base").get<int>() + w.config_.at("skill_points_per_level").get<int>()*(level-1);
            for (const auto& [skill,rank] : s.at("skills").items()) { (void)skill; points -= rank.get<int>(); }
            if (points > 0) {
                std::vector<std::string> useful;
                for (const auto& [skill,d] : w.config_.at("skills").items())
                    if (!d.contains("replaces") && d.at("source")[0].get<int>()+1 == class_id && d.at("level").get<int>() <= level && (d.contains("damage") || d.contains("heal"))) {
                        useful.push_back(skill); if (d.contains("requires")) useful.push_back(d.at("requires"));
                    }
                Json ranks = Json::object();
                for (; points > 0 && !useful.empty(); --points) {
                    const auto rank = [&](const std::string& k) { return s.at("skills").value(k,0) + ranks.value(k,0); };
                    const auto pick = *std::min_element(useful.begin(),useful.end(),[&](const auto& l, const auto& r){return rank(l) < rank(r);});
                    if (rank(pick) >= w.config_.at("skill_rank_max").get<int>()) break;
                    ranks[pick] = ranks.value(pick,0)+1;
                }
                if (!ranks.empty() && w.bot_act(id,{{"action","skill_learn"},{"skills",ranks}})) return true;
            }
            if (!w.config_.contains("character_points") || !s.contains("resistances")) return false;
            int free = w.config_.at("character_points").at("resistance_per_level").get<int>()*(level-1);
            for (const auto& [stat,n] : s.at("resistances").items()) { (void)stat; free -= n.get<int>(); }
            if (free <= 0) return false;
            static constexpr std::array<const char*,5> elements{"ates","buz","elektrik","asit","zehir"};
            Json spend = Json::object(); for (int i = 0; i < free; ++i) { const char* e = elements[size_t(i)%elements.size()]; spend[e] = spend.value(e,0)+1; }
            return w.bot_act(id,{{"action","stat_points"},{"points",spend}}); }},
        {"loot", 48, ALL, [](World& w, uint32_t id, Actor& a, Bot&) { // Ganimet of own kills: open (currency), then take what fits
            for (auto corpse : w.bot_npcs("enemy",a.character.at("state").at("zone"))) {
                const auto& n = w.npcs_.at(corpse);
                if (n.hp > 0 || n.owner != id || n.looted || !n.drop.is_array() || !w.close(id,corpse,w.config_.at("aoi"))) continue;
                if (!w.close(id,corpse,w.config_.at("interaction_range"))) return w.bot_act(id,{{"action","loot"},{"target",corpse}}); // walks over
                w.bot_act(id,{{"action","loot"},{"target",corpse}});
                for (size_t slot = 0; slot < n.drop.size() && a.character.at("state").at("inventory").size() < w.config_.at("inventory_slots").get<size_t>(); ++slot)
                    if (n.drop[slot].is_object()) w.bot_act(id,{{"action","loot_take"},{"target",corpse},{"slot",slot}});
                return true;
            }
            return false; }},
        {"equip", 46, ALL, [](World& w, uint32_t id, Actor& a, Bot&) { // wear a carried upgrade in any gear slot (autogear)
            const auto& s = a.character.at("state"); const int level = s.at("level"), class_id = a.character.at("class_id"), sex = a.character.at("appearance").value("sex",1);
            for (const char* slot : gear_slots) {
                int worn = s.at("equipment").contains(slot) ? score(w.config_.at("items").at(w.owned(s,s.at("equipment").at(slot)).at("item").get<std::string>())) : -1;
                std::string pick;
                for (const auto& entry : s.at("inventory")) {
                    const auto& item = w.config_.at("items").at(entry.at("item").get<std::string>());
                    if (item.value("slot","") == slot && fits(item,level,class_id,sex) && score(item) > worn) { worn = score(item); pick = entry.at("uid"); }
                }
                if (!pick.empty() && w.bot_act(id,{{"action","equip"},{"uid",pick}})) return true;
            }
            return false; }},
        {"errand", 44, ALL, [](World& w, uint32_t id, Actor& a, Bot& b) { return b.errand && w.bot_reach(id,a,b,b.errand,b.request); }},
        {"chores", 40, ROAM, [](World& w, uint32_t id, Actor& a, Bot& b) { return w.bot_chores(id,a,b); }},
        {"quests", 36, ROAM, [](World& w, uint32_t id, Actor& a, Bot& b) { // hand-ins and errands in any activity; a dismissed companion waits
            return !(b.behaviour == "companion" && b.mode == "town") && w.bot_quests(id,a,b); }},
        {"travel", 22, TRAVEL, [](World& w, uint32_t id, Actor& a, Bot& b) { // explore another level-fitting zone, then grind there
            const std::string zone = a.character.at("state").at("zone"); const int level = a.character.at("state").at("level");
            if (b.ground.empty()) {
                std::vector<std::string> options;
                for (const auto& [z,door] : w.bot_zones(b,zone)) if (door) for (auto e : w.bot_npcs("enemy",z)) if (const int l = w.npcs_.at(e).definition.value("level",1); l <= level+2 && l >= level-6) { options.push_back(z); break; }
                if (options.empty()) { b.mode = "grind"; return false; }
                b.ground = options[roll(uint32_t(options.size()))];
            }
            if (b.ground == base_zone(w.config_,zone) || !w.bot_travel(id,a,b,b.ground)) { b.mode = "grind"; return false; }
            return true; }},
        {"hunt", 20, GRIND|QUEST, [](World& w, uint32_t id, Actor& a, Bot& b) { return w.bot_hunt(id,a,b); }},
        {"town", 10, ROAM, [](World& w, uint32_t id, Actor& a, Bot& b) { return w.bot_town(id,a,b); }},
    };
    const auto& s = a.character.at("state");
    ++b.thinks; b.zones.insert(s.at("zone").get<std::string>());
    if (b.unblock && tick_ >= b.unblock) { b.blocked.clear(); b.unblock = 0; }
    bot_social(id,a,b);
    if (const int level = s.at("level"); level > b.level_seen) { if (roll(2)) bot_say(b,"level",{{"channel","local"}}); b.level_seen = level; b.refused.clear(); }
    if (b.target) if (const auto f = npcs_.find(b.target); f == npcs_.end() || f->second.hp <= 0) { // kill accounting (admin metric)
        if (f != npcs_.end() && f->second.owner == id) {
            ++b.kills;
            for (const auto& [who,threat] : f->second.threat) { (void)threat; if (who != id && who != b.ally && actors_.contains(who) && !actors_.at(who).bot && (!group(id) || group(who) != group(id))) { ++b.steals; break; } }
        }
        b.target = 0;
    }
    if (tick_ >= b.mode_until && !b.leader) { // "new rpg status update": re-roll the activity by the behaviour's weights
        const auto [lo,hi] = pair_setting(config_,"mode_seconds",180,720);
        b.mode = weighted(profile(config_,b.behaviour)); b.mode_until = tick_ + 20*span(lo,hi); b.ground.clear(); b.dry = 0; b.home.clear();
    }
    const unsigned mode = b.leader ? FOLLOW : b.mode == "grind" ? GRIND : b.mode == "quest" ? QUEST : b.mode == "travel" ? TRAVEL : TOWN;
    for (const auto& rule : rules) if ((rule.modes & mode) && rule.run(*this,id,a,b)) { b.last = rule.name; return; }
    b.last = "idle"; ++b.idle;
}

void World::bots_tick() {
    if (bots_.empty()) return;
    const uint32_t think = std::max(1, setting(config_,"think_ticks",10));
    if (bot_search_owner_) { // one slice of the bot search in flight; dropped when its bot left or stopped asking for it
        const int slice = std::max(100, setting(config_,"route_nodes",4000));
        if (!bots_.contains(bot_search_owner_) || tick_ - bot_search_tick_ > 200) { bot_search_owner_ = 0; bot_search_ = RouteSearch{}; }
        else if (!bot_search_.over) { route_step(bot_search_,slice); bot_search_.over = bot_search_.over || bot_search_.popped >= 12*slice; }
    }
    std::vector<uint32_t> due; for (const auto& [id,b] : bots_) if (tick_ >= b.next_think) due.push_back(id);
    for (auto id : due) {
        const auto found = bots_.find(id); if (found == bots_.end()) continue;
        auto& a = actors_.at(id); auto& b = found->second;
        if (a.kicked) { remove(id); continue; } // a moderator kick despawns the bot; its lease is released by the host
        // Session over (RandomPlayerbotMgr in-world time): log out when not busy with a player, return after the offline time.
        if (b.logout_at && tick_ >= b.logout_at && !a.engaged && !b.leader && !group(id) && !trade(id) && a.character.at("state").at("hp").get<int>() > 0) {
            const auto [lo,hi] = pair_setting(config_,"offline_minutes",2,15);
            if (roll(3) == 0) bot_act(id,{{"action","chat"},{"channel","local"},{"text","/elsalla"}});
            bot_returns_.emplace_back(tick_ + 1200*span(lo,hi), b.spec); remove(id); continue;
        }
        b.next_think = tick_ + think/2 + roll(think+1); // think jitter: no two bots keep step
        try { bot_think(id,a,b); } catch (const std::exception&) {} // one bot's misjudgement never stops the world tick
    }
}
}
