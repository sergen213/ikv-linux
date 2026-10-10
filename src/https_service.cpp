#include "ikv/https_service.hpp"
#include "ikv/store.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/ssl.h>

#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace ikv {
namespace {
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;
using namespace std::chrono_literals;

bool private_address(const net::ip::address& address) {
    if (address.is_loopback()) return true;
    if (address.is_v4()) {
        const auto n = address.to_v4().to_uint();
        return (n & 0xff000000U) == 0x0a000000U ||
               (n & 0xfff00000U) == 0xac100000U ||
               (n & 0xffff0000U) == 0xc0a80000U;
    }
    const auto v6 = address.to_v6();
    if (v6.is_v4_mapped()) {
        const auto bytes = v6.to_bytes();
        return private_address(net::ip::address_v4({bytes[12], bytes[13], bytes[14], bytes[15]}));
    }
    return (v6.to_bytes()[0] & 0xfeU) == 0xfcU;
}

std::string asset(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open bundled administration assets");
    const auto size = input.tellg();
    if (size < 0 || size > 1024 * 1024) throw std::runtime_error("Invalid administration asset size");
    std::string text(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(text.data(), static_cast<std::streamsize>(text.size())))
        throw std::runtime_error("Cannot read bundled administration assets");
    return text;
}

struct Service {
    Store& store;
    net::io_context& io;
    net::thread_pool& workers;
    std::string html;
    std::string javascript;
    const std::size_t capacity;
    std::size_t active{}; // One I/O thread; public and administration have independent admission slots.
};

class Session : public std::enable_shared_from_this<Session> {
    beast::ssl_stream<beast::tcp_stream> stream_;
    beast::flat_buffer buffer_{73728};
    http::request_parser<http::string_body> parser_;
    http::response<http::string_body> response_;
    Service& service_;
    bool management_;
    bool private_peer_;
    std::string peer_;
    bool closed_{};

    void close() {
        if (closed_) return;
        closed_ = true;
        boost::system::error_code ignored;
        beast::get_lowest_layer(stream_).socket().shutdown(tcp::socket::shutdown_both, ignored);
        beast::get_lowest_layer(stream_).socket().close(ignored);
        --service_.active;
    }

    void respond(unsigned status, std::string body, std::string_view type = "application/json", const std::string& disposition = {}) {
        response_ = {static_cast<http::status>(status), 11};
        response_.set(http::field::content_type, beast::string_view(type.data(), type.size()));
        if (!disposition.empty()) response_.set(http::field::content_disposition, disposition);
        response_.set(http::field::cache_control, "no-store");
        response_.set(http::field::strict_transport_security, "max-age=31536000");
        response_.set("X-Content-Type-Options", "nosniff");
        response_.set("Referrer-Policy", "no-referrer");
        response_.set("Content-Security-Policy",
                      "default-src 'none'; script-src 'self'; style-src 'unsafe-inline'; "
                      "connect-src 'self'; img-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'");
        response_.set(http::field::x_frame_options, "DENY");
        response_.keep_alive(false);
        response_.body() = std::move(body);
        response_.prepare_payload();
        beast::get_lowest_layer(stream_).expires_after(10s);
        http::async_write(stream_, response_, [self = shared_from_this()](boost::system::error_code ec, std::size_t) {
            // One request per connection keeps admission/deadlines bounded without idle keepalive slots.
            if (ec) { self->close(); return; }
            beast::get_lowest_layer(self->stream_).expires_after(10s);
            self->stream_.async_shutdown([self](boost::system::error_code) { self->close(); });
        });
    }

    void error(unsigned status, std::string_view code, std::string_view message) {
        respond(status, Json{{"error", {{"code", code}, {"message", message}}}}.dump());
    }

