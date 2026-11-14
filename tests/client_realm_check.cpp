// NONCANONICAL wave 30 client contract checks: transfer event parsing, realm-suffixed names, Ad-Realm addressing,
// the world-endpoint and --realm parsers. The reconnect state machine itself runs in tests/client_transfer.py.
#include "ikv/client_realms.hpp"
#include "ikv/client_session.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <limits>
#include <vector>
#include <stdexcept>
#include <string>

namespace {
using Json = nlohmann::json;
void require(bool condition, const char* what) { if (!condition) throw std::runtime_error(what); }
template<class F> void rejects(F&& action, const char* what) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, what);
}
const std::string ticket = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
Json transfer_event() {
    return {{"type", "transfer"}, {"host", "shard-a2.example"}, {"port", 7001}, {"ticket", ticket}, {"reason", "party"}, {"realm", "Galata"}};
}

void transfer_events() {
    const auto t = ikv::parse_transfer(transfer_event());
    require(t.host == "shard-a2.example" && t.port == 7001 && t.ticket == ticket && t.reason == "party" && t.realm == "Galata",
            "valid transfer event misparsed");
    for (const auto* reason : {"party", "home", "layer", "instance"}) {
        auto e = transfer_event(); e["reason"] = reason;
        require(ikv::parse_transfer(e).reason == reason, "transfer reason rejected");
    }
    auto unnamed = transfer_event(); unnamed.erase("realm");
    require(ikv::parse_transfer(unnamed).realm.empty(), "transfer without realm rejected");
    auto ipv6 = transfer_event(); ipv6["host"] = "::1"; ipv6["port"] = 65535;
    require(ikv::parse_transfer(ipv6).port == 65535, "IPv6 host or maximum port rejected");
    auto same_host = transfer_event(); same_host["host"] = ""; // shard without --public-host: keep the current host
    require(ikv::parse_transfer(same_host).host.empty(), "empty transfer host rejected");
    const std::vector<std::pair<const char*, Json>> bad{
        {"type", "door"}, {"host", "a b"}, {"host", std::string(254, 'h')}, {"host", "h\x7f"}, {"host", 5},
        {"port", 0}, {"port", 65536}, {"port", -1}, {"port", "7001"}, {"port", 7001.5},
        {"ticket", ticket.substr(1)}, {"ticket", ticket + "0"}, {"ticket", std::string(64, 'A')}, {"ticket", std::string(64, 'g')},
        {"ticket", nullptr}, {"reason", "trade"}, {"reason", ""}, {"realm", std::string(65, 'r')}, {"realm", "a\nb"}, {"realm", 1}};
    for (const auto& [key, value] : bad) {
        auto e = transfer_event(); e[key] = value;
        rejects([&] { ikv::parse_transfer(e); }, "malformed transfer event accepted");
    }
    for (const auto* key : {"host", "port", "ticket", "reason"}) {
        auto e = transfer_event(); e.erase(key);
        rejects([&] { ikv::parse_transfer(e); }, "transfer event missing a field accepted");
    }
    rejects([] { ikv::parse_transfer(Json::array()); }, "non-object transfer accepted");
}

void realm_names() {
    require(ikv::realm_label({{"name", "Ahmet"}}) == "Ahmet", "plain name changed");
    require(ikv::realm_label({{"name", "Ahmet"}, {"realm", "Galata"}}) == "Ahmet (Galata)", "realm suffix missing");
    require(ikv::realm_label({{"name", "Ahmet"}, {"realm", ""}}) == "Ahmet", "empty realm suffixed");
    require(ikv::realm_label({{"peer_name", "Ayşe"}, {"peer_realm", "Üsküdar"}}, "peer_name", "peer_realm") == "Ayşe (Üsküdar)",
            "trade peer realm missing");
    auto who = ikv::split_realm_name("Ahmet");
    require(who && who->name == "Ahmet" && who->realm.empty(), "plain whisper target misparsed");
    who = ikv::split_realm_name(" Ahmet-Galata Kulesi ");
    require(who && who->name == "Ahmet" && who->realm == "Galata Kulesi", "Ad-Realm misparsed");
    who = ikv::split_realm_name("Şükrü-Eski-Sunucu");
    require(who && who->name == "Şükrü" && who->realm == "Eski-Sunucu", "first dash must separate");
    for (const auto* bad : {"", "  ", "-Galata", "Ahmet-", "Ali Veli", "Ahmet-\tx\n"})
        require(!ikv::split_realm_name(bad), "malformed whisper target accepted");
    require(!ikv::split_realm_name(std::string(65, 'a')) && !ikv::split_realm_name("Ahmet-" + std::string(65, 'r')), "oversized target accepted");
    Json action{{"action", "group_invite"}};
    ikv::address_by_name(action, {"Ahmet", "Galata"});
    require(action == Json{{"action", "group_invite"}, {"name", "Ahmet"}, {"realm", "Galata"}}, "group invite by name/realm wrong");
    Json local{{"action", "chat"}};
    ikv::address_by_name(local, {"Ahmet", ""});
    require(local == Json{{"action", "chat"}, {"name", "Ahmet"}}, "same-realm name address carries a realm");
}

void world_endpoints() {
    const auto w = ikv::parse_world_endpoint({{"host", "127.0.0.1"}, {"port", 7000}, {"instance", "shard-a1"}});
    require(w.host == "127.0.0.1" && w.port == 7000 && w.instance == "shard-a1", "world endpoint misparsed");
    // A shard without --public-host reports an empty host: the client uses the account service's host.
    require(ikv::parse_world_endpoint({{"host", ""}, {"port", 7001}, {"instance", "a2"}}).host.empty(), "empty world host rejected");
    const Json good{{"host", "h"}, {"port", 1}, {"instance", "i"}};
    const std::vector<std::pair<const char*, Json>> bad{
        {"host", "a b"}, {"host", std::string(254, 'h')}, {"host", 1}, {"host", nullptr}, {"port", 0}, {"port", 65536},
        {"port", "7000"}, {"port", std::numeric_limits<std::uint64_t>::max()}, {"port", -5}, {"instance", ""},
        {"instance", std::string(129, 'i')}, {"instance", "a\x01"}, {"instance", nullptr}};
    for (const auto& [key, value] : bad) {
        auto e = good; e[key] = value;
        rejects([&] { ikv::parse_world_endpoint(e); }, "malformed world endpoint accepted");
    }
    for (const auto* key : {"host", "port", "instance"}) {
        auto e = good; e.erase(key);
        rejects([&] { ikv::parse_world_endpoint(e); }, "world endpoint missing a field accepted");
    }
    rejects([] { ikv::parse_world_endpoint(Json::array()); }, "non-object world endpoint accepted");

    require(ikv::parse_realm_id("1") == 1 && ikv::parse_realm_id("32767") == 32767, "valid --realm rejected");
    for (const auto* bad : {"", "0", "01", "-1", "+1", "32768", "99999", "1a", "123456", " 1"})
        rejects([&] { ikv::parse_realm_id(bad); }, "malformed --realm accepted");
}
} // namespace

int main() {
    try {
        transfer_events();
        realm_names();
        world_endpoints();
        std::cout << "client realm checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "client realm check failed: " << e.what() << '\n';
        return 1;
    }
}
