// Server-side bot players (NONCANONICAL: the original game had no bots). A bot is an ordinary World actor without a network
// peer: every move goes through destination() and every deed through handle(), exactly the requests a client sends, so range,
// cooldown, resource, ownership, door and quest rules bind bots like players. Behaviour is a small per-bot state machine
// thought every bots.think_ticks: town (wander, sit, emote, Yerel chat), hunt (level-appropriate monsters, skills, Ganimet,
// rest, repair, selling, quests) and companion (accepts a player's group invitation, follows through doors, assists, heals).
#include "ikv/world.hpp"
#include <openssl/rand.h>
#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

namespace ikv {
namespace {
int64_t distance2(int32_t x, int32_t y, int32_t ox, int32_t oy) { const int64_t dx = int64_t(x)-ox, dy = int64_t(y)-oy; return dx*dx+dy*dy; }
uint32_t roll(uint32_t n) { thread_local std::mt19937 engine{std::random_device{}()}; return n ? std::uniform_int_distribution<uint32_t>(0,n-1)(engine) : 0; }
const Json& settings(const Json& config) { static const Json none = Json::object(); return config.contains("bots") ? config.at("bots") : none; }
int setting(const Json& config, const char* key, int fallback) { return settings(config).value(key, fallback); }
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
    if (spec.contains("zone")) {
        zone = spec.at("zone").get<std::string>();
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
    while (actors_.contains(next_bot_) || npcs_.contains(next_bot_)) ++next_bot_;
    const auto id = next_bot_;
    admit(id, account, character, placed, false, true); ++next_bot_;
    const auto& s = actors_.at(id).character.at("state"); const auto& spawn = config_.at("zones").at(s.at("zone").get<std::string>()).at("spawn");
    Bot b; b.behaviour = behaviour; b.home = s.at("zone"); b.anchor_x = spawn[0]; b.anchor_y = spawn[1];
    const uint32_t think = setting(config_,"think_ticks",10);
    b.next_think = tick_ + 1 + id % think; b.chat_ready = tick_ + 20*roll(setting(config_,"chat_seconds",300)+1);
    bots_.emplace(id, std::move(b)); return id;
}

Json World::bots() const {
    Json rows = Json::array();
    for (const auto& [id,b] : bots_) {
        const auto& a = actors_.at(id); const auto& s = a.character.at("state");
        const char* activity = s.at("hp").get<int>() == 0 ? "dead" : a.engaged ? "fighting" : b.resting ? "resting" : b.errand ? "errand"
                             : b.lost ? "lost" : b.leader ? "following" : a.moving ? "walking" : "idle";
        rows.push_back({{"id",id},{"character_id",a.character_id},{"name",a.character.at("name")},{"class_id",a.character.at("class_id")},{"level",s.at("level")},
                        {"behaviour",b.behaviour},{"zone",s.at("zone")},{"x",s.at("x")},{"y",s.at("y")},{"state",activity},{"hp",s.at("hp")},{"leader",b.leader}});
    }
    return rows;
}
std::vector<std::pair<int64_t,int64_t>> World::released_bots() { return std::exchange(released_bots_, {}); }

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
    // NONCANONICAL creation kit: the default bag plus the best class weapon of the bot's level, so an administrator-chosen level can hunt.
    const auto cap = config_.at("resource_cap").get<int>();
    Json state{{"level",level},{"hp",cap},{"mana",cap},{"inventory",Json::array()}};
    for (const auto& item : config_.at("defaults").at("inventory")) state["inventory"].push_back({{"uid",uid()},{"item",item.at("item")},{"quantity",item.at("quantity")}});
    std::string weapon;
    for (const auto& [key,item] : config_.at("items").items())
        if (item.value("slot","") == "weapon" && item.at("price").get<int64_t>() > 0 && fits(item,level,class_id,sex) &&
            (weapon.empty() || item.at("level").get<int>() > config_.at("items").at(weapon).at("level").get<int>())) weapon = key;
    if (!weapon.empty() && !std::any_of(state["inventory"].begin(),state["inventory"].end(),[&](const Json& e){return config_.at("items").at(e.at("item").get<std::string>()).value("slot","") == "weapon";}))
        state["inventory"].push_back({{"uid",uid()},{"item",weapon},{"quantity",1}});
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

bool World::bot_act(uint32_t id, const Json& request) {
    try { handle(id, request); return true; } catch (...) { return false; } // refused like a client request (rule, range, cooldown, store)
}
// Walk toward (x,y) in straight legs of at most aoi/4 that clear_path() accepts, fanning out ±30°/60°/90° around the direct
// heading, like a player clicking visible ground. A* per bot think (1000-node searches that fail across the original support
// grids) was the bot tick cost, so only after four thinks without progress (legs circling an obstacle) does the bot plan the
// whole walk with one larger route() search, at most one such search per world tick; false when that fails.
bool World::bot_walk(uint32_t id, Actor& a, Bot& b, int32_t x, int32_t y) {
    const auto& s = a.character.at("state"); const std::string zone = s.at("zone"); const int32_t sx = at(s,"x"), sy = at(s,"y");
    const double dist = std::sqrt(double(distance2(sx,sy,x,y))), leg = std::min(dist, config_.at("aoi").get<double>()/4);
    if (std::abs(int64_t(x)-b.goal_x)+std::abs(int64_t(y)-b.goal_y) > config_.at("aoi").get<int64_t>()/8) { b.goal_x = x; b.goal_y = y; b.goal_best = INT64_MAX; b.stalls = 0; b.routed = false; }
    if (b.routed && a.moving) return true; // following the searched route
    b.routed = false;
    if (const auto d = distance2(sx,sy,x,y); d < b.goal_best - b.goal_best/10) { b.goal_best = d; b.stalls = 0; } else ++b.stalls;
    const double progress = std::max(dist - leg/2, config_.at("interaction_range").get<double>()/2);
    if (a.moving && !a.route.empty() && double(distance2(a.route.back().first,a.route.back().second,x,y)) <= progress*progress) return true;
    if (dist < 1) return false;
    if (b.stalls >= 4) {
        if (bot_search_tick_ == tick_) return true; // another bot searched this tick: retry next think
        bot_search_tick_ = tick_; b.stalls = 0;
        auto points = route(zone,sx,sy,x,y,setting(config_,"route_nodes",30000));
        if (points.empty()) return false;
        a.route = std::move(points); a.waypoint = 0; a.moving = true; a.dx = a.dy = 0; a.engaged = 0; b.routed = true; // as destination()
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
    if (found == npcs_.end() || found->second.definition.at("zone") != s.at("zone")) { b.errand = 0; return false; }
    if (close(id,target,config_.at("interaction_range"))) {
        b.errand = 0; b.stuck = 0; bool ok = true;
        if (request.at("action") == "bot_shop") { // sell the junk, then top up healing potions
            auto own = s; const auto& shop = found->second.definition.at("shop");
            for (const auto& entry : own.at("inventory")) {
                const auto& item = config_.at("items").at(entry.at("item").get<std::string>()); bool keep = item.contains("heal") || item.contains("mana") || item.at("price").get<int64_t>() <= 0;
                for (const auto& [slot,worn] : own.at("equipment").items()) { (void)slot; keep = keep || worn == entry.at("uid"); }
                for (const auto& [quest,p] : own.at("quests").items()) { (void)p; keep = keep || config_.at("quests").at(quest).at("objective").value("item",Json()) == entry.at("item"); }
                if (!keep) bot_act(id,{{"action","sell"},{"target",target},{"uid",entry.at("uid")},{"quantity",entry.at("quantity")}});
            }
            const auto& now = a.character.at("state"); int potions = 0; std::string best;
            for (const auto& entry : now.at("inventory")) if (config_.at("items").at(entry.at("item").get<std::string>()).contains("heal")) potions += entry.at("quantity").get<int>();
            for (const auto& item : shop) { const auto& d = config_.at("items").at(item.get<std::string>());
                if (d.contains("heal") && d.at("level").get<int>() <= now.at("level").get<int>() && (best.empty() || d.at("heal").get<int>() > config_.at("items").at(best).at("heal").get<int>())) best = item; }
            if (!best.empty() && potions < 5) {
                const int64_t price = config_.at("items").at(best).at("price"), count = std::min<int64_t>(5-potions, now.at("currency").get<int64_t>()/2/std::max<int64_t>(1,price));
                if (count > 0) bot_act(id,{{"action","buy"},{"target",target},{"item",best},{"quantity",count}});
            }
        } else ok = bot_act(id,request);
        if (!ok && found->second.definition.at("kind") == "door") { b.blocked.insert(target); b.unblock = tick_ + 1200; }
        if (!ok) b.quest_ready = tick_ + 600; // a refused chore waits before it is retried
        return ok;
    }
    const auto give_up = [&] { b.errand = 0; b.stuck = 0; b.quest_ready = tick_ + 600; b.blocked.insert(target); b.unblock = tick_ + 1200; return false; };
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
    (void)b; const auto& s = a.character.at("state"); const int class_id = a.character.at("class_id");
    const auto ratio = [&](const Json& v) { return v.at("hp").get<int>()*100/std::max(1,v.at("max_hp").get<int>()); };
    if (ratio(s) < 35) for (const auto& entry : s.at("inventory")) { // potion
        const auto& item = config_.at("items").at(entry.at("item").get<std::string>());
        if (item.contains("heal") && item.at("level").get<int>() <= s.at("level").get<int>() && bot_act(id,{{"action","use"},{"uid",entry.at("uid")}})) return true;
    }
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
        if (bot_act(id,request)) return true;
    }
    return false;
}

bool World::bot_upkeep(uint32_t id, Actor& a, Bot& b) {
    const auto& s = a.character.at("state"); const std::string zone = s.at("zone");
    const int level = s.at("level"), class_id = a.character.at("class_id"), sex = a.character.at("appearance").value("sex",1);
    // Yetenekler: spend points on the lowest-ranked damage/heal skill (and the stance a technique requires).
    int points = config_.at("skill_points_base").get<int>() + config_.at("skill_points_per_level").get<int>()*(level-1);
    for (const auto& [skill,rank] : s.at("skills").items()) { (void)skill; points -= rank.get<int>(); }
    if (points > 0) {
        std::vector<std::string> useful;
        for (const auto& [skill,d] : config_.at("skills").items())
            if (!d.contains("replaces") && d.at("source")[0].get<int>()+1 == class_id && d.at("level").get<int>() <= level && (d.contains("damage") || d.contains("heal"))) {
                useful.push_back(skill); if (d.contains("requires")) useful.push_back(d.at("requires"));
            }
        Json ranks = Json::object();
        for (; points > 0 && !useful.empty(); --points) {
            const auto rank = [&](const std::string& k) { return s.at("skills").value(k,0) + ranks.value(k,0); };
            const auto pick = *std::min_element(useful.begin(),useful.end(),[&](const auto& l, const auto& r){return rank(l) < rank(r);});
            if (rank(pick) >= config_.at("skill_rank_max").get<int>()) break;
            ranks[pick] = ranks.value(pick,0)+1;
        }
        if (!ranks.empty() && bot_act(id,{{"action","skill_learn"},{"skills",ranks}})) return true;
    }
    // Ganimet of own kills: open (currency), then take what fits.
    for (auto corpse : bot_npcs("enemy",zone)) {
        const auto& n = npcs_.at(corpse);
        if (n.hp > 0 || n.owner != id || n.looted || !n.drop.is_array() || !close(id,corpse,config_.at("aoi"))) continue;
        if (!close(id,corpse,config_.at("interaction_range"))) return bot_act(id,{{"action","loot"},{"target",corpse}}); // walks over
        bot_act(id,{{"action","loot"},{"target",corpse}});
        for (size_t slot = 0; slot < n.drop.size() && a.character.at("state").at("inventory").size() < config_.at("inventory_slots").get<size_t>(); ++slot)
            if (n.drop[slot].is_object()) bot_act(id,{{"action","loot_take"},{"target",corpse},{"slot",slot}});
        return true;
    }
    // Wield the best fitting weapon carried.
    {
        const Json* worn = s.at("equipment").contains("weapon") ? &config_.at("items").at(owned(s,s.at("equipment").at("weapon")).at("item").get<std::string>()) : nullptr;
        for (const auto& entry : s.at("inventory")) {
            const auto& item = config_.at("items").at(entry.at("item").get<std::string>());
            if (item.value("slot","") == "weapon" && fits(item,level,class_id,sex) && (!worn || item.at("level").get<int>() > worn->at("level").get<int>()))
                if (bot_act(id,{{"action","equip"},{"uid",entry.at("uid")}})) return true;
        }
    }
    if (b.errand) return bot_reach(id,a,b,b.errand,b.request);
    if (tick_ < b.quest_ready) return false;
    // Chores: a worn-down piece goes to the örs; a full bag or an empty potion belt to a merchant.
    const auto nearest = [&](const std::string& kind) {
        uint32_t best = 0; int64_t d = INT64_MAX;
        for (auto n : bot_npcs(kind,zone)) if (const auto e = distance2(at(s,"x"),at(s,"y"),npcs_.at(n).x,npcs_.at(n).y); e < d) { d = e; best = n; }
        return best;
    };
    bool worn_down = false;
    for (const auto& [slot,uid] : s.at("equipment").items()) {
        (void)slot; const auto& entry = owned(s,uid.get<std::string>()); const auto& item = config_.at("items").at(entry.at("item").get<std::string>());
        worn_down = worn_down || (entry.contains("durability") && item.contains("durability") && entry.at("durability").get<int>()*4 < item.at("durability").get<int>());
    }
    if (worn_down) if (const auto anvil = nearest("anvil")) return bot_reach(id,a,b,anvil,{{"action","repair"},{"target",anvil}});
    int potions = 0; for (const auto& entry : s.at("inventory")) if (config_.at("items").at(entry.at("item").get<std::string>()).contains("heal")) potions += entry.at("quantity").get<int>();
    if (s.at("inventory").size()+3 >= config_.at("inventory_slots").get<size_t>() || (potions == 0 && s.at("currency").get<int64_t>() >= 100))
        if (const auto merchant = nearest("merchant")) return bot_reach(id,a,b,merchant,{{"action","bot_shop"},{"target",merchant}});
    b.quest_ready = tick_ + 400;
    // Görevler: hand in a finished quest here, else take one this zone can serve (at most three at once).
    for (const auto& [quest,p] : s.at("quests").items()) {
        const auto& q = config_.at("quests").at(quest); const auto& o = q.at("objective");
        const int count = o.at("kind") == "talk" ? 1 : o.at("count").get<int>();
        const uint32_t to = q.contains("turn_in") ? q.at("turn_in").get<uint32_t>() : q.at("giver").get<uint32_t>();
        if (p.at("progress").get<int>() >= count && npcs_.contains(to) && npcs_.at(to).definition.at("zone") == zone)
            return bot_reach(id,a,b,to,{{"action","quest_complete"},{"target",to},{"quest",quest}});
    }
    if (s.at("quests").size() >= 3) return false;
    const auto& enemies = bot_npcs("enemy",zone);
    uint32_t pick = 0; std::string chosen; int64_t closest = INT64_MAX;
    for (auto giver : bot_npcs("quest",zone)) {
        const auto& g = npcs_.at(giver); const auto away = distance2(at(s,"x"),at(s,"y"),g.x,g.y);
        if (b.blocked.contains(giver) || away >= closest) continue;
        for (const auto& offered : quests_at(g,s)) {
            const std::string quest = offered; const auto& q = config_.at("quests").at(quest); const auto& o = q.at("objective");
            const auto& done = s.at("quests_done");
            if (s.at("quests").contains(quest) || std::binary_search(done.begin(),done.end(),offered) || q.at("level").get<int>() > level ||
                (q.contains("requires") && !std::binary_search(done.begin(),done.end(),q.at("requires")))) continue;
            bool doable;
            if (o.at("kind") == "talk") { const uint32_t to = q.value("turn_in",q.at("giver")).get<uint32_t>(); doable = npcs_.contains(to) && npcs_.at(to).definition.at("zone") == zone; }
            else doable = std::any_of(enemies.begin(),enemies.end(),[&](uint32_t e){const auto& d = npcs_.at(e).definition; return d.value("template",Json()) == o.at("target") && d.value("level",1) <= level+2;});
            if (doable) { pick = giver; chosen = quest; closest = away; break; }
        }
    }
    return pick && bot_reach(id,a,b,pick,{{"action","quest_accept"},{"target",pick},{"quest",chosen}});
}

void World::bot_town(uint32_t id, Actor& a, Bot& b) {
    if (a.moving || tick_ < b.idle_until) return;
    const auto& s = a.character.at("state");
    const auto& chat = settings(config_).value("chat", Json::array());
    if (tick_ >= b.chat_ready && !chat.empty()) {
        bot_act(id,{{"action","chat"},{"channel","local"},{"text",chat[roll(uint32_t(chat.size()))]}});
        const uint32_t period = setting(config_,"chat_seconds",300); b.chat_ready = tick_ + 20*(period/2 + roll(period+1));
    }
    const auto r = roll(100); const int radius = config_.at("aoi").get<int>()/4;
    if (r < 55) { // wander: a walkable spot around the town anchor, sometimes beside a townsfolk NPC
        const std::string zone = s.at("zone"); const bool home = zone == b.home;
        int32_t cx = home ? b.anchor_x : at(s,"x"), cy = home ? b.anchor_y : at(s,"y");
        if (const auto& folk = bot_npcs("merchant",zone); roll(4) == 0 && !folk.empty()) {
            const auto& n = npcs_.at(folk[roll(uint32_t(folk.size()))]);
            if (distance2(n.x,n.y,cx,cy) <= int64_t(radius)*radius*4) { cx = n.x; cy = n.y; }
        }
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
}

void World::bot_hunt(uint32_t id, Actor& a, Bot& b) {
    const auto& s = a.character.at("state"); const std::string zone = s.at("zone"); const int level = s.at("level");
    const auto ratio = [&](const char* now, const char* most) { return s.at(now).get<int>()*100/std::max(1,s.at(most).get<int>()); };
    if (b.resting) { // regeneration runs while not fighting; /otur shows it
        if (ratio("hp","max_hp") < 95 || ratio("mana","max_mana") < 80) return;
        b.resting = false; bot_act(id,{{"action","chat"},{"channel","local"},{"text","/kalk"}});
    }
    if (ratio("hp","max_hp") < 55 || ratio("mana","max_mana") < 20) { b.resting = true; b.errand = 0; bot_act(id,{{"action","chat"},{"channel","local"},{"text","/otur"}}); return; }
    if (bot_upkeep(id,a,b)) return;
    // Hunting ground: here or the nearest zone through doors with monsters of a fitting level.
    const auto suits = [&](const Json& d) { const int l = d.value("level",1); return l <= level+2 && l >= level-6 && !d.value("banner",false); };
    const auto zones = bot_zones(b,zone); std::string ground;
    for (const auto& [z,door] : zones) { (void)door; for (auto e : bot_npcs("enemy",z)) if (suits(npcs_.at(e).definition)) { ground = z; break; } if (!ground.empty()) break; }
    if (!ground.empty() && ground != base_zone(config_,zone) && ground != zone) { if (bot_travel(id,a,b,ground)) return; }
    // Quarry: keep the current one while it stays a fair target, else a free monster (not another player's kill), quest targets
    // first, then the nearest.
    const auto fair = [&](uint32_t e) { const auto f = npcs_.find(e); return f != npcs_.end() && f->second.hp > 0 && f->second.definition.at("zone") == zone &&
                                        suits(f->second.definition) && (!f->second.owner || f->second.owner == id) && !b.blocked.contains(e); };
    uint32_t quarry = b.quarry && fair(b.quarry) ? b.quarry : 0; int64_t best = INT64_MAX;
    if (!quarry) for (auto e : bot_npcs("enemy",zone)) {
        if (!fair(e)) continue;
        const auto& n = npcs_.at(e); int64_t d = distance2(at(s,"x"),at(s,"y"),n.x,n.y);
        for (const auto& [quest,p] : s.at("quests").items()) { (void)p; if (config_.at("quests").at(quest).at("objective").value("target",Json()) == n.definition.value("template",Json())) d /= 4; }
        if (d < best) { best = d; quarry = e; }
    }
    if (!quarry) { if (!a.moving) bot_town(id,a,b); return; }
    // Approach in legs, attack from a quarter view; a quarry the engine keeps calling unreachable is skipped for a minute.
    if (b.quarry != quarry) { b.quarry = quarry; b.misses = 0; b.attacked = false; }
    if (b.attacked && !a.engaged) { b.attacked = false; ++b.misses; }
    if (b.misses >= 3) { b.blocked.insert(quarry); b.unblock = tick_ + 1200; b.quarry = 0; return; }
    const auto& n = npcs_.at(quarry); const int64_t near = config_.at("aoi").get<int64_t>()/4;
    if (distance2(at(s,"x"),at(s,"y"),n.x,n.y) > near*near) { if (!bot_walk(id,a,b,n.x,n.y)) ++b.misses; return; }
    b.attacked = bot_act(id,{{"action","attack"},{"target",quarry}});
}

void World::bot_follow(uint32_t id, Actor& a, Bot& b) {
    const auto& leader = actors_.at(b.leader); const auto& ls = leader.character.at("state"); const auto& s = a.character.at("state");
    if (ls.at("zone") != s.at("zone")) {
        if (bot_travel(id,a,b,ls.at("zone"))) { b.lost = false; return; }
        if (!b.lost) { b.lost = true; bot_act(id,{{"action","chat"},{"channel","group"},{"text","Seni bulamıyorum, bu kapıdan geçemiyorum."}}); }
        return;
    }
    b.lost = false;
    // Assist: the leader's target, or whatever bites the leader.
    uint32_t foe = 0;
    if (const auto t = npcs_.find(leader.engaged); t != npcs_.end() && t->second.hp > 0 && t->second.definition.at("kind") == "enemy") foe = leader.engaged;
    if (!foe) for (auto e : bot_npcs("enemy",s.at("zone"))) if (npcs_.at(e).hp > 0 && npcs_.at(e).threat.contains(b.leader)) { foe = e; break; }
    if (foe && a.engaged != foe && bot_act(id,{{"action","attack"},{"target",foe}})) return;
    if (a.engaged) return;
    if (bot_upkeep(id,a,b)) return;
    const int follow = config_.at("aoi").get<int>()/10;
    if (distance2(at(s,"x"),at(s,"y"),at(ls,"x"),at(ls,"y")) > int64_t(follow)*follow) {
        const int spread = follow/3;
        bot_walk(id,a,b,at(ls,"x")+int32_t(roll(uint32_t(2*spread+1)))-spread,at(ls,"y")+int32_t(roll(uint32_t(2*spread+1)))-spread);
    }
}

void World::bot_think(uint32_t id, Actor& a, Bot& b) {
    const auto& s = a.character.at("state");
    if (b.unblock && tick_ >= b.unblock) { b.blocked.clear(); b.unblock = 0; }
    if (s.at("hp").get<int>() == 0) { b.resting = false; b.errand = 0; if (tick_ >= a.resurrect_ready) bot_act(id,{{"action","resurrect"}}); return; }
    // Companion: accept a player's invitation; leave on /dismiss (group or whisper), Gruptan Çıkar or the leader's logout.
    if (b.behaviour == "companion" && !b.leader) if (const auto invite = invites_.find(id); invite != invites_.end()) {
        const auto from = invite->second;
        if (actors_.contains(from) && !actors_.at(from).bot && bot_act(id,{{"action","group_accept"}})) {
            b.leader = from; b.errand = 0; b.inbox = Json::array(); bot_act(id,{{"action","chat"},{"channel","group"},{"text","Emrindeyim, yol göster!"}});
        } else invites_.erase(id);
    }
    if (b.leader) {
        bool dismissed = !actors_.contains(b.leader) || !group(id) || group(id) != group(b.leader);
        for (const auto& message : b.inbox) {
            const std::string text = message.at("text");
            dismissed = dismissed || (message.at("from") == b.leader && (text.starts_with("/dismiss") || text.starts_with("/ayrıl")));
        }
        b.inbox = Json::array();
        if (dismissed) {
            if (group(id)) { bot_act(id,{{"action","chat"},{"channel","group"},{"text","Allahaısmarladık!"}}); bot_act(id,{{"action","group_leave"}}); }
            b.leader = 0; b.lost = false; b.errand = 0;
        }
    }
    b.inbox = Json::array();
    // Defence: answer any monster biting this bot.
    if (!a.engaged) for (auto e : bot_npcs("enemy",s.at("zone"))) if (npcs_.at(e).hp > 0 && npcs_.at(e).threat.contains(id)) { b.resting = false; bot_act(id,{{"action","attack"},{"target",e}}); break; }
    if (bot_fight(id,a,b) || a.engaged) return;
    if (b.leader) bot_follow(id,a,b);
    else if (b.behaviour == "hunt") bot_hunt(id,a,b);
    else bot_town(id,a,b);
}

void World::bots_tick() {
    if (bots_.empty()) return;
    const uint32_t think = std::max(1, setting(config_,"think_ticks",10));
    std::vector<uint32_t> due; for (const auto& [id,b] : bots_) if (tick_ >= b.next_think) due.push_back(id);
    for (auto id : due) {
        const auto found = bots_.find(id); if (found == bots_.end()) continue;
        auto& a = actors_.at(id);
        if (a.kicked) { remove(id); continue; } // a moderator kick despawns the bot; its lease is released by the host
        found->second.next_think = tick_ + think;
        try { bot_think(id,a,found->second); } catch (const std::exception&) {} // one bot's misjudgement never stops the world tick
    }
}
}