    void dispatch() {
        const auto& request = parser_.get();
        const std::string target(request.target());
        if (target.empty() || target.front() != '/' || target.size() > 2048 ||
            target.find_first_of("\r\n\t #") != std::string::npos) {
            error(400, "invalid_target", "A bounded origin-form request target is required");
            return;
        }
        const auto query = target.find('?');
        const std::string_view path(target.data(), query == std::string::npos ? target.size() : query);
        const bool admin_route = path.starts_with("/v1/admin") || path.starts_with("/admin");
        if (admin_route && (!management_ || !private_peer_)) {
            error(404, "not_found", "Route not found");
            return;
        }
        if (query != std::string::npos) {
            const std::string_view parameters(target.data() + query + 1, target.size() - query - 1);
            if (request.method() != http::verb::get ||
                (path != "/v1/admin/accounts" && path != "/v1/admin/characters") ||
                !parameters.starts_with("search=") || parameters.find('&') != std::string_view::npos) {
                error(400, "invalid_query", "Only administration search may appear in a request URL");
                return;
            }
        }
        if (request.count(http::field::host) != 1 || request.count(http::field::authorization) > 1 ||
            request.count(http::field::origin) > 1) {
            error(400, "invalid_headers", "A single Host and unambiguous authorization are required");
            return;
        }
        const auto origin = request[http::field::origin];
        if (!origin.empty() && origin != "https://" + std::string(request[http::field::host])) {
            error(403, "cross_origin", "Cross-origin requests are not permitted");
            return;
        }
        if (request.method() == http::verb::get && (path == "/admin" || path == "/admin/")) {
            respond(200, service_.html, "text/html; charset=utf-8");
            return;
        }
        if (request.method() == http::verb::get && path == "/admin.js") {
            respond(200, service_.javascript, "text/javascript; charset=utf-8");
            return;
        }
        Json body = Json::object();
        if (!request.body().empty()) {
            const auto header_type = request[http::field::content_type];
            const std::string_view type(header_type.data(), header_type.size());
            if (!type.starts_with("application/json") ||
                (type.size() > 16 && type[16] != ';')) {
                error(415, "content_type", "Request bodies must use application/json");
                return;
            }
            body = Json::parse(request.body(), nullptr, false);
            if (body.is_discarded() || !body.is_object()) {
                error(400, "invalid_json", "A JSON object is required");
                return;
            }
        }
        std::string bearer;
        const auto header_authorization = request[http::field::authorization];
        const std::string_view authorization(header_authorization.data(), header_authorization.size());
        if (!authorization.empty()) {
            if (!authorization.starts_with("Bearer ") || authorization.size() != 71) {
                error(401, "invalid_session", "Geçerli bir oturum gerekli; yeniden giriş yapın.");
                return;
            }
            bearer = std::string(authorization.substr(7));
        }
        const std::string method(request.method_string());
        const bool offline_restore = method == "POST" && path == "/v1/admin/restore";
        // Downloads are raw SQL, not JSON; Store validates the name and administrator role before returning bytes.
        const std::string download = method == "GET" && path.starts_with("/v1/admin/backups/")
            ? std::string(path.substr(std::string_view("/v1/admin/backups/").size())) : std::string();
        net::post(service_.workers, [self = shared_from_this(), method, target, bearer = std::move(bearer),
                                      body = std::move(body), offline_restore, download]() {
            unsigned status = 200;
            std::string result;
            bool file = false;
            try {
                if (offline_restore) {
                    const auto identity = self->service_.store.authenticate(bearer);
                    if (!identity.admin) {
                        status = 403;
                        result = Json{{"error", {{"code", "admin_required"}, {"message", "Administrator role required"}}}}.dump();
                    } else {
                        status = 409;
                        result = Json{{"error", {{"code", "offline_restore_required"},
                            {"message", "Stop auth and world, then use ikv-auth --dsn-file FILE --restore-backup FILE"}}}}.dump();
                    }
                } else if (!download.empty()) {
                    result = self->service_.store.backup_download(bearer, download, self->peer_);
                    file = true;
                } else {
                    result = self->service_.store.api(method, target, bearer, body, self->peer_).dump();
                }
            } catch (const ApiError& e) {
                status = static_cast<unsigned>(e.status);
                result = Json{{"error", {{"code", e.code}, {"message", e.what()}}}}.dump();
            } catch (...) {
                status = 500;
                result = R"({"error":{"code":"internal_error","message":"İşlem tamamlanamadı."}})";
            }
            net::post(self->service_.io, [self, status, result = std::move(result), file, download]() mutable {
                if (file) self->respond(status, std::move(result), "application/sql", "attachment; filename=\"" + download + "\"");
                else self->respond(status, std::move(result));
            });
        });
    }

public:
    Session(tcp::socket socket, ssl::context& tls, Service& service, bool management)
        : stream_(std::move(socket), tls), service_(service), management_(management) {
        const auto address = beast::get_lowest_layer(stream_).socket().remote_endpoint().address();
        private_peer_ = private_address(address);
        peer_ = std::string(management_ ? "admin:" : "player:") + address.to_string();
        parser_.body_limit(65536);
        parser_.header_limit(8192);
    }

