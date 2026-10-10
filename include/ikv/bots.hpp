#pragma once
#include "ikv/store.hpp"
#include "ikv/world.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace ikv {
// Server-side bot population (NONCANONICAL). Bot characters belong to the reserved Store bot account, hold ordinary character
// leases of this world instance and are admitted through World::add_bot; World::remove persists them like players.
class BotHost {
public:
    BotHost(World& world, Store& store, std::string instance);
    void start(std::optional<std::uint32_t> total); // gameplay.json bots.spawn counts, or exactly `total` bots round-robin
    Json command(const std::string& command, const Json& arguments); // bots_spawn / bots_despawn (admin world commands)
    void pump(); // release the leases of bots the world removed (despawn, moderator kick)
    void stop(); // after the final save: release every remaining bot lease
private:
    std::uint32_t spawn(const Json& spec, Json& roster);
    World& world_;
    Store& store_;
    std::string instance_;
    std::int64_t account_ = 0;
};
}
