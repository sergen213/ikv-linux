#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ikv {
// One authoritative state's presentation change, queued while a load freezes presentation.
struct PendingMutation {
    unsigned source = 0;
    bool reset = false; // zone change: actors not listed are detached
    // NONCANONICAL wave-16 walk-through (state "crossed"): the door id in the previous zone. A reset whose presentation
    // carries on (no load, no fade): listed actors keep their models where they are installed.
    std::uint32_t flip = 0;
    std::vector<std::shared_ptr<const nlohmann::json>> actors;
    std::vector<std::uint32_t> ticks; // per actors[i]: the state tick its row arrived with
    std::vector<std::uint32_t> removed;
    nlohmann::json events;
    std::size_t bytes = 0;
};

// Applies `next` on top of `into` (same zone, `next` not a reset): a later row replaces an actor's earlier one, a
// removal drops it, a re-add cancels an earlier removal (the actor stays attached), events append in arrival order.
inline void fold_mutation(PendingMutation& into, PendingMutation&& next) {
    const auto row_of = [&](std::uint32_t id) {
        return std::find_if(into.actors.begin(), into.actors.end(), [&](const auto& row) { return row->at("id") == id; });
    };
    for (const auto id : next.removed) {
        if (const auto row = row_of(id); row != into.actors.end()) {
            into.ticks.erase(into.ticks.begin() + (row - into.actors.begin()));
            into.actors.erase(row);
        }
        if (std::find(into.removed.begin(), into.removed.end(), id) == into.removed.end()) into.removed.push_back(id);
    }
    for (std::size_t i = 0; i < next.actors.size(); ++i) {
        const auto id = next.actors[i]->at("id").get<std::uint32_t>();
        std::erase(into.removed, id);
        if (const auto row = row_of(id); row != into.actors.end()) {
            into.ticks[static_cast<std::size_t>(row - into.actors.begin())] = next.ticks[i];
            *row = std::move(next.actors[i]);
        } else {
            into.actors.push_back(std::move(next.actors[i]));
            into.ticks.push_back(next.ticks[i]);
        }
    }
    for (auto& event : next.events) into.events.push_back(std::move(event));
}
} // namespace ikv
