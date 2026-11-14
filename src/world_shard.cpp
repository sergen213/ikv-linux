// NONCANONICAL wave 30 realms and shards (docs/plans/2026-11-14-wave30-realms.md, docs/SERVER_ARCHITECTURE.md "Shards").
// The original game listed several servers (launcher.exe 0x0048dbe0 construct_serverlist_url) but a client only ever talked
// to one; everything here is independent: by-name invites, whispers and guild invites that reach a character on another shard
// over the PostgreSQL bus, the party phase (the joiner reconnects to the leader's shard with a single-use ticket), the
// 60-second guest grace that sends an ungrouped guest home, guild chat and roster fan-out, and realm names on rows.
#include "ikv/world.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace ikv {
namespace {
constexpr uint32_t ticks_per_second = 20; // server_main tick_period 50 ms
constexpr uint32_t invite_ticks = 60 * ticks_per_second, guest_grace = 60 * ticks_per_second;
// Case-insensitive comparison of Turkish names (PostgreSQL lower() on the Store side): ASCII plus the Turkish capitals.
std::string folded(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c >= 'A' && c <= 'Z') { out.push_back(static_cast<char>(c + 32)); continue; }
        if (i + 1 < value.size()) {
            const auto n = static_cast<unsigned char>(value[i + 1]);
            const char* lower = nullptr;
            if (c == 0xC3 && n == 0x87) lower = "\xC3\xA7";      // Ç
            else if (c == 0xC3 && n == 0x96) lower = "\xC3\xB6"; // Ö
            else if (c == 0xC3 && n == 0x9C) lower = "\xC3\xBC"; // Ü
            else if (c == 0xC4 && n == 0x9E) lower = "\xC4\x9F"; // Ğ
            else if (c == 0xC4 && n == 0xB0) lower = "i";        // İ
            else if (c == 0xC5 && n == 0x9E) lower = "\xC5\x9F"; // Ş
            if (lower) { out += lower; ++i; continue; }
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}
// A name/realm field of a by-name request or bus body: 1..64 bytes without control characters, else empty.
std::string field(const Json& source, const char* key) {
    const auto found = source.find(key);
    if (found == source.end() || !found->is_string()) return {};
    auto value = found->get<std::string>();
    if (value.empty() || value.size() > 64 ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; })) return {};
    return value;
}
}

void World::shard_link(ShardLink link) { link_ = std::move(link); }
void World::shard_realms(std::map<int, std::string> names) { realm_names_ = std::move(names); }
int World::realm_of(uint32_t id) const { const auto found = actors_.find(id); return found == actors_.end() ? 1 : found->second.realm; }
std::string World::realm_name(int realm) const { const auto found = realm_names_.find(realm); return found == realm_names_.end() ? std::string() : found->second; }
int World::realm_id(const std::string& name) const {
    const auto wanted = folded(name);
    for (const auto& [id, value] : realm_names_) if (folded(value) == wanted) return id;
    return 0;
}
std::optional<std::string> World::realm_label(uint32_t viewer, uint32_t subject) const {
    const auto v = actors_.find(viewer), s = actors_.find(subject);
    if (v == actors_.end() || s == actors_.end() || v->second.realm == s->second.realm) return std::nullopt;
    auto name = realm_name(s->second.realm);
    if (name.empty() || name.size() > 64) return std::nullopt; // the client's realm field bound
    return name;
}
uint32_t World::find_player(int realm, const std::string& name) const {
    const auto wanted = folded(name);
    for (const auto& [id, a] : actors_)
        if (!a.bot && !a.kicked && (realm == 0 || a.realm == realm) && folded(a.character.at("name").get<std::string>()) == wanted) return id;
    return 0;
}
uint32_t World::find_character(int64_t character_id) const {
    for (const auto& [id, a] : actors_) if (a.character_id == character_id) return id;
    return 0;
}