    void start() {
        ++service_.active;
        beast::get_lowest_layer(stream_).expires_after(10s);
        stream_.async_handshake(ssl::stream_base::server, [self = shared_from_this()](boost::system::error_code ec) {
            if (ec) { self->close(); return; }
            beast::get_lowest_layer(self->stream_).expires_after(10s);
            http::async_read(self->stream_, self->buffer_, self->parser_,
                [self](boost::system::error_code read_error, std::size_t) {
                    if (!read_error) { self->dispatch(); return; }
                    if (read_error == http::error::body_limit)
                        self->error(413, "body_too_large", "Request body exceeds 64 KiB");
                    else if (read_error == http::error::header_limit)
                        self->error(431, "headers_too_large", "Request headers exceed 8 KiB");
                    else if (read_error == beast::error::timeout || read_error == net::error::operation_aborted ||
                             read_error == net::error::eof || read_error == ssl::error::stream_truncated)
                        self->close();
                    else self->error(400, "invalid_http", "Malformed HTTP request");
                });
        });
    }
};

class Listener {
    tcp::acceptor acceptor_;
    ssl::context& tls_;
    Service& service_;
    bool management_;
public:
    Listener(net::io_context& io, ssl::context& tls, Service& service,
             const std::string& bind, std::uint16_t port, bool management)
        : acceptor_(io), tls_(tls), service_(service), management_(management) {
        const auto address = net::ip::make_address(bind);
        if (management && !private_address(address))
            throw std::runtime_error("Administration bind must be loopback, RFC1918 or IPv6 ULA");
        const tcp::endpoint endpoint(address, port);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(net::socket_base::reuse_address(true));
        if (address.is_v6()) acceptor_.set_option(net::ip::v6_only(true));
        acceptor_.bind(endpoint);
        acceptor_.listen(static_cast<int>(service_.capacity));
    }
    std::uint16_t port() const { return acceptor_.local_endpoint().port(); }
    void start() {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                boost::system::error_code endpoint_error;
                const auto peer = socket.remote_endpoint(endpoint_error);
                if (!endpoint_error && service_.active < service_.capacity && (!management_ || private_address(peer.address())))
                    std::make_shared<Session>(std::move(socket), tls_, service_, management_)->start();
                else socket.close(ec);
            }
            if (acceptor_.is_open() && !service_.io.stopped()) start();
        });
    }
};
} // namespace

void run_https(Store& store, const HttpsConfig& config) {
    net::io_context io;
    ssl::context tls(ssl::context::tls_server);
    tls.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3 |
                    ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
    if (SSL_CTX_set_min_proto_version(tls.native_handle(), TLS1_2_VERSION) != 1)
        throw std::runtime_error("Cannot enforce TLS 1.2 minimum");
    tls.use_certificate_chain_file(config.certificate.string());
    tls.use_private_key_file(config.private_key.string(), ssl::context::pem);
    if (SSL_CTX_check_private_key(tls.native_handle()) != 1)
        throw std::runtime_error("TLS certificate and private key do not match");
    net::thread_pool player_workers(4);
    net::thread_pool admin_workers(2);
    Service player_service{store, io, player_workers, {}, {}, 8, 0};
    Service admin_service{store, io, admin_workers, asset(config.web_root / "admin.html"),
                          asset(config.web_root / "admin.js"), 4, 0};
    Listener player(io, tls, player_service, config.bind, config.port, false);
    Listener admin(io, tls, admin_service, config.admin_bind, config.admin_port, true);
    net::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&io](boost::system::error_code, int) { io.stop(); });
    player.start();
    admin.start();
    std::cout << "READY auth_port=" << player.port() << " admin_port=" << admin.port() << std::endl;
    io.run();
    player_workers.join();
    admin_workers.join();
}
} // namespace ikv
