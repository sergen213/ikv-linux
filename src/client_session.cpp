#include "ikv/client_session.hpp"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ikv {
namespace {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
constexpr auto io_timeout = std::chrono::seconds(5);
constexpr std::size_t queue_limit = 262144;
constexpr std::size_t pump_limit = 65536;
[[noreturn]] void invalid() { throw std::runtime_error("invalid server message"); }
[[noreturn]] void transport_error() { throw std::runtime_error("secure connection failed"); }

int remaining_ms(Clock::time_point deadline) {
    const auto remaining = deadline - Clock::now();
    if (remaining <= Clock::duration::zero()) throw std::runtime_error("secure connection timed out");
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count() + 1);
}
short wait_socket(int fd, short events, Clock::time_point deadline) {
    for (;;) {
        pollfd descriptor{fd, events, 0};
        const int ready = poll(&descriptor, 1, remaining_ms(deadline));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (descriptor.revents & POLLNVAL)) transport_error();
        if (ready == 0) continue;
        return descriptor.revents;
    }
}
short tls_want(SSL* ssl, int result) {
    const int error = SSL_get_error(ssl, result);
    if (error == SSL_ERROR_WANT_READ) return POLLIN;
    if (error == SSL_ERROR_WANT_WRITE) return POLLOUT;
    transport_error();
}

// OpenSSL's socket BIO writes without MSG_NOSIGNAL on Linux. Block SIGPIPE
// only on this thread, consume only a newly generated signal, and restore the
// caller's mask without changing process-wide signal handlers.
class NoSigpipe {
public:
    NoSigpipe() {
        sigemptyset(&blocked_);
        sigaddset(&blocked_, SIGPIPE);
        if (pthread_sigmask(SIG_BLOCK, &blocked_, &old_) != 0) transport_error();
        sigset_t pending{};
        sigpending(&pending);
        previously_pending_ = sigismember(&pending, SIGPIPE) == 1;
    }
    ~NoSigpipe() {
        if (!previously_pending_) {
            const timespec zero{};
            while (sigtimedwait(&blocked_, nullptr, &zero) < 0 && errno == EINTR) {}
        }
        pthread_sigmask(SIG_SETMASK, &old_, nullptr);
    }
private:
    sigset_t blocked_{};
    sigset_t old_{};
    bool previously_pending_ = false;
};

struct Resolution {
    std::mutex mutex;
    std::condition_variable ready;
    addrinfo* addresses = nullptr;
    int status = EAI_AGAIN;
    bool complete = false;
    ~Resolution() { if (addresses) freeaddrinfo(addresses); }
};
std::shared_ptr<Resolution> resolve(const std::string& host, std::uint16_t port, Clock::time_point deadline) {
    // A stalled system resolver must not stall the render thread or destructor.
    // Cap surviving workers as well as the caller's wait; workers own all data.
    static std::atomic<unsigned> workers{0};
    auto resolution = std::make_shared<Resolution>();
    if (workers.fetch_add(1) >= 8) {
        workers.fetch_sub(1);
        transport_error();
    }
    try {
        std::thread([resolution, host, service = std::to_string(port)] {
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            addrinfo* addresses = nullptr;
            const int status = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
            {
                std::lock_guard lock(resolution->mutex);
                resolution->addresses = addresses;
                resolution->status = status;
                resolution->complete = true;
            }
            resolution->ready.notify_one();
            workers.fetch_sub(1);
        }).detach();
    } catch (...) {
        workers.fetch_sub(1);
        transport_error();
    }
    std::unique_lock lock(resolution->mutex);
    if (!resolution->ready.wait_until(lock, deadline, [&] { return resolution->complete; }))
        throw std::runtime_error("secure connection timed out");
    if (resolution->status != 0 || !resolution->addresses) transport_error();
    return resolution;
}

