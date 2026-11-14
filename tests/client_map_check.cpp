// NONCANONICAL wave 29: ikv::ClientMap marker data and geometry (minimap quest markers, Dünya Haritası).
#include "ikv/client_map.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
using Json = nlohmann::json;
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
bool near_value(float a, float b) { return std::abs(a - b) < 1e-3f; }
}

int main() {
    using ikv::ClientMap;
    const Json catalog = {
        {"q1", {{"name", "İlk"}, {"giver", 100}, {"objective", {{"kind", "kill"}, {"count", 3}}}}},
        {"q2", {{"name", "Konuş"}, {"giver", 100}, {"turn_in", 200}, {"objective", {{"kind", "talk"}}}}},
        {"q3", {{"name", "Sonra"}, {"giver", 100}, {"requires", "q9"}, {"objective", {{"kind", "visit"}, {"zone", "eminonu"}, {"x", 3000}, {"y", -6000}, {"radius", 1500}}}}},
        {"q4", {{"name", "Otomatik"}, {"giver", 100}, {"auto", true}, {"objective", {{"kind", "kill"}, {"count", 1}}}}}};
    const Json fresh = {{"quests", Json::object()}, {"quests_done", Json::array()}};
    check(ClientMap::quest_mark(Json::array({"q1"}), 100, fresh, catalog) == ClientMap::mark_available, "giver offers a takeable quest");
    check(ClientMap::quest_mark(Json::array({"q1"}), 100 + 65536 * 2, fresh, catalog) == ClientMap::mark_available, "instance copy of the giver offers it too");
    check(ClientMap::quest_mark(Json::array({"q3"}), 100, fresh, catalog) == ClientMap::mark_none, "unmet prerequisite gives no mark");
    check(ClientMap::quest_mark(Json::array({"q4"}), 100, fresh, catalog) == ClientMap::mark_none, "auto quest is not taken at its giver");
    check(ClientMap::quest_mark(Json::array({"q4"}), 555, fresh, catalog) == ClientMap::mark_available, "auto quest is offered at the auto-quest NPC");
    const Json working = {{"quests", {{"q1", {{"progress", 1}}}, {"q2", {{"progress", 0}}}}}, {"quests_done", Json::array({"q9"})}};
    check(ClientMap::quest_mark(Json::array({"q1"}), 100, working, catalog) == ClientMap::mark_pending, "unfinished quest at its turn-in is pending");
    check(ClientMap::quest_mark(Json::array({"q1", "q3"}), 100, working, catalog) == ClientMap::mark_available, "an available quest outranks a pending one");
    check(ClientMap::quest_mark(Json::array({"q2"}), 200, working, catalog) == ClientMap::mark_ready, "talk quest is ready at its turn-in");
    check(ClientMap::quest_mark(Json::array({"q2"}), 100, working, catalog) == ClientMap::mark_none, "a giver that is not the turn-in shows nothing");
    const Json done = {{"quests", {{"q1", {{"progress", 3}}}}}, {"quests_done", Json::array()}};
    check(ClientMap::quest_mark(Json::array({"q1"}), 100, done, catalog) == ClientMap::mark_ready, "a finished kill count is ready");

    // Edge clamp: inside points stay, outside points land on the inset border along the ray from the centre.
    auto e = ClientMap::clamp_to_rect(50, 50, 0, 0, 100, 100, 10);
    check(!e.clamped && near_value(e.x, 50) && near_value(e.y, 50), "inside point unchanged");
    e = ClientMap::clamp_to_rect(250, 50, 0, 0, 100, 100, 10);
    check(e.clamped && near_value(e.x, 90) && near_value(e.y, 50) && near_value(e.angle, 0), "right of the view clamps to the right edge");
    e = ClientMap::clamp_to_rect(50, -400, 0, 0, 100, 100, 10);
    check(e.clamped && near_value(e.x, 50) && near_value(e.y, 10) && e.angle < 0, "above the view clamps to the top edge");
    e = ClientMap::clamp_to_rect(150, 150, 0, 0, 100, 100, 0);
    check(e.clamped && near_value(e.x, 100) && near_value(e.y, 100), "diagonal clamps to the corner");

    // Texels follow the original minimap readout 1024 - 2/3 * position.
    const auto t = ClientMap::texel(379.5f, 759.f);
    check(near_value(t[0], 771) && near_value(t[1], 518), "Agah Efendi texel (VOD 771,518)");

    // View: zoom keeps the pointer texel fixed, pan is clamped to the 2048-texel art.
    ClientMap map;
    map.zoom_at(4, 512, 512);
    check(map.zoom > 2.4f && map.zoom < 2.5f, "four wheel steps zoom by 1.25^4");
    const float h = map.half();
    check(near_value((512 - (map.centre_u - h)) / (2 * h), 0.25f), "zoom keeps the pointer texel under the pointer");
    map.pan(-5000, 0);
    check(near_value(map.centre_u, h), "pan clamps to the left edge");
    map.zoom_at(-20, 0, 0);
    check(near_value(map.zoom, 1) && near_value(map.centre_u, 1024) && near_value(map.centre_v, 1024), "zooming out fully shows the whole map");

    // Authoritative catalogs.map, party rows and visible group members.
    const Json state = {{"zone", "eminonu"},
        {"character", {{"state", fresh}}},
        {"catalogs", {{"quests", catalog}, {"map", {
            {"npcs", Json::array({{{"id", 100}, {"kind", "questgiver"}, {"name", "Agah Efendi"}, {"x", 379500}, {"y", 759000}, {"mark", 2}},
                                  {{"id", 300}, {"kind", "door"}, {"x", 1000}, {"y", 2000}, {"to", 1}},
                                  {{"id", 301}, {"kind", "merchant"}, {"name", "Kasa"}, {"x", 0}, {"y", 0}, {"banker", true}}})},
            {"objectives", Json::array({{{"quest", "q1"}, {"x", 3000}, {"y", -6000}, {"r", 45000}}})}}}}},
        {"party", Json::array({{{"id", 7}, {"name", "Uzak"}, {"zone", "eminonu"}, {"x", 10000}, {"y", 20000}},
                               {{"id", 8}, {"name", "Başka"}, {"zone", "misir"}, {"x", 1}, {"y", 1}}})},
        {"entities", Json::array({{{"id", 9}, {"kind", "player"}, {"name", "Yakın"}, {"x", 5000}, {"y", 6000}},
                                  {{"id", 10}, {"kind", "player"}, {"name", "Yabancı"}, {"x", 5000}, {"y", 6000}}})}};
    map.update(state, 1, Json::array({1, 7, 9}));
    check(map.authoritative() && map.npcs().size() == 3, "catalog NPCs are listed");
    check(map.npcs()[0].mark == ClientMap::mark_available && near_value(map.npcs()[0].x, 379.5f) && near_value(map.npcs()[0].z, 759), "mark and scene position");
    check(map.npcs()[1].to == 1 && map.npcs()[1].name.empty(), "door destination registry id");
    check(map.npcs()[2].banker && std::string(ClientMap::role(map.npcs()[2])) == "Bankacı", "banker role");
    check(map.objectives().size() == 1 && map.objectives()[0].name == "İlk" && near_value(map.objectives()[0].r, 45), "objective named from the quest catalog");
    check(map.party().size() == 2 && map.party()[0].name == "Uzak" && map.party()[1].name == "Yakın", "same-zone party rows and visible members only");

    // Without catalogs.map (older server): visible listed entities with derived marks, visit spots of this zone.
    Json old = state;
    old["catalogs"].erase("map");
    old["character"]["state"] = {{"quests", {{"q3", {{"progress", 0}}}}}, {"quests_done", Json::array({"q9"})}};
    old["entities"].push_back({{"id", 100}, {"kind", "questgiver"}, {"name", "Agah Efendi"}, {"x", 379500}, {"y", 759000}, {"quests", Json::array({"q1", "q3"})}});
    old["entities"].push_back({{"id", 400}, {"kind", "enemy"}, {"name", "Fare"}, {"x", 0}, {"y", 0}});
    map.update(old, 1, Json::array());
    check(!map.authoritative() && map.npcs().size() == 1 && map.npcs()[0].mark == ClientMap::mark_available, "fallback marks visible givers");
    check(map.objectives().size() == 1 && map.objectives()[0].quest == "q3" && near_value(map.objectives()[0].x, 3), "fallback visit spot");

    // Procedural art: opaque centre, transparent corner.
    const auto disc = ClientMap::disc_image();
    check(disc.width == 32 && disc.rgba[(16 * 32 + 16) * 4 + 3] == 255 && disc.rgba[3] == 0, "disc coverage");
    const auto arrow = ClientMap::arrow_image();
    check(arrow.rgba[(20 * 32 + 16) * 4 + 3] == 255 && arrow.rgba[(4 * 32 + 3) * 4 + 3] == 0, "arrow coverage");
    std::cout << "client map markers ok\n";
}
