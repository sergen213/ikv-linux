#pragma once
// NONCANONICAL: the Galata Bridge (Eminönü <-> Karaköy) as one seamless world.
// The original shipped both bridge stubs broken off over the Golden Horn and joined the zones by boat. Here the client
// renders Eminönü, the completed bridge and Karaköy as one scene in the Eminönü frame, and the server keeps the two zones
// with their own walk grids, transferring a player invisibly when they walk across the seam in the middle of the bridge.
// Karaköy coordinates map into the Eminönü frame by a fixed rigid transform (rotation about Y, translation, water level).
#include <cstdint>
#include <optional>
namespace ikv {
struct Scene;
struct Zone;
namespace galata {
inline constexpr unsigned eminonu_registry = 0, karakoy_registry = 69; // source registry rows
inline constexpr unsigned eminonu_support = 0, karakoy_support = 13;    // their support (walk grid) zones
inline constexpr float section = 155.2f;                                // one original bridge section, along the deck
inline constexpr int sections = 4;                                      // new sections between the stubs
inline constexpr float gap = section * sections;                        // stub end to stub end
inline constexpr float height_offset = -2.62099f;                       // Karaköy y + offset = Eminönü y (water levels meet)
inline constexpr float deck_eminonu = 20.f, deck_karakoy = 30.f + height_offset; // walk heights at the stub ends
inline constexpr float walk_half_width = 35.f;                          // walkable half width of the deck
inline constexpr float seam_to_karakoy = 326.f, seam_to_eminonu = 317.f; // transfer distances from the Eminönü stub end

struct Point { float x = 0, z = 0; };
struct BridgeCoordinates { float s = 0, w = 0; }; // s: along the bridge from the Eminönü stub end, w: across it
Point karakoy_to_eminonu(float x, float z);
Point eminonu_to_karakoy(float x, float z);
BridgeCoordinates bridge_coordinates(float eminonu_x, float eminonu_z);
// The walkable deck height at s, in the Eminönü frame.
float deck_height(float s);
// Scene frame (Eminönü) <-> authoritative zone frame, for a linked registry; other registries are unchanged.
bool linked(unsigned registry);
unsigned scene_registry(unsigned registry); // the registry whose scene shows this one (Karaköy -> Eminönü)
Point to_scene(unsigned registry, float x, float z);
Point from_scene(unsigned registry, float x, float z);
float native_yaw(unsigned registry, float yaw); // a scene-frame heading in the registry's own (map) frame
// Marks the new deck walkable in an original support grid (Eminönü 0 or Karaköy 13; no effect on other zones).
void paint_bridge(Zone& zone, unsigned support_id);
// The linked zone a position (authoritative millimetres) has walked into across the seam, if any.
struct Crossing { unsigned registry = 0; std::int32_t x = 0, y = 0; };
std::optional<Crossing> crossing(unsigned registry, std::int32_t x, std::int32_t y);
// Ground under a scene-frame point: the scene's own grid, else the linked Karaköy grid.
std::optional<float> scene_ground(const Scene& scene, float x, float z);
// Builds the merged world from an Eminönü scene and a decoded Karaköy scene, before resources load (scene.cpp).
void merge(Scene& eminonu, Scene karakoy, void (*append)(Scene&, Scene));
// The new sections reuse the stub section's baked lightmap, whose tram and lamp-post shadows would repeat on every copy:
// after resources load, each copy samples one unshadowed texel of its own lightmap instead.
void clean_lightmaps(Scene& scene);
} // namespace galata
} // namespace ikv
