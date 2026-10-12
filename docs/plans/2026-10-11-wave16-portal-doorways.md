# Wave 16 — see-through doorways and instant doors (2026-10-11)

User ask (correcting wave 15): doors open in real time and the place behind them is already loaded. Chosen option:
**see-through doorway** (WoW-interior style): through an open doorway the destination zone is drawn live; the player
walks through with no switch, fade or black frame. Doors that are not walk-through openings (Ruh Taşı, Drezin, boats,
elevators, zeplin, hatches, wells, outdoor markers) become **instant doors**: the destination is GPU-resident before use,
so the switch happens in one frame. The wave-15 fade stays only as the fallback (admin/arena jumps, a destination not yet
resident).

Research: `analysis/porting-wave16/{Doorways,PortalRender,PortalWorld}/report.md`, `Doorways/doorways.json`.

## Facts the design rests on
- Zones are separate maps in the same origin-centred ±1536 frame, so a doorway pair is joined by a rigid transform
  M (yaw + translation incl. vertical; scale 1). No doorway has an authored hole: each is a closed wall with a door
  model, so the portal replaces that wall area and portal clipping is mandatory (the transformed interior overlaps the
  exterior).
- Door leaf models (`kapi_balikpazari_dis/_ic`, Antrepo doors) are skinned [INFERENCE: opening animation].
- Movement is server-authoritative (click-to-move A*, 800 units/tick at 20 Hz); the client interpolates 2 ticks behind.

## First portal set
| pair | door ids | zones | confidence |
|---|---|---|---|
| Antrepo | 30005 ↔ 30006 | eminonu ↔ antrepo interior | high (interior fits the warehouse, scale 1) |
| Mısır Balıkpazarı gate | 30000 ↔ 30001 | eminonu ↔ misir | medium (interior is ~1.14× larger; looking back from inside is exact only at this gate) |
| Umut Dergisi | 132061 ↔ 132178 | karakoy ↔ its room | high, quest-gated (282): exercises closed portals |
Every other door: instant door.

## Contracts

### Data (config door NPC, server units ×1000)
`"door":{…,"portal":{"pair":<door id>,"threshold":[[ax,ay],[bx,by]],"floor":<world y×1000>,"height":<opening height×1000>}}`.
Entering = moving from the right to the left of a→b. M maps midpoint(a,b) → midpoint(pair.b,pair.a), direction a→b →
pair.b→pair.a, vertical by floor(this) − floor(pair); the pair's portal is M⁻¹. Copies: a door in instance copy n pairs
with pair id + n·65536. Authored in a generator data file (not by hand in gameplay.json), validated by World.

### Protocol (adopted from PortalWorld §3, with floor/height added)
- Door entity (state): `"portal":{"open":true,"zone":"…","registry":N,"threshold":[[..],[..]],"exit":[[..],[..]],"floor":[f_this,f_exit],"height":h}`
  or `{"open":false,"reason":"…","threshold":…,"floor":[f_this],"height":h}`; oriented for this observer.
- Linked entity: normal player/NPC row in its own zone frame plus `"portal":<door id>`; no nested portal records.
- Top level: `"crossed":{"door":<door id in the previous zone>}` in the first state after a walk-through.
- Snapshot unchanged (linked players included, raw coordinates). Binary destination: 14 bytes, or 18 with a trailing
  u32 portal door id (x,y then in the linked zone's frame).
- Combat, interaction, trade and local chat do not cross portals (close() already requires the same zone).
- Gates (gmonly, guild_zones, required_quest, required_death, lockouts, disabled_zones) are evaluated per observer each
  state through one `door_gate()`; a closed portal is drawn shut and refuses crossing with the gate's reason.

### Renderer API (include/ikv/renderer.hpp)
```cpp
struct PortalView {
    const Scene* scene = nullptr;            // resident destination, stable address
    Matrix4 to_primary{};                     // rigid: destination world → primary world
    std::array<std::array<float,3>,4> quad{}; // opening in primary world coords; destination is beyond its plane
    std::span<const RenderEntity> entities;   // destination-side actors, destination coords
};
virtual void render(const Scene&, std::span<const RenderEntity>, std::uint32_t self, float dt,
                    std::span<const PortalView> portals = {}) = 0;
virtual void make_resident(const Scene&) = 0;   // start/continue budgeted background upload (incl. RT); idempotent
virtual bool resident(const Scene&) const = 0;  // fully uploaded: usable as primary or portal without a stall
virtual void evict(const Scene&) = 0;           // deferred free; Scene must outlive the next render()
```
- The primary is the Scene containing the camera eye. When the primary becomes the Scene that was last frame's portal
  view (eye crossed), temporal state (FSR history, previous frame, motion) continues with no reset.
- Switching primary to any other resident Scene (instant door) is a camera cut: FSR reset allowed, no black frame.
- A Scene that is not resident and is passed as primary keeps today's behaviour (full load), so the fade fallback works.
- Resident cap: primary + 2. `release_scene()`/`set_fade()` stay.

## Slices (parallel)
1. **Server** — data authoring + generator, validation, `door_gate`, `move_zone`, crossing in `advance()`, portal
   destination route, `crossed`, linked AOI in `state()`/`snapshot()`/`project_event()`, peek/vacated, wire bound,
   protocol 18-byte destination; world_test/protocol_test.
2. **Vulkan renderer** — per-scene resource sets, budgeted streaming, per-set RT TLAS, portal view (clip distances,
   seal, destination Frame), per-set temporal history, instant switch.
3. **OpenGL renderer** — the same API: resident sets, budgeted upload, clip-distance portal view, seal.
4. **Client** — Scene ownership (stable addresses), residency driving (open portals + nearest instant door), session
   validation, `flip` mutation, eye-crossing primary swap, history transforms, ground/camera/pick through portals,
   clock/fog on both scenes, audio/minimap preload, self actor in both scenes, door open/close animation, instant-door
   swap, fade fallback.

## Acceptance
- A player clicks into the Antrepo / through the Balıkpazarı gate and walks in with no fade log, no loading image and
  continuous frames (worst frame recorded), on GL, Vulkan raster, Vulkan RT and RT+FSR3+FG; the interior is visible
  through the doorway beforehand, and players/NPCs on the far side are visible through it.
- Umut Dergisi stays shut without quest 282 and opens with it.
- A Ruh Taşı / boat use switches in one frame when resident (no black frame), fade on a miss.
- 3 validation trees, RT smoke, launcher smoke; docs; AppImage release.

## Phasing
Still deferred (see wave 15 plan).
