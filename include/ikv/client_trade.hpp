#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>

namespace ikv {
// Single-threaded consent state for authoritative trade events, not a local draft.
class ClientTrade {
public:
    using Json = nlohmann::json;

    // False for duplicates, older revisions or rejected events. Malformed/conflicting
    // trade events fail closed; unrelated events leave the trade untouched.
    bool receive(const Json& event, std::uint32_t self);
    bool active() const noexcept { return active_; }
    const Json& offer() const noexcept { return event_; }
    std::uint32_t peer() const noexcept { return peer_; }
    std::uint64_t revision() const noexcept { return revision_; }

    // The renderer must call this only after all terms of this revision were seen.
    void mark_presented(std::uint64_t revision) noexcept;
    std::optional<Json> accept_action();
    // Local edits/sends block consent until authoritative revision/phase changes.
    void invalidate_consent() noexcept;
    // Required at disconnect/reconnect; server revisions restart with the world.
    void reset() noexcept;

private:
    Json event_;
    std::uint64_t revision_ = 0;
    std::uint32_t peer_ = 0;
    bool active_ = false;
    bool recipient_ = false;
    bool presented_ = false;
    bool blocked_ = false;
    bool pending_accept_ = false;
};
} // namespace ikv