// By-name forms of group_invite, private chat and guild_invite (native "/davet Ad[-Realm]", "/f Ad-Realm ", "/lonca Ad[-Realm]"):
// a character on this shard is handled exactly like the {target}/{to} form; one on another shard gets a bus message.
// Also the acceptance of an invite that arrived over the bus. Returns false for every other request.
bool World::shard_action(uint32_t id, const std::string& action, const Json& request) {
    const bool inviting = action == "group_invite" || action == "guild_invite";
    const bool whisper = action == "chat" && request.value("channel", std::string()) == "private" && !request.contains("to");
    if ((inviting && !request.contains("target") && request.contains("name")) || (whisper && request.contains("name"))) {
        auto& a = actors_.at(id);
        const auto name = field(request, "name");
        if (name.empty()) { reject(id, "target", "Geçersiz oyuncu adı."); return true; }
        int realm = 0;
        if (request.contains("realm")) {
            realm = realm_id(field(request, "realm"));
            if (!realm) { reject(id, "target", "Böyle bir diyar yok."); return true; }
        }
        // Without a realm, a player of the sender's own realm comes first, then anyone here, then the sender's realm elsewhere.
        uint32_t local = find_player(realm ? realm : a.realm, name);
        if (!local && !realm) local = find_player(0, name);
        if (local) {
            Json forwarded = request;
            forwarded.erase("name"); forwarded.erase("realm");
            forwarded[whisper ? "to" : "target"] = local;
            handle(id, forwarded);
            return true;
        }
        if (!realm) realm = a.realm;
        const auto current = a.character.at("state");
        const auto from_name = a.character.at("name").get<std::string>();
        if (action == "group_invite") {
            const auto leader = group(id);
            if (leader && leader != id) { reject(id, "group", "Yalnızca grup lideri davet edebilir."); return true; }
            if (leader && groups_.at(leader).size() >= 6) { reject(id, "group", "Grup dolu."); return true; }
        } else if (action == "guild_invite") {
            const bool member = current.contains("guild") && !current.at("guild").is_null();
            if (!member || current.at("guild").at("leader_character_id") != a.character_id) { reject(id, "guild", "Bunu yalnızca Lonca Reisi yapabilir."); return true; }
            if (current.at("guild").at("members").size() >= 64) { reject(id, "guild", "Lonca dolu."); return true; }
        } else {
            if (a.muted || a.externally_muted) { reject(id, "muted", "Konuşmanız yasaklandı."); return true; }
            if (tick_ < a.chat_ready) { reject(id, "rate", "Bu yazışma kanalını gereksiz yere kirletmemeye özen gösteriniz."); return true; }
            if (request.contains("link")) { reject(id, "chat", "Başka sunucudaki bir oyuncuya eşya bağlantısı gönderilemez."); return true; }
        }
        std::string text;
        if (whisper) {
            const auto& value = request.at("text");
            if (!value.is_string() || value.get_ref<const std::string&>().empty() || value.get_ref<const std::string&>().size() > 512) { reject(id, "chat", "Mesaj geçersiz."); return true; }
            text = value.get<std::string>();
            for (unsigned char c : text) if (c < 32 && c != '\t') { reject(id, "chat", "Mesajda geçersiz karakter var."); return true; }
        }
        std::optional<std::string> where;
        if (link_ && link_->locate) where = link_->locate(realm, name);
        if (!where || *where == link_->instance) { reject(id, "target", "Oyuncu oyunda değil."); return true; }
        Json body{{"to_name", name}, {"to_realm", realm}, {"from_char", a.character_id}, {"from_name", from_name}, {"from_realm", a.realm}};
        if (action == "group_invite") { body["kind"] = "invite"; body["leader_instance"] = link_->instance; }
        else if (action == "guild_invite") { body["kind"] = "guild_invite"; body["guild"] = current.at("guild"); body["instance"] = link_->instance; }
        else { body["kind"] = "whisper"; body["text"] = text; }
        link_->send(*where, body);
        if (whisper) {
            emit(id, {{"type", "chat"}, {"channel", "private"}, {"from", id}, {"name", from_name}, {"text", text}});
            a.chat_ready = tick_ + 10;
        } else emit(id, {{"type", "ack"}, {"action", action}});
        return true;
    }
    if (action == "group_accept" && !invites_.contains(id) && remote_invites_.contains(id)) {
        const auto invite = remote_invites_.at(id);
        if (tick_ > invite.at("expires").get<uint32_t>()) { remote_invites_.erase(id); reject(id, "group", "Grup davetinin süresi doldu."); return true; }
        if (group(id)) { reject(id, "group", "Zaten bir gruptasınız."); return true; }
        const auto instance = invite.at("leader_instance").get<std::string>();
        std::optional<ShardInfo> destination;
        if (link_ && link_->shard) destination = link_->shard(instance);
        if (!destination) { remote_invites_.erase(id); reject(id, "group", "Davet eden oyuncunun sunucusu şu anda kapalı."); return true; }
        if (destination->players >= destination->capacity) { reject(id, "group", "Hedef sunucu dolu; gruba katılamazsınız."); return true; }
        auto& a = actors_.at(id);
        remote_invites_.erase(id);
        if (trade(id)) cancel_trade(id);
        transfers_.push_back({id, instance, "party", {{"leader_char", invite.at("from_char")}, {"guest", true}}});
        link_->send(instance, {{"kind", "invite_answer"}, {"to_char", invite.at("from_char")}, {"from_char", a.character_id},
                               {"from_name", a.character.at("name")}, {"from_realm", a.realm}, {"accepted", true}});
        emit(id, {{"type", "ack"}, {"action", action}});
        return true;
    }
    return false;
}

