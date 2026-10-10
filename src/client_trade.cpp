#include "ikv/client_trade.hpp"

#include <array>
#include <climits>
#include <string>
#include <string_view>

namespace ikv {
namespace {
using Json = ClientTrade::Json;

bool number(const Json& value, std::uint64_t minimum, std::uint64_t maximum) {
    if (!value.is_number_integer()) return false;
    if (!value.is_number_unsigned() && value.get<std::int64_t>() < 0) return false;
    const auto n = value.get<std::uint64_t>();
    return n >= minimum && n <= maximum;
}
bool text(const Json& value, std::size_t maximum) {
    if (!value.is_string()) return false;
    const auto& s = value.get_ref<const std::string&>();
    return !s.empty() && s.size() <= maximum && s.find('\0') == std::string::npos;
}
bool terms(const Json& offer) {
    if (!offer.is_object() || offer.size() != 3 || !offer.contains("items") ||
        !offer.contains("currency") || !offer.contains("accepted") ||
        !number(offer.at("currency"), 0, 1000000000) || !offer.at("accepted").is_boolean()) return false;
    const auto& items = offer.at("items");
    if (!items.is_array() || items.size() > 32) return false;
    std::array<std::string_view, 32> seen{};
    std::size_t count = 0;
    for (const auto& item : items) {
        if (!item.is_object() || item.size() != 4 || !item.contains("uid") ||
            !item.contains("item") || !item.contains("name") || !item.contains("quantity") ||
            !text(item.at("uid"), 32) || !text(item.at("item"), 32) ||
            !text(item.at("name"), 128) || !number(item.at("quantity"), 1, 1000)) return false;
        const auto& uid = item.at("uid").get_ref<const std::string&>();
        for (std::size_t i = 0; i < count; ++i) if (seen[i] == uid) return false;
        seen[count++] = uid;
    }
    return true;
}
bool same_terms(const Json& a, const Json& b) {
    for (const auto* side : {"own", "other"}) {
        if (a.at(side).at("items") != b.at(side).at("items") ||
            a.at(side).at("currency") != b.at(side).at("currency")) return false;
    }
    return a.at("initiator") == b.at("initiator") && a.at("peer_name") == b.at("peer_name");
}
} // namespace

bool ClientTrade::receive(const Json& event, std::uint32_t self) {
    if (!event.is_object() || !event.contains("type") || !event.at("type").is_string()) return false;
    const auto& type = event.at("type").get_ref<const std::string&>();
    if (type == "error") {
        if (!active_ || !pending_accept_ || event.size() != 5 || !event.contains("code") ||
            event.at("code") != "trade_persistence" || !event.contains("peer") ||
            !number(event.at("peer"), 1, INT32_MAX) || event.at("peer") != peer_ ||
            !event.contains("revision") || !number(event.at("revision"), 1, UINT64_MAX) ||
            event.at("revision") != revision_ || !event.contains("message") ||
            !text(event.at("message"), 128) || event_.at("phase") != "open" ||
            event_.at("own").at("accepted") != false || event_.at("other").at("accepted") != true) return false;
        // Only this attributable failed confirmation permits an explicit retry.
        // The current authoritative terms must be presented again first.
        presented_ = blocked_ = pending_accept_ = false;
        return true;
    }
    if (type != "trade" && type != "trade_invite" && type != "trade_complete" && type != "trade_cancel") return false;
    auto reject = [this] { invalidate_consent(); return false; };
    if (type == "trade_complete" || type == "trade_cancel") {
        if (type == "trade_complete") {
            if (event.size() != 2 || !event.contains("peer") ||
                !number(event.at("peer"), 1, INT32_MAX) ||
                (peer_ && event.at("peer").get<std::uint32_t>() != peer_)) return reject();
        } else if (event.size() != 1) return reject();
        event_ = event;
        active_ = recipient_ = presented_ = pending_accept_ = false;
        blocked_ = true;
        return true;
    }
    if (!self || self > INT32_MAX || !event.contains("revision") ||
        !number(event.at("revision"), 1, UINT64_MAX)) return reject();
    const auto revision = event.at("revision").get<std::uint64_t>();
    // Old snapshots must not cancel consent to a newer revision.
    if (revision < revision_ || (!active_ && revision <= revision_)) return false;
    const bool invite = type == "trade_invite";
    const auto* peer_key = invite ? "from" : "peer";
    if (!event.contains(peer_key) || !number(event.at(peer_key), 1, INT32_MAX) ||
        !event.contains("phase") || !event.at("phase").is_string()) return reject();
    const auto peer = event.at(peer_key).get<std::uint32_t>();
    const auto& phase = event.at("phase").get_ref<const std::string&>();
    if (peer == self || (phase != "invited" && phase != "open") ||
        (invite && (phase != "invited" || event.size() != 4))) return reject();
    bool recipient = invite;
    if (!invite) {
        if (event.size() != 8 || !event.contains("initiator") ||
            !number(event.at("initiator"), 1, INT32_MAX) || !event.contains("peer_name") ||
            !text(event.at("peer_name"), 128) || !event.contains("own") || !event.contains("other") ||
            !terms(event.at("own")) || !terms(event.at("other"))) return reject();
        const auto initiator = event.at("initiator").get<std::uint32_t>();
        if (initiator != self && initiator != peer) return reject();
        recipient = initiator != self;
        if (phase == "invited" && (!event.at("own").at("items").empty() ||
            !event.at("other").at("items").empty() || event.at("own").at("currency") != 0 ||
            event.at("other").at("currency") != 0 || event.at("own").at("accepted") != false ||
            event.at("other").at("accepted") != false)) return reject();
    }
    // Terms are bounded above; the framed protocol owns the encoded-byte limit.
    if (active_ && peer != peer_) return reject();
    if (!active_ && !event_.is_null() && phase != "invited") return false;
    if (active_ && revision == revision_) {
        const auto& old_type = event_.at("type").get_ref<const std::string&>();
        if (invite && old_type == "trade") {
            // The server sends full terms before its redundant recipient notification.
            if (event_.at("phase") == "invited" && recipient_) return false;
            return reject();
        }
        if (!invite && old_type == "trade") {
            if (!same_terms(event_, event)) return reject();
            const auto& old_phase = event_.at("phase");
            if (old_phase != phase) {
                if (old_phase != "invited" || phase != "open") return reject();
            } else {
                for (const auto* side : {"own", "other"}) {
                    if (event_.at(side).at("accepted") == true && event.at(side).at("accepted") == false) return reject();
                }
                if (event_ == event) return false;
                for (const auto* side : {"own", "other"})
                    event_.at(side).at("accepted") = event.at(side).at("accepted");
                return true;
            }
        } else if (invite) {
            return event_ == event ? false : reject();
        }
        // Invitation enrichment/opening changes what was displayed at the same revision.
    }
    event_ = event;
    peer_ = peer;
    revision_ = revision;
    recipient_ = recipient;
    active_ = true;
    presented_ = blocked_ = pending_accept_ = false;
    return true;
}

void ClientTrade::mark_presented(std::uint64_t revision) noexcept {
    if (active_ && !blocked_ && revision == revision_) presented_ = true;
}
std::optional<ClientTrade::Json> ClientTrade::accept_action() {
    if (!active_ || !presented_ || blocked_) return std::nullopt;
    const auto& phase = event_.at("phase");
    if (phase == "invited") {
        if (!recipient_) return std::nullopt;
    } else if (phase != "open" || event_.at("own").at("accepted") == true) return std::nullopt;
    Json action{{"action", "trade_accept"}, {"revision", revision_}};
    invalidate_consent();
    pending_accept_ = true;
    return action;
}
void ClientTrade::invalidate_consent() noexcept {
    presented_ = false;
    blocked_ = true;
    pending_accept_ = false;
}
void ClientTrade::reset() noexcept {
    event_ = nullptr;
    revision_ = peer_ = 0;
    active_ = recipient_ = presented_ = blocked_ = pending_accept_ = false;
}
} // namespace ikv
