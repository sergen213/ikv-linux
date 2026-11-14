// NONCANONICAL wave 30: persistent group-zone/raid lockouts and raids (the original game had groups of six and group zones,
// "Grup bölgeleri", but no raids and no lockout table; docs/SERVER_ARCHITECTURE.md "Instances, lockouts and raids").
//
// Lockouts: a Şef's kill credit in a group-zone copy (activities.lockout seconds) or a raid copy (zone raid.lockout) is stamped
// into the character state as before and also handed to the lockout sink (the server stores it with Store::set_lockout), and
// admission merges the stored lockouts back (record "lockouts", from Store::lockouts). A lockout therefore survives a
// reconnect, a lost state save and a phase transfer to another shard of the same database.
//
// Raids: a group leader converts the group into a raid (raid_convert) and becomes its leader; the raid leader invites solo
// players (they join the first party with room) or group leaders (their whole group becomes a new party). At most
// activities.raid.groups parties of six (4×6 = 24 ≤ the 32-player shard cap). Each party is mirrored into groups_ so the
// party frames, group chat, group-zone copies and party rows keep their per-group behaviour. Raid zones carry
// "raid": {min, lockout} next to "instances": a raid of at least `min` members enters one copy together (instance()), a
// player outside a raid still enters the shared zone as before. Şefs in raid copies are scaled by activities.raid
// boss_hp/boss_damage/boss_xp. Kill XP and the random Ganimet owner are shared by every raid member in range (sharers()).
#include "ikv/world.hpp"
#include <algorithm>
#include <ctime>
#include <stdexcept>
#include <string>