std::int64_t integer(const Json& value, std::int64_t minimum, std::int64_t maximum) {
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) invalid();
    const auto number = value.get<std::int64_t>();
    if (number < minimum || number > maximum) invalid();
    return number;
}
void text(const Json& value, std::size_t maximum, bool allow_empty = false) {
    if (!value.is_string()) invalid();
    const auto& string = value.get_ref<const std::string&>();
    if ((!allow_empty && string.empty()) || string.size() > maximum || string.find('\0') != std::string::npos) invalid();
}
void array_limit(const Json& value, std::size_t maximum) {
    if (!value.is_array() || value.size() > maximum) invalid();
}
using Bounds = std::array<std::int32_t, 4>;
Bounds zone_bounds(const Json& body) {
    text(body.at("zone"), 128);
    const auto& source = body.at("source_registry_id");
    if (!source.is_null()) integer(source, 0, 70);
    const auto& values = body.at("bounds");
    if (!values.is_array() || values.size() != 4) invalid();
    Bounds bounds{};
    for (std::size_t i = 0; i < bounds.size(); ++i)
        bounds[i] = static_cast<std::int32_t>(integer(values[i], -coordinate_limit, coordinate_limit));
    if (bounds[0] >= bounds[2] || bounds[1] >= bounds[3]) invalid();
    if (!source.is_null() && bounds != Bounds{-coordinate_limit, -coordinate_limit, 1534500, 1534500}) invalid();
    return bounds;
}
bool contains(const Bounds& bounds, std::int32_t x, std::int32_t y) {
    return x >= bounds[0] && y >= bounds[1] && x <= bounds[2] && y <= bounds[3];
}
void coordinates(const Json& value, const Bounds& bounds) {
    const auto x = static_cast<std::int32_t>(integer(value.at("x"), -coordinate_limit, coordinate_limit));
    const auto y = static_cast<std::int32_t>(integer(value.at("y"), -coordinate_limit, coordinate_limit));
    if (!contains(bounds, x, y)) invalid();
}
// Efsun prefix ids (prefixes.dat uniqueid) carried by an item instance or a worn visual.
void prefix_ids(const Json& value) {
    array_limit(value, 2);
    for (const auto& id : value) integer(id, 1, 999999);
}
void visual_equipment(const Json& equipment) {
    array_limit(equipment, 16);
    std::uint16_t occupied = 0;
    for (const auto& item : equipment) {
        if (!item.is_object()) invalid();
        const auto slot = integer(item.at("slot"), 0, 15);
        const auto bit = static_cast<std::uint16_t>(1U << slot);
        if ((occupied & bit) != 0) invalid();
        occupied |= bit;
        integer(item.at("source_uid"), 1, UINT32_MAX);
        if (item.contains("prefixes")) prefix_ids(item.at("prefixes"));
    }
}
void health(const Json& value) {
    const auto maximum = integer(value.at("max_hp"), 1, std::numeric_limits<std::int32_t>::max());
    integer(value.at("hp"), 0, maximum);
}
void uid(const Json& value) {
    text(value, 32);
    const auto& string = value.get_ref<const std::string&>();
    if (string.size() != 32 || !std::all_of(string.begin(), string.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    })) invalid();
}
Bounds validate_state(const Json& body, std::uint32_t self, std::uint32_t tick) {
    if (!body.is_object() || integer(body.at("self"), 1, UINT32_MAX) != self ||
        integer(body.at("tick"), 0, UINT32_MAX) < tick) invalid();
    const auto bounds = zone_bounds(body);
    const auto& character = body.at("character");
    if (!character.is_object()) invalid();
    text(character.at("name"), 128);
    integer(character.at("class_id"), 1, 3);
    if (!valid_appearance(character.at("appearance"))) invalid();
    visual_equipment(character.at("equipment"));
    const auto& state = character.at("state");
    if (!state.is_object()) invalid();
    text(state.at("zone"), 128);
    if (state.at("zone") != body.at("zone")) invalid();
    coordinates(state, bounds);
    health(state);
    const auto max_mana = integer(state.at("max_mana"), 0, INT32_MAX);
    integer(state.at("mana"), 0, max_mana);
    integer(state.at("level"), 1, INT32_MAX);
    integer(state.at("xp"), 0, INT32_MAX);
    integer(body.at("xp_interval"), 1, 10000000); // world xp_needed caps at the 10,000,000 per-level bound (level ≥ 58 exceeds 1e6)
    if (body.contains("clock")) {
        const auto& clock = body.at("clock");
        if (!clock.is_object() || !clock.at("hour").is_number()) invalid();
        const auto hour = clock.at("hour").get<double>();
        if (!(hour >= 0 && hour < 24)) invalid();
        const auto& environment = clock.at("environment");
        if (!environment.is_array() || environment.size() != 11) invalid();
        for (const auto& byte : environment) integer(byte, 0, 255);
        // Original thgf fog start/end distances (record +0xec/+0xf0).
        const auto& fog = clock.at("fog");
        if (!fog.is_array() || fog.size() != 2 || !fog[0].is_number() || !fog[1].is_number()) invalid();
        const auto start = fog[0].get<double>(), end = fog[1].get<double>();
        if (!(start >= 0 && start <= end && end <= 1e6)) invalid();
    }
    integer(state.at("currency"), 0, INT64_MAX);
    const auto& inventory = state.at("inventory");
    array_limit(inventory, 32);
    std::array<std::string_view, 32> owned_ids{};
    const auto owned_end = owned_ids.begin() + static_cast<std::ptrdiff_t>(inventory.size());
    for (std::size_t i = 0; i < inventory.size(); ++i) {
        const auto& item = inventory[i];
        if (!item.is_object()) invalid();
        uid(item.at("uid"));
        text(item.at("item"), 128);
        integer(item.at("quantity"), 1, INT32_MAX);
        if (item.contains("prefixes")) prefix_ids(item.at("prefixes"));
        if (item.contains("durability")) integer(item.at("durability"), 0, INT32_MAX);
        owned_ids[i] = item.at("uid").get_ref<const std::string&>();
    }
    std::sort(owned_ids.begin(), owned_end);
    if (std::adjacent_find(owned_ids.begin(), owned_end) != owned_end) invalid();
    // Banka (open banker session only) and learned Üretim recipes (world.cpp economy()).
    if (body.contains("bank")) {
        const auto& bank = body.at("bank");
        if (!bank.is_object()) invalid();
        integer(bank.at("currency"), 0, INT64_MAX); integer(bank.at("banker"), 1, UINT32_MAX); integer(bank.at("tab"), 0, 1); // shown tab (original page 0x611454)
        const auto& stored = bank.at("items"); array_limit(stored, static_cast<std::size_t>(integer(bank.at("slots"), 1, 64))); // the shown tab's 8×8 cells
        for (const auto& item : stored) {
            if (!item.is_object()) invalid();
            uid(item.at("uid")); text(item.at("item"), 128); integer(item.at("quantity"), 1, INT32_MAX);
            if (item.contains("tab")) integer(item.at("tab"), 1, 1); // second Banka tab; absent = first
        }
    }
    if (state.contains("recipes")) { array_limit(state.at("recipes"), 200); for (const auto& recipe : state.at("recipes")) text(recipe, 128); }
    const auto& equipment = state.at("equipment");
    if (!equipment.is_object() || equipment.size() > 32) invalid();
    for (const auto& [slot, item] : equipment.items()) {
        if (slot.empty() || slot.size() > 128) invalid();
        uid(item);
        if (!std::binary_search(owned_ids.begin(), owned_end, std::string_view(item.get_ref<const std::string&>()))) invalid();
    }
    const auto& quests = state.at("quests");
    if (!quests.is_object() || quests.size() > 256) invalid();
    for (const auto& [id, quest] : quests.items()) {
        if (id.empty() || id.size() > 128 || !quest.is_object()) invalid();
        integer(quest.at("progress"), 0, INT32_MAX);
        if (quest.contains("deadline")) integer(quest.at("deadline"), 0, UINT32_MAX);
    }
    if (state.contains("quests_done")) { // completed quest ids (sorted), bounded by the 8 KiB character state
        const auto& done = state.at("quests_done");
        array_limit(done, 1024);
        for (const auto& id : done) text(id, 128);
    }
    // Karakter Bilgisi point pools and derived values (world.cpp state()).
    for (const char* group : {"resistances", "crafts"}) {
        if (!state.contains(group)) continue;
        const auto& points = state.at(group);
        if (!points.is_object() || points.size() > 6) invalid();
        for (const auto& [stat, value] : points.items()) {
            if (stat.empty() || stat.size() > 16) invalid();
            integer(value, 1, 1000);
        }
    }
    if (body.contains("stats")) {
        const auto& stats = body.at("stats");
        if (!stats.is_object()) invalid();
        for (const char* key : {"attack", "defense", "attack_cooldown", "charisma", "resistance_points", "craft_points"})
            integer(stats.at(key), 0, INT32_MAX);
        const auto& damage = stats.at("weapon_damage");
        if (!damage.is_array() || damage.size() != 2 || integer(damage[0], 0, INT32_MAX) > integer(damage[1], 0, INT32_MAX)) invalid();
        // Worn Efsun attribute sums in hundredths (world.cpp state(); ids per original 004338f0) and crit tenths/multiplier.
        if (stats.contains("efsun")) {
            const auto& efsun = stats.at("efsun");
            if (!efsun.is_object() || efsun.size() > 128) invalid();
            for (const auto& [id, value] : efsun.items()) { if (id.empty() || id.size() > 3) invalid(); integer(value, INT32_MIN, INT32_MAX); }
        }
        if (stats.contains("crit")) { const auto& crit = stats.at("crit"); if (!crit.is_array() || crit.size() != 2) invalid(); integer(crit[0], 0, INT32_MAX); integer(crit[1], 1, 10); }
        if (stats.contains("armor")) { const auto& armor = stats.at("armor"); if (!armor.is_array() || armor.size() != 2) invalid(); integer(armor[0], 0, 1000); integer(armor[1], 0, 100); } // Zırh tenths / cap
    }
    const auto& buffs = state.at("buffs");
    array_limit(buffs, 8);
    for (const auto& buff : buffs) {
        if (!buff.is_object()) invalid();
        text(buff.at("id"), 128);
        integer(buff.at("expires"), 0, UINT32_MAX);
        integer(buff.at("armor"), 0, INT32_MAX);
    }
    const auto& entities = body.at("entities");
    array_limit(entities, 96);
    std::array<std::uint32_t, 96> entity_ids{};
    const auto entity_end = entity_ids.begin() + static_cast<std::ptrdiff_t>(entities.size());
    // NONCANONICAL wave-16 doorways (world.cpp state()): a door's "portal" record, oriented for this observer, and the
    // linked rows behind open ones, tagged with that door's id.
    std::array<std::uint32_t, 96> open_doors{}, linked_tags{};
    std::size_t open_count = 0, tag_count = 0;
    const auto point = [](const Json& value) {
        if (!value.is_array() || value.size() != 2) invalid();
        return std::array{integer(value[0], -coordinate_limit, coordinate_limit), integer(value[1], -coordinate_limit, coordinate_limit)};
    };
    const auto segment = [&](const Json& value) {
        if (!value.is_array() || value.size() != 2 || point(value[0]) == point(value[1])) invalid();
    };
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const auto& entity = entities[i];
        if (!entity.is_object()) invalid();
        entity_ids[i] = static_cast<std::uint32_t>(integer(entity.at("id"), 1, UINT32_MAX));
        if (entity_ids[i] == self) invalid();
        text(entity.at("kind"), 32);
        text(entity.at("name"), 128);
        const auto& kind = entity.at("kind").get_ref<const std::string&>();
        if (kind == "player") { integer(entity.at("class_id"), 1, 3); if (entity.contains("posture")) integer(entity.at("posture"), 1, 255); } // held RP clip
        else if (kind == "enemy" || kind == "merchant" || kind == "questgiver" || kind == "node" || kind == "anvil" || kind == "door" || kind == "summon" || kind == "trap" || kind == "reset" || kind == "prop") { // node/anvil/door/reset/prop: classes.dat <loc>; summon: Boz Ayı/Tesla Küresi/Çağrı; trap: Labirent Tuzağı
            integer(entity.at("source_uid"), 1, UINT32_MAX);
            integer(entity.at("source_class"), 1, 65535); // classes.dat classid reaches 527
            if (entity.contains("destination")) { if (kind != "door") invalid(); integer(entity.at("destination"), 0, 70); } // NONCANONICAL seamless door target row
        } else invalid();
        if (const auto portal = entity.find("portal"); portal != entity.end() && portal->is_object()) {
            if (kind != "door") invalid();
            const bool open = portal->at("open").get<bool>();
            segment(portal->at("threshold"));
            const auto& floor = portal->at("floor");
            if (!floor.is_array() || floor.size() != (open ? 2U : 1U)) invalid();
            for (const auto& y : floor) integer(y, -coordinate_limit, coordinate_limit);
            integer(portal->at("height"), 1, 100000);
            if (const auto apron = portal->find("apron"); apron != portal->end()) { // walkable strip depth before each threshold
                if (!apron->is_array() || apron->size() != floor.size()) invalid();
                for (const auto& depth : *apron) integer(depth, 0, 100000);
            }
            if (open) {
                text(portal->at("zone"), 128); segment(portal->at("exit"));
                if (const auto& registry = portal->at("registry"); !registry.is_null()) integer(registry, 0, 70); // null: no source scene
                open_doors[open_count++] = entity_ids[i];
            } else text(portal->at("reason"), 512);
        } else if (portal != entity.end()) {
            if (kind == "door" || entity.contains("destination")) invalid(); // linked doors are not sent; no nested portals
            linked_tags[tag_count++] = static_cast<std::uint32_t>(integer(*portal, 1, UINT32_MAX));
        }
        if (!valid_appearance(entity.at("appearance"))) invalid();
        visual_equipment(entity.at("equipment"));
        coordinates(entity, bounds);
        health(entity);
        if (!entity.at("alive").is_boolean() ||
            entity.at("alive").get<bool>() != (entity.at("hp").get<std::int64_t>() > 0)) invalid();
    }
    // A linked row must name an open portal door of the same state.
    for (std::size_t i = 0; i < tag_count; ++i)
        if (std::find(open_doors.begin(), open_doors.begin() + static_cast<std::ptrdiff_t>(open_count), linked_tags[i]) ==
            open_doors.begin() + static_cast<std::ptrdiff_t>(open_count)) invalid();
    if (body.contains("crossed")) { const auto& crossed = body.at("crossed"); if (!crossed.is_object()) invalid(); integer(crossed.at("door"), 1, UINT32_MAX); }
    std::sort(entity_ids.begin(), entity_end);
    if (std::adjacent_find(entity_ids.begin(), entity_end) != entity_end) invalid();
    if (body.contains("party")) { // NONCANONICAL connected party (world.cpp state()): the other group members, any zone
        const auto& party = body.at("party");
        array_limit(party, 5);
        for (const auto& member : party) {
            if (!member.is_object() || integer(member.at("id"), 1, UINT32_MAX) == self) invalid();
            text(member.at("name"), 128); integer(member.at("level"), 1, INT32_MAX); integer(member.at("class_id"), 1, 3);
            health(member); integer(member.at("mana"), 0, integer(member.at("max_mana"), 0, INT32_MAX)); text(member.at("zone"), 128);
            if (const auto& source = member.at("source_registry_id"); !source.is_null()) integer(source, 0, 70);
            if (!member.at("alive").is_boolean() || member.at("alive").get<bool>() != (member.at("hp").get<std::int64_t>() > 0)) invalid();
        }
    }
    const auto& events = body.at("events");
    array_limit(events, 32);
    for (const auto& event : events) {
        if (!event.is_object()) invalid();
        text(event.at("type"), 64);
        const auto& type = event.at("type").get_ref<const std::string&>();
        const bool identity = type == "combat" || type == "skill" || type == "death" ||
            type == "resurrect" || type == "equip" || type == "unequip";
        for (const char* key : {"entity", "source", "target"}) {
            if (identity || event.contains(key)) integer(event.at(key), 1, UINT32_MAX);
        }
        if (type == "correction") coordinates(event, bounds);
        if (type == "skill") {
            text(event.at("skill"), 32); text(event.at("kind"), 16);
            const auto& source = event.at("skill_source");
            if (!source.is_array() || source.size() != 2) invalid();
            integer(source[0], 0, 2); integer(source[1], 0, 15); // skillno 15: Boz Ayı, the slot-8 variant record
        }
        if (type == "equip" || type == "unequip") visual_equipment(event.at("equipment"));
    }
    return bounds;
}
void validate_snapshot(const Json& body, std::span<const Player> players, std::uint32_t self,
                       std::uint32_t snapshot_tick, const Bounds& bounds) {
    for (const auto& player : players)
        if (!contains(bounds, player.x, player.y)) invalid();
    if (body.at("tick").get<std::uint32_t>() != snapshot_tick) return;
    const auto matches = [&](std::uint32_t id, const Json& position) {
        const auto player = std::lower_bound(players.begin(), players.end(), id,
            [](const Player& value, std::uint32_t target) { return value.id < target; });
        if (player == players.end() || player->id != id ||
            position.at("x") != player->x || position.at("y") != player->y) invalid();
    };
    matches(self, body.at("character").at("state"));
    std::size_t expected = 1;
    for (const auto& entity : body.at("entities")) {
        if (entity.at("kind") != "player") continue;
        ++expected;
        matches(entity.at("id").get<std::uint32_t>(), entity);
    }
    if (players.size() != expected) invalid();
}
} // namespace

