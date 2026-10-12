#include "ikv/client_mutation.hpp"

#include <cstdlib>
#include <iostream>

namespace {
using Json = nlohmann::json;
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
ikv::PendingMutation mutation(std::uint32_t tick, std::initializer_list<Json> rows, std::vector<std::uint32_t> removed = {},
                              Json events = nullptr) {
    ikv::PendingMutation m;
    for (const auto& row : rows) { m.actors.push_back(std::make_shared<const Json>(row)); m.ticks.push_back(tick); }
    m.removed = std::move(removed);
    m.events = std::move(events);
    return m;
}
}

int main() {
    auto into = mutation(1, {{{"id", 1}, {"hp", 100}}, {{"id", 2}, {"hp", 50}}}, {3}, Json::array({{{"type", "a"}}}));
    ikv::fold_mutation(into, mutation(2, {{{"id", 1}, {"hp", 80}}, {{"id", 3}, {"hp", 9}}}, {2, 4}, Json::array({{{"type", "b"}}})));
    check(into.actors.size() == 2 && into.ticks.size() == 2, "one row per actor");
    check(into.actors[0]->at("id") == 1 && into.actors[0]->at("hp") == 80 && into.ticks[0] == 2, "later row and its tick win");
    check(into.actors[1]->at("id") == 3 && into.ticks[1] == 2, "re-added actor keeps its row");
    check(into.removed == std::vector<std::uint32_t>{2, 4}, "re-add cancels removal; removals drop earlier rows");
    check(into.events.size() == 2 && into.events[0].at("type") == "a" && into.events[1].at("type") == "b", "events keep order");
    ikv::fold_mutation(into, mutation(3, {}, {4}));
    check(into.removed == std::vector<std::uint32_t>{2, 4} && into.events.size() == 2, "repeat removal is not duplicated");
    auto empty = mutation(1, {});
    ikv::fold_mutation(empty, mutation(2, {}, {}, Json::array({{{"type", "c"}}})));
    check(empty.events.is_array() && empty.events.size() == 1, "events append onto an event-free mutation");
    auto flip = mutation(4, {{{"id", 1}, {"hp", 70}, {"zone", 30}}}); flip.source = 30; flip.reset = true; flip.flip = 2147493937u;
    ikv::fold_mutation(flip, mutation(5, {{{"id", 1}, {"hp", 60}, {"zone", 30}}}, {}, Json::array({{{"type", "d"}}})));
    check(flip.flip == 2147493937u && flip.source == 30 && flip.reset, "a same-zone successor folds into a walk-through, which stays one");
    check(flip.actors.size() == 1 && flip.actors[0]->at("hp") == 60 && flip.events.size() == 1, "folded walk-through keeps the latest rows");
}
