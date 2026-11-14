#pragma once

#include "ikv/assets.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ikv {
// NONCANONICAL wave 29: quest markers on the Harita minimap and the Dünya Haritası (M) window. The original client had
// neither; positions come from the server's catalogs.map (world_map.cpp) when it sends one, else from the visible entities.
// Coordinates are scene units (wire / 1000); map texels follow the original minimap readout 1024 - position * 2/3 (00477775)
// on the 2048-texel ui\zoneNN.dds art.
class ClientMap {
public:
    using Json = nlohmann::json;
    // catalogs.map mark values: an accepted quest handed in here, not finished ('?' grey); a quest to take ('!'); a finished
    // quest to hand in ('?').
    enum Mark : int { mark_none = 0, mark_pending = 1, mark_available = 2, mark_ready = 3 };
    struct Npc { std::uint32_t id = 0; std::string kind, name; float x = 0, z = 0; int mark = mark_none, to = -1; bool banker = false; };
    struct Objective { std::string quest, name; float x = 0, z = 0, r = 0; };
    struct Member { std::uint32_t id = 0; std::string name; float x = 0, z = 0; };
    // Rebuild from an accepted state (client_session validated catalogs.map). `group` holds the own group's member ids.
    void update(const Json& state, std::uint32_t self, const Json& group);
    const std::vector<Npc>& npcs() const noexcept { return npcs_; }
    const std::vector<Objective>& objectives() const noexcept { return objectives_; }
    const std::vector<Member>& party() const noexcept { return party_; }
    bool authoritative() const noexcept { return authoritative_; } // the server sent catalogs.map

    // Görev al mark of one visible NPC from its entity "quests" offer list, as client dialogue_quests pages them (fallback for
    // servers without catalogs.map).
    static int quest_mark(const Json& offered, std::uint32_t npc, const Json& character_state, const Json& quest_catalog);
    static std::array<float, 2> texel(float x, float z) { return {1024 - x * (2.f / 3), 1024 - z * (2.f / 3)}; }
    // Point p kept inside the rectangle (l,t,r,b) shrunk by `inset`: unchanged when inside, else moved toward the rectangle's
    // centre onto its border; `angle` is the screen direction from the centre to p (radians, 0 = +x, y down).
    struct Edge { float x = 0, y = 0, angle = 0; bool clamped = false; };
    static Edge clamp_to_rect(float px, float py, float l, float t, float r, float b, float inset);
    // Turkish role line shown beside an NPC name (Dünya Haritası list and tooltip).
    static const char* role(const Npc& npc);

    // Dünya Haritası view: centre texel and zoom (1 = the whole 2048-texel zone art fits the view; up to 8).
    float zoom = 1, centre_u = 1024, centre_v = 1024;
    float half() const { return 1024 / zoom; }
    void zoom_at(int steps, float u, float v); // wheel steps toward/away from texel (u,v), keeping it under the pointer
    void pan(float du, float dv);              // texel offsets
    void reset() { zoom = 1; centre_u = centre_v = 1024; }

    // Procedural RGBA marker art (white; tinted when drawn): an arrow pointing to texture-up, a filled disc and a ring.
    static assets::DdsImage arrow_image();
    static assets::DdsImage disc_image();
    static assets::DdsImage ring_image();

private:
    void clamp_view();
    std::vector<Npc> npcs_;
    std::vector<Objective> objectives_;
    std::vector<Member> party_;
    bool authoritative_ = false;
};
} // namespace ikv