struct ClientSession::Impl {
    struct Frame {
        std::vector<std::uint8_t> bytes;
        Clock::time_point deadline;
        bool secret;
        ~Frame() { if (secret && !bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size()); }
        Frame(std::vector<std::uint8_t> data, bool sensitive)
            : bytes(std::move(data)), deadline(Clock::now() + io_timeout), secret(sensitive) {}
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
    };
    int fd = -1;
    SSL_CTX* context = nullptr;
    SSL* ssl = nullptr;
    Decoder decoder;
    std::deque<Frame> outgoing;
    std::array<std::uint8_t, 8192> read_buffer{};
    std::size_t queued = 0;
    std::size_t offset = 0;
    short write_want = POLLOUT;
    bool write_retry = false;
    short read_want = POLLIN;
    short shutdown_want = POLLOUT;
    bool stopping = false;
    bool shutdown_sent = false;
    bool shutdown_retry = false;
    bool read_closed = false;
    bool finished = false;
    std::uint32_t sequence = 0;
    std::uint32_t intent_tick = 0;
    std::uint32_t state_tick = 0;
    bool intent_sent = false;
    bool state_seen = false;
    Bounds bounds{};
    int dx = 0;
    int dy = 0;
    Clock::time_point read_deadline = Clock::now() + io_timeout;
    Clock::time_point stop_deadline{};