namespace ikv {
namespace {
constexpr size_t party_size = 6; // the original group size (group_invite "Grup dolu.")
[[noreturn]] void fail(const std::string& message) { throw std::invalid_argument("gameplay config: raid: " + message); }
int64_t whole(const Json& v, int64_t lo, int64_t hi, const std::string& what) {
    if (!v.is_number_integer() || v.get<int64_t>() < lo || v.get<int64_t>() > hi) fail(what + " must be an integer in range");
    return v.get<int64_t>();
}
int64_t now() { return int64_t(std::time(nullptr)); }
}

void World::set_lockout_sink(std::function<void(int64_t, const std::string&, int64_t)> sink) { lockout_sink_ = std::move(sink); }

void World::raid_validate() {
    auto& activities = config_.at("activities");
    size_t groups = 0;
    if (activities.contains("raid")) {
        const auto& r = activities.at("raid");
        if (!r.is_object()) fail("activities.raid must be an object");
        groups = size_t(whole(r.at("groups"), 2, 4, "groups")); // 4×6 = 24 stays inside the 32-player shard cap
        for (const char* key : {"boss_hp", "boss_damage", "boss_xp"}) whole(r.at(key), 1, 20, key);
    }
    for (const auto& [name, zone] : config_.at("zones").items()) {
        if (!zone.contains("raid") || zone.contains("instance_of")) continue; // copies inherit the validated block
        if (!groups) fail("zone " + name + " is a raid zone but activities.raid is missing");
        if (!zone.contains("instances")) fail("raid zone " + name + " needs instance copies");
        const auto& rule = zone.at("raid");
        if (!rule.is_object()) fail("zone " + name + " raid block must be an object");
        whole(rule.at("min"), 2, int64_t(groups * party_size), "zone " + name + " raid.min");
        whole(rule.at("lockout"), 0, 2592000, "zone " + name + " raid.lockout");
    }
    if (!groups) return;
    // Scaled Şefs in raid copies (the shared zone keeps the original values).
    const auto& r = activities.at("raid");
    const int hp = r.at("boss_hp"), damage = r.at("boss_damage"), xp = r.at("boss_xp");
    for (auto& [id, n] : npcs_) {
        (void)id; auto& d = n.definition;
        const auto& zone = config_.at("zones").at(d.at("zone").get<std::string>());
        if (!d.value("boss", false) || !zone.contains("raid") || !zone.contains("instance_of")) continue;
        d["hp"] = d.at("hp").get<int>() * hp; n.hp = d.at("hp");
        if (d.contains("damage")) d["damage"] = d.at("damage").get<int>() * damage;
        if (d.contains("damage_range")) for (auto& v : d["damage_range"]) v = v.get<double>() * damage;
        if (d.contains("skills")) for (auto& s : d["skills"]) s["damage"] = s.at("damage").get<int>() * damage;
        d["xp"] = d.at("xp").get<int>() * xp;
        d["raid_boss"] = true;
    }
}

uint32_t World::raid_of(uint32_t id) const {
    for (const auto& [key, r] : raids_) for (const auto& p : r.parties) if (std::find(p.begin(), p.end(), id) != p.end()) return key;
    return 0;
}
std::vector<uint32_t> World::raid_members(uint32_t raid) const {
    std::vector<uint32_t> members;
    if (const auto r = raids_.find(raid); r != raids_.end()) for (const auto& p : r->second.parties) members.insert(members.end(), p.begin(), p.end());
    return members;
}

void World::raid_sync(uint32_t raid, const std::vector<uint32_t>& gone) {
    const auto& r = raids_.at(raid);
    const auto members = raid_members(raid);
    const auto involved = [&](uint32_t m) { return std::find(members.begin(), members.end(), m) != members.end() || std::find(gone.begin(), gone.end(), m) != gone.end(); };
    for (auto it = groups_.begin(); it != groups_.end();) if (std::any_of(it->second.begin(), it->second.end(), involved)) it = groups_.erase(it); else ++it;
    const size_t groups = config_.at("activities").at("raid").at("groups");
    Json parties = Json::array();
    for (size_t p = 0; p < groups; ++p) { parties.push_back(r.parties[p]); if (!r.parties[p].empty()) groups_[r.parties[p].front()] = r.parties[p]; }
    for (const auto& p : r.parties) for (auto m : p) {
        emit(m, {{"type", "raid"}, {"leader", r.leader}, {"parties", parties}});
        emit(m, {{"type", "group"}, {"leader", p.front()}, {"members", p}});
    }
}

void World::raid_disband(uint32_t raid) {
    const auto r = raids_.at(raid); raids_.erase(raid);
    for (auto it = raid_invites_.begin(); it != raid_invites_.end();) if (it->second == raid) it = raid_invites_.erase(it); else ++it;
    for (const auto& p : r.parties) {
        for (auto it = groups_.begin(); it != groups_.end();) if (!p.empty() && it->first == p.front()) it = groups_.erase(it); else ++it;
        if (p.size() > 1) groups_[p.front()] = p; // each party of two or more stays a plain group
        for (auto m : p) {
            emit(m, {{"type", "raid"}, {"leader", 0}, {"parties", Json::array()}});
            emit(m, p.size() > 1 ? Json{{"type", "group"}, {"leader", p.front()}, {"members", p}} : Json{{"type", "group"}, {"leader", 0}, {"members", Json::array()}});
        }
    }
}

void World::raid_remove(uint32_t id) {
    const auto raid = raid_of(id); if (!raid) return;
    auto& r = raids_.at(raid);
    for (auto& p : r.parties) std::erase(p, id);
    for (auto it = groups_.begin(); it != groups_.end();) if (std::find(it->second.begin(), it->second.end(), id) != it->second.end()) it = groups_.erase(it); else ++it;
    emit(id, {{"type", "raid"}, {"leader", 0}, {"parties", Json::array()}});
    emit(id, {{"type", "group"}, {"leader", 0}, {"members", Json::array()}});
    const auto members = raid_members(raid);
    if (members.size() < 2) { raid_disband(raid); return; } // a raid of one is no raid
    if (r.leader == id) r.leader = members.front(); // succession: the first member of the first party leads
    raid_sync(raid);
}

void World::raid_action(uint32_t id, const std::string& action, const Json& request) {
    const auto refuse = [](const char* code, std::string message) { throw Refusal{code, std::move(message)}; };
    const auto& activities = config_.at("activities");
    if (!activities.contains("raid")) refuse("raid", "Bu sunucuda baskınlar kapalı.");
    const size_t groups = activities.at("raid").at("groups");
    const auto target = [&]() -> uint32_t {
        const auto& v = request.contains("target") ? request.at("target") : Json();
        if (!v.is_number_integer() || v.get<int64_t>() <= 0 || v.get<int64_t>() > int64_t(UINT32_MAX)) refuse("target", "Geçersiz hedef.");
        const auto t = v.get<uint32_t>(); if (!actors_.contains(t)) refuse("target", "Oyuncu oyunda değil.");
        return t;
    };
    const auto raid = raid_of(id);
    const bool leading = raid && raids_.at(raid).leader == id;
    const auto lead = [&]() { if (!leading) refuse("raid", "Bunu yalnızca baskın lideri yapabilir."); };
    // What a player brings into a raid: the whole group when leading one, else only themselves.
    const auto incoming = [&](uint32_t t) { const auto g = group(t); return g ? groups_.at(g) : std::vector<uint32_t>{t}; };
    const auto fits = [&](uint32_t r, const std::vector<uint32_t>& come) {
        const auto& parties = raids_.at(r).parties;
        if (raid_members(r).size() + come.size() > groups * party_size) return false;
        for (size_t p = 0; p < groups; ++p) if (come.size() > 1 ? parties[p].empty() : parties[p].size() < party_size) return true;
        return false;
    };
    if (action == "raid_convert") {
        if (raid) refuse("raid", "Zaten bir baskındasınız.");
        if (group(id) != id) refuse("raid", "Baskın kurmak için grup lideri olmalısınız.");
        const auto key = next_raid_++;
        auto& r = raids_[key]; r.leader = id; r.parties[0] = groups_.at(id);
        raid_sync(key); return;
    }
    if (action == "raid_invite") {
        lead(); const auto t = target();
        if (t == id || raid_of(t)) refuse("raid", "Bu kişi zaten bir baskında.");
        if (group(t) && group(t) != t) refuse("raid", "Yalnızca grup liderleri ya da grupsuz oyuncular baskına davet edilebilir.");
        const auto come = incoming(t);
        if (!fits(raid, come)) refuse("raid", "Baskın dolu.");
        raid_invites_[t] = raid;
        emit(t, {{"type", "raid_invite"}, {"from", id}, {"name", actors_.at(id).character.at("name")}, {"size", come.size()}});
        return;
    }
    if (action == "raid_accept") {
        const auto invite = raid_invites_.find(id);
        if (invite == raid_invites_.end() || !raids_.contains(invite->second)) { raid_invites_.erase(id); refuse("raid", "Baskın davetiniz yok."); }
        if (raid) refuse("raid", "Zaten bir baskındasınız.");
        if (group(id) && group(id) != id) refuse("raid", "Baskın davetini grup lideri kabul etmelidir.");
        const auto key = invite->second; raid_invites_.erase(invite);
        const auto come = incoming(id);
        if (!fits(key, come)) refuse("raid", "Baskın dolu.");
        auto& parties = raids_.at(key).parties;
        for (size_t p = 0; p < groups; ++p) {
            if (come.size() > 1 && parties[p].empty()) { parties[p] = come; break; }
            if (come.size() == 1 && parties[p].size() < party_size) { parties[p].push_back(id); break; }
        }
        for (auto m : come) invites_.erase(m);
        raid_sync(key); return;
    }
    if (action == "raid_decline") { raid_invites_.erase(id); return; }
    if (action == "raid_leave") { if (!raid) refuse("raid", "Bir baskında değilsiniz."); raid_remove(id); return; }
    if (action == "raid_kick") {
        lead(); const auto t = target();
        if (t == id || raid_of(t) != raid) refuse("raid", "Bu kişi baskınızda değil.");
        raid_remove(t); return;
    }
    if (action == "raid_move") {
        lead(); const auto t = target();
        if (raid_of(t) != raid) refuse("raid", "Bu kişi baskınızda değil.");
        const auto& v = request.contains("party") ? request.at("party") : Json();
        if (!v.is_number_integer() || v.get<int64_t>() < 0 || v.get<int64_t>() >= int64_t(groups)) refuse("raid", "Geçersiz baskın grubu.");
        auto& parties = raids_.at(raid).parties; auto& to = parties[v.get<size_t>()];
        if (std::find(to.begin(), to.end(), t) != to.end()) refuse("raid", "Bu kişi zaten o grupta.");
        if (to.size() >= party_size) refuse("raid", "Grup dolu.");
        for (auto& p : parties) std::erase(p, t);
        to.push_back(t);
        raid_sync(raid); return;
    }
    if (action == "raid_disband") { lead(); raid_disband(raid); return; }
    refuse("request", "Bilinmeyen baskın işlemi.");
}

Json World::raid_view(uint32_t id) const {
    const auto raid = raid_of(id); if (!raid) return nullptr;
    const auto& r = raids_.at(raid);
    const size_t groups = config_.at("activities").at("raid").at("groups");
    Json parties = Json::array(), rows = Json::array();
    for (size_t p = 0; p < groups; ++p) {
        parties.push_back(r.parties[p]);
        if (std::find(r.parties[p].begin(), r.parties[p].end(), id) != r.parties[p].end()) continue; // own party: "party" rows / entities
        for (auto m : r.parties[p]) {
            const auto& c = actors_.at(m).character; const auto& s = c.at("state");
            const int hp = s.at("hp"), max = std::max(1, s.at("max_hp").get<int>());
            const int percent = hp > 0 ? std::clamp(int(int64_t(hp) * 100 / max), 1, 100) : 0; // > 0 exactly while alive
            rows.push_back(Json::array({m, c.at("name"), c.at("class_id"), s.at("level"), percent,
                                        config_.at("zones").at(s.at("zone").get<std::string>()).value("source_registry_id", Json(nullptr))}));
        }
    }
    return {{"leader", r.leader}, {"parties", std::move(parties)}, {"rows", std::move(rows)}};
}

std::vector<uint32_t> World::sharers(uint32_t owner) const {
    std::vector<uint32_t> pool, result;
    if (const auto raid = raid_of(owner)) pool = raid_members(raid);
    else if (const auto leader = group(owner)) pool = groups_.at(leader);
    for (auto m : pool) if (close(owner, m, config_.at("aoi")) && actors_.at(m).character.at("state").at("hp").get<int>() > 0) result.push_back(m);
    if (result.empty()) result.push_back(owner);
    return result;
}

std::string World::entry(uint32_t id, const std::string& to) {
    const auto& zone = config_.at("zones").at(to);
    const auto& state = actors_.at(id).character.at("state");
    const auto until = state.value("lockouts", Json::object()).value(to, int64_t(0));
    const bool here = state.at("zone") == to; // a door inside its own zone (copies rewrite such doors to the copy)
    if (zone.contains("raid") && raid_of(id) && !here) {
        const auto destination = instance(id, to);
        if (destination == to) throw Refusal{"raid", "Bu baskın bölgesinin bütün kopyaları dolu; daha sonra tekrar deneyin."};
        const int min = zone.at("raid").at("min");
        if (raid_members(raid_of(id)).size() < size_t(min))
            throw Refusal{"raid", "Bu baskın bölgesine en az " + std::to_string(min) + " kişilik bir baskınla girilebilir."};
        if (until > now())
            throw Refusal{"lockout", "Bu baskın bölgesinin şefini yakın zamanda yendiniz; " + std::to_string((until - now() + 3599) / 3600) + " saat sonra tekrar girebilirsiniz."};
        return destination;
    }
    if (zone.contains("raid")) return to; // outside a raid (or a door inside the zone): the shared zone, as before wave 30
    // Group-zone lockout (wave 15 rule and message, unchanged).
    if (until > now() && zone.contains("instances"))
        throw Refusal{"lockout", "Bu grup bölgesinin şefini yakın zamanda yendiniz; " + std::to_string((until - now() + 59) / 60) + " dakika sonra tekrar girebilirsiniz."};
    return here ? to : instance(id, to);
}

std::pair<std::string, int64_t> World::stamp_lockout(Json& state, const Json& definition) const {
    if (!definition.value("boss", false)) return {};
    const auto& zone = config_.at("zones").at(definition.at("zone").get<std::string>());
    if (!zone.contains("instance_of")) return {};
    const auto base = zone.at("instance_of").get<std::string>();
    const int64_t seconds = zone.contains("raid") ? zone.at("raid").at("lockout").get<int64_t>() : config_.at("activities").at("lockout").get<int64_t>();
    const int64_t until = now() + seconds;
    state["lockouts"][base] = until;
    return {base, until};
}

void World::persist_lockouts(const std::vector<std::tuple<int64_t, std::string, int64_t>>& stamped) {
    if (!lockout_sink_) return;
    for (const auto& [character, zone, until] : stamped) {
        try { lockout_sink_(character, zone, until); }
        catch (const std::exception& e) { // the character state still holds the stamp; the next save carries it
            if (logs_.size() == 64) logs_.erase(logs_.begin());
            logs_.push_back({{"tick", tick_}, {"result", "rejected"}, {"code", "lockout_persistence"}, {"character", character}, {"zone", zone}});
        }
    }
}

void World::merge_lockouts(Json& state, const Json& record) const {
    if (state.contains("lockouts") && !state.at("lockouts").is_object()) return; // admission validation rejects it
    Json merged = state.value("lockouts", Json::object());
    for (auto it = merged.begin(); it != merged.end();) if (it->is_number_integer() && it->get<int64_t>() <= now()) it = merged.erase(it); else ++it;
    if (const auto stored = record.find("lockouts"); stored != record.end() && stored->is_object())
        for (const auto& [zone, until] : stored->items())
            if (!zone.empty() && zone.size() <= 32 && until.is_number_integer() && until.get<int64_t>() > now())
                if (!merged.contains(zone) || !merged.at(zone).is_number_integer() || merged.at(zone).get<int64_t>() < until.get<int64_t>()) merged[zone] = until;
    if (merged.empty()) state.erase("lockouts"); else state["lockouts"] = std::move(merged);
}
}
