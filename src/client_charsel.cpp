#include "ikv/client_charsel.hpp"

#include "ikv/audio.hpp"
#include "ikv/client_ui.hpp"
#include "ikv/effects.hpp"
#include "ikv/protocol.hpp"
#include "ikv/renderer.hpp"

#include <SDL3/SDL.h>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace ikv {
namespace {
using Json = nlohmann::json;
using Rgba = std::array<std::uint8_t, 4>;
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace http = beast::http;

// ---- HTTPS character API ----
// Bounded synchronous exchange: every asynchronous step has a 10 s stream deadline.
net::awaitable<void> exchange(beast::ssl_stream<beast::tcp_stream>& stream, const std::string& host,
                              const std::string& port, http::request<http::string_body>& request,
                              http::response_parser<http::string_body>& parser) {
    auto& tcp = beast::get_lowest_layer(stream);
    net::ip::tcp::resolver resolver(co_await net::this_coro::executor);
    tcp.expires_after(std::chrono::seconds(10));
    const auto endpoints = co_await resolver.async_resolve(host, port, net::use_awaitable);
    co_await tcp.async_connect(endpoints, net::use_awaitable);
    tcp.expires_after(std::chrono::seconds(10));
    co_await stream.async_handshake(ssl::stream_base::client, net::use_awaitable);
    tcp.expires_after(std::chrono::seconds(10));
    co_await http::async_write(stream, request, net::use_awaitable);
    beast::flat_buffer buffer;
    co_await http::async_read(stream, buffer, parser, net::use_awaitable);
    tcp.expires_after(std::chrono::seconds(2));
    boost::system::error_code ignored;
    co_await stream.async_shutdown(net::redirect_error(net::use_awaitable, ignored));
}
std::string text(const Json& value, std::size_t limit) {
    if (!value.is_string() || value.get_ref<const std::string&>().empty() || value.get_ref<const std::string&>().size() > limit)
        throw std::runtime_error("account service returned an invalid character row");
    return value.get<std::string>();
}
Json character_row(const Json& c) {
    if (!c.is_object() || !c.contains("id") || !c.at("id").is_number_integer() || c.at("id").get<std::int64_t>() <= 0 ||
        !c.contains("class_id") || !c.at("class_id").is_number_integer() || c.at("class_id").get<std::int64_t>() < 1 ||
        c.at("class_id").get<std::int64_t>() > 3 || !c.contains("appearance") || !valid_appearance(c.at("appearance")) ||
        !c.contains("state") || !c.at("state").is_object())
        throw std::runtime_error("account service returned an invalid character row");
    const auto& state = c.at("state");
    const auto level = state.contains("level") && state.at("level").is_number_integer() ? state.at("level").get<std::int64_t>() : 1;
    // state.visual: the world's worn {slot, source_uid} list (World::persisted_visual); absent before the first save.
    Json visual = Json::array();
    if (state.contains("visual")) {
        const auto& list = state.at("visual");
        if (!list.is_array() || list.size() > 16) throw std::runtime_error("account service returned an invalid character row");
        for (const auto& item : list) {
            if (!item.is_object() || !item.contains("slot") || !item.at("slot").is_number_unsigned() || item.at("slot").get<unsigned>() > 15 ||
                !item.contains("source_uid") || !item.at("source_uid").is_number_unsigned() || item.at("source_uid").get<std::uint64_t>() > UINT32_MAX)
                throw std::runtime_error("account service returned an invalid character row");
            visual.push_back({{"slot", item.at("slot")}, {"source_uid", item.at("source_uid")}});
        }
    }
    return {{"id", c.at("id").get<std::uint64_t>()}, {"name", text(c.at("name"), 64)}, {"class_id", c.at("class_id")},
            {"appearance", c.at("appearance")}, {"level", std::clamp<std::int64_t>(level, 1, 1000)},
            {"zone", state.contains("zone") && state.at("zone").is_string() ? state.at("zone").get<std::string>().substr(0, 64) : std::string{}},
            {"visual", std::move(visual)}};
}
} // namespace

