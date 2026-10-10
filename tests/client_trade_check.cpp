#include "ikv/client_trade.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using Json = nlohmann::json;
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
Json offer(std::uint64_t revision = 2) {
    return {{"type","trade"},{"peer",7},{"peer_name","Tüccar"},{"initiator",7},{"phase","open"},{"revision",revision},
            {"own",{{"items",Json::array()},{"currency",12},{"accepted",false}}},
            {"other",{{"items",Json::array({{{"uid","peer-uid"},{"item","tonic"},{"name","Şifa Toniği"},{"quantity",3}}})},
                       {"currency",47},{"accepted",false}}}};
}
}

int main() {
    ikv::ClientTrade trade;
    check(!trade.active() && !trade.accept_action(), "empty model cannot accept");
    const auto first = offer();
    check(trade.receive(first, 9) && trade.active() && trade.peer() == 7 && trade.revision() == 2,
          "authoritative offer becomes active");
    check(trade.offer() == first && trade.offer().at("other").at("items")[0].at("item") == "tonic" &&
          trade.offer().at("other").at("items")[0].at("name") == "Şifa Toniği" &&
          trade.offer().at("other").at("items")[0].at("quantity") == 3 &&
          trade.offer().at("other").at("currency") == 47,
          "peer identity, name, quantity and currency remain inspectable");
    check(!trade.accept_action(), "unpresented offer cannot accept");
    trade.mark_presented(1);
    check(!trade.accept_action(), "old presentation cannot consent to latest revision");
    trade.mark_presented(2);
    const auto accept = trade.accept_action();
    check(accept && *accept == Json{{"action","trade_accept"},{"revision",2}}, "accept binds displayed revision");
    trade.mark_presented(2);
    check(!trade.accept_action(), "pending acceptance cannot be sent twice");

    auto acknowledged = first;
    acknowledged["own"]["accepted"] = true;
    acknowledged["other"]["accepted"] = true;
    check(trade.receive(acknowledged, 9) && trade.offer() == acknowledged, "same revision acceptance update is retained");
    trade.mark_presented(2);
    check(!trade.accept_action(), "own accepted state forbids duplicate confirmation");

    auto second = offer(3);
    check(trade.receive(second, 9), "new revision retained");
    check(!trade.accept_action(), "new revision invalidates prior consent");
    trade.mark_presented(2);
    check(!trade.accept_action(), "wrong old revision remains ineligible");
    trade.mark_presented(3);
    auto peer_accepted = second;
    peer_accepted["other"]["accepted"] = true;
    check(trade.receive(peer_accepted, 9) && trade.offer().at("other").at("accepted") == true,
          "peer acceptance updates at the same revision are not stale");
    check(trade.accept_action().has_value(), "unchanged displayed terms permit confirmation after peer acceptance");
    const Json failed{{"type","error"},{"code","trade_persistence"},{"peer",7},{"revision",3},
                      {"message","Alışveriş kaydedilemedi."}};
    auto wrong_failure = failed; wrong_failure["revision"] = 2;
    check(!trade.receive(wrong_failure, 9), "stale persistence error cannot unlock pending acceptance");
    trade.mark_presented(3);
    check(!trade.accept_action(), "stale failure leaves acceptance consumed");
    check(!trade.receive(peer_accepted, 9), "duplicate authoritative terms do not unlock consumed acceptance");
    check(trade.receive(failed, 9) && trade.offer() == peer_accepted,
          "attributable failure preserves authoritative terms and allows fresh presentation");
    check(!trade.accept_action(), "failed confirmation cannot retry without fresh presentation");
    trade.mark_presented(3);
    check(trade.accept_action().has_value(), "freshly presented failed terms allow explicit retry");
    trade.invalidate_consent();
    check(!trade.receive(failed, 9), "persistence error cannot unlock local draft invalidation");
    trade.mark_presented(3);
    check(!trade.accept_action(), "local draft remains blocked after unrelated confirmation failure");

    check(trade.receive(offer(4), 9), "later revision clears pending confirmation");
    trade.mark_presented(4);
    check(!trade.receive(offer(3), 9), "older revision ignored");
    check(trade.accept_action().has_value(), "older revision cannot invalidate current presentation");
    check(trade.receive(offer(5), 9), "another revision retained");
    trade.mark_presented(5);
    auto conflict = offer(5);
    conflict["other"]["currency"] = 48;
    check(!trade.receive(conflict, 9), "changed terms at identical revision rejected");
    trade.mark_presented(5);
    check(!trade.accept_action() && trade.offer() == offer(5), "conflicting revision fails closed without replacing terms");
    check(!trade.receive(offer(5), 9), "duplicate cannot repair conflicted revision");
    trade.mark_presented(5);
    check(!trade.accept_action(), "conflict remains blocked until new revision");

    check(trade.receive(offer(6), 9), "new authoritative terms recover from conflict");
    trade.mark_presented(6);
    trade.invalidate_consent();
    trade.mark_presented(6);
    check(!trade.accept_action(), "local draft change blocks old authoritative consent");
    check(!trade.receive(offer(6), 9), "duplicate event does not settle a local draft");
    trade.mark_presented(6);
    check(!trade.accept_action(), "pending local offer requires a newer authoritative revision");
    check(trade.receive(offer(7), 9), "authoritative update settles pending draft");
    trade.mark_presented(7);
    check(trade.accept_action().has_value(), "newly displayed authoritative update restores consent");

    check(trade.receive(Json{{"type","trade_complete"},{"peer",7}}, 9), "completion retained");
    trade.mark_presented(7);
    check(!trade.active() && !trade.accept_action() && trade.offer().at("type") == "trade_complete", "terminal clears consent");
    check(!trade.receive(offer(7), 9) && !trade.active(), "old revision cannot reopen completed trade");
    trade.reset();
    check(!trade.active() && trade.peer() == 0 && trade.revision() == 0 && !trade.accept_action(), "reconnect clears state");

    const Json invite{{"type","trade_invite"},{"from",7},{"phase","invited"},{"revision",1}};
    check(trade.receive(invite, 9), "authoritative invitation retained");
    check(!trade.accept_action(), "invitation also requires presentation");
    trade.mark_presented(1);
    check(trade.accept_action().has_value(), "recipient may accept presented invitation");
    trade.reset();
    auto outgoing = offer(1); outgoing["phase"] = "invited"; outgoing["initiator"] = 9;
    outgoing["own"]["currency"] = outgoing["other"]["currency"] = 0;
    outgoing["other"]["items"] = Json::array();
    check(trade.receive(outgoing, 9), "outgoing invitation tracked");
    trade.mark_presented(1);
    check(!trade.accept_action(), "requester cannot consent to own invitation");
    check(trade.receive(Json{{"type","trade_cancel"}}, 9), "cancellation retained");
    check(!trade.active() && !trade.accept_action(), "cancel clears consent");
    check(!trade.receive(outgoing, 9), "old invitation cannot reopen canceled trade");
    auto incoming = outgoing; incoming["initiator"] = 7; incoming["revision"] = 100;
    check(trade.receive(incoming, 9), "new invitation revision is not fixed to one");
    trade.mark_presented(100);
    const Json redundant{{"type","trade_invite"},{"from",7},{"phase","invited"},{"revision",100}};
    check(!trade.receive(redundant, 9) && trade.offer() == incoming,
          "redundant invitation preserves authoritative terms and presentation");
    check(trade.accept_action().has_value(), "initiator identifies recipient without notification");
    auto opened = incoming; opened["phase"] = "open";
    check(trade.receive(opened, 9), "same revision invitation opening retained");
    check(!trade.accept_action(), "phase change invalidates invitation presentation");
    trade.mark_presented(100);
    check(trade.accept_action().has_value(), "presented open phase may confirm independently");

    for (const auto* field : {"uid","item","name","quantity"}) {
        trade.reset(); auto invalid = offer(); invalid["other"]["items"][0].erase(field);
        check(!trade.receive(invalid, 9) && !trade.accept_action(), "incomplete peer item rejected");
    }
    for (const auto* field : {"uid","item","name"}) {
        trade.reset(); auto invalid = offer(); invalid["other"]["items"][0][field] = std::string(129, 'x');
        check(!trade.receive(invalid, 9), "oversized item strings rejected");
    }
    trade.reset(); auto invalid = offer(); invalid["other"]["items"].push_back(invalid["other"]["items"][0]);
    check(!trade.receive(invalid, 9), "duplicate offered uid rejected");
    invalid = offer(); invalid["other"]["currency"] = -1;
    check(!trade.receive(invalid, 9), "negative currency rejected");
    invalid = offer(); invalid["other"]["currency"] = 1000000001;
    check(!trade.receive(invalid, 9), "currency envelope enforced");
    invalid = offer(); invalid["other"]["items"][0]["quantity"] = 1001;
    check(!trade.receive(invalid, 9), "quantity envelope enforced");
    invalid = offer(); invalid["other"]["accepted"] = 1;
    check(!trade.receive(invalid, 9), "confirmation state must be boolean");
    invalid = offer(); invalid["phase"] = "invented";
    check(!trade.receive(invalid, 9), "unknown phase rejected");
    invalid = offer(); invalid["revision"] = -1;
    check(!trade.receive(invalid, 9), "negative revision rejected");
    invalid = offer(); invalid["revision"] = std::numeric_limits<std::uint64_t>::max();
    trade.reset(); check(trade.receive(invalid, 9), "unsigned revision range retained without signed narrowing");
    invalid = offer(); invalid["revision"] = 0;
    trade.reset(); check(!trade.receive(invalid, 9), "zero revision rejected");
    invalid = offer(); invalid["peer"] = std::numeric_limits<std::uint64_t>::max();
    check(!trade.receive(invalid, 9), "peer identity cannot wrap into player range");
    invalid = offer(); invalid["initiator"] = 10;
    check(!trade.receive(invalid, 9), "unrelated initiator cannot grant invitation consent");
    invalid = offer(); invalid["other"]["items"][0]["uid"] = std::string("bad\0uid", 7);
    check(!trade.receive(invalid, 9), "embedded nul identifier rejected");
    invalid = offer();
    for (int i = 0; i < 33; ++i) {
        auto row = invalid["other"]["items"][0]; row["uid"] = "row-" + std::to_string(i);
        invalid["own"]["items"].push_back(row);
    }
    check(!trade.receive(invalid, 9), "item row limit enforced on both offers");
    invalid["own"]["items"].erase(32);
    invalid["other"]["items"] = invalid["own"]["items"];
    for (const auto* side : {"own", "other"})
        for (auto& row : invalid[side]["items"]) row["name"] = std::string(128, 'x');
    check(trade.receive(invalid, 9) && trade.offer() == invalid,
          "all sixty-four legal rows remain retained even above old eight-KiB envelope");
    return 0;
}
