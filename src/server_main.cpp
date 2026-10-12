#include "ikv/protocol.hpp"
#include "ikv/store.hpp"
#include "ikv/world.hpp"
#include "ikv/bots.hpp"
#include "ikv/scene.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/rand.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_connections = 64, max_players = 32, max_output = 262144;
constexpr auto tick_period = std::chrono::milliseconds(50);
constexpr auto close_timeout = std::chrono::seconds(2);
volatile std::sig_atomic_t stopping = 0;
extern "C" void stop_signal(int) { stopping = 1; }
class Socket {
public:
    explicit Socket(int fd = -1) : fd_(fd) {}
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int get() const { return fd_; }
    void close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
private:
    int fd_;
};
void nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        throw std::runtime_error("socket flags failed");
}
long resident_kib() {
    std::ifstream statm("/proc/self/statm");
    long pages = 0, resident = 0;
    statm >> pages >> resident;
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
}
// Administrator dry run: parse and construct a World from the configured gameplay file without touching the live one.
// Source support grids are not reloaded, so grid-walkability of configured points is checked only at real startup.
ikv::Json validate_config(const std::string& path) {
    try {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file || file.tellg() <= 0 || file.tellg() > 4194304) throw std::runtime_error("gameplay file missing or larger than 4 MiB");
        file.seekg(0);
        ikv::World probe(ikv::Json::parse(file), [](std::span<const ikv::CharacterSave>) {});
        const auto status = probe.status();
        return {{"ok", true}, {"path", path}, {"zones", status.at("zones").size()}, {"npcs", status.at("npcs")}};
    } catch (const std::exception& e) { return {{"ok", false}, {"path", path}, {"error", e.what()}}; }
}
std::string secret_file(const std::string& path) {
    Socket file(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat st {};
    if (file.get() < 0 || fstat(file.get(), &st) != 0 || !S_ISREG(st.st_mode) ||
        (st.st_mode & 0777) != 0600 || st.st_uid != geteuid() || st.st_size <= 0 || st.st_size > 8192)
        throw std::runtime_error("DSN file must be owner-only regular file mode0600");
    std::string value(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto n = read(file.get(), value.data() + offset, value.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("DSN file read failed");
        offset += static_cast<std::size_t>(n);
    }
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    if (value.empty() || value.find('\0') != std::string::npos) throw std::runtime_error("invalid DSN file");
    return value;
}
std::string instance_id() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) throw std::runtime_error("random failure");
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "world-";
    for (auto b : bytes) { result.push_back(hex[b >> 4]); result.push_back(hex[b & 15]); }
    return result;
}
enum class TlsOperation { handshake, idle, write, read_want_write, shutdown, shutdown_read };
struct Peer {
    Peer(int fd, std::uint64_t connection_id, SSL_CTX* context)
        : socket(fd), ssl(SSL_new(context), SSL_free), connection(connection_id), accepted(Clock::now()) {
        if (!ssl || SSL_set_fd(ssl.get(), fd) != 1) throw std::runtime_error("TLS peer setup failed");
        SSL_set_accept_state(ssl.get());
        SSL_set_mode(ssl.get(), SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        output.reserve(16384);
    }
    Socket socket;
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl;
    std::uint64_t connection;
    Clock::time_point accepted;
    ikv::Decoder decoder;
    bool established = false, closing = false, held_lease = false;
    bool reported_want_write = false;
    TlsOperation operation = TlsOperation::handshake;
    short want = POLLIN;
    const char* close_reason = nullptr;
    Clock::time_point close_deadline{};
    std::array<std::uint8_t, 16384> read_buffer{};
    std::uint32_t id = 0, sequence = 0;
    ikv::Identity identity{};
    std::int64_t character_id = 0;
    std::string session;
    std::string catalogs; // last catalogs sent; states omit an unchanged copy (client keeps the previous one)
    std::vector<std::uint8_t> output;
    std::size_t sent = 0, write_size = 0;
    std::uint32_t actions = 0;
    Clock::time_point action_window = Clock::now();
};
void disconnect(Peer& peer, const char* reason) {
    if (peer.socket.get() < 0) return;
    std::cerr << "peer=" << peer.connection << " close=" << reason << '\n';
    peer.socket.close();
}
void begin_close(Peer& peer, const char* reason) {
    if (peer.socket.get() < 0 || peer.closing) return;
    if (!peer.established) { disconnect(peer, reason); return; }
    peer.closing = true;
    peer.close_reason = reason;
    peer.close_deadline = Clock::now() + close_timeout;
}
bool queue(Peer& peer, const ikv::Message& message) {
    if (peer.socket.get() < 0 || peer.closing) return false;
    auto packet = ikv::encode(message);
    const auto pending = peer.output.size() - peer.sent;
    if (packet.size() > max_output - pending) { disconnect(peer, "output_limit"); return false; }
    if (peer.output.size() + packet.size() > max_output) {
        peer.output.erase(peer.output.begin(), peer.output.begin() + static_cast<std::ptrdiff_t>(peer.sent));
        peer.sent = 0;
    }
    peer.output.insert(peer.output.end(), packet.begin(), packet.end());
    return true;
}
void handshake(Peer& peer) {
    if (peer.operation != TlsOperation::handshake || peer.socket.get() < 0) return;
    ERR_clear_error();
    const int n = SSL_accept(peer.ssl.get());
    const int error = SSL_get_error(peer.ssl.get(), n);
    if (n == 1) { peer.established = true; peer.operation = TlsOperation::idle; return; }
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        peer.want = error == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN;
        return;
    }
    disconnect(peer, "tls_handshake");
}
void flush(Peer& peer) {
    if (peer.operation != TlsOperation::idle && peer.operation != TlsOperation::write) return;
    while (peer.socket.get() >= 0 && peer.sent < peer.output.size()) {
        if (peer.closing && Clock::now() >= peer.close_deadline) { disconnect(peer, "close_timeout"); return; }
        if (peer.write_size == 0) peer.write_size = std::min<std::size_t>(16384, peer.output.size() - peer.sent);
        const bool was_want_write = peer.operation == TlsOperation::write && peer.want == POLLOUT;
        peer.operation = TlsOperation::write;
        ERR_clear_error();
        const int n = SSL_write(peer.ssl.get(), peer.output.data() + peer.sent, static_cast<int>(peer.write_size));
        const int error = SSL_get_error(peer.ssl.get(), n);
        if (n > 0) {
            peer.sent += static_cast<std::size_t>(n); peer.write_size = 0; peer.operation = TlsOperation::idle;
            continue;
        }
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            if (error == SSL_ERROR_WANT_WRITE && !was_want_write && !peer.reported_want_write) {
                std::cerr << "actor=" << peer.id << " peer=" << peer.connection
                          << " blocked=want_write pending=" << peer.output.size() - peer.sent
                          << " write_size=" << peer.write_size << '\n';
                peer.reported_want_write = true;
            }
            peer.want = error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
            return;
        }
        disconnect(peer, "tls_write"); return;
    }
    if (peer.socket.get() >= 0) { peer.output.clear(); peer.sent = 0; }
}
int read_tls(Peer& peer, int& error) {
    ERR_clear_error();
    const int n = SSL_read(peer.ssl.get(), peer.read_buffer.data(), static_cast<int>(peer.read_buffer.size()));
    error = SSL_get_error(peer.ssl.get(), n);
    if (n > 0 || error == SSL_ERROR_WANT_READ || error == SSL_ERROR_ZERO_RETURN)
        peer.operation = TlsOperation::idle;
    else if (error == SSL_ERROR_WANT_WRITE) {
        peer.operation = TlsOperation::read_want_write; peer.want = POLLOUT;
    }
    return n;
}
void advance_close(Peer& peer) {
    if (!peer.closing || peer.socket.get() < 0) return;
    if (Clock::now() >= peer.close_deadline) { disconnect(peer, "close_timeout"); return; }
    if (peer.operation == TlsOperation::shutdown_read) {
        // A sent close_notify is only a half-close. Drain without dispatching:
        // closing with unread TCP input can reset and truncate our final output.
        std::size_t read_bytes = 0;
        while (read_bytes < 65536) {
            ERR_clear_error();
            const int n = SSL_read(peer.ssl.get(), peer.read_buffer.data(), static_cast<int>(peer.read_buffer.size()));
            if (n > 0) { read_bytes += static_cast<std::size_t>(n); peer.want = POLLIN; continue; }
            const int error = SSL_get_error(peer.ssl.get(), n);
            if (error == SSL_ERROR_ZERO_RETURN) { disconnect(peer, peer.close_reason); return; }
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                peer.want = error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
                return;
            }
            disconnect(peer, "tls_read"); return;
        }
        return;
    }
    if (peer.operation == TlsOperation::read_want_write) {
        int error = SSL_ERROR_NONE;
        const int n = read_tls(peer, error); // Complete the stable-buffer retry, but never dispatch its input.
        if (n <= 0 && error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE &&
            error != SSL_ERROR_ZERO_RETURN) { disconnect(peer, "tls_read"); return; }
        if (peer.operation != TlsOperation::idle) return;
    }
    flush(peer);
    if (peer.socket.get() < 0 || (peer.operation != TlsOperation::idle && peer.operation != TlsOperation::shutdown)) return;
    if (Clock::now() >= peer.close_deadline) { disconnect(peer, "close_timeout"); return; }
    peer.operation = TlsOperation::shutdown;
    ERR_clear_error();
    const int n = SSL_shutdown(peer.ssl.get());
    if (n == 1) { disconnect(peer, peer.close_reason); return; }
    if (n == 0) { peer.operation = TlsOperation::shutdown_read; peer.want = POLLIN; return; }
    const int error = SSL_get_error(peer.ssl.get(), n);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        peer.want = error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
        return;
    }
    disconnect(peer, "tls_shutdown");
}
short poll_interest(const Peer& peer) {
    if (peer.operation != TlsOperation::idle) return peer.want;
    if (peer.closing) return POLLOUT;
    return static_cast<short>(POLLIN | (peer.sent < peer.output.size() ? POLLOUT : 0));
}
std::size_t joined(const std::list<Peer>& peers) {
    return static_cast<std::size_t>(std::count_if(peers.begin(), peers.end(), [](const Peer& p) {
        return p.id != 0; // Retain the admission slot until World has actually detached the actor.
    }));
}
void send_state(Peer& peer, ikv::World& world) {
    if (peer.closing || peer.socket.get() < 0) return;
    auto state = world.state(peer.id);
    // Catalogs are ~21 KB of mostly static data. Resending them at 20 Hz filled the output_limit in a few
    // seconds whenever a healthy client paused reading (scene load, first GPU upload).
    if (const auto catalogs = state.find("catalogs"); catalogs != state.end()) {
        auto dumped = catalogs->dump();
        if (dumped == peer.catalogs) state.erase(catalogs);
        else peer.catalogs = std::move(dumped);
    }
    bool kicked = false;
    if (state.contains("events") && state["events"].is_array())
        for (const auto& event : state["events"]) if (event.value("type", "") == "kick") kicked = true;
    ikv::Message message{ikv::Type::state}; message.json = state.dump();
    if (!queue(peer, message)) return;
    if (kicked) begin_close(peer, "admin_kick"); // The final event is queued before closing forbids new output.
    flush(peer);
}
void receive(Peer& peer, std::list<Peer>& peers, ikv::World& world, ikv::Store& store,
             const std::string& instance, std::uint32_t& next_id) {
    if (peer.closing || (peer.operation != TlsOperation::idle && peer.operation != TlsOperation::read_want_write)) return;
    std::size_t read_bytes = 0;
    while (peer.socket.get() >= 0 && !peer.closing && read_bytes < 65536) {
        if (peer.operation != TlsOperation::idle && peer.operation != TlsOperation::read_want_write) return;
        int error = SSL_ERROR_NONE;
        const int n = read_tls(peer, error);
        if (n <= 0) {
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                return;
            }
            if (error == SSL_ERROR_ZERO_RETURN) {
                try { peer.decoder.finish(); begin_close(peer, "eof"); }
                catch (const std::invalid_argument&) { disconnect(peer, "truncated_protocol"); }
            } else disconnect(peer, "tls_read");
            return;
        }
        read_bytes += static_cast<std::size_t>(n);
        try {
            for (const auto& message : peer.decoder.feed(std::span(peer.read_buffer).first(static_cast<std::size_t>(n)))) {
                if (peer.closing || peer.socket.get() < 0) return;
                if (peer.id == 0) {
                    if (message.type != ikv::Type::hello) { disconnect(peer, "handshake_state"); return; }
                    if (joined(peers) >= max_players || next_id == 0 || next_id >= 0x70000000U) { // 0x70000000+: bot actors
                        disconnect(peer, "player_limit"); return;
                    }
                    const auto identity = store.authenticate(message.session);
                    const auto character_id = static_cast<std::int64_t>(message.character_id);
                    auto record = store.enter_character(identity, character_id, instance);
                    const auto id = next_id++;
                    peer.identity = identity; peer.character_id = character_id; peer.held_lease = true;
                    peer.session = message.session;
                    world.add(id, identity.account_id, character_id, record, identity.admin); world.entitlement(id, identity.plus, identity.kasa);
                    peer.id = id;
                    ikv::Message welcome{ikv::Type::welcome}; welcome.id = id; welcome.tick = world.tick();
                    if (!queue(peer, welcome)) return;
                    send_state(peer, world);
                } else if (message.type == ikv::Type::input || message.type == ikv::Type::destination) {
                    if (message.sequence <= peer.sequence) { disconnect(peer, "sequence"); return; }
                    peer.sequence = message.sequence;
                    if (message.type == ikv::Type::input) world.input(peer.id, message.dx, message.dy, message.sequence);
                    else world.destination(peer.id, message.x, message.y, message.sequence, message.id);
                } else if (message.type == ikv::Type::action) {
                    const auto now = Clock::now();
                    if (now - peer.action_window >= std::chrono::seconds(1)) { peer.actions = 0; peer.action_window = now; }
                    if (++peer.actions > 40) { disconnect(peer, "action_rate"); return; }
                    world.action(peer.id, ikv::Json::parse(message.json));
                    send_state(peer, world);
                } else if (message.type == ikv::Type::goodbye) {
                    begin_close(peer, "goodbye"); return;
                } else { disconnect(peer, "joined_state"); return; }
            }
        } catch (const ikv::ApiError&) { disconnect(peer, "authentication_or_store"); return; }
        catch (const std::invalid_argument&) { disconnect(peer, "protocol"); return; }
        catch (const ikv::Json::exception&) { disconnect(peer, "json"); return; }
        catch (const std::exception&) { disconnect(peer, "world_operation"); return; }
    }
}
struct Options {
    std::string bind = "127.0.0.1", cert, key, dsn_file, gameplay = "config/gameplay.json";
    std::string asset_root;
    std::uint16_t port = 7000;
    std::optional<std::uint32_t> ticks, bots;
};
std::uint32_t number(std::string_view value) {
    std::uint32_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || value.empty())
        throw std::invalid_argument("invalid numeric option");
    return result;
}
Options options(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option(argv[i]);
        if (option == "--help") {
            std::cout << "Independent authenticated IKV community world (custom protocol v2)\n"
                         "Usage: ikv-server --cert FILE --key FILE --dsn-file FILE [--bind IPv4] [--port 7000]\n"
                         "                  [--gameplay config/gameplay.json] [--ticks N] [--bots N (0..256; default gameplay.json bots.spawn)]\n"
                         "                  [--asset-root ROOT (required by source-associated gameplay zones)]\n";
            std::exit(0);
        }
        if (i + 1 >= argc) throw std::invalid_argument("incomplete option");
        const std::string value(argv[++i]);
        if (option == "--bind") result.bind = value;
        else if (option == "--cert") result.cert = value;
        else if (option == "--key") result.key = value;
        else if (option == "--dsn-file") result.dsn_file = value;
        else if (option == "--gameplay") result.gameplay = value;
        else if (option == "--asset-root") result.asset_root = value;
        else if (option == "--port") {
            const auto port = number(value);
            if (port > 65535) throw std::invalid_argument("invalid port");
            result.port = static_cast<std::uint16_t>(port);
        } else if (option == "--ticks") {
            const auto ticks = number(value);
            if (!ticks) throw std::invalid_argument("ticks must be positive");
            result.ticks = ticks;
        } else if (option == "--bots") {
            const auto bots = number(value);
            if (bots > 256) throw std::invalid_argument("bots must be 0..256");
            result.bots = bots;
        } else throw std::invalid_argument("unknown option");
    }
    if (result.cert.empty() || result.key.empty() || result.dsn_file.empty())
        throw std::invalid_argument("TLS certificate, key and DSN file are required");
    return result;
}
int run(const Options& opts) {
    struct sigaction signal_action {};
    signal_action.sa_handler = stop_signal; sigemptyset(&signal_action.sa_mask);
    if (sigaction(SIGINT, &signal_action, nullptr) < 0 || sigaction(SIGTERM, &signal_action, nullptr) < 0)
        throw std::runtime_error("signal setup failed");
    signal_action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &signal_action, nullptr) < 0) throw std::runtime_error("signal setup failed");
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    if (!context || SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1 ||
        SSL_CTX_use_certificate_chain_file(context.get(), opts.cert.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(context.get(), opts.key.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(context.get()) != 1) throw std::runtime_error("TLS configuration failed");
    SSL_CTX_set_options(context.get(), SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    ikv::Store store(secret_file(opts.dsn_file));
    std::ifstream config_file(opts.gameplay, std::ios::binary | std::ios::ate);
    if (!config_file || config_file.tellg() <= 0 || config_file.tellg() > 4194304) // wave 11: spawn groups/props took gameplay.json past 1 MiB
        throw std::runtime_error("invalid gameplay configuration file");
    config_file.seekg(0);
    auto config = ikv::Json::parse(config_file);
    std::map<unsigned, std::shared_ptr<const ikv::Zone>> support_grids;
    std::map<std::string, std::shared_ptr<const ikv::Zone>> source_zones;
    if (config.contains("zones") && config["zones"].is_object()) {
        for (const auto& [name, zone] : config["zones"].items()) {
            if (!zone.is_object() || !zone.contains("source_registry_id")) continue;
            const auto& registry = zone["source_registry_id"];
            if (!registry.is_number_integer() || registry < 0 || registry > 70)
                throw std::invalid_argument("invalid source registry ID");
            if (opts.asset_root.empty())
                throw std::invalid_argument("source-associated gameplay zones require --asset-root");
            const auto support_id = ikv::source_scene_descriptor(registry.get<unsigned>()).support_id;
            auto [grid, inserted] = support_grids.try_emplace(support_id);
            if (inserted)
                grid->second = std::make_shared<const ikv::Zone>(ikv::load_zone(opts.asset_root, support_id));
            source_zones.emplace(name, grid->second);
        }
    }
    ikv::World world(std::move(config), [&store](std::span<const ikv::CharacterSave> states) { store.save_characters(states); });
    world.set_source_zones(std::move(source_zones));
    if (auto settings = store.world_settings(); !settings.empty()) {
        try { world.admin("settings", settings); } catch (const std::exception&) { std::cerr << "stored_world_settings_ignored\n"; }
    }
    const auto instance = instance_id();
    ikv::BotHost bots(world, store, instance); // NONCANONICAL server-side bot players (reserved bot account)
    bots.start(opts.bots);
    sockaddr_in address {}; address.sin_family = AF_INET; address.sin_port = htons(opts.port);
    if (inet_pton(AF_INET, opts.bind.c_str(), &address.sin_addr) != 1) throw std::invalid_argument("bind must be numeric IPv4");
    Socket listener(socket(AF_INET, SOCK_STREAM, 0));
    if (listener.get() < 0) throw std::runtime_error("socket failed");
    nonblocking(listener.get());
    const int reuse = 1;
    if (setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(listener.get(), static_cast<int>(max_connections)) < 0) throw std::runtime_error("listen failed");
    socklen_t address_size = sizeof(address);
    if (getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &address_size) < 0)
        throw std::runtime_error("bound address failed");
    std::list<Peer> peers;
    std::uint64_t next_connection = 1;
    std::uint32_t next_id = 1;
    const auto started = Clock::now();
    auto next_tick = started + tick_period, next_heartbeat = started, next_save = started + std::chrono::seconds(5);
    double last_tick_ms = 0;
    std::vector<double> recent_ticks; // tick durations since the previous heartbeat (status.tick_ms_recent), at most 64
    int exit_code = 0;
    // Administrator restart/shutdown countdown (POST /v1/admin/control delay_seconds); announced in game before exit.
    std::optional<Clock::time_point> exit_at;
    std::string exit_command, exit_message;
    std::int64_t exit_notice = -1;
    auto initial_status = world.status();
    initial_status["instance"] = instance; initial_status["players"] = 0; initial_status["connections"] = 0;
    initial_status["tick"] = 0; initial_status["world_port"] = ntohs(address.sin_port);
    store.world_status(initial_status);
    std::cout << "READY world_port=" << ntohs(address.sin_port) << " bind=" << opts.bind << std::endl;
    while (!stopping) {
        const auto now = Clock::now();
        for (auto it = peers.begin(); it != peers.end();) {
            if (!it->closing && it->id == 0 && now - it->accepted >= std::chrono::seconds(5)) disconnect(*it, "handshake_timeout");
            advance_close(*it);
            if (it->socket.get() >= 0) { ++it; continue; }
            if (it->id) {
                try { world.remove(it->id); } catch (const std::exception&) {
                    exit_code = 1; std::cerr << "character_save_failed\n"; break;
                }
                it->id = 0;
            }
            if (it->held_lease) {
                try { store.leave_character(it->identity.account_id, it->character_id); }
                catch (const std::exception&) { exit_code = 1; std::cerr << "character_release_failed\n"; break; }
                it->held_lease = false;
            }
            it = peers.erase(it);
        }
        if (exit_code != 0) break;
        std::array<pollfd, max_connections + 1> pollfds {};
        std::array<Peer*, max_connections + 1> polled {};
        std::size_t count = 1; pollfds[0] = {listener.get(), POLLIN, 0};
        for (auto& peer : peers) {
            pollfds[count] = {peer.socket.get(), poll_interest(peer), 0}; polled[count++] = &peer;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(next_tick - Clock::now()).count();
        const int ready = poll(pollfds.data(), static_cast<nfds_t>(count), static_cast<int>(std::clamp<std::int64_t>(remaining, 0, 50)));
        if (ready < 0) { if (errno == EINTR) continue; throw std::runtime_error("poll failed"); }
        if (pollfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) throw std::runtime_error("listener failed");
        if (pollfds[0].revents & POLLIN) {
            for (std::size_t attempt = 0; attempt < max_connections; ++attempt) {
                const int fd = accept(listener.get(), nullptr, nullptr);
                if (fd < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    throw std::runtime_error("accept failed");
                }
                if (peers.size() >= max_connections) { ::close(fd); continue; }
                try { nonblocking(fd); } catch (...) { ::close(fd); throw; }
                peers.emplace_back(fd, next_connection++, context.get());
            }
        }
        for (std::size_t i = 1; i < count; ++i) {
            auto& peer = *polled[i]; const auto events = pollfds[i].revents;
            if (events & (POLLERR | POLLNVAL)) { disconnect(peer, "socket"); continue; }
            if (peer.closing) { advance_close(peer); continue; }
            if (peer.operation == TlsOperation::handshake && (events & peer.want)) handshake(peer);
            if (peer.established && peer.socket.get() >= 0) {
                if ((peer.operation == TlsOperation::write && (events & peer.want)) ||
                    (peer.operation == TlsOperation::idle && peer.sent < peer.output.size() && (events & POLLOUT)))
                    flush(peer);
                if (peer.socket.get() >= 0 &&
                    ((peer.operation == TlsOperation::read_want_write && (events & peer.want)) ||
                     (peer.operation == TlsOperation::idle &&
                      ((events & (POLLIN | POLLHUP)) || SSL_pending(peer.ssl.get()) > 0))))
                    receive(peer, peers, world, store, instance, next_id);
            }
            if (peer.closing) advance_close(peer);
            else if (peer.socket.get() >= 0 && (events & POLLHUP)) disconnect(peer, "socket_hangup");
        }
        const auto after_io = Clock::now();
        if (after_io >= next_tick) {
            const auto begin = Clock::now();
            // ponytail: one bounded32-player authority loop; shard only with measured demand.
            unsigned catchup = 0;
            do { world.advance(); next_tick += tick_period; } while (Clock::now() >= next_tick && ++catchup < 4);
            if (Clock::now() >= next_tick) next_tick = Clock::now() + tick_period;
            for (auto& peer : peers) {
                if (!peer.id || peer.closing || peer.socket.get() < 0) continue;
                try {
                    send_state(peer, world);
                    if (peer.closing || peer.socket.get() < 0) continue;
                    ikv::Message snapshot{ikv::Type::snapshot}; snapshot.tick = world.tick(); snapshot.players = world.snapshot(peer.id);
                    if (queue(peer, snapshot)) flush(peer);
                }
                catch (const std::exception&) { disconnect(peer, "snapshot"); }
            }
            last_tick_ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            if (recent_ticks.size() < 64) recent_ticks.push_back(std::round(last_tick_ms * 100) / 100);
            if ((opts.ticks && world.tick() >= *opts.ticks) || world.tick() == std::numeric_limits<std::uint32_t>::max()) break;
        }
        try { bots.pump(); } catch (const std::exception&) { std::cerr << "bot_release_failed\n"; exit_code = 1; break; }
        if (Clock::now() >= next_save) {
            try { world.save_all(); } catch (const std::exception&) {
                std::cerr << "periodic_save_failed\n"; exit_code = 1; break;
            }
            next_save = Clock::now() + std::chrono::seconds(5);
        }
        if (Clock::now() >= next_heartbeat) {
            try {
                for (auto& peer : peers) if (peer.id && !peer.closing && peer.socket.get() >= 0) {
                    try {
                        const auto identity = store.authenticate(peer.session);
                        if (identity.account_id != peer.identity.account_id || identity.admin != peer.identity.admin) {
                            disconnect(peer, "identity_changed");
                        } else {
                            const auto record = store.character(identity.account_id, peer.character_id);
                            world.moderation(peer.id, record.value("moderation", ikv::Json::object()).value("muted", false));
                            world.entitlement(peer.id, identity.plus, identity.kasa); // an administrator's Plus/Kasa Erişimi grant/revoke applies within a second
                        }
                    } catch (const std::exception&) { disconnect(peer, "session_revoked"); }
                }
                for (const auto& command : store.take_world_commands()) {
                    const auto name = command.at("command").get<std::string>();
                    const auto& arguments = command.at("arguments");
                    ikv::Json result{{"ok", true}};
                    try {
                        if (name == "restart" || name == "shutdown") {
                            const auto delay = arguments.value("delay_seconds", 0);
                            exit_at = Clock::now() + std::chrono::seconds(delay); exit_command = name; exit_notice = -1;
                            exit_message = arguments.value("message", std::string());
                            result["delay_seconds"] = delay;
                        } else if (name == "cancel") {
                            result["cancelled"] = exit_at.has_value();
                            if (exit_at) world.admin("announce", {{"message", exit_command == "restart" ? "Sunucunun yeniden başlatılması iptal edildi." : "Sunucunun kapatılması iptal edildi."}});
                            exit_at.reset();
                        } else if (name == "validate_config") result = validate_config(opts.gameplay);
                        else if (name == "bots_spawn" || name == "bots_despawn") result = bots.command(name, arguments);
                        else result = world.admin(name, arguments);
                    } catch (const std::exception& e) { result = {{"ok", false}, {"error", e.what()}}; }
                    store.finish_world_command(command.at("id").get<std::int64_t>(), result);
                }
                if (exit_at) {
                    const auto left = std::max<std::int64_t>(0, std::chrono::ceil<std::chrono::seconds>(*exit_at - Clock::now()).count());
                    if (left != exit_notice && (exit_notice < 0 || left == 300 || left == 120 || left == 60 || left == 30 || left == 10 || (left > 0 && left <= 5))) {
                        world.admin("announce", {{"message", "Sunucu " + std::to_string(left) + " saniye içinde " +
                            (exit_command == "restart" ? "yeniden başlatılacak." : "kapatılacak.") + (exit_message.empty() ? "" : " " + exit_message)}});
                    }
                    exit_notice = left;
                    if (left == 0) { exit_code = exit_command == "restart" ? 75 : 0; break; }
                }
                auto status = world.status();
                status["instance"] = instance; status["players"] = joined(peers); status["connections"] = peers.size();
                status["tick"] = world.tick(); status["tick_ms"] = last_tick_ms; status["tick_ms_recent"] = std::exchange(recent_ticks, {});
                status["uptime_seconds"] = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count();
                status["world_port"] = ntohs(address.sin_port);
                status["memory_kib"] = resident_kib();
                const auto exit_left = exit_at ? std::max<std::int64_t>(0, std::chrono::ceil<std::chrono::seconds>(*exit_at - Clock::now()).count()) : -1;
                status["countdown"] = exit_at ? ikv::Json{{"command", exit_command}, {"seconds", exit_left}} : ikv::Json(nullptr);
                store.world_status(status);
            } catch (const std::exception&) {
                // Do not keep serving authority when session/lease refresh cannot be enforced.
                std::cerr << "heartbeat_failed\n"; exit_code = 1; break;
            }
            next_heartbeat = Clock::now() + std::chrono::seconds(1);
        }
    }
    listener.close();
    const auto drain_deadline = Clock::now() + close_timeout;
    for (auto& peer : peers) begin_close(peer, "server_stop");
    for (;;) {
        std::array<pollfd, max_connections> pollfds {};
        std::array<Peer*, max_connections> polled {};
        std::size_t count = 0;
        for (auto& peer : peers) {
            advance_close(peer);
            if (peer.socket.get() < 0) continue;
            pollfds[count] = {peer.socket.get(), poll_interest(peer), 0}; polled[count++] = &peer;
        }
        if (count == 0) break;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(drain_deadline - Clock::now()).count();
        if (remaining <= 0) {
            for (auto& peer : peers) disconnect(peer, "close_timeout");
            break;
        }
        const int ready = poll(pollfds.data(), static_cast<nfds_t>(count),
                               static_cast<int>(std::min<std::int64_t>(remaining, 50)));
        if (ready < 0) {
            if (errno == EINTR) continue;
            for (auto& peer : peers) disconnect(peer, "socket");
            exit_code = 1; break;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (pollfds[i].revents & (POLLERR | POLLNVAL)) disconnect(*polled[i], "socket");
            else if (pollfds[i].revents) advance_close(*polled[i]);
        }
    }
    bool saved = true;
    try { world.save_all(); } catch (const std::exception&) { std::cerr << "shutdown_save_failed\n"; saved = false; exit_code = 1; }
    for (auto& peer : peers) {
        if (peer.held_lease && saved) {
            try {
                store.leave_character(peer.identity.account_id, peer.character_id);
                peer.held_lease = false;
            }
            catch (const std::exception&) { std::cerr << "shutdown_release_failed\n"; exit_code = 1; }
        }
    }
    if (saved) try { bots.stop(); } catch (const std::exception&) { std::cerr << "shutdown_release_failed\n"; exit_code = 1; }
    std::cout << "STOP tick=" << world.tick() << std::endl;
    return exit_code;
}
}
int main(int argc, char** argv) {
    try { return run(options(argc, argv)); }
    catch (const std::exception&) { std::cerr << "server startup/runtime error\n"; return 1; }
}