    ~Impl() {
        if (ssl) SSL_free(ssl);
        if (context) SSL_CTX_free(context);
        if (fd >= 0) close(fd);
    }
    void queue(const Message& message, bool secret = false) {
        auto bytes = encode(message);
        if (bytes.size() > queue_limit - queued || outgoing.size() >= 128) {
            if (secret) OPENSSL_cleanse(bytes.data(), bytes.size());
            throw std::runtime_error("outgoing queue limit exceeded");
        }
        const auto size = bytes.size();
        try { outgoing.emplace_back(std::move(bytes), secret); }
        catch (...) {
            if (secret && !bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
            throw;
        }
        queued += size;
    }
    void connect_to(const std::string& host, std::uint16_t port, const std::filesystem::path& ca) {
        const auto deadline = Clock::now() + io_timeout;
        context = SSL_CTX_new(TLS_client_method());
        if (!context || SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) != 1 ||
            SSL_CTX_load_verify_locations(context, ca.c_str(), nullptr) != 1) transport_error();
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION);
        const auto resolution = resolve(host, port, deadline);
        for (auto* address = resolution->addresses; address; address = address->ai_next) {
            remaining_ms(deadline);
            fd = socket(address->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, address->ai_protocol);
            if (fd < 0) continue;
            bool connected = connect(fd, address->ai_addr, address->ai_addrlen) == 0;
            if (!connected && errno == EINPROGRESS) {
                wait_socket(fd, POLLOUT, deadline);
                int error = 0;
                socklen_t size = sizeof(error);
                connected = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0;
            }
            if (connected) break;
            close(fd);
            fd = -1;
        }
        if (fd < 0) transport_error();
        ssl = SSL_new(context);
        if (!ssl || SSL_set_fd(ssl, fd) != 1) transport_error();
        auto* verification = SSL_get0_param(ssl);
        X509_VERIFY_PARAM_set_hostflags(verification, X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        std::array<unsigned char, 16> ip{};
        if (inet_pton(AF_INET, host.c_str(), ip.data()) == 1 || inet_pton(AF_INET6, host.c_str(), ip.data()) == 1) {
            if (X509_VERIFY_PARAM_set1_ip_asc(verification, host.c_str()) != 1) transport_error();
        } else if (SSL_set1_host(ssl, host.c_str()) != 1 || SSL_set_tlsext_host_name(ssl, host.c_str()) != 1) transport_error();
        NoSigpipe no_sigpipe;
        for (;;) {
            remaining_ms(deadline);
            ERR_clear_error();
            const int result = SSL_connect(ssl);
            if (result == 1) break;
            wait_socket(fd, tls_want(ssl, result), deadline);
        }
        remaining_ms(deadline);
        if (SSL_get_verify_result(ssl) != X509_V_OK) transport_error();
        read_deadline = Clock::now() + io_timeout;
    }
    // A deadline measures the peer's silence, not this process's own pause: data or writability that is already
    // available after a main-thread stall (scene load, first GPU upload) is serviced before any timeout is judged.
    void deadlines(short available) const {
        const auto now = Clock::now();
        const short failed = POLLERR | POLLHUP;
        if ((stopping && now >= stop_deadline) ||
            (!stopping && now >= read_deadline && SSL_pending(ssl) <= 0 && !(available & (read_want | failed))) ||
            (!outgoing.empty() && now >= outgoing.front().deadline && !(available & (write_want | failed))))
            throw std::runtime_error("secure connection timed out");
    }
    Clock::time_point deadline() const {
        auto value = stopping ? stop_deadline : read_deadline;
        if (!outgoing.empty()) value = std::min(value, outgoing.front().deadline);
        return value;
    }
    void flush() {
        std::size_t sent = 0;
        while (!outgoing.empty() && sent < pump_limit) {
            auto& bytes = outgoing.front().bytes;
            std::size_t count = 0;
            // Front storage, pointer and length stay unchanged across WANT_*.
            ERR_clear_error();
            const int result = SSL_write_ex(ssl, bytes.data() + offset, bytes.size() - offset, &count);
            if (result != 1) { write_want = tls_want(ssl, result); write_retry = true; return; }
            if (count == 0 || count > bytes.size() - offset) transport_error();
            sent += count;
            queued -= count;
            offset += count;
            write_want = POLLOUT;
            write_retry = false;
            if (offset == bytes.size()) { outgoing.pop_front(); offset = 0; }
        }
    }
    void shutdown() {
        ERR_clear_error();
        const int result = SSL_shutdown(ssl);
        if (result == 1) { finished = true; shutdown_sent = true; shutdown_retry = false; return; }
        if (result == 0) { shutdown_sent = true; shutdown_retry = false; return; }
        shutdown_want = tls_want(ssl, result);
        shutdown_retry = true;
    }
};

