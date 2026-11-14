#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ikv {
class ClientUI;
// NONCANONICAL wave 30 raid roster (world_instance.cpp; docs/network/CUSTOM_PROTOCOL.md "Raids"). The original client had no
// raids. Membership comes from "raid" events and from state "raid" (leader, parties, compact rows [id, name, class_id, level,
// hp percent, source_registry_id] of the other parties); the own party's members come from the state's "party" rows and
// player entities. A frame may omit rows (the server budgets them), so the last known row of a member is kept; a raid absent
// from 100 consecutive frames (5 s, e.g. after a phase transfer to a shard without it) is dropped.
class ClientRaid {
public:
    using Json = nlohmann::json;
    struct Member { std::uint32_t id = 0; std::string name; int class_id = 0, level = 0, percent = 0; };

    // A {type:"raid",leader,parties} event; false when it is not one or is malformed. leader 0 clears the raid.
    bool receive(const Json& event);
    // An accepted state frame (client_session validated the rest); `self` is the own entity id.
    void update(const Json& state, std::uint32_t self);
    bool active() const noexcept { return leader_ != 0; }
    std::uint32_t leader() const noexcept { return leader_; }
    const std::array<std::vector<std::uint32_t>, 4>& parties() const noexcept { return parties_; }
    const Member* member(std::uint32_t id) const;
    void reset();
    // Compact roster frame at x,y (screen pixels, `scale` = UI scale): one column per party, a name and an HP bar per member.
    // Returns the drawn width and height (0,0 when not in a raid).
    std::array<int, 2> paint(ClientUI& ui, int x, int y, float scale) const;

private:
    bool parties_from(const Json& value, std::uint32_t leader);
    std::uint32_t leader_ = 0;
    std::array<std::vector<std::uint32_t>, 4> parties_{};
    std::map<std::uint32_t, Member> known_;
    int missing_ = 0; // consecutive frames without state "raid"
};
} // namespace ikv