void World::shard_message(const Json& body) {
    if (!body.is_object()) return;
    const auto kind = body.value("kind", std::string());
    const auto from_name = field(body, "from_name");
    const int from_realm = body.value("from_realm", 1);
    const auto tag = [&](uint32_t viewer, Json event) { // the sender's realm name when it differs from the viewer's
        if (from_realm != actors_.at(viewer).realm) if (auto name = realm_name(from_realm); !name.empty() && name.size() <= 64) event["realm"] = name;
        return event;
    };
    const auto addressed = [&]() -> uint32_t {
        if (body.contains("to_char")) return find_character(body.at("to_char").get<int64_t>());
        const auto name = field(body, "to_name");
        return name.empty() ? 0 : find_player(body.value("to_realm", 0), name);
    };
    const auto answer = [&](const std::string& reason) {
        if (link_ && body.contains("leader_instance"))
            link_->send(body.at("leader_instance").get<std::string>(), {{"kind", "invite_answer"}, {"to_char", body.at("from_char")}, {"accepted", false}, {"reason", reason}});
    };
    if (kind == "invite") {
        const auto t = addressed();
        if (!t) { answer("Oyuncu oyunda değil."); return; }
        if (group(t)) { answer("Bu kişi zaten bir grupta."); return; }
        Json invite = body; invite["expires"] = tick_ + invite_ticks;
        remote_invites_[t] = std::move(invite);
        invites_.erase(t); // the newest invitation wins
        emit(t, tag(t, {{"type", "group_invite"}, {"from", 0}, {"name", from_name}}));
    } else if (kind == "invite_answer") {
        const auto t = addressed();
        if (t && !body.value("accepted", false)) reject(t, "group", field(body, "reason").empty() ? "Davet reddedildi." : field(body, "reason"));
    } else if (kind == "whisper") {
        const auto t = addressed();
        if (t && !ignores(t, body.value("from_char", int64_t(0))))
            emit(t, tag(t, {{"type", "chat"}, {"channel", "private"}, {"from", 0}, {"name", from_name}, {"text", body.value("text", std::string())}}));
    } else if (kind == "kick_party") {
        if (const auto t = addressed()) leave_group(t);
    } else if (kind == "guild_invite") {
        const auto t = addressed();
        if (!t || !body.contains("guild") || !body.at("guild").is_object()) return;
        const auto& s = actors_.at(t).character.at("state");
        if (s.contains("guild") && !s.at("guild").is_null()) return;
        Json guild = shard_guild_latest(body.at("guild"));
        Json stored = guild; stored["remote"] = {{"from_char", body.at("from_char")}, {"instance", body.at("instance")}};
        guild_invites_[t] = std::move(stored);
        emit(t, tag(t, {{"type", "guild_invite"}, {"from", 0}, {"name", from_name}, {"guild", std::move(guild)}}));
    } else if (kind == "guild_chat") {
        const auto guild = body.value("guild", std::string());
        for (const auto& [id, a] : actors_) {
            const auto& s = a.character.at("state");
            if (!s.contains("guild") || s.at("guild").is_null() || s.at("guild").at("id") != guild || ignores(id, body.value("from_char", int64_t(0)))) continue;
            emit(id, tag(id, {{"type", "chat"}, {"channel", "guild"}, {"from", 0}, {"name", from_name}, {"text", body.value("text", std::string())}}));
        }
    } else if (kind == "guild_roster") {
        if (!body.contains("guild") || !body.at("guild").is_object()) return;
        const auto& guild = body.at("guild");
        const auto id = guild.at("id").get<std::string>();
        const auto revision = guild.at("revision").get<int64_t>();
        auto& known = guild_rosters_[id];
        if (known.is_object() && known.at("revision").get<int64_t>() >= revision) return;
        known = guild;
        for (auto& [actor_id, a] : actors_) {
            auto& s = a.character["state"];
            if (!s.contains("guild") || s.at("guild").is_null() || s.at("guild").at("id") != id || s.at("guild").at("revision").get<int64_t>() >= revision) continue;
            const auto& members = guild.at("members");
            if (std::find(members.begin(), members.end(), Json(a.character_id)) != members.end()) {
                s["guild"] = guild; // the registry already holds this revision; the next save writes identical JSON
                emit(actor_id, {{"type", "guild"}, {"guild", guild}});
            } else {
                s.erase("guild"); // kicked or left on another shard
                emit(actor_id, {{"type", "guild"}, {"guild", nullptr}});
            }
        }
    }
}

