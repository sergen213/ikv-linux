// NONCANONICAL wave 30 realm/shard Store boundaries against a real PostgreSQL database that tests/realm_store_test.py
// provisioned and migrated (001-009, populated before 009). Usage: realm-store-test DSN_FILE
#include "ikv/store.hpp"
#include <libpq-fe.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#ifndef _WIN32
#include <poll.h>
#endif

namespace {
int failures = 0;
void check(bool condition, const std::string& what) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << what << "\n"; }
}
void refused(const std::function<void()>& action, int status, const std::string& code, const std::string& what) {
    try { action(); check(false, what + " (no error)"); }
    catch (const ikv::ApiError& e) { check(e.status == status && e.code == code, what + " (got " + std::to_string(e.status) + " " + e.code + ")"); }
}
std::string query(PGconn* c, const std::string& sql) {
    PGresult* r = PQexec(c, sql.c_str());
    const auto status = PQresultStatus(r);
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) { std::cerr << PQresultErrorMessage(r); PQclear(r); std::exit(2); }
    std::string out = status == PGRES_TUPLES_OK && PQntuples(r) ? PQgetvalue(r, 0, 0) : "";
    PQclear(r);
    return out;
}
std::int64_t now_unix() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
}

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: realm-store-test DSN_FILE\n"; return 2; }
    std::ifstream file(argv[1]); std::stringstream buffer; buffer << file.rdbuf();
    auto dsn = buffer.str(); while (!dsn.empty() && (dsn.back() == '\n' || dsn.back() == ' ')) dsn.pop_back();
    PGconn* raw = PQconnectdb(dsn.c_str());
    if (PQstatus(raw) != CONNECTION_OK) { std::cerr << "cannot connect\n"; return 2; }
    PQexec(raw, "SET client_encoding='UTF8'");

    ikv::Store a(dsn), b(dsn), c(dsn); // three shard processes
    const std::string A1 = "test-a1", A2 = "test-a2", A3 = "test-a3-full", B1 = "test-b1", C1 = "test-c1";
    query(raw, "INSERT INTO realms(id,name,sort) VALUES (7,'Test Diyarı',7) ON CONFLICT DO NOTHING");

    // Registry and least-loaded choice: dead and full shards are never picked.
    query(raw, "INSERT INTO world_instances(instance,status,realm_id,public_host,port,capacity,players,updated_at) VALUES ('test-dead','{}',1,'dead.example',7100,32,0,now()-interval '20 seconds')");
    a.world_status({{"instance", A1}});
    a.register_shard({A1, 1, "a1.example", 7001, 5, 32});
    c.world_status({{"instance", A2}});
    c.register_shard({A2, 1, "a2.example", 7002, 2, 32});
    query(raw, "INSERT INTO world_instances(instance,status,realm_id,public_host,port,capacity,players) VALUES ('" + A3 + "','{}',1,'a3.example',7003,32,32)");
    b.world_status({{"instance", B1}});
    b.register_shard({B1, 7, "b1.example", 7101, 0, 32});
    auto picked = a.pick_shard(1);
    check(picked && picked->instance == A2 && picked->host == "a2.example" && picked->port == 7002 && picked->players == 2, "pick_shard chooses the least-loaded live shard with room");
    c.register_shard({A2, 1, "a2.example", 7002, 32, 32}); // A2 now full: A1 is the only one with room
    picked = a.pick_shard(1);
    check(picked && picked->instance == A1, "pick_shard skips full shards");
    c.register_shard({A2, 1, "a2.example", 7002, 2, 32});
    check(!a.pick_shard(99), "pick_shard of a realm without shards is empty");
    check(!a.shard("test-dead") && a.shard(B1) && a.shard(B1)->realm_id == 7, "shard() returns live shards only");
    refused([&] { a.register_shard({A1, 1, "a1.example", 7001, 5, 33}); }, 400, "invalid_capacity", "capacity above 32 is refused");
    refused([&] { a.register_shard({A1, 999, "a1.example", 7001, 5, 32}); }, 400, "invalid_realm", "unknown realm is refused");

    // Character entry on A1.
    const auto character = std::stoll(query(raw, "SELECT id FROM characters WHERE name='Ayşe' AND realm_id=1"));
    const auto account = std::stoll(query(raw, "SELECT account_id FROM characters WHERE id=" + std::to_string(character)));
    const auto other = std::stoll(query(raw, "SELECT id FROM characters WHERE name='Mehmet' AND realm_id=1"));
    const ikv::Identity who{account, false};
    check(a.character_realm(character) == 1, "character_realm reports the home realm");
    a.enter_character(who, character, A1);
    auto located = b.locate(1, "ayşe");
    check(located && located->instance == A1, "locate finds the online character's shard");
    check(!b.locate(7, "Ayşe"), "locate is realm-scoped");
    refused([&] { c.enter_character(who, character, C1); }, 409, "character_unavailable", "a live lease without a ticket blocks entry");

    // Tickets: single use, bound to character and destination, expiring.
    refused([&] { b.issue_ticket(character, B1, A1, "party", {}); }, 409, "lease_lost", "only the lease holder issues tickets");
    refused([&] { a.issue_ticket(character, A1, "test-dead", "party", {}); }, 503, "shard_offline", "a dead destination is refused");
    refused([&] { a.issue_ticket(character, A1, A3, "party", {}); }, 503, "shard_full", "a full destination is refused");
    refused([&] { a.issue_ticket(character, A1, B1, "bogus", {}); }, 400, "invalid_reason", "unknown reason is refused");
    auto ticket = a.issue_ticket(character, A1, B1, "party", {{"zone", "Eminönü"}, {"x", 10}});
    check(ticket.size() == 64, "raw ticket is 64 hex characters");
    check(query(raw, "SELECT count(*) FROM transfer_tickets WHERE ticket_hash=decode('" + ticket + "','hex')") == "0", "raw ticket value is not stored");
    check(query(raw, "SELECT count(*) FROM transfer_tickets WHERE ticket_hash=sha256(convert_to('" + ticket + "','UTF8'))") == "1", "only the SHA-256 of the ticket is stored");
    refused([&] { b.redeem_ticket(ticket, other, B1); }, 403, "invalid_ticket", "wrong character is refused");
    refused([&] { b.redeem_ticket(ticket, character, C1); }, 403, "invalid_ticket", "wrong destination is refused");
    refused([&] { b.redeem_ticket(std::string(64, 'g'), character, B1); }, 403, "invalid_ticket", "malformed ticket is refused");
    auto payload = b.redeem_ticket(ticket, character, B1);
    check(payload == ikv::Json({{"zone", "Eminönü"}, {"x", 10}}), "redeem returns the payload");
    refused([&] { b.redeem_ticket(ticket, character, B1); }, 403, "invalid_ticket", "a ticket is single-use");
    auto stale = a.issue_ticket(character, A1, A2, "layer", {});
    query(raw, "UPDATE transfer_tickets SET expires_at=now()-interval '1 second' WHERE ticket_hash=sha256(convert_to('" + stale + "','UTF8'))");
    refused([&] { c.redeem_ticket(stale, character, A2); }, 403, "invalid_ticket", "an expired ticket is refused");
    refused([&] { c.enter_character(who, character, A2); }, 409, "character_unavailable", "an expired ticket gives no lease handoff");

    // Lease handoff via a live ticket; the source neither fails its heartbeat nor overwrites the newer state.
    auto handoff = a.issue_ticket(character, A1, B1, "party", {});
    check(b.redeem_ticket(handoff, character, B1).is_object(), "handoff ticket redeemed");
    auto entered = b.enter_character(who, character, B1);
    check(entered.at("id") == character && entered.at("realm_id") == 1, "destination entered the character via the ticket");
    check(query(raw, "SELECT lease_instance FROM characters WHERE id=" + std::to_string(character)) == B1, "lease now belongs to the destination");
    try { a.world_status({{"instance", A1}}); check(true, ""); } catch (const std::exception& e) { check(false, std::string("source heartbeat survives the handoff: ") + e.what()); }
    try { a.save_character(account, character, {{"stale", true}}); } catch (const std::exception& e) { check(false, std::string("late source save is dropped, not failed: ") + e.what()); }
    check(query(raw, "SELECT state ? 'stale' FROM characters WHERE id=" + std::to_string(character)) == "f", "late source save did not overwrite state");
    b.save_character(account, character, {{"phased", true}});
    check(query(raw, "SELECT state ? 'phased' FROM characters WHERE id=" + std::to_string(character)) == "t", "destination saves the character");

    // Lockouts round trip.
    const auto expires = now_unix() + 3600;
    b.set_lockout(character, "Yerebatan", expires);
    b.set_lockout(character, "Eski", now_unix() - 5);
    auto lockouts = a.lockouts(character);
    check(lockouts.size() == 1 && lockouts.value("Yerebatan", 0LL) == expires, "lockout round trip: " + lockouts.dump());
    b.set_lockout(character, "Yerebatan", 0);
    check(a.lockouts(character).empty(), "past expiry clears the lockout");
    refused([&] { b.set_lockout(character, "", expires); }, 400, "invalid_zone", "empty lockout zone is refused");

    // Bus: NOTIFY wakes the LISTEN socket; rows are taken oldest first, 64 per call.
    const int fd = b.bus_fd();
    check(fd >= 0, "bus_fd returns a socket");
    check(b.bus_take(B1).empty(), "bus starts empty");
    for (int i = 0; i < 70; ++i) a.bus_send(B1, {{"kind", "whisper"}, {"seq", i}});
    a.bus_send(A1, {{"kind", "kick_party"}, {"to_char", 1}});