ClientSession::ClientSession(std::string host, std::uint16_t port, std::filesystem::path ca,
                             std::string_view token, std::uint64_t character)
    : impl_(std::make_unique<Impl>()) {
    if (host.empty() || host.size() > 253 || host.find('\0') != std::string::npos ||
        std::any_of(host.begin(), host.end(), [](unsigned char c) { return c <= 32 || c >= 127; }) ||
        port == 0 || ca.empty() || ca.native().find('\0') != std::string::npos ||
        character == 0 || character > INT64_MAX || token.size() != 64 ||
        !std::all_of(token.begin(), token.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        throw std::invalid_argument("invalid secure connection configuration");
    impl_->connect_to(host, port, ca);
    Message hello{};
    hello.type = Type::hello;
    hello.session.assign(token);
    hello.character_id = character;
    try { impl_->queue(hello, true); }
    catch (...) { OPENSSL_cleanse(hello.session.data(), hello.session.size()); throw; }
    OPENSSL_cleanse(hello.session.data(), hello.session.size());
}
ClientSession::~ClientSession() = default;

void ClientSession::receive(Message message) {
    try {
        if (self == 0) {
            if (message.type != Type::welcome || message.id == 0) invalid();
            self = message.id;
            tick = message.tick;
        } else if (message.type == Type::snapshot) {
            if (!impl_->state_seen || message.tick <= tick || message.tick < impl_->state_tick ||
                message.players.empty() || message.players.size() > 32) invalid();
            std::uint32_t previous_id = 0;
            bool contains_self = false;
            for (const auto& player : message.players) {
                if (player.id <= previous_id || player.x < -coordinate_limit || player.x > coordinate_limit ||
                    player.y < -coordinate_limit || player.y > coordinate_limit) invalid();
                previous_id = player.id;
                contains_self = contains_self || player.id == self;
            }
            if (!contains_self) invalid();
            validate_snapshot(state, message.players, self, message.tick, impl_->bounds);
            players = std::move(message.players);
            tick = message.tick;
            has_snapshot = true;
            if ((impl_->dx != 0 || impl_->dy != 0) && !impl_->stopping) intent(impl_->dx, impl_->dy);
        } else if (message.type == Type::state) {
            auto body = Json::parse(message.json);
            const auto bounds = validate_state(body, self, tick);
            const auto body_tick = static_cast<std::uint32_t>(integer(body.at("tick"), 0, UINT32_MAX));
            if (impl_->state_seen && body_tick < impl_->state_tick) invalid();
            if (impl_->state_seen && body.at("zone") == state.at("zone") &&
                (body.at("source_registry_id") != state.at("source_registry_id") ||
                 body.at("bounds") != state.at("bounds"))) invalid();
            // Per-action state can supersede an accepted binary snapshot within
            // the same World tick. Only incoming snapshots cross-check positions
            // against the latest preceding state, never the reverse.
            // The world sends catalogs on admission and whenever they change; other states keep the last accepted copy.
            if (const auto catalogs = body.find("catalogs"); catalogs != body.end()) {
                if (!catalogs->is_object()) invalid();
            } else if (!impl_->state_seen || !state.contains("catalogs")) invalid();
            else body["catalogs"] = std::move(state["catalogs"]);
            impl_->state_tick = body_tick;
            impl_->state_seen = true;
            impl_->bounds = bounds;
            state = std::move(body);
        } else invalid();
        impl_->read_deadline = Clock::now() + io_timeout;
    } catch (const Json::exception&) { invalid(); }
}

void ClientSession::pump(int wait_ms, const std::function<void(Type)>& on_message) {
    if (wait_ms < 0) throw std::invalid_argument("invalid pump timeout");
    if (eof || impl_->finished) return;
    try {
        NoSigpipe no_sigpipe;
        short events = impl_->read_closed ? 0 : impl_->read_want;
        if (!impl_->outgoing.empty() && impl_->read_want != POLLOUT) events |= impl_->write_want;
        if (impl_->stopping && impl_->outgoing.empty() && (!impl_->shutdown_sent || impl_->shutdown_retry))
            events |= impl_->shutdown_want;
        if (impl_->write_retry) events = impl_->write_want;
        else if (impl_->read_want == POLLOUT && !impl_->read_closed) events = POLLOUT;
        else if (impl_->shutdown_retry) events = impl_->shutdown_want;
        pollfd descriptor{impl_->fd, events, 0};
        // Past a deadline the poll is immediate: it reports what arrived during a stall before deadlines() judges silence.
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(impl_->deadline() - Clock::now()).count();
        const int timeout = SSL_pending(impl_->ssl) > 0 ? 0 : static_cast<int>(std::clamp<long long>(left + 1, 0, wait_ms));
        const int ready = poll(&descriptor, 1, timeout);
        if (ready < 0 && errno == EINTR) return;
        if (ready < 0 || (descriptor.revents & POLLNVAL)) transport_error();
        impl_->deadlines(descriptor.revents);
        const short available = descriptor.revents;
        if (!impl_->outgoing.empty() && impl_->read_want != POLLOUT &&
            (available & (impl_->write_want | POLLERR | POLLHUP))) impl_->flush();
        // A write WANT_* must retry with its original buffer before another TLS
        // operation; a read WANT_WRITE similarly remains the active operation.
        if (impl_->write_retry) return;
        if (impl_->read_want != POLLOUT && impl_->stopping && impl_->outgoing.empty() &&
            (!impl_->shutdown_sent || impl_->shutdown_retry) &&
            (available & (impl_->shutdown_want | POLLERR | POLLHUP))) {
            impl_->shutdown();
            if (impl_->finished) { impl_->decoder.finish(); if (self == 0) invalid(); eof = true; return; }
            if (impl_->shutdown_retry) return;
        }
        if (!impl_->read_closed && (SSL_pending(impl_->ssl) > 0 || (available & (impl_->read_want | POLLERR | POLLHUP)))) {
            std::size_t received = 0;
            while (received < pump_limit) {
                std::size_t count = 0;
                ERR_clear_error();
                const int result = SSL_read_ex(impl_->ssl, impl_->read_buffer.data(), impl_->read_buffer.size(), &count);
                if (result != 1) {
                    const int error = SSL_get_error(impl_->ssl, result);
                    if (error == SSL_ERROR_ZERO_RETURN) {
                        impl_->decoder.finish();
                        if (self == 0) invalid();
                        impl_->read_closed = true;
                        impl_->read_want = POLLIN;
                        impl_->outgoing.clear();
                        impl_->queued = 0;
                        impl_->offset = 0;
                        if (!impl_->stopping) {
                            impl_->stopping = true;
                            impl_->stop_deadline = Clock::now() + std::chrono::seconds(2);
                        }
                        impl_->shutdown();
                        eof = impl_->finished;
                        return;
                    }
                    impl_->read_want = tls_want(impl_->ssl, result);
                    break;
                }
                if (count == 0) transport_error();
                impl_->read_want = POLLIN;
                received += count;
                for (auto& message : impl_->decoder.feed(std::span(impl_->read_buffer.data(), count))) {
                    const auto type = message.type;
                    receive(std::move(message));
                    if (on_message) on_message(type);
                }
            }
        }
        return;
    } catch (const Json::exception&) {
        eof = true;
        invalid();
    } catch (const std::invalid_argument&) {
        eof = true;
        invalid();
    } catch (...) {
        eof = true;
        throw;
    }
}

void ClientSession::destination(std::int32_t x, std::int32_t y, std::uint32_t door) {
    if (x < -coordinate_limit || x > coordinate_limit || y < -coordinate_limit || y > coordinate_limit)
        throw std::invalid_argument("invalid destination");
    if (self == 0 || !impl_->state_seen || eof || impl_->stopping) throw std::runtime_error("session is not ready");
    if (!contains(impl_->bounds, x, y)) throw std::invalid_argument("destination outside current zone");
    if (impl_->sequence == UINT32_MAX) throw std::runtime_error("input sequence exhausted");
    if (door && std::none_of(state.at("entities").begin(), state.at("entities").end(), [&](const Json& e) {
            return e.at("id") == door && e.contains("portal") && e.at("portal").is_object() && e.at("portal").at("open") == true; }))
        throw std::invalid_argument("destination door is not an open portal");
    Message message{};
    message.type = Type::destination;
    message.sequence = impl_->sequence + 1;
    message.x = x;
    message.y = y;
    message.id = door; // protocol: a nonzero id appends the u32 portal door (18-byte body)
    impl_->queue(message);
    ++impl_->sequence;
}
void ClientSession::action(const Json& body) {
    if (!body.is_object()) throw std::invalid_argument("invalid action");
    if (self == 0 || eof || impl_->stopping) throw std::runtime_error("session is not ready");
    Message message{};
    message.type = Type::action;
    try { message.json = body.dump(); impl_->queue(message); }
    catch (const Json::exception&) { throw std::invalid_argument("invalid action"); }
}
void ClientSession::intent(int dx, int dy) {
    if (dx < -1 || dx > 1 || dy < -1 || dy > 1 || (dx != 0 && dy != 0))
        throw std::invalid_argument("invalid movement intent");
    const bool changed = dx != impl_->dx || dy != impl_->dy;
    impl_->dx = dx;
    impl_->dy = dy;
    if (!changed && dx == 0 && dy == 0) return;
    if (!has_snapshot || eof || impl_->stopping || (impl_->intent_sent && impl_->intent_tick == tick && !changed)) return;
    const auto player = std::find_if(players.begin(), players.end(), [&](const auto& value) { return value.id == self; });
    if (player == players.end()) invalid();
    const auto& bounds = impl_->bounds;
    destination(std::clamp(player->x + dx * 100, bounds[0], bounds[2]),
                std::clamp(player->y + dy * 100, bounds[1], bounds[3]));
    impl_->intent_tick = tick;
    impl_->intent_sent = true;
}
void ClientSession::stop() {
    if (impl_->stopping) return;
    if (!eof) {
        Message message{};
        message.type = Type::goodbye;
        impl_->queue(message);
    }
    impl_->stopping = true;
    impl_->stop_deadline = Clock::now() + std::chrono::seconds(2);
}
bool ClientSession::drained() const noexcept { return impl_->stopping && (eof || impl_->finished); }
} // namespace ikv
