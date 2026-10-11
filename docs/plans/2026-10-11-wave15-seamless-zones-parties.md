# Wave 15 — seamless zone doors, connected parties (2026-10-11)

User ask: no loading screens between zones; WoW-like phasing, but party members are never cut off from each other.

## Constraints found (read-only scouting)

- Zones are not one map. The 15 base zones (71 registry rows with instance copies, 18 support grids) each occupy the same
  origin-centred ±1536-unit frame and connect only through 94 `<loc>` door NPCs (`world.cpp` door `interact`,
  `transfer()`). Stitching them into an open world would be invented content: out of scope.
- Client zone change (`client_main.cpp` `apply_pending` → `begin_loading` → `load_authoritative_scene`): random
  `loadingN.jpg`, CPU scene decode, renderer destroyed and recreated, full GPU upload. Eminönü load is 0.24–0.41 s after
  wave 12, so the remaining cost can be hidden.
- Server visibility is `World::close()` / `nearby()` / `project_event()` (same zone string + `aoi` 150 m). Party HP/position
  reach a client only through those, so a member in another zone vanishes from the party frames. `group_invite` /
  `group_accept` require `close()`, so the chat-menu "Gruba Davet Et" fails for anyone outside the AOI.
- Per-group zone copies (`instance()`) already keep a party in one copy of savas/migrat/siginak/cemberli.

## Phase 1 — seamless door transitions (client; one server field)

1. Server: door entities in `World::state()` carry `destination`, the destination zone's `source_registry_id`.
2. Client: while a door entity is within view, decode its destination scene on a worker (existing `load_cpu` /
   resident-texture dedup) and keep one prefetched scene. Instance copies whose descriptor content equals the prefetched
   one reuse it.
3. Client: a zone change during play no longer shows `loadingN.jpg`. It fades the frame out, swaps the scene (prefetched
   or freshly decoded), uploads, attaches actors and fades in. Keep the GPU renderer alive across the swap where the
   renderer allows it. The login/character-select first load keeps the original loading screen.
4. Both renderers (GL, Vulkan raster, Vulkan RT) must survive repeated swaps without leaks or validation errors.

Acceptance: walking through a door on GL and Vulkan(+RT) shows no loading image; transition time and the worst frame are
measured with and without a prefetch hit; tests pass in the 3 validation trees.

## Phase 2 — connected parties (server + client UI)

1. `group_invite` / `group_accept` work for any online character (the chat context menu already offers the invite).
2. Every state frame carries the status of the other party members wherever they are (id, name, level, class, hp/max,
   mana/max, zone, alive). Party frames and the Grup window use it when the member is not a visible entity, and show the
   member's zone when it differs.
3. Party rule for any isolation mechanism (instance copies now, phases later): a party resolves to its leader's copy.
   `instance()` already does this on door entry. No forced moves of members.

Acceptance: `world-state` regressions for a cross-zone invite/accept and cross-zone member status; a two-client or
bot smoke shows a member in another zone in the party frame with zone text.

## Phase 3 — phasing (deferred, not in this wave)

No quest in `quests_original.json` changes the world, so no phase content exists yet. Candidates when it is wanted:
per-player rescue NPCs (a freed Cin currently disappears for everyone), per-player `required_death` doors, and
crowd layering. Design when started: one `visible(observer, target)` predicate replacing `close()`'s zone test and the
~15 inline `zone ==` checks; a party always uses its leader's phase; invites exempt.

## Release

Validation trees `build-full`, `build-gl-only`, `build-full-sanitized`; RT smoke; signed launcher smoke;
`docs/PROGRESS.md` / `docs/KNOWN_ISSUES.md`; AppImage + GitHub release.