CharacterApi::CharacterApi(std::string_view origin, std::filesystem::path ca, std::string_view token)
    : ca_(std::move(ca)), token_(token) {
    // Origin only: https://host[:port][/]; no credentials, path, query or fragment.
    if (!origin.starts_with("https://")) throw std::invalid_argument("--auth requires an https:// origin");
    origin.remove_prefix(8);
    if (origin.ends_with('/')) origin.remove_suffix(1);
    if (origin.empty() || origin.find_first_of("/?#@\\ \t\r\n") != std::string_view::npos)
        throw std::invalid_argument("--auth must be an HTTPS origin without path, query or credentials");
    std::string_view host = origin, port = "443";
    if (origin.front() == '[') {
        const auto close = origin.find(']');
        if (close == std::string_view::npos) throw std::invalid_argument("invalid --auth IPv6 origin");
        host = origin.substr(1, close - 1);
        if (close + 1 < origin.size()) {
            if (origin[close + 1] != ':') throw std::invalid_argument("invalid --auth origin");
            port = origin.substr(close + 2);
        }
    } else if (const auto colon = origin.rfind(':'); colon != std::string_view::npos) {
        host = origin.substr(0, colon); port = origin.substr(colon + 1);
    }
    int number = 0;
    for (const char c : port) number = c >= '0' && c <= '9' && number < 65536 ? number * 10 + (c - '0') : 70000;
    if (host.empty() || host.size() > 253 || port.empty() || number < 1 || number > 65535)
        throw std::invalid_argument("invalid --auth host or port");
    host_ = host; port_ = port;
}
Json CharacterApi::request(std::string_view method, const std::string& target, const Json& body) {
    net::io_context io;
    ssl::context tls(ssl::context::tls_client);
    tls.set_options(ssl::context::no_compression);
    if (SSL_CTX_set_min_proto_version(tls.native_handle(), TLS1_2_VERSION) != 1)
        throw std::runtime_error("TLS configuration failed");
    tls.load_verify_file(ca_.string());
    tls.set_verify_mode(ssl::verify_peer);
    beast::ssl_stream<beast::tcp_stream> stream(io, tls);
    auto* handle = stream.native_handle();
    auto* verification = SSL_get0_param(handle);
    X509_VERIFY_PARAM_set_hostflags(verification, X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    boost::system::error_code literal;
    (void)net::ip::make_address(host_, literal);
    if (!literal) {
        if (X509_VERIFY_PARAM_set1_ip_asc(verification, host_.c_str()) != 1) throw std::runtime_error("TLS address pinning failed");
    } else if (SSL_set1_host(handle, host_.c_str()) != 1 || SSL_set_tlsext_host_name(handle, host_.c_str()) != 1)
        throw std::runtime_error("TLS host name pinning failed");
    http::request<http::string_body> request{http::string_to_verb(method), target, 11};
    request.set(http::field::host, port_ == "443" ? host_ : host_ + ":" + port_);
    request.set(http::field::user_agent, "IKV-native-client/1");
    request.set(http::field::accept, "application/json");
    request.set(http::field::authorization, "Bearer " + std::string(token_));
    if (!body.is_null()) { request.set(http::field::content_type, "application/json"); request.body() = body.dump(); }
    request.prepare_payload();
    http::response_parser<http::string_body> parser;
    parser.body_limit(1024 * 1024);
    std::exception_ptr failure;
    net::co_spawn(io, exchange(stream, host_, port_, request, parser), [&](std::exception_ptr e) { failure = e; });
    io.run();
    if (failure) {
        try { std::rethrow_exception(failure); }
        catch (const std::exception& e) { throw std::runtime_error(std::string("Hesap sunucusuna ulaşılamadı: ") + e.what()); }
    }
    const auto& response = parser.get();
    const auto& bytes = response.body();
    // A raw NUL could end nlohmann's parse early and smuggle a suffix (escaped \u0000 stays valid).
    Json value = bytes.find('\0') == std::string::npos ? Json::parse(bytes, nullptr, false) : Json(Json::value_t::discarded);
    const auto status = response.result_int();
    if (status < 200 || status >= 300) {
        std::string message;
        if (value.is_object() && value.contains("error") && value.at("error").is_object() && value.at("error").contains("message") &&
            value.at("error").at("message").is_string()) message = value.at("error").at("message").get<std::string>().substr(0, 256);
        throw std::runtime_error(message.empty() ? "Hesap sunucusu isteği reddetti (" + std::to_string(status) + ")" : message);
    }
    if (value.is_discarded()) throw std::runtime_error("account service returned invalid JSON");
    return value;
}
Json CharacterApi::list() {
    const auto value = request("GET", "/v1/characters", nullptr);
    if (!value.is_object() || !value.contains("characters") || !value.at("characters").is_array() || value.at("characters").size() > 64)
        throw std::runtime_error("account service returned an invalid character list");
    Json rows = Json::array();
    for (const auto& c : value.at("characters")) rows.push_back(character_row(c));
    return rows;
}
void CharacterApi::create(std::string_view name, int class_id, const Json& appearance) {
    (void)character_row(request("POST", "/v1/characters", {{"name", name}, {"class_id", class_id}, {"appearance", appearance}}));
}
void CharacterApi::remove(std::uint64_t id, std::string_view confirm_name) {
    (void)request("DELETE", "/v1/characters/" + std::to_string(id), {{"confirm_name", confirm_name}});
}


namespace {
// ---- Screen ----
constexpr std::uint32_t slot_entity = 0x7f000000u, preview_entity = slot_entity + 8;
constexpr float chargen_yaw = 2.45f; // camera heading measured from the VOD 00:30 backdrop
constexpr Rgba white{255, 255, 255, 255}, yellow{255, 255, 0, 255}, shadow{0, 0, 0, 255};
constexpr std::array<const char*, 3> class_names{"Savaşçı", "Büyücü", "Şifacı"};
// Original creator tables (protocol.cpp valid_appearance): {female, male} upper bounds.
constexpr std::array<std::array<int, 2>, 5> cosmetic_max{{{9, 9}, {20, 17}, {9, 4}, {24, 14}, {1, 4}}};
constexpr std::array<const char*, 5> cosmetic_keys{"face", "hair", "hair_color", "beard", "beard_color"};
// 004afed0 HAZIRLAYANLAR: lines added by 0048b0a0 to a cTextBox (0048ae50) capped at 367 lines (0048acf0 0x16f), so the
// first of the 368 adds (a spacer) is dropped. ARGB colour 0 marks the invisible "." spacers.
struct CreditLine { std::uint32_t argb; const char* text; };
constexpr std::array<CreditLine, 367> credit_lines{{
    {0xff22dd22u, "İSTANBUL \"KIYAMET VAKTİ\""}, {0xff22dd22u, "KARAKÖY"}, {0x00000000u, "."},
    {0xffffff11u, "\"SOBEE STUDIOS\""}, {0x00000000u, "."}, {0xffff77aau, "Proje lideri, Lider Oyun Tasarımı ve Senaryo"},
    {0xffffffffu, "Ö. Kürşad Karamahmutoğlu"}, {0x00000000u, "."}, {0xffff77aau, "Lider Artist"}, {0xffffffffu, "Murat Afşar"},
    {0x00000000u, "."}, {0xffff77aau, "Lider Bölüm Tasarımcısı"}, {0xffffffffu, "H. Fevzi Altuncu"}, {0x00000000u, "."},
    {0xffff77aau, "Lider Programcı"}, {0xffffffffu, "Erdoğan Atmaca"}, {0x00000000u, "."},
    {0xffff77aau, "Görsel Geliştirme ve Tasarım Yönetmeni"}, {0xffffffffu, "Kerem S. Hünal"}, {0x00000000u, "."},
    {0xffff77aau, "Proje Lideri Yardımcısı"}, {0xffffffffu, "Nihat Özpay"}, {0x00000000u, "."},
    {0xffff77aau, "Yardımcı Oyun Tasarımı"}, {0xffffffffu, "Muhammed Yeşilmen"}, {0x00000000u, "."},
    {0xffff77aau, "Konsept Grafik"}, {0xffffffffu, "Kerem S. Hünal"}, {0x00000000u, "."}, {0xffff77aau, "Bölüm Tasarımı"},
    {0xffffffffu, "H. Fevzi Altuncu"}, {0xffffffffu, "Ö. Kürşad Karamahmutoğlu"}, {0xffffffffu, "Burak Karamuk"},
    {0x00000000u, "."}, {0xffff77aau, "Karakter Modelleme ve Animasyonları"}, {0xffffffffu, "Murat Afşar"}, {0x00000000u, "."},
    {0xffff77aau, "3B Grafik"}, {0xffffffffu, "Murat Afşar"}, {0xffffffffu, "H. Fevzi Altuncu"},
    {0xffffffffu, "Ö. Kürşad Karamahmutoğlu"}, {0xffffffffu, "Burak Karamuk"}, {0x00000000u, "."}, {0xffff77aau, "Programlama"},
    {0xffffffffu, "Erdoğan Atmaca"}, {0xffffffffu, "Burak Demirkol"}, {0x00000000u, "."},
    {0xffff77aau, "Programlama Yardımcısı"}, {0xffffffffu, "Muhammed Yeşilmen"}, {0x00000000u, "."}, {0xffff77aau, "2B Grafik"},
    {0xffffffffu, "Kerem S. Hünal"}, {0x00000000u, "."}, {0xffff77aau, "Görsel Efektler"},
    {0xffffffffu, "Ö. Kürşad Karamahmutoğlu"}, {0x00000000u, "."}, {0xffff77aau, "Ses Efektleri"}, {0xffffffffu, "Nihat Özpay"},
    {0x00000000u, "."}, {0xffff77aau, "Müzikler"}, {0xffffffffu, "Emre Yücelen"}, {0x00000000u, "."}, {0xffffffffu, "Hakir"},
    {0xffffffffu, "Haliç"}, {0xffffffffu, "Köprü"}, {0xffffffffu, "Ölümden Sonra"}, {0xffffffffu, "Tamamlanmamış Senfoni"},
    {0xffffffffu, "Welcome to War"}, {0x00000000u, "."}, {0xffff77aau, "Ek Müzikler"}, {0xffffffffu, "Emir Diril"},
    {0x00000000u, "."}, {0xffffffffu, "Farelerin Öfkesi"}, {0xffffffffu, "Gustav"}, {0xffffffffu, "Hybanda Köy*"},
    {0x00000000u, "."}, {0xffff77aau, "Ud Taksimi"}, {0xffffffffu, "Fethi Karamahmutoğlu*"}, {0x00000000u, "."},
    {0xffff77aau, "Seslendirme Yönetmeni"}, {0xffffffffu, "Tuba Erdem"}, {0x00000000u, "."}, {0xffff77aau, "Seslendirenler"},
    {0xffffffffu, "Engin Alkan"}, {0xffffffffu, "Zeki Atlı"}, {0xffffffffu, "Arda Aydın"}, {0xffffffffu, "Ayşegül Bingöl"},
    {0xffffffffu, "Bora Sivri"}, {0x00000000u, "."}, {0xffff77aau, "Teşekkürler"}, {0xffffffffu, "Halide Karamahmutoğlu"},
    {0xffffffffu, "Cem Sermen"}, {0xffffffffu, "Başar Ünder"}, {0x00000000u, "."},
    {0xffff77aau, "Web Sitesi ve Forum Geliştirme"}, {0xffffffffu, "Nihat Özpay"}, {0xffffffffu, "Muhammed Yeşilmen"},
    {0x00000000u, "."}, {0xffff77aau, "Teaser Çizimleri"}, {0xffffffffu, "Kerem S. Hünal"}, {0x00000000u, "."},
    {0xffff77aau, "Teaser Montaj"}, {0xffffffffu, "Kerem S. Hünal"}, {0xffffffffu, "Nihat Özpay"}, {0x00000000u, "."},
    {0xffff77aau, "Lider Topluluk Yöneticisi"}, {0xffffffffu, "Nihat Özpay"}, {0x00000000u, "."}, {0xffff77aau, "Test Lideri"},
    {0xffffffffu, "Muhammed Yeşilmen"}, {0x00000000u, "."}, {0xffff77aau, "Teknik Destek"}, {0xffffffffu, "Deniz Kılıç"},
    {0x00000000u, "."}, {0xffff77aau, "İç Sürüm Testi"}, {0xffffffffu, "Özlem Duru"}, {0xffffffffu, "Ümit Gül"},
    {0xffffffffu, "Sinan Karaduman"}, {0xffffffffu, "Turan Savacı"}, {0xffffffffu, "Oktay Şen"}, {0xffffffffu, "İhsan Yedier"},
    {0x00000000u, "."}, {0xffff77aau, "Yardımcı Testler"}, {0xffffffffu, "Numan Atmaca"}, {0xffffffffu, "M.Yasin Ay"},
    {0xffffffffu, "Ali Barışan"}, {0xffffffffu, "M. Can Berrak"}, {0xffffffffu, "Serkan Boğa"}, {0xffffffffu, "Oğuzhan Buğday"},
    {0xffffffffu, "Nazlıcan Çopur"}, {0xffffffffu, "Erhan Duran"}, {0xffffffffu, "Enes Güneş"}, {0xffffffffu, "H.Ö.Faruk Kafes"},
    {0xffffffffu, "M. Çağlar Kılıç"}, {0xffffffffu, "Mirkan Kızıl"}, {0xffffffffu, "Gökhan Kuş"}, {0xffffffffu, "Mehmet Mart"},
    {0xffffffffu, "Nuri Öztürk"}, {0xffffffffu, "Alp Onur Serap"}, {0xffffffffu, "Muhsin Tuğrul"},
    {0xffffffffu, "Murat Yıldırım"}, {0x00000000u, "."}, {0xffff77aau, "Kaynaklar"},
    {0xffffffffu, "Derviş Hasan'ın söylediği bazı özlü sözler; Gölpınarlı A. (1970). Mevlana Celaleddin Divan-ı Kebir'den Seçmeler İstanbul: Milli Eğitim Basımevi"},
    {0x00000000u, "."},
    {0xffffffffu, "G.R.C. Kapı Görevlisi'nin bazı sıradan metinleri; Deniz H. (1969) En Yeni Muaşeret Kaideleri İstanbul: Anten Yayınevi"},
    {0x00000000u, "."},
    {0xffffffffu, "Fethi Karamahmutoğlu'nun ud taksimi sanatçının deneme kayıtlarından hak sahiplerinin izni ile kullanılmıştır."},
    {0x00000000u, "."},
    {0xffffffffu, "Perihan Hanım'ın mektubu, Halide Karamahmutoğlu tarafından yeniden düzenlenmiş ve onun el yazısı ile kaleme alınmıştır."},
    {0x00000000u, "."},
    {0xffffffffu, "“Radyo Tiyatrosu” seslendirmeleri ve Hybanda Köy müziği, Kader Mühürleri oyunundan alınmıştır."},
    {0x00000000u, "."}, {0xff22dd22u, "İSTANBUL \"KIYAMET VAKTİ\""}, {0xff22dd22u, "ARZ VE LODOS PAKETİ"}, {0x00000000u, "."},
    {0xffffff11u, "\"SOBEE STUDIOS\""}, {0x00000000u, "."}, {0xffff77aau, "Ana Ekip  (Alfabetik)"}, {0x00000000u, "."},
    {0xffffffffu, "Murat Afşar"}, {0xffffffffu, "Gökhan Altay"}, {0xffffffffu, "Fevzi Altuncu"}, {0xffffffffu, "Mevlüt Dinç"},
    {0xffffffffu, "Ö. Kürşad Karamahmutoğlu"}, {0xffffffffu, "Niyazi Selimoğlu"}, {0xffffffffu, "Cem Sermen"},
    {0xffffffffu, "N. Özgür Soner"}, {0xffffffffu, "Ali Tunç"}, {0xffffffffu, "Emre Yücelen"}, {0x00000000u, "."},
    {0xffff77aau, "Yardımcı Ekip"}, {0x00000000u, "."}, {0xffffffffu, "Gökçe Abatay"}, {0xffffffffu, "Ferdi Bakırezer"},
    {0xffffffffu, "Fatih Güngör"}, {0xffffffffu, "Nihat Özpay"}, {0xffffffffu, "Muhammed Yeşilmen"}, {0x00000000u, "."},
    {0xff22dd22u, "İSTANBUL \"KIYAMET VAKTİ\""}, {0xff22dd22u, "ÇEMBERLİTAŞ PAKETİ"}, {0x00000000u, "."},
    {0xffffff11u, "\"SOBEE STUDIOS\""}, {0x00000000u, "."}, {0xffff77aau, "Ana Ekip  (Alfabetik)"}, {0x00000000u, "."},
    {0xffffffffu, "Murat Afşar"}, {0xffffffffu, "Fevzi Altuncu"}, {0xffffffffu, "Mevlüt Dinç"},
    {0xffffffffu, "Ö. Kürşad Karamahmutoğlu"}, {0xffffffffu, "N. Özgür Soner"}, {0xffffffffu, "Ali Tunç"},
    {0xffffffffu, "Emre Yücelen"}, {0x00000000u, "."}, {0xffff77aau, "Yardımcı Ekip"}, {0x00000000u, "."},
    {0xffffffffu, "Yalım Alatlı"}, {0xffffffffu, "Gökçe Atabay"}, {0xffffffffu, "Kerem Hünal"}, {0xffffffffu, "Nihat Özpay"},
    {0xffffffffu, "Arif Ç. Türken"}, {0xffffffffu, "Muhammed Yeşilmen"}, {0x00000000u, "."}, {0xffff77aau, "İç Sürüm Testi"},
    {0x00000000u, "."}, {0xffffffffu, "Ahmet Avcı"}, {0xffffffffu, "Yasin Cihan"}, {0xffffffffu, "Onur Gürsoy"},
    {0xffffffffu, "Fatih Kaplan"}, {0xffffffffu, "Ferdi Tatlısu"}, {0xffffffffu, "Hatem Zencir"}, {0x00000000u, "."},
    {0xff22dd22u, "GRUP BÖLGELERİ PAKETİ"}, {0x00000000u, "."}, {0xffffff11u, "\"SOBEE STUDIOS\""}, {0x00000000u, "."},
    {0xffff77aau, "Ana Ekip (Alfabetik)"}, {0x00000000u, "."}, {0xffffffffu, "Murat Afşar"}, {0xffffffffu, "Yalım Alatlı"},
    {0xffffffffu, "Fevzi Altuncu"}, {0xffffffffu, "Mevlüt Dinç"}, {0xffffffffu, "Ö. Kürşad Karamahmutoğlu"},
    {0xffffffffu, "N. Özgür Soner"}, {0xffffffffu, "Emre Yücelen"}, {0x00000000u, "."}, {0xffff77aau, "Yardımcı Ekip"},
    {0x00000000u, "."}, {0xffffffffu, "Gökçe Atabay (Klavuz Grafik Tasarım)"}, {0xffffffffu, "Kerem Hünal (Video)"},
    {0xffffffffu, "Nihat Özpay (Test Lideri, Ses)"}, {0xffffffffu, "Cem Sermen (Dengeleme Yardım)"},
    {0xffffffffu, "Ali Tunç (Kutu Tasarım)"}, {0xffffffffu, "Arif Ç. Türken (Test Lideri)"}, {0x00000000u, "."},
    {0xffff77aau, "İç Sürüm Testi"}, {0x00000000u, "."}, {0xffffffffu, "Ahmet Avcı"}, {0xffffffffu, "Bülent Birsen"},
    {0xffffffffu, "Oğuzhan Erdoğan"}, {0xffffffffu, "Muhammed Yeşilmen"}, {0xffffffffu, "İsmet Soner Yılmazer"},
    {0x00000000u, "."}, {0xff22dd22u, "İSTANBUL \"KIYAMET VAKTİ\""}, {0xff22dd22u, "DERİNDEKİ SIR PAKETİ"}, {0x00000000u, "."},
    {0xffffff11u, "\"SOBEE STUDIOS\""}, {0x00000000u, "."}, {0xffff77aau, "Ana Ekip (Alfabetik)"}, {0x00000000u, "."},
    {0xffffffffu, "Murat Afşar"}, {0xffffffffu, "Yalım Alatlı"}, {0xffffffffu, "Fevzi Altuncu"}, {0xffffffffu, "Mevlüt Dinç"},
    {0xffffffffu, "Ö. Kürşad Karamahmutoğlu"}, {0xffffffffu, "N. Özgür Soner"}, {0xffffffffu, "Emre Yücelen"},
    {0x00000000u, "."}, {0xff22dd22u, "İSTANBUL \"KIYAMET VAKTİ\""}, {0xff22dd22u, "METEOR VE ADA BÖLGELERİ PAKETİ"},
    {0x00000000u, "."}, {0xffffff11u, "\"SOBEE STUDIOS\""}, {0x00000000u, "."}, {0xffff77aau, "Ana Ekip (Alfabetik)"},
    {0x00000000u, "."}, {0xffffffffu, "Murat Afşar"}, {0xffffffffu, "Yalım Alatlı"}, {0xffffffffu, "Fevzi Altuncu"},
    {0xffffffffu, "Mevlüt Dinç"}, {0xffffffffu, "Ö. Kürşad Karamahmutoğlu"}, {0xffffffffu, "N. Özgür Soner"},
    {0xffffffffu, "Emre Yücelen"}, {0x00000000u, "."}, {0xffff77aau, "Yardımcı Ekip"}, {0x00000000u, "."},
    {0xffffffffu, "Devrim Dikol (Ek Optimizasyon)"}, {0x00000000u, "."}, {0xff22dd22u, "İSTANBUL \"KIYAMET VAKTİ\""},
    {0x00000000u, "."}, {0xffffffffu, "Hazırlayanlar (Alfabetik)"}, {0x00000000u, "."}, {0xffffff11u, "\"SOBEE STUDIOS\""},
    {0x00000000u, "."}, {0xffff77aau, "DİREKTÖR"}, {0x00000000u, "."}, {0xffffffffu, "Mevlüt Dinç"}, {0x00000000u, "."},
    {0xffff77aau, "PROJE LİDERİ"}, {0x00000000u, "."}, {0xffffffffu, "N. Özgür Soner"}, {0x00000000u, "."},
    {0xffff77aau, "OYUN TASARIMI"}, {0x00000000u, "."}, {0xffffffffu, "Ö. Kürşad Karamahmutoğlu"},
    {0xffffffffu, "N. Özgür Soner"}, {0x00000000u, "."}, {0xffff77aau, "SENARYO"}, {0x00000000u, "."},
    {0xffffffffu, "Ö. Kürşad Karamahmutoğlu"}, {0xffffffffu, "N. Özgür Soner"}, {0x00000000u, "."}, {0xffff77aau, "PROGRAMLAMA"},
    {0x00000000u, "."}, {0xffffffffu, "Bilgem Çakır"}, {0xffffffffu, "Deniz Piri"}, {0xffffffffu, "Oğuz Sandıkçı"},
    {0xffffffffu, "Cem Sermen"}, {0xffffffffu, "N. Özgür Soner"}, {0x00000000u, "."}, {0xffff77aau, "3B GRAFİK"},
    {0x00000000u, "."}, {0xffffffffu, "Murat Afşar"}, {0xffffffffu, "Fevzi Altuncu"}, {0xffffffffu, "Ö. Kürşad Karamahmutoğlu"},
    {0x00000000u, "."}, {0xffff77aau, "2B GRAFİK"}, {0x00000000u, "."}, {0xffffffffu, "Yalım Alatlı"},
    {0xffffffffu, "Ö. Kürşad Karamahmutoğlu"}, {0x00000000u, "."}, {0xffff77aau, "KAPLAMALAR"}, {0x00000000u, "."},
    {0xffffffffu, "Murat Afşar"}, {0xffffffffu, "Yalım Alatlı"}, {0xffffffffu, "Ö. Kürşad Karamahmutoğlu"}, {0x00000000u, "."},
    {0xffff77aau, "ANİMASYONLAR"}, {0x00000000u, "."}, {0xffffffffu, "Murat Afşar"}, {0x00000000u, "."},
    {0xffff77aau, "BÖLÜM TASARIMI"}, {0x00000000u, "."}, {0xffffffffu, "Fevzi Altuncu"},
    {0xffffffffu, "Ö. Kürşad Karamahmutoğlu"}, {0x00000000u, "."}, {0xffff77aau, "KONSEPT GRAFİK ve MATERYALLER"},
    {0x00000000u, "."}, {0xffffffffu, "Yalım Alatlı"}, {0xffffffffu, "Ö. Kürşad Karamahmutoğlu"}, {0x00000000u, "."},
    {0xffff77aau, "ORİJİNAL MÜZİK"}, {0x00000000u, "."}, {0xffffffffu, "Emre Yücelen"}, {0x00000000u, "."},
    {0xffff77aau, "PROJEYE ÖNCEDEN KATKISI OLANLAR"}, {0x00000000u, "."}, {0xffffffffu, "Tunç Yalgın (Proje Asistanı)"},
    {0xffffffffu, "Engin Cilasun (Programlama)"}, {0xffffffffu, "Serkan Özay (2B Grafik)"},
    {0xffffffffu, "Genco Serpen (Programlama)"}, {0xffffffffu, "Tolga Tatari (Web Tasarım)"},
    {0xffffffffu, "Atilla Uygun (Programlama)"}, {0x00000000u, "."}, {0xffff77aau, "TEŞEKKÜRLER"}, {0x00000000u, "."},
    {0xffffffffu, "Yusuf Ağabey"}, {0xffffffffu, "Türkçan Ağabey"}, {0xffffffffu, "Yakup Demirden"},
    {0xffffffffu, "Yasin Demirden"}, {0xffffffffu, "Sıla Dinç"}, {0xffffffffu, "Nejati Dinç"}, {0xffffffffu, "İsmail Döner"},
    {0xffffffffu, "Volkan Gümüşlü"}, {0xffffffffu, "Rıza Kuruüzümcü"}, {0xffffffffu, "Gülseven Medar"},
    {0xffffffffu, "Rupen Melkisetoğlu"}, {0x00000000u, "."}
}};

struct Hit { int x, y, w, h; std::string value; };

class Screen {
public:
    Screen(SDL_Window* window, Renderer& renderer, GameAudio& audio, EffectSystem* effects, Scene& scene,
           const std::filesystem::path& installation, const CharacterScreenArt& art, CharacterApi& api)
        : window_(window), renderer_(renderer), audio_(audio), effects_(effects), scene_(scene),
          installation_(installation), art_(art), api_(api) {
        for (const auto& node : scene_.nodes) {
            const auto& m = node.world.matrix;
            // Slot nodes rotate about Y; RenderEntity yaw uses the same Ry convention (actors face local -Z).
            const std::array<float, 4> pose{m[12], m[13], m[14], std::atan2(m[8], m[0])};
            for (unsigned i = 0; i < 4; ++i)
                if (node.name.starts_with("c_character" + std::to_string(i))) slots_[i] = pose;
            if (node.name == "c_create_adamyarat") preview_ = pose;
        }
        if (effects_) effects_->attach_scene(scene_);
    }
    std::optional<std::uint64_t> run(const std::function<bool()>& cancelled) {
        refresh();
        auto last = std::chrono::steady_clock::now();
        float elapsed = 0;
        bool announced = false;
        while (!chosen_ && !quit_ && !cancelled()) {
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.f, .1f);
            last = now; elapsed += dt;
            events();
            if (quit_ || chosen_) break;
            update_scene(scene_, elapsed);
            audio_.frontend(credits_);
            audio_.update();
            draw(dt);
            if (!announced) {
                std::cerr << "Character screen presented characters=" << rows_.size() << '\n';
                std::cout << "CHARACTER_SCREEN characters=" << rows_.size() << '\n' << std::flush;
                announced = true;
            }
            // Presentation paces frames; only cap a non-blocking present at 250 fps (see Application::run).
            if (const auto spent = std::chrono::steady_clock::now() - now; spent < std::chrono::milliseconds(4))
                SDL_DelayPrecise(static_cast<Uint64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(4) - spent).count()));
        }
        return chosen_;
    }