#ifndef _WIN32
    pollfd waiting{fd, POLLIN, 0};
    check(poll(&waiting, 1, 3000) == 1 && (waiting.revents & POLLIN), "NOTIFY makes the bus socket readable");
#endif
    auto first = b.bus_take(B1);
    check(first.size() == 64, "bus_take is bounded to 64");
    bool ordered = first.size() == 64;
    for (std::size_t i = 0; ordered && i < first.size(); ++i) ordered = first[i].at("seq") == static_cast<int>(i);
    check(ordered, "bus_take returns oldest first");
    auto second = b.bus_take(B1);
    check(second.size() == 6 && second.front().at("seq") == 64 && second.back().at("seq") == 69, "second take returns the rest in order");
    check(b.bus_take(B1).empty(), "messages are consumed once");
    auto own = a.bus_take(A1);
    check(own.size() == 1 && own[0].at("kind") == "kick_party", "messages reach only their destination");
    refused([&] { a.bus_send(B1, {{"text", std::string(5000, 'x')}}); }, 400, "invalid_message", "bodies above 4 KiB are refused");
    refused([&] { a.bus_send(B1, ikv::Json::array()); }, 400, "invalid_message", "non-object bodies are refused");

    b.leave_character(account, character);
    PQfinish(raw);
    if (failures) { std::cerr << failures << " realm store check(s) failed\n"; return 1; }
    std::cout << "realm store checks passed\n";
    return 0;
}
