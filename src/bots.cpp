#include "ikv/bots.hpp"
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>

namespace ikv {
BotHost::BotHost(World& world, Store& store, std::string instance) : world_(world), store_(store), instance_(std::move(instance)) {}

// Reuse an idle persisted bot (within two levels of a requested level), else create one with a fresh original-style name.
std::uint32_t BotHost::spawn(const Json& spec, Json& roster) {
    if (!account_) account_ = store_.bot_account();
    const int level = spec.value("level", 0);
    const auto admit = [&](std::int64_t character, const Json& record) {
        try { return world_.add_bot(account_, character, record, spec); }
        catch (...) { store_.leave_character(account_, character); throw; }
    };
    for (auto& row : roster) {
        if (row.at("leased").get<bool>() || (level && std::abs(row.at("level").get<int>() - level) > 2)) continue;
        row["leased"] = true; // taken by this or an earlier spawn of the same command
        try { return admit(row.at("id"), store_.enter_character({account_, false}, row.at("id"), instance_)); }
        catch (const ApiError&) { continue; } // leased elsewhere meanwhile
    }
    for (int attempt = 0; attempt < 24; ++attempt) {
        auto fresh = world_.bot_character(std::max(level, 1));
        auto name = fresh.at("name").get<std::string>();
        if (attempt >= 16) name = name.substr(0, 28) + " " + std::to_string(attempt); // the name pool is exhausted
        std::int64_t character;
        try { character = store_.create_bot_character(account_, name, fresh.at("class_id"), fresh.at("appearance"), fresh.at("state")); }
        catch (const ApiError& e) { if (e.code == "conflict") continue; throw; }
        return admit(character, store_.enter_character({account_, false}, character, instance_));
    }
    throw std::runtime_error("no free bot character name");
}

void BotHost::start(std::optional<std::uint32_t> total) {
    const auto plan = world_.bot_plan(total ? int(*total) : -1);
    if (plan.empty()) return;
    try {
        if (!account_) account_ = store_.bot_account();
        auto roster = store_.bot_characters(account_);
        for (const auto& spec : plan) spawn(spec, roster);
    } catch (const std::exception&) { std::cerr << "bot_spawn_failed\n"; } // players are served without the missing bots
    std::cout << "BOTS spawned=" << world_.bots().size() << std::endl;
}

Json BotHost::command(const std::string& command, const Json& arguments) {
    if (command == "bots_spawn") {
        const auto count = arguments.at("count").get<int>();
        if (count < 1 || count > 50) throw std::invalid_argument("count must be 1..50");
        Json spec{{"behaviour", arguments.at("behaviour")}};
        if (arguments.contains("zone")) spec["zone"] = arguments.at("zone");
        if (arguments.contains("level")) spec["level"] = std::clamp(arguments.at("level").get<int>(), 1, 100);
        if (!account_) account_ = store_.bot_account();
        auto roster = store_.bot_characters(account_);
        Json spawned = Json::array();
        try { for (int n = 0; n < count; ++n) spawned.push_back(spawn(spec, roster)); }
        catch (const std::exception& e) { // report the partial population instead of hiding the bots already in the world
            if (spawned.empty()) throw;
            return {{"ok", false}, {"spawned", spawned}, {"error", e.what()}};
        }
        return {{"ok", true}, {"spawned", spawned}};
    }
    if (command == "bots_despawn") {
        const bool all = arguments.value("all", false);
        std::set<std::uint32_t> ids;
        if (arguments.contains("ids")) for (const auto& id : arguments.at("ids")) ids.insert(id.get<std::uint32_t>());
        int despawned = 0;
        for (const auto& row : world_.bots())
            if (all || ids.contains(row.at("id").get<std::uint32_t>()) || (arguments.contains("zone") && row.at("zone") == arguments.at("zone"))) {
                world_.remove(row.at("id")); ++despawned;
            }
        pump();
        return {{"ok", true}, {"despawned", despawned}};
    }
    throw std::invalid_argument("unknown bot command");
}

void BotHost::pump() { for (const auto& [account, character] : world_.released_bots()) store_.leave_character(account, character); }
void BotHost::stop() { for (const auto& row : world_.bots()) store_.leave_character(account_, row.at("character_id")); }
}