private:
    SDL_Window* window_;
    Renderer& renderer_;
    GameAudio& audio_;
    EffectSystem* effects_;
    Scene& scene_;
    std::filesystem::path installation_;
    const CharacterScreenArt& art_;
    CharacterApi& api_;
    std::unique_ptr<ClientUI> ui_;
    int width_ = 0, height_ = 0, font_px_ = 0;
    float k_ = 1;
    std::array<std::array<float, 4>, 4> slots_{};
    std::array<float, 4> preview_{};
    Json rows_ = Json::array();
    std::vector<std::uint32_t> attached_;
    int selected_ = -1;
    bool creating_ = false, editing_ = false, dirty_ = true, quit_ = false, credits_ = false; // credits_: roll visible
    std::chrono::steady_clock::time_point credits_since_{};
    std::vector<std::pair<Rgba, std::string>> credit_rows_; int credit_font_px_ = 0; // credit_lines wrapped at the column width
    std::optional<std::uint64_t> chosen_;
    // Creation choices: sex 0 female / 1 male (Erkek default), class 1..3, five cosmetic indices.
    int sex_ = 1, class_ = 1;
    std::array<int, 5> cosmetics_{};
    std::string name_ = "isimsiz";
    float preview_turn_ = 0;
    std::string message_, confirm_; // modal notice; confirm_ holds the action awaiting Evet
    std::vector<Hit> hits_;
    std::string hover_;
    float mouse_x_ = 0, mouse_y_ = 0;

    int U(float v) const { return static_cast<int>(std::lround(v * k_)); }
    void click() { audio_.play_event("ui\\click.raw"); }
    // 004b8230 box: a one-button notice plays ui\warning.raw, a yes/no confirm ui\alert.raw.
    void notice(std::string text) { message_ = std::move(text); confirm_.clear(); dirty_ = true; audio_.play_event("ui\\warning.raw"); }
    SourceAppearance appearance(int class_id, const Json& a, const Json& visual = Json::array()) const {
        SourceAppearance source;
        source.source_class = static_cast<unsigned>(2 * class_id + a.value("sex", 1));
        source.face = a.value("face", 0u); source.hair = a.value("hair", 0u); source.hair_color = a.value("hair_color", 0u);
        source.beard = a.value("beard", 0u); source.beard_color = a.value("beard_color", 0u);
        for (const auto& item : visual) source.equipment.push_back({item.at("slot").get<unsigned>(), item.at("source_uid").get<unsigned>()});
        return source;
    }
    Json creation_appearance() const {
        Json a{{"sex", sex_}};
        for (std::size_t i = 0; i < cosmetics_.size(); ++i) a[cosmetic_keys[i]] = cosmetics_[i];
        return a;
    }
    void detach_all() {
        for (const auto id : attached_) detach_source_actor(scene_, id);
        attached_.clear();
    }
    void attach(std::uint32_t id, int class_id, const Json& a, const Json& visual = Json::array()) {
        attach_source_actor(scene_, installation_, appearance(class_id, a, visual), id);
        set_actor_action(scene_, id, 2, 0); // original PC Idle clip
        attached_.push_back(id);
    }
    // cModuleCharsel: the account's first four characters stand on c_character0..3, dressed in their worn items.
    void refresh() {
        try { rows_ = api_.list(); }
        catch (const std::exception& e) { rows_ = Json::array(); notice(e.what()); }
        detach_all();
        for (std::size_t i = 0; i < rows_.size() && i < slots_.size(); ++i) {
            try { attach(slot_entity + static_cast<std::uint32_t>(i), rows_[i].at("class_id").get<int>(), rows_[i].at("appearance"), rows_[i].at("visual")); }
            catch (const std::exception& e) { notice(std::string("Karakter görünümü yüklenemedi: ") + e.what()); }
        }
        selected_ = rows_.empty() ? -1 : 0;
        dirty_ = true;
    }
    void rebuild_preview() {
        detach_all();
        try { attach(preview_entity, class_, creation_appearance()); }
        catch (const std::exception& e) { notice(std::string("Karakter görünümü yüklenemedi: ") + e.what()); }
        dirty_ = true;
    }
    void begin_create() {
        if (rows_.size() >= 4) { notice("En fazla 4 karakteriniz olabilir."); return; }
        creating_ = true; sex_ = 1; class_ = 1; cosmetics_ = {}; name_ = "isimsiz"; preview_turn_ = 0;
        rebuild_preview();
    }
    void end_create() {
        creating_ = false; editing_ = false;
        refresh();
    }
    void submit_create() {
        if (const auto error = original_character_name_error(name_); !error.empty()) { notice(error); return; }
        try {
            api_.create(name_, class_, creation_appearance());
            end_create();
            for (std::size_t i = 0; i < rows_.size(); ++i) if (rows_[i].at("name") == name_) selected_ = static_cast<int>(i);
        } catch (const std::exception& e) { notice(e.what()); }
    }
    void handle(const std::string& value) {
        dirty_ = true;
        if (!message_.empty()) {
            if (value == "ok" && !confirm_.empty()) {
                const auto action = confirm_; message_.clear(); confirm_.clear();
                if (action == "delete" && selected_ >= 0) {
                    const auto& row = rows_.at(static_cast<std::size_t>(selected_));
                    try { api_.remove(row.at("id").get<std::uint64_t>(), row.at("name").get<std::string>()); refresh(); }
                    catch (const std::exception& e) { notice(e.what()); }
                }
            } else if (value == "ok" || value == "cancel") { message_.clear(); confirm_.clear(); }
            return;
        }
        // 004afed0: a click while the roll is shown hides it; otherwise it (re)starts at y = screen height (004902c0).
        if (value == "credits") { credits_ = !credits_; credits_since_ = std::chrono::steady_clock::now(); return; }
        if (!creating_) {
            if (value == "quit") quit_ = true;
            else if (value == "new") begin_create();
            else if (value == "delete") {
                if (selected_ < 0) notice("Silmek için bir karakter seçmelisiniz.");
                else { message_ = "Karakterinizi silmek istediğinizden emin misiniz?"; confirm_ = "delete"; audio_.play_event("ui\\alert.raw"); }
            } else if (value == "start") {
                if (selected_ < 0) notice("Başlamak için bir karakter seçmelisiniz.");
                else chosen_ = rows_.at(static_cast<std::size_t>(selected_)).at("id").get<std::uint64_t>();
            }
            return;
        }
        if (value == "name") { editing_ = true; return; }
        editing_ = false;
        if (value == "create") submit_create();
        else if (value == "sex0" || value == "sex1") {
            const int sex = value.back() - '0';
            if (sex != sex_) { sex_ = sex; cosmetics_ = {}; rebuild_preview(); }
        } else if (value.starts_with("class")) {
            const int chosen = value.back() - '0';
            if (chosen != class_) { class_ = chosen; rebuild_preview(); }
        } else if (value.starts_with("cos")) {
            const auto index = static_cast<std::size_t>(value[3] - '0');
            const int range = cosmetic_max[index][static_cast<std::size_t>(sex_)] + 1;
            cosmetics_[index] = (cosmetics_[index] + (value[4] == '+' ? 1 : range - 1)) % range;
            rebuild_preview();
        }
    }
    std::pair<float, float> pixel(float x, float y) const {
        int ww = 0, wh = 0; SDL_GetWindowSize(window_, &ww, &wh);
        return {x * static_cast<float>(width_) / static_cast<float>(std::max(ww, 1)),
                y * static_cast<float>(height_) / static_cast<float>(std::max(wh, 1))};
    }
    const Hit* hit_at(float x, float y) const {
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
            if (x >= static_cast<float>(it->x) && x < static_cast<float>(it->x + it->w) &&
                y >= static_cast<float>(it->y) && y < static_cast<float>(it->y + it->h)) return &*it;
        return nullptr;
    }
    void events() {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) { quit_ = true; return; }
            if (event.type == SDL_EVENT_MOUSE_MOTION) {
                std::tie(mouse_x_, mouse_y_) = pixel(event.motion.x, event.motion.y);
                // "Karakteri farenin sağ tuşuna basılı tutarak çevirebilirsiniz" (0x5993f8).
                if (creating_ && (event.motion.state & SDL_BUTTON_RMASK)) preview_turn_ += event.motion.xrel * .01f;
                const auto* hit = hit_at(mouse_x_, mouse_y_);
                const std::string hover = hit ? hit->value : std::string{};
                if (hover != hover_) { hover_ = hover; dirty_ = true; }
                continue;
            }
            dirty_ = true;
            if (event.type == SDL_EVENT_TEXT_INPUT && editing_) {
                const std::string candidate = name_ + event.text.text;
                if (candidate.size() <= 64) name_ = candidate;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                const auto key = event.key.key;
                if (key == SDLK_ESCAPE) {
                    if (!message_.empty()) handle("cancel");
                    else if (credits_) credits_ = false;
                    else if (creating_) end_create();
                } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                    if (!message_.empty()) handle("ok");
                    else handle(creating_ ? "create" : "start");
                } else if (key == SDLK_BACKSPACE && editing_ && !name_.empty()) {
                    do { const auto c = static_cast<unsigned char>(name_.back()); name_.pop_back(); if ((c & 0xc0) != 0x80) break; } while (!name_.empty());
                }
            }
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
                const auto [x, y] = pixel(event.button.x, event.button.y);
                if (const auto* hit = hit_at(x, y)) { if (!hit->value.empty()) { click(); handle(std::string(hit->value)); } }
                else if (message_.empty() && !creating_) pick(x, y);
            }
        }
    }
    std::optional<std::array<int, 2>> project(float x, float y, float z) const {
        const auto m = camera_view_projection(renderer_.camera(), width_, height_);
        const float cx = m[0] * x + m[4] * y + m[8] * z + m[12], cy = m[1] * x + m[5] * y + m[9] * z + m[13],
                    cw = m[3] * x + m[7] * y + m[11] * z + m[15];
        if (cw < .1f) return {};
        return std::array<int, 2>{static_cast<int>(std::lround((cx / cw + 1) * .5f * static_cast<float>(width_))),
                                  static_cast<int>(std::lround((1 - cy / cw) * .5f * static_cast<float>(height_)))};
    }
    // 004afa80 selects the character whose model the mouse ray hits; the native client uses its projected body column.
    void pick(float x, float y) {
        for (std::size_t i = 0; i < rows_.size() && i < slots_.size(); ++i) {
            const auto& s = slots_[i];
            const auto head = project(s[0], s[1] + 7.5f, s[2]), feet = project(s[0], s[1], s[2]);
            if (!head || !feet) continue;
            const float half = static_cast<float>((*feet)[1] - (*head)[1]) * .18f;
            if (std::abs(x - static_cast<float>((*feet)[0])) <= half && y >= static_cast<float>((*head)[1]) && y <= static_cast<float>((*feet)[1])) {
                selected_ = static_cast<int>(i); click(); return;
            }
        }
    }
    // Camera framing measured from VOD 00:05 (charsel) and 00:30 (chargen) at 1280x720, fov 50.
    void set_camera() {
        Camera camera = renderer_.camera();
        camera.fov = 50;
        if (creating_) {
            // VOD 00:30 keeps the Galata tower behind the actor's left and turns the preview toward the camera.
            const float yaw = chargen_yaw, right_x = std::cos(yaw), right_z = -std::sin(yaw);
            camera.yaw = yaw; camera.pitch = .04f; camera.distance = 8.5f;
            camera.target = {preview_[0] + 1.25f * right_x, preview_[1] + 4.3f, preview_[2] + 1.25f * right_z};
        } else {
            const auto& a = slots_[0];
            const auto& b = slots_[3];
            camera.target = {(a[0] + b[0]) * .5f + 1.4f, (a[1] + b[1]) * .5f + 6.5f, (a[2] + b[2]) * .5f + .75f};
            camera.yaw = 2.655f; camera.pitch = -.03f; camera.distance = 20.5f;
        }
        renderer_.set_camera(camera);
    }
    void draw(float dt) {
        const int old_width = width_, old_height = height_;
        if (!SDL_GetWindowSizeInPixels(window_, &width_, &height_) || width_ <= 0 || height_ <= 0) return;
        if (width_ != old_width || height_ != old_height) { renderer_.resize(width_, height_); dirty_ = true; }
        // The original UI is fixed-pixel at 1080p; native scales it 1.2x (minimum) for readable Verdana.
        k_ = 1.2f * std::max(1.f, static_cast<float>(height_) / 1080.f);
        if (const int px = U(10); !ui_ || px != font_px_) { ui_ = std::make_unique<ClientUI>(art_.font, px); font_px_ = px; dirty_ = true; }
        set_camera();
        std::vector<RenderEntity> poses;
        if (creating_) poses.push_back({preview_entity, preview_[0], preview_[1], preview_[2], chargen_yaw - 3.14159265f + preview_turn_});
        else for (const auto id : attached_) {
            const auto& s = slots_[id - slot_entity];
            poses.push_back({id, s[0], s[1], s[2], s[3]});
        }
        if (dirty_ || editing_ || credits_) paint();
        if (effects_) {
            // Yellow ground ring under the selected character (VOD 02:11): 004aede0 attaches GroundHilite tinted (1,1,0,1).
            std::vector<RenderEntity> ring;
            const auto selected = std::find_if(poses.begin(), poses.end(), [&](const auto& p) { return p.id == slot_entity + static_cast<std::uint32_t>(selected_); });
            if (!creating_ && selected_ >= 0 && selected != poses.end()) {
                ring.push_back(*selected);
                effects_->attach("GroundHilite", ring.front().id, {1, 1, 0, 1});
            }
            effects_->update(dt, ring);
            renderer_.set_particles(effects_->frame(renderer_.camera()));
        }
        renderer_.render(scene_, poses, 0, dt);
    }
    // ---- 2D: original UI1 slate tile (24x24 at 28,588) nine-sliced, Verdana text with a 1px shadow. ----
    void atlas(float x, float y, float w, float h, float u, float v, float sw, float sh) {
        const float l = u / 1024.5f, r = (u + sw) / 1024.5f, t = v / 1023.5f, b = (v + sh) / 1023.5f;
        ui_->image(art_.ui1, {x, y, w, h}, {{{l, t}, {r, t}, {l, b}, {r, b}}});
    }
    void tile(int x, int y, int w, int h) {
        const float b = 6, d = static_cast<float>(U(b) > 0 ? U(b) : 1), u = 28, v = 588, s = 24;
        const float fx = static_cast<float>(x), fy = static_cast<float>(y), fw = static_cast<float>(w), fh = static_cast<float>(h);
        const std::array<float, 4> xs{fx, fx + d, fx + fw - d, fx + fw}, ys{fy, fy + d, fy + fh - d, fy + fh};
        const std::array<float, 4> us{u, u + b, u + s - b, u + s}, vs{v, v + b, v + s - b, v + s};
        for (std::size_t i = 0; i < 3; ++i) for (std::size_t j = 0; j < 3; ++j)
            atlas(xs[i], ys[j], xs[i + 1] - xs[i], ys[j + 1] - ys[j], us[i], vs[j], us[i + 1] - us[i], vs[j + 1] - vs[j]);
    }
    void label(int x, int y, std::string_view text, Rgba color = white) {
        ui_->text(x + 1, y + 1, text, shadow); ui_->text(x, y, text, color);
    }
    void centered(int cx, int cy, std::string_view text, Rgba color = white) {
        label(cx - ui_->text_width(text) / 2, cy - ui_->line_height() / 2, text, color);
    }
    // Charsel text buttons: slate box with white capitals (VOD 02:11), tooltip below on hover.
    void text_button(int x, int y, const char* text, const char* value, std::initializer_list<const char*> tooltip) {
        const int w = U(135), h = U(22);
        tile(x, y, w, h);
        centered(x + w / 2, y + h / 2, text, hover_ == value ? yellow : white);
        hits_.push_back({x, y, w, h, value});
        if (hover_ != value) return;
        int tw = 0;
        for (const auto* line : tooltip) tw = std::max(tw, ui_->text_width(line));
        const int lh = ui_->line_height(), bx = std::min(x, width_ - tw - U(14)), by = y + h + U(6);
        tile(bx, by, tw + U(12), lh * static_cast<int>(tooltip.size()) + U(8));
        int ly = by + U(4);
        for (const auto* line : tooltip) { label(bx + U(6), ly, line); ly += lh; }
    }
    void icon(int x, int y, float u, float v, const std::string& value) {
        atlas(static_cast<float>(x), static_cast<float>(y), static_cast<float>(U(47)), static_cast<float>(U(43)), u, v, 47, 43);
        hits_.push_back({x, y, U(47), U(43), value});
    }
    void arrows(int x, int yc, std::size_t index) {
        const int s = U(12), y = yc - U(6.5f);
        atlas(static_cast<float>(x), static_cast<float>(y), static_cast<float>(s), static_cast<float>(U(13)), 99, 754, 12, 13);
        atlas(static_cast<float>(x + U(14)), static_cast<float>(y), static_cast<float>(s), static_cast<float>(U(13)), 113, 754, 12, 13);
        hits_.push_back({x, y, s, U(13), "cos" + std::to_string(index) + "-"});
        hits_.push_back({x + U(14), y, s, U(13), "cos" + std::to_string(index) + "+"});
    }
    void check(int x, int y, const char* value, bool cancel = false) {
        atlas(static_cast<float>(x), static_cast<float>(y), static_cast<float>(U(14)), static_cast<float>(U(14)), cancel ? 81 : 65, hover_ == value ? 705 : 689, 14, 14);
        hits_.push_back({x, y, U(14), U(14), value});
    }
    // 'Karakter Yaratma' panel, positions measured from VOD 00:30 (1080p units, panel 184x276 at x 1518, y 213).
    void paint_creation() {
        // Anchors follow the VOD composition as screen fractions; sizes use k_.
        const int pw = U(184), ph = U(276), px = width_ * 1702 / 1920 - pw, py = height_ * 213 / 1080;
        const std::string title = "Karakter Yaratma";
        tile(px, py - ui_->line_height() - U(2), ui_->text_width(title) + U(10), ui_->line_height() + U(4));
        label(px + U(5), py - ui_->line_height(), title);
        tile(px, py, pw, ph);
        hits_.push_back({px, py, pw, ph, {}});
        const int cx = px + pw / 2;
        auto at = [&](float ux, float uy) { return std::array<int, 2>{px + U(ux), py + U(uy)}; };
        centered(cx, py + U(12), "Karakterin cinsiyeti");
        const auto male = at(36, 22), female = at(100, 22);
        icon(male[0], male[1], sex_ == 1 ? 0 : 48, 724, "sex1");
        icon(female[0], female[1], sex_ == 0 ? 0 : 48, 772, "sex0");
        centered(male[0] + U(23.5f), py + U(76), "Erkek", sex_ == 1 ? yellow : white);
        centered(female[0] + U(23.5f), py + U(76), "Kadın", sex_ == 0 ? yellow : white);
        centered(cx, py + U(91), "Karakterin sınıfı");
        // Class art: Savaşçı 96/144,772; Büyücü 0/48,820; Şifacı 96/144,820 (coloured = selected).
        static constexpr std::array<std::array<float, 3>, 3> classes{{{96, 144, 772}, {0, 48, 820}, {96, 144, 820}}};
        for (int c = 1; c <= 3; ++c) {
            const auto& art = classes[static_cast<std::size_t>(c - 1)];
            const auto pos = at(4 + 64 * static_cast<float>(c - 1), 101);
            icon(pos[0], pos[1], class_ == c ? art[0] : art[1], art[2], "class" + std::to_string(c));
            centered(pos[0] + U(23.5f), py + U(155), class_names[static_cast<std::size_t>(c - 1)], class_ == c ? yellow : white);
        }
        centered(cx, py + U(171), "Karakterin fiziksel özellikleri");
        const bool female_jewel = sex_ == 0;
        const std::array<std::pair<const char*, float>, 5> rows{{{"Yüz şekli", 187}, {"Saç şekli", 203}, {"Saç rengi", 219},
            {female_jewel ? "Takı şekli" : "Sakal şekli", 203}, {female_jewel ? "Takı rengi" : "Sakal rengi", 219}}};
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const float left = i < 3 ? 7 : 88;
            const int yc = py + U(rows[i].second);
            arrows(px + U(left), yc, i);
            label(px + U(left + 31), yc - ui_->line_height() / 2, rows[i].first);
        }
        centered(cx, py + U(235), "Karakterin ismi");
        const auto field = at(19, 243);
        atlas(static_cast<float>(field[0]), static_cast<float>(field[1]), static_cast<float>(U(146)), static_cast<float>(U(16)), 130, 723, 108, 10);
        const bool caret = editing_ && SDL_GetTicks() / 500 % 2 == 0;
        std::string shown = name_;
        while (!shown.empty() && ui_->text_width(shown + "|") > U(138)) {
            do shown.erase(0, 1); while (!shown.empty() && (static_cast<unsigned char>(shown.front()) & 0xc0) == 0x80);
        }
        label(field[0] + U(4), field[1] + U(8) - ui_->line_height() / 2, shown + (caret ? "|" : ""));
        hits_.push_back({field[0], field[1], U(146), U(16), "name"});
        const auto ok = at(149, 270);
        check(ok[0], ok[1], "create");
        // VOD 00:30: boxed hint bottom-left (0x5993f8).
        const std::string hint = "Karakteri farenin sağ tuşuna basılı tutarak çevirebilirsiniz";
        const int hx = width_ * 128 / 1920, hy = height_ * 1048 / 1080;
        tile(hx, hy, ui_->text_width(hint) + U(10), ui_->line_height() + U(4));
        label(hx + U(5), hy + U(2), hint);
    }
    // Charsel (VOD 02:11): YENİ KARAKTER / KARAKTERİ SİL centred top, BAŞLAT top right, HAZIRLAYANLAR bottom right.
    void paint_selection() {
        const int top = height_ * 39 / 1080;
        text_button(width_ * 634 / 1920, top, "YENİ KARAKTER", "new", {"Yeni bir karakter oluşturmak", "için buraya tıklayın"});
        text_button(width_ * 894 / 1920, top, "KARAKTERİ SİL", "delete", {"Oluşturulmuş karakterinizi", "silmek için buraya tıklayın"});
        text_button(width_ * 1829 / 1920 - U(135), top, "BAŞLAT", "start", {"Seçtiğiniz karakterinizle oyuna", "başlamak için buraya tıklayın"});
        if (selected_ < 0 || static_cast<std::size_t>(selected_) >= rows_.size()) return;
        const auto& row = rows_[static_cast<std::size_t>(selected_)];
        const auto& s = slots_[static_cast<std::size_t>(selected_)];
        const auto head = project(s[0], s[1] + 7.5f, s[2]);
        if (!head) return;
        // Info box above-left of the selected character (VOD: İsim / Seviye / Sınıf / Yer).
        const auto zone = row.at("zone").get<std::string>();
        std::string place = zone.empty() ? "—" : zone; // a never-played character has no saved zone yet
        for (unsigned id = 0; id < 71; ++id) if (source_scene_descriptor(id).key == zone) { place = source_scene_descriptor(id).label; break; }
        const std::array<std::string, 4> lines{"İsim: " + row.at("name").get<std::string>(), "Seviye: " + std::to_string(row.at("level").get<int>()),
            "Sınıf: " + std::string(class_names[static_cast<std::size_t>(row.at("class_id").get<int>() - 1)]), "Yer: " + place};
        int w = U(120);
        for (const auto& line : lines) w = std::max(w, ui_->text_width(line) + U(10));
        const int lh = ui_->line_height(), h = 4 * lh + U(8);
        const int x = std::clamp((*head)[0] - U(48), 0, std::max(0, width_ - w)), y = std::max(U(70), (*head)[1] - U(260));
        tile(x, y, w, h);
        for (std::size_t i = 0; i < lines.size(); ++i) label(x + U(5), y + U(4) + static_cast<int>(i) * lh, lines[i]);
    }
    // 004afed0 roll: cTextBox at x 20, y = screen height, 260 × (height + 5224) (00490260), no background (+0xe8 0), word-
    // wrapped (+0x18c 1) lines centred (DrawText format +0xe4 1) inside 4 px side / 2 px top padding (+0x20c/+0x210), pitch
    // line height + 2 (+0x214), each over a 0x7f000000 shadow 1 px down-right (00489110).
    // ponytail: its parent frame 0x77d604 has no writer in the image (the shipped handler dereferences it null), so the
    // scroll that would carry the box up is unrecovered: it rises 40 px/s here and restarts once the last line has passed.
    void paint_credits() {
        const int column = U(252), lh = ui_->line_height(), pitch = lh + U(2);
        if (credit_font_px_ != font_px_) {
            credit_rows_.clear(); credit_font_px_ = font_px_;
            for (const auto& line : credit_lines) {
                const Rgba color{static_cast<std::uint8_t>(line.argb >> 16), static_cast<std::uint8_t>(line.argb >> 8),
                                 static_cast<std::uint8_t>(line.argb), static_cast<std::uint8_t>(line.argb >> 24)};
                std::string row;
                for (std::string_view rest = line.text; !rest.empty();) {
                    const auto space = rest.find(' ');
                    const auto word = rest.substr(0, space);
                    rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
                    const auto candidate = row.empty() ? std::string(word) : row + ' ' + std::string(word);
                    if (!row.empty() && ui_->text_width(candidate) > column) { credit_rows_.push_back({color, std::move(row)}); row = word; }
                    else row = candidate;
                }
                credit_rows_.push_back({color, std::move(row)});
            }
        }
        const float rise = static_cast<float>(U(40)) * std::chrono::duration<float>(std::chrono::steady_clock::now() - credits_since_).count();
        const float travel = static_cast<float>(height_ + U(2) + static_cast<int>(credit_rows_.size()) * pitch);
        int y = height_ + U(2) - static_cast<int>(std::fmod(rise, travel));
        const int cx = U(20) + U(4) + column / 2;
        for (const auto& [color, text] : credit_rows_) {
            if (color[3] && y > -lh && y < height_) {
                const int x = cx - ui_->text_width(text) / 2;
                ui_->text(x + 1, y + 1, text, {0, 0, 0, 127});
                ui_->text(x, y, text, color);
            }
            y += pitch;
        }
    }
    void paint_message() {
        const int w = std::max(U(220), ui_->text_width(message_) + U(24)), h = ui_->line_height() + U(40);
        const int x = (width_ - w) / 2, y = (height_ - h) / 2;
        hits_.push_back({0, 0, width_, height_, {}}); // modal
        tile(x, y, w, h);
        centered(width_ / 2, y + U(14), message_);
        if (confirm_.empty()) check(x + w / 2 - U(7), y + h - U(20), "ok");
        else { check(x + w / 2 - U(20), y + h - U(20), "ok"); check(x + w / 2 + U(6), y + h - U(20), "cancel", true); }
    }
    void paint() {
        ui_->begin(width_, height_); hits_.clear();
        static constexpr std::array<std::array<float, 2>, 4> whole{{{0, 0}, {1, 0}, {0, 1}, {1, 1}}};
        const float s = static_cast<float>(height_) / 1080.f;
        ui_->image(art_.logo, {-34 * s, 14 * s, 256 * s, 64 * s}, whole); // same placement as the loading screen
        if (creating_) paint_creation();
        else {
            paint_selection();
            text_button(width_ * 1860 / 1920 - 2 * U(135) - U(6), height_ * 1068 / 1080 - U(22), "ÇIKIŞ", "quit", {});
            text_button(width_ * 1860 / 1920 - U(135), height_ * 1068 / 1080 - U(22), "HAZIRLAYANLAR", "credits", {});
            label(width_ * 48 / 1920, height_ * 1059 / 1080 - ui_->line_height() / 2, "Sürüm : 6.0.0");
        }
        if (credits_) paint_credits();
        if (!message_.empty()) paint_message();
        renderer_.set_overlay(ui_->overlay());
        dirty_ = false;
    }
};
} // namespace

std::optional<std::uint64_t> run_character_screen(SDL_Window* window, Renderer& renderer, GameAudio& audio,
                                                  EffectSystem* effects, Scene& scene,
                                                  const std::filesystem::path& installation,
                                                  const CharacterScreenArt& art, CharacterApi& api,
                                                  const std::function<bool()>& cancelled) {
    Screen screen(window, renderer, audio, effects, scene, installation, art, api);
    return screen.run(cancelled);
}
} // namespace ikv