void World::shard_guild(const Json& guild) {
    if (!guild.is_object()) return;
    guild_rosters_[guild.at("id").get<std::string>()] = guild;
    if (link_ && link_->send) link_->send("*", {{"kind", "guild_roster"}, {"guild", guild}, {"revision", guild.at("revision")}});
}
void World::shard_guild_chat(const Json& guild, const Json& line) {
    if (!link_ || !link_->send || !line.contains("text") || line.at("text").get_ref<const std::string&>().empty()) return; // item links stay local
    const auto from = line.at("from").get<uint32_t>();
    link_->send("*", {{"kind", "guild_chat"}, {"guild", guild.at("id")}, {"from_char", actors_.at(from).character_id}, {"from_name", line.at("name")},
                      {"from_realm", realm_of(from)}, {"text", line.at("text")}});
}
Json World::shard_guild_latest(const Json& guild) const {
    Json latest = guild;
    const auto newer = [&](const Json& candidate) {
        if (candidate.is_object() && candidate.at("id") == latest.at("id") && candidate.at("revision").get<int64_t>() > latest.at("revision").get<int64_t>()) latest = candidate;
    };
    if (const auto known = guild_rosters_.find(guild.at("id").get<std::string>()); known != guild_rosters_.end()) newer(known->second);
    for (const auto& [id, a] : actors_) {
        (void)id;
        const auto& s = a.character.at("state");
        if (s.contains("guild")) newer(s.at("guild"));
    }
    return latest;
}

std::vector<ShardTransfer> World::shard_transfers() { return std::exchange(transfers_, {}); }
void World::shard_departing(uint32_t id, Json transfer) {
    const auto found = actors_.find(id);
    if (found == actors_.end()) return;
    found->second.departing = true;
    cancel_trade(id); leave_group(id); invites_.erase(id); remote_invites_.erase(id);
    emit(id, std::move(transfer));
}
void World::shard_failed(uint32_t id, const std::string& message) {
    const auto found = actors_.find(id);
    if (found == actors_.end()) return;
    found->second.departing = false;
    if (!message.empty()) reject(id, "transfer", message);
}
void World::shard_arrived(uint32_t id, const Json& payload) {
    const auto found = actors_.find(id);
    if (found == actors_.end() || !payload.is_object()) return;
    auto& a = found->second;
    a.guest = payload.value("guest", false);
    if (!payload.contains("leader_char")) return;
    const auto leader = find_character(payload.at("leader_char").get<int64_t>());
    const bool open = leader && leader != id && !group(id) && (!group(leader) || group(leader) == leader) &&
                      (!groups_.contains(leader) || groups_.at(leader).size() < 6);
    if (!open) { reject(id, "group", "Grup dolu ya da dağıldı; yine de bu sunucudasınız."); return; }
    if (!groups_.contains(leader)) groups_[leader] = {leader};
    groups_[leader].push_back(id);
    for (auto other : groups_.at(leader)) emit(other, {{"type", "group"}, {"leader", leader}, {"members", groups_.at(leader)}});
}
// A guest (home realm differs from this shard's, or phased here from another layer) with no group for 60 s is sent to a shard
// of its home realm; when none has room (or the least loaded is this one) it stays and is retried 60 s later.
void World::shard_tick() {
    if (!link_) return;
    for (auto& [id, a] : actors_) {
        if (a.bot || a.departing || !(a.guest || a.realm != link_->realm)) continue;
        if (group(id)) { a.alone_since = 0; continue; }
        if (!a.alone_since) {
            a.alone_since = tick_ + 1;
            reject(id, "guest", "Grubunuz olmadığı için bir dakika içinde kendi diyarınıza döneceksiniz.");
            continue;
        }
        if (tick_ + 1 - a.alone_since < guest_grace || tick_ < a.home_retry) continue;
        a.home_retry = tick_ + guest_grace;
        if (std::none_of(transfers_.begin(), transfers_.end(), [&](const ShardTransfer& t) { return t.id == id; }))
            transfers_.push_back({id, "", "home", {{"realm", a.realm}}});
    }
}
}
