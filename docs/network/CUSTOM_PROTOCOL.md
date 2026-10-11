# Custom v2 application protocol (state/action JSON)

## Scope and provenance

- Independent wire for the native client (`src/client_session.cpp`) and world server (`src/server_main.cpp`, `src/world.cpp`). It is **not** the original İKV protocol and not an original opcode reconstruction (see [PACKET_CATALOG.md](PACKET_CATALOG.md) for original evidence).
- Transport, framing, binary message layouts, decoder poisoning and EOF rules: [PROTOCOL_SPECIFICATION.md](PROTOCOL_SPECIFICATION.md#independent-authenticated-custom-wire-version-2). Connection state machine: [MESSAGE_STATE_MACHINE.md](MESSAGE_STATE_MACHINE.md). This file documents the JSON carried by `state` (type 6) and `action` (type 8) as implemented now.
- TLS ≥ 1.2 on both ends, no compression; server also disables renegotiation (`src/server_main.cpp:376-380`). Client verifies the chain against the configured CA plus host/IP SAN (`src/client_session.cpp:426-469`).
- Every gameplay rule and number below is independent. `config/gameplay.json` is NONCANONICAL as a whole (`src/world.cpp:97` requires the word in `provenance`); rules whose code comments say NONCANONICAL are marked so.
- Coordinates are integer "wire units": 1000 per original scene unit, range ±1,536,000 (`include/ikv/protocol.hpp:15`).

## Session flow

| Step | Behaviour | Code |
| --- | --- | --- |
| Connect | TLS accept; a peer without a player id 5 s after accept is closed (`handshake_timeout`). Max 64 connections. | `src/server_main.cpp:39,434,469` |
| hello | Must be the first message (`handshake_state` otherwise). `session` = 64 lowercase hex, `character_id` ∈ [1, INT64_MAX]. | `src/server_main.cpp:285-286`, `src/protocol.cpp:42-47` |
| Admission | Joined players < 32 and id space left (`player_limit`); `Store::authenticate`, `Store::enter_character` (lease), `World::add` (character validation, `src/world.cpp:681-795`). Store failure → `authentication_or_store`; World rejection → `protocol`. | `src/server_main.cpp:287-296,316-317` |
| welcome | `{id, tick}`; id ∈ 1..0x7fffffff, then the first `state` is sent immediately (always with `catalogs`). | `src/server_main.cpp:297-300` |
| Tick stream | Every 50 ms tick (≤ 4 catch-up steps per loop) each joined peer gets `state` then `snapshot`. | `src/server_main.cpp:493-508` |
| input / destination | Sequence must strictly increase per connection, else `sequence` close; World also ignores non-increasing sequences. | `src/server_main.cpp:301-305`, `src/world.cpp:804-819` |
| action | ≤ 40 per 1-second window per connection, else `action_rate` close. Each action is handled by `World::action`, then an extra `state` is sent immediately (several states may share one tick). | `src/server_main.cpp:306-311` |
| Heartbeat | Every 1 s: session re-authenticated (`session_revoked`/`identity_changed` close), external mute refreshed. | `src/server_main.cpp:518-530` |
| goodbye | Client: queued on `stop()`, then 2 s to drain (`src/client_session.cpp:702-711`). Server: graceful close `goodbye`. | `src/server_main.cpp:312-313` |
| Kick | A state containing a `kick` event is queued, then graceful close `admin_kick`. | `src/server_main.cpp:255-260` |

There is **no server→client close message**. The client sees TLS `close_notify`/EOF. Close reasons are server log strings only (`peer=N close=<reason>`, `src/server_main.cpp:119`):

| Kind | Reasons |
| --- | --- |
| Graceful: drain queued output, TLS shutdown, 2 s deadline (`close_timeout`) | `goodbye`, `admin_kick`, `eof`, `server_stop` |
| Immediate | `handshake_timeout`, `tls_handshake`, `handshake_state`, `player_limit`, `authentication_or_store`, `protocol`, `json`, `world_operation`, `joined_state` (any other type after join, e.g. a second hello), `sequence`, `action_rate`, `output_limit`, `truncated_protocol`, `tls_read`, `tls_write`, `tls_shutdown`, `socket`, `socket_hangup`, `snapshot`, `session_revoked`, `identity_changed` |

Client-side: 5 s read silence fails the session; outgoing queue ≤ 256 KiB and ≤ 128 frames (`src/client_session.cpp:32-33,412-417,472-479`). The native client sends movement intent as `destination` = own snapshot position ± 100 per axis, clamped to zone bounds (`src/client_session.cpp:686-701`).

## Message types

| Type | Dir | Payload | Notes |
| --- | --- | --- | --- |
| `hello` 1 | C→S | `session`, `character_id` | Only once, first. |
| `welcome` 2 | S→C | `id`, `tick` | Client requires it before anything else (`src/client_session.cpp:535-538`). |
| `input` 3 | C→S | `sequence`, `dx`, `dy` | Cardinal only; `(0,0)` stops. Diagonal fails decode → `protocol` close (`World::input` would also reject it as `movement`, `src/world.cpp:808`). Moving clears auto-attack. |
| `snapshot` 4 | S→C | `tick`, `players[{id,x,y}]` | Self plus at most 31 AOI players (same zone, within `aoi`; group members first, then nearest — the same set as the state's player entities), ID-sorted (`World::snapshot`). Server-side bots are ordinary player entities. |
| `goodbye` 5 | C→S | — | |
| `state` 6 | S→C | JSON object, below | |
| `destination` 7 | C→S | `sequence`, `x`, `y` | Routed by `World::route` (direct segment, or bounded A* in source zones); unroutable → `correction{reason:"blocked_destination"}` (`src/world.cpp:812-819`). |
| `action` 8 | C→S | JSON object `{ "action": NAME, ...fields }` | Table below. |

## State object (`World::state`, `src/world.cpp:1555-1666`)

| Field | Type | Meaning |
| --- | --- | --- |
| `self` | uint32 | Own player id. |
| `tick` | uint32 | World tick. |
| `zone` | string | Current zone key. |
| `bounds` | `[minx,miny,maxx,maxy]` | Zone bounds. Source zones are fixed at `[-1536000,-1536000,1534500,1534500]` (`src/world.cpp:127`). |
| `source_registry_id` | int 0..70 \| null | Original SourceRegistry row of the zone. |
| `character` | object | `name`, `class_id` (1 warrior, 2 mage, 3 healer), `appearance`, `state` (below), `equipment` = visible equipment array `[{slot (source slot 0..15), source_uid, prefixes?}]` (weapon prefixes only while durability > 0; `src/world.cpp:389-401`). |
| `entities` | array | Other players and NPCs in AOI (below). |
| `events` | array | Drained event queue (below). |
| `catalogs` | object | `items{id:def}`, `quests{id:def}` (active quests and the offers of included NPCs), `skills{id:def}` (own class only), `recipes{scroll_id:def}` (learned only). Item catalog = inventory, quest rewards, learned recipe products/components, open-bank items, plus shop/loot items of included NPCs. Item defs are the config entries: `price` (Lira), `clan_price?` (classes.dat pvpprice, Klan Parası), `damage_range? [low,high]` (classes.dat damagelow/high, hundredths), `attack_type?` (K/D/E letters: Kesici/Delici/Ezici), `armor?` (dmgreduction in basis points), `crit? [chance %, multiplier]`, `attack_cooldown?`, `heal?`/`mana?`. **Omitted when unchanged** (below). |
| `xp_interval` | int | XP needed to leave the current level = `round(formulas.xp_first × formulas.xp_growth^(level−1))` (VOD-fitted, 1500 at level 1), capped at 10,000,000 (reached from level 58 up; the client accepts 1..10,000,000); `character.state.xp` is the progress inside that interval. |
| `xp_total` | int | Cumulative XP of all completed levels; the Karakter Bilgisi "Puan" shows `xp_total + xp` over `xp_total + xp_interval` (VOD 18:30 1811/3183). |
| `skill_points` | int ≥ 0 | Unspent skill points = `skill_points_base + skill_points_per_level×(level−1) − Σranks` (NONCANONICAL, `src/world.cpp:75-79`). |
| `contacts_online` | `[character_id]` | Friends currently in this world. |
| `stats` | object | Karakter Bilgisi values from the original 00402f60 formulas over `formulas` (`World::fighter`): `attack`, `defense` (Saldırı/Savunma in tenths), `attack_cooldown` (ticks), `weapon_damage [min,max]` (Silah Hasarı incl. Efsun element maxima), `armor [Zırh % in tenths, cap %]`, `crit [chance in tenths of %, multiplier]`, `efsun {attribute_id: sum×100}`, `charisma`, `resistance_points`, `craft_points` (unspent pools). NONCANONICAL, optional: `clan_rank` (clan members: the last `social.clan_ranks` row whose Şan threshold is reached), `guild_rank` (`Lonca Reisi` or `Üye`), `arena_power` (skill amount bonus % from the Er Meydanı capture areas the own clan holds in the current arena; absent when 0). The client shows the ranks right of the name in Karakter Bilgisi and in Klan ve Lonca, and `arena_power` as a "Meydan gücü +%N" tag under the own effect row. |
| `clock` | object, optional | Present when config has `clock`. `hour` ∈ [0,24) = `start_hour + tick/72000×rate` mod 24; `environment` = 11 bytes linearly blended between hourly records: ambient RGB, sun RGB, horizon RGB, shadow, glow (`src/world.cpp:109-118,1570-1579`); `fog` = `[start, end]` = config `clock.fog` (original `thgf` 004a4680 float params 6/7, client record +0xec/+0xf0; the client draws fog from start·view to end·view+110 and hides emitters beyond √0.5·end·view, `0046b630`/`0046b660`/`00478586`). NONCANONICAL values; shape follows original `thgf` hour + byte record + fog distances. |
| `bank` | object, optional | Only while a banker session is open and the banker is within `interaction_range`: `items[{uid,item,quantity,prefixes?,durability?}]`, `currency`, `slots`, `banker` (NPC id) (`src/world.cpp:1609-1612`). |

### `character.state`

| Field | Meaning |
| --- | --- |
| `zone`, `x`, `y` | Position. |
| `hp`, `max_hp`, `mana`, `max_mana` | Resources; maxima ≤ `resource_cap`. |
| `level` (1..100), `xp`, `currency` | Progress; currency ≤ `currency_cap`. |
| `inventory` | `[{uid, item, quantity, prefixes?[≤2], durability?}]`; `uid` = 32 lowercase hex (OpenSSL random, `src/world.cpp:44-51`). |
| `equipment` | `{slot: uid}`, ≤ 8 entries, each uid owned. |
| `quests` | Active quests `{quest_id: {progress[, deadline]}}` (deadline = world tick of a timed quest; restarted at admission), ≤ 20 accepted (`quest_active_max`). |
| `quests_done` | Sorted array of completed quest ids (legacy `{completed:true}` entries migrate here at admission; retired ids drop). |
| `skills` | `{skill_id: rank}`, rank 1..`skill_rank_max`. |
| `resistances` | `{ates\|buz\|elektrik\|asit\|zehir: points}`. |
| `crafts` | `{silahtar\|zirhci\|kimyaci\|madenci\|sarraf\|lokman: points}`. |
| `recipes` | Learned recipe scroll ids, ≤ `economy.recipe_limit`. |
| `buffs` | `[{id, skill_source, expires, armor, attack?, regen?, hot?, speed?, power?, absorb?, immune?, wrath?, stance?}]`, ≤ 8; cleared on admission; `skill_source` = the skills.dat [classindex, skillno] of the casting skill (effect icon). Harmful PvP skill effects (`hostile_skill`) add `{id, source, skill_source, expires, armor:0, harmful:true, stun?, silence?, speed?, vulnerable?, armor_break?, dot?, drain?, next?}`: `stun` stops movement, `silence` blocks skills, `dot` strikes and `drain` takes that much Kudret every 60 ticks for its living caster, `armor_break` cuts Zırh by that percent. Other players' entities and NPCs carry the same list as `debuffs` `[{id, expires, skill_source?}]` (target/party effect icons). |
| `guild` (optional/null) | `{id, name, leader_character_id, members[character_id], revision, level?, funds?}`: `level` 1..100 (absent = 1; "Lonca : <%s><%d>", gates `guild_level` goods), `funds` 0..999,000,000 Lonca Parası from `guild_donate` (NONCANONICAL progression). |
| `clan`, `san`, `clan_currency` (optional) | Clan 1..15; Şan ≤ 1e9; Klan Parası ≤ 999,000,000. An enemy-clan kill pays the killer the guide Şan (1, 10 or 100 by the 3000-Şan thresholds of both players, × both Karizmas/10000, at least 1 unless a Karizma is 0) and the same Klan Parası. |
| `karizma`, `karizma_reset` (optional) | Guide Karizma 0..100 (absent = `character_points.charisma`, 100); −20 per enemy-clan death; at 0, `karizma_reset` (Unix seconds) restores 100 two hours later. Shown as `stats.charisma`. |
| `lockouts` (optional) | `{group_zone: unix_expiry}` ≤ 32: kill credit on a Şef (`boss` NPC) inside a group-zone copy bars that zone's doors until expiry (`activities.lockout` seconds, NONCANONICAL). |
| `kabadayi` (optional) | `true` in Kabadayı mode; removed on death and on admission (`src/world.cpp:619,784`). |
| `contacts` (optional) | `{friends: [[character_id, name]], ignored: [[character_id, name]]}`, each ≤ `social.contacts_max`. |
| `quickbar` (optional) | Hızlı Erişim Çubuğu: exactly 10 slots, each `null`, `{skill}` or `{item: uid}` (original `tdbp` 0044b330 / hcpe 0044b400). Absent until the first drop; unknown skills load as `null`. |
| `boz_ayi` (optional) | `true` while the Savaşçı's slot 8 holds Boz Ayı instead of Kanatma (exe 0044ccf0 flag; NONCANONICAL player toggle). |

### Entities

Player (`src/world.cpp:1628-1637`): `id`, `kind:"player"`, `name`, `appearance`, `class_id`, `equipment` (visible array), `x`, `y`, `hp`, `max_hp`, `level`, `alive`, `clan` (0 = none), `kabadayi`, `guild` (guild **name**, only if in a guild), `posture` (optional 1..255: held RP clip from `social.postures` — sit/lay — until the player moves, fights or dies; late joiners play it).

NPC (`src/world.cpp:1640-1651`): `id` (> 0x7fffffff), `kind` ∈ `enemy|merchant|questgiver|node` (gathering node) `|anvil` (örs) `|door` (classes.dat `<loc>` door/crystal/barrier/elevator; client shows `cursor_interact`, a click sends `interact`) `|summon` (Boz Ayı/Tesla Küresi/Çağrı, ids from 0xf0000000, carries `master` = caster id; friendly, not attackable), `name`, `appearance` (`{}` default), `equipment:[]`, `source_uid`, `source_class`, `x`, `y`, `hp`, `max_hp`, `alive`, `quests`, `shop`, `owner` (loot claimant, 0 = none), `looted`, `clan` (clan NPCs: recruiters, clan merchants, Er Meydanı guardians and banners; relation 00478090/00478050 makes one hostile to a player of the other nonzero clan: the client gives it the attack cursor, attacks it on click and offers no İşlemler items), `recruiter:true` (classes.dat claninviter, entity flag 0x20000: İşlemler Klana Katıl/Klandan Ayrıl), `banker:true` (banker only; the configured banker is a `questgiver`). A state carries at most 96 entities (nearest NPCs first). An instance copy's state reports `zone` `<zone><n>` and that copy's `source_registry_id` (e.g. `migrat3` → 29).

NPC kind `trap` (Labirent Tuzağı, config `trap {range, damage, cooldown}`): not attackable or interactable; every `cooldown` ticks it strikes each living player of its zone within `range` (`combat` event with the trap as `source`). Door `interact` additionally refuses (`door`) while `door.required_death` (classes.dat requiredentitydeathid, a classid) names a living NPC in the door's zone copy.

Packing order: every queued event's bytes are reserved first (the queue is delivered once and then cleared, while entities are resent every frame); group members always; other players nearest-first while they fit; then NPCs nearest-first, each only if it plus the catalog items it introduces fits the 65,533-byte budget. Events: all critical events are kept, then the newest noncritical events that fit (all of them unless group members alone overflow); kept events stay in queue order (`World::state` in `src/world.cpp`).

### Catalogs-on-change rule

- Server keeps the last sent `catalogs` dump per peer (`Peer::catalogs`) and erases `catalogs` from a state whose dump is byte-identical; any change resends the whole object (`src/server_main.cpp:111,245-254`). The first state after welcome always carries it.
- Client: present `catalogs` must be an object; absent `catalogs` on the first state, or with no previous copy, is `invalid server message`; otherwise the previous copy is carried forward into the accepted state (`src/client_session.cpp:567-571`).

### Client acceptance checks (`validate_state`, `src/client_session.cpp:197-345`; `receive`, `:533-579`)

- `self` equals welcome id; `tick` ≥ last welcome/snapshot tick and ≥ previous state tick.
- Within one zone, `bounds` and `source_registry_id` must not change; source zones must use the fixed bounds; `character.state.zone == zone`.
- Self and every entity position inside `bounds`; `hp ≤ max_hp`; entity `alive == (hp > 0)`; entity ids unique and ≠ self; `kind` must be `player` or one of the NPC kinds above; player `class_id` 1..3; NPC `source_uid` 1..UINT32_MAX, `source_class` 1..65535.
- Inventory uids 32 lowercase hex and unique; equipment uids owned; visual equipment slot 0..15 unique; prefixes ≤ 2, ids 1..999999.
- `clock.hour` ∈ [0,24), `environment` exactly 11 values 0..255, `fog` two numbers 0 ≤ start ≤ end ≤ 1e6; `stats.crit` multiplier 1..10.
- Events with type `combat|skill|death|resurrect|equip|unequip` need `entity`, `source`, `target` ∈ 1..UINT32_MAX; `correction` coordinates inside bounds; `skill` needs `skill` text; `equip`/`unequip` need valid `equipment`.
- Snapshot: only after a state; tick > last tick and ≥ state tick; 1..32 players, ascending ids, contains self, inside bounds. When snapshot tick equals state tick, ids/positions must equal self + player entities exactly (`src/client_session.cpp:346-365`).

## Actions (`World::handle`, `src/world.cpp:1054-1428`; `World::social`, `:1006-1035`; `World::economy`, `:1675-1746`)

Envelope: JSON object ≤ 8,192 dumped bytes; `action` string 1..32 bytes (`src/world.cpp:1055-1057`). While dead only `resurrect`, `chat`, `trade_cancel`, `group_leave`, `quickbar` are accepted (`dead`) (`:1058`). A kicked actor gets `permission`. Unknown name → `action`. Missing/mistyped field → `request` ("Malformed action fields", `:1050`). Integer out of range → `quantity`; entity id not in 1..UINT32_MAX → `target`. Text fields are 1..N bytes without NUL (`:34-39`). Persistence failure → `persistence`, state unchanged (`:563-575`).

Common NPC check `npc()`: exists (`target`), within `interaction_range` (`range`), alive (`dead`), right kind (`target`) (`src/world.cpp:557-562`).

"ack" = `{type:"ack", action}` to the sender after a successful commit.

| Action | Fields | Effect / key rejections (code) | Reply |
| --- | --- | --- | --- |
| `interact` | `target` | Non-enemy NPC beyond `interaction_range` but in `aoi` → walk there, then interact. In range: anvil → `repair`; door → gates (`door.gmonly` → `permission`; `door.required_quest` = original requiredquestid whose config quest (`source_quest_id`) is neither taken nor done → `quest`), then transfer to `door.to`/`door.arrive` (a grouped player entering an instanced zone goes to the group's copy `<zone><n>`, else a free copy; solo players stay in the shared zone); others → dialogue. Enemy → `target`. | ack (walk/door) / `dialogue` |
| `quest_accept` | `target`, `quest` | Quest in the NPC's offer (`quests_at`: its catalog; an `auto_quests` NPC (GB-Anı) offers deleted auto quests whose prerequisite is done) (`quest`); non-auto: NPC is the giver (or its instance copy, id + n·65536); auto: NPC has `auto_quests` (`quest`); not active nor done (`quest`); level (`level`); `requires` done (`quest`); < 20 active (`quest`). Talk quests start at progress 1; timed ones get `deadline`. | ack + `quest` event `accepted` |
| `quest_complete` | `target`, `quest` | Offered here and NPC is `turn_in` (default giver) (`quest`); active with progress ≥ count (talk: 1) (`quest`); collect: carried items handed over (`quest`). Grants XP (halved if level > quest level+5), currency, items; moves the id to `quests_done`; grants every `auto` quest that `requires` it. | ack + `quest` events `completed` (+`text`), `accepted` |
| `quest_abandon` | `quest` | Active quest removed (`quest`). | ack |
| `stat_points` | `points` {stat: 1..1000} (≤ 11 stats) | Every stat in the resistance or craft list (`stat`); the committed totals stay within each unspent pool (`points`). Karakter Bilgisi ✓ (00402a80, 'targ'/'cnch'): −/+ only move client-pending points; invested points are permanent (no refund; only `point_reset`). Pools NONCANONICAL. | ack |
| `attack` | `target` | Not `immune` (`state`). Player target must be hostile: different nonzero clans or both Kabadayı, same zone, alive, outside `social.safe_zones` (`target`); NPC must be a living enemy or a Klan OOK guard (non-enemy NPC with `clan` and combat stats) of another clan than the sender's nonzero clan (`target`/`dead`); within `aoi` (`range`). Starts auto-attack. A guard bites only players of the other nonzero clan; a felled guard leaves no Ganimet and refuses `interact` until it respawns (`dead`). | ack |
| `skill_learn` | `skills` `{skill: points}` (1–16 skills, 1–15 points each; the Yetenekler ✓ batch) | For every skill: known (`skill`), own class (`class`), level (`level`), slot holds this variant (`variant`), resulting rank ≤ max (`rank`); total ≤ unspent points (`points`). All-or-nothing. A variant (`replaces`, Boz Ayı) invests in its replaced skill's rank. | ack |
| `skill` | `skill`, `target` (enemy/ally skills; optional for summons) | Rank ≥ 1 (`rank`), slot holds this variant (`variant`), not passive (`passive`), not immune or `silence`d (`state`), required stance (`requires`), Gazap heal block (`wrath`), cooldown + 20-tick global (`cooldown`), mana (`resource`), range (`range`), target validity (`target`/`dead`), buff cap 8 (`buff`). `enemy`/`enemies` reach living enemy NPCs and players hostile under `attack`'s rule (damage via `strike`, timed effects as harmful buffs). `summon` spawns the configured classes.dat template at the caster (one per skill); a Boz Ayı recast while it lives costs no mana or cooldown and sends it at `target` (enemy NPC) or beside the caster. Detail-text `procs` roll: `free` (no mana), `stun`/`silence`, `multiply` (vs `vulnerable` NPCs), passive `dot`. | ack + `skill`/`hit`/`resurrect` |
| `skill_variant` | `skill` (a skill with `replaces`) | Own class (`class`), level (`level`); toggles the `boz_ayi`-style flag. NONCANONICAL (official grant rule unrecovered). | ack |
| `quickbar` | `slots` (10 × `null`\|`{skill}`\|`{item: 32-hex uid}`) | Exactly ten valid slots (`quickbar`); stored in `character.state.quickbar`. | ack |
| `resurrect` | — | Dead (`state`), `death_resurrect_delay` elapsed (`cooldown`), resurrection point walkable (`collision`); full HP/mana at the nearest of the zone spawn and the zone's Ruh Taşı arrivals, or for a clan member its clan's nearest standing banner (the death itself took the `death_fee`). | ack + `resurrect` |
| `loot` | `target` | Dead, unlooted, owned by sender (`ownership`); beyond `interaction_range` but in `aoi` → walk then open. Opening (original `btlr`→`sdbt`) grants the box currency (NONCANONICAL timing) and lists the Ganimet slots (loot table, rolled drop). | ack + `loot_open` |
| `loot_take` | `target`, `slot` | As `loot` but within `interaction_range` (`range`); slot holds an item (`slot`); bag space (`inventory`). The last taken slot marks the corpse looted. Original `fatl`. | ack + `loot` |
| `repair` | `target` | Anvil NPC; no active trade (`trade`); something worn down (`repair`); cost NONCANONICAL (`item_rules.repair_denominator`). | ack + `repair` |
| `point_reset` | `target`, `price` | Reset-machine NPC (`kind` `reset`) in range; no active trade (`trade`); GBM 0-B (`reset` `crafts`) needs Plus Paket (`plus`); `price` equals the current offer (`price`); something to reset (`reset`); enough Lira (`currency`). `skills` (GBM-0) clears `skills` and `resistances`, `crafts` (GBM 0-B) clears `crafts`. Price NONCANONICAL: `point_reset.<kind>` × level, Plus Paket ÷ 3 (guide). `interact` on the machine sends `reset_offer`. | ack + `point_reset` |
| `equip` | `uid` | No trade (`trade`); owned (`ownership`); level (`level`), has slot (`equipment`), class (`class`), sex (`sex`), each refused with the original "Bu eşyayı giyemezsiniz."; ≤ 8 slots (`equipment`). | ack + `equip` |
| `unequip` | `slot` | No trade; slot occupied (`equipment`). | ack + `unequip` |
| `use` | `uid` | Recipe scroll → learn recipe: not known, memory not full (`recipe`), scroll consumed. Otherwise: no trade; level; item has heal/mana (`item`); not already full (`resource`); consumes one. | ack |
| `split` | `uid`, `quantity` | Original `mtes` after the Shift-click amount box (0042f690/004336c0). No trade ("Ticaret esnasında bunu yapamazsınız.", `trade`); quantity < stack (`quantity`); free slot ("Çantanızda yer yok!!", `inventory`). | ack |
| `item_move` | `uid`, `to` (grid cell) | Bag reorder (drag onto another bag cell): the dragged unworn cell swaps with cell `to` of the unworn cells; worn cells refuse (`equipped`, "Giyilenler penceresi içerisinde taşıma yapamazsınız."). NONCANONICAL swap. | ack |
| `item_destroy` | `uid` | Original `spro` after the "Bu eşya kalıcı olarak yok edilecektir. Emin misiniz?" confirm (a cell dropped outside every window, 004344b0). No trade (`trade`); owned; not worn ("Giyilmekte olan nesneler yok edilemez.", `equipped`); item `flags` bit 2 refuses ("Bu eşya yok edilemez.", `destroy`, NONCANONICAL for every account). Removes the whole cell. | ack |
| `buy` | `target`, `item`, `quantity` 1..1000 | Merchant; no trade; item in shop (`item`); then the client 00431700 order: Lira `price` (`currency`), Klan Parası `clan_price` (`clan_currency`), lonca membership and `guild.level` ≥ item `guild_level` (`guild`), Akçe ≥ trunc(`akce_price`/5) (`akce`); bag space (`inventory`). Deducts exactly those amounts. | ack |
| `sell` | `target`, `uid`, `quantity` 1..1000 | Original `llps` after the 0x591d60 confirm (a cell dropped on the Satıcı window). Merchant; no trade; owned; then 004344b0's order: `flags` bit 2 ("Bu eşya satılamaz.", `sell`), no `price` and no `clan_price` (0x591eb8, `sell`), `clan_price` without `flags` bit 4 or with bits 1/2 (0x591e08, `sell`), worn (0x591c98, `equipped`); enough (`quantity`). Pays price×qty/5, or /3 for a Plus Paket account (`accounts.plus`); Lira-less Klan Parası goods pay clan_price×qty/5 (/3) in Klan Parası (NONCANONICAL). | ack |
| `trade_request` | `target` | Online, ≠ self, not kicked, alive (`trade`); within `trade_range` (`range`); neither trading (`trade`); fresh revision (`trade_revision`). | ack + `trade` (both) + `trade_invite` (target) |
| `trade_accept` | `revision` | Active trade, both alive and in range (`trade`); `revision` present, positive, equal to current (`trade_revision`). Invited phase: only recipient opens it. Open phase: sets own confirmation; second confirmation revalidates both offers and commits both characters in one transaction. | `trade` / `trade_complete`; failure `error{code:"trade_persistence",peer,revision}` |
| `trade_offer` | `items[{uid,quantity}]`, `currency` | Open phase only (`trade`); ≤ 32 items, unique uids (`trade`), owned, quantity 1..1000 ≤ owned (`quantity`), not equipped (0x591bf0, `equipped`), not bound: item `flags` bits 2/4 except source uid 14000 ("Bu eşya size bağlanmıştır. Kimseye veremezsiniz.", `bound`, 00431220); currency ≤ own (`currency`). New revision, clears both confirmations. | `trade` (both) |
| `trade_cancel` | — | Active trade (`trade`). | ack + `trade_cancel` (both) |
| `chat` | `channel`, `text` ≤ 512 B or `link` (uid), `to` (private) | Not muted (`muted`); one message/emote per 10 ticks (`rate`); no control chars except tab (`chat`). Channels: `local`, `roleplay` (AOI), `zone`, `private` (`to` online; echoed to sender), `group` (`group`), `guild` (`guild`), `clan` (`clan`); else `chat`. `zone` (Bölge) and `guild` (Lonca) need Plus Paket (`plus`, guide; refusal wording NONCANONICAL). `link`: Ctrl-click item link (0042e8c0) of the sender's own bag/worn entry (`ownership`), not on `roleplay`; delivered as `link {item, definition, prefixes?}`. `local`/`roleplay` text `/name` = emote from `social.emotes` (`emote`, `dead`). Listeners ignoring the speaker receive nothing. | `chat` / `emote` |
| `group_invite` | `target` | Online, ≠ self, ungrouped (`group`); within `aoi` (`range`); sender leader or ungrouped, group < 6 (`group`). | ack + `group_invite` |
| `group_accept` | — | Pending invite, ungrouped (`group`); inviter in `aoi` (`range`) and still leader; group < 6 (`group`). | `group` (members) |
| `group_leave` | — | Always succeeds; first remaining member becomes leader; a group left with one member dissolves. | ack + `group` |
| `group_kick` | `target` | Sender leads a group containing `target` ≠ self (`group`). Original İşlemler "Gruptan Çıkar". | ack + `group` |
| `complain` | `target` | Online player ≠ self (`target`); at most 3 per session, 10 for a Plus Paket account (guide; NONCANONICAL per-session count, `complain`). Original "Şikayet Et" (`lpmc`); NONCANONICAL delivery to online administrators. | ack; admins get `complaint` |
| `teleport_here` | `target` | Admin only (`permission`); online target ≠ self. Moves the target to the administrator's zone/position (original GM "Yanına Işınla", `|tele`). | ack |
| `guild_create` | `name` 2..32 B | Not in a guild (`guild`). | ack + `guild` |
| `guild_invite` | `target` | Sender leader; target unguilded; < 64 members (`guild`). | ack + `guild_invite` |
| `guild_accept` | — | Pending invite; inviting leader online; < 64 (`guild`). | ack + `guild` |
| `guild_leave` | — | Member (`guild`); leader passes to first remaining member. | ack + `guild` |
| `guild_kick` | `target` | Sender leader; target member (`guild`). | ack + `guild` |
| `guild_donate` | `amount` 1..999,000,000 | Member (`guild`); Klan Parası ≥ amount (`clan_currency`); guild level < 100 (`guild`). Moves the amount into the guild's `funds` (Lonca Parası); level L rises to L+1 while funds ≥ L × `economy.guild_level_cost` (NONCANONICAL). Client: Klan ve Lonca "Bağış yap" with the amount typed on the chat line. | ack + `guild` |
| `contact_add` | `list` (`friends`\|`ignored`), `target` | Online player ≠ self (`target`); moves entry to that list; list < `contacts_max` (`contacts`). | ack |
| `contact_remove` | `list`, `character_id` | Entry on that list (`contacts`). | ack |
| `clan_join` | `target` | Clan recruiter NPC (`recruiter`, `target`); clanless (`clan`); level ≥ 5 (`level`, guide). | ack |
| `clan_leave` | `target` | Own clan's recruiter (`clan`); clears `clan`, `san`, `clan_currency`. | ack |
| `kabadayi` | — | Not already Kabadayı (`kabadayi`). | ack |
| `arena_list` | `stake` (bool) | — (`request` if not boolean). Chat `/em` (`stake:false`) and `/kpem` (`stake:true`) map here (guide). | `arena` with `arenas[{code, players}]` |
| `arena_join` | `code` 1..20, `stake` 0..999,000,000 | Alive (`dead`); in a clan (`clan`); not already in an arena; existing code (`arena`); a running match of the other kind refuses (`arena`); enough Klan Parası for the stake (`clan_currency`). Enters Er Meydanı `code` (zone `activities.arena.zone`, its copies for 2..20) at the clan's first standing banner. Chat `/savas N` = stake 0. | ack + `arena` to the arena |
| `arena_stake` | `code`, `stake` > 0 | Every `arena_join` check, without entering. Chat `/kpsavas N [miktar]` (stake default `activities.arena.stake`) maps here; the client shows the event text in a Dikkat box and sends `arena_join {code, stake}` on Evet (NONCANONICAL: no original stake prompt strings). | `arena_stake` |
| `arena_leave` | — | Inside an arena (`arena`); returns to the clan headquarters (`activities.arena.homes`); the stake stays in the pot. | ack |
| `craft` | `recipe` | Learned (`recipe`); profession points ≥ recipe level (`level`); components in bag (`components`); always succeeds (NONCANONICAL). | `craft` |
| `recipe_forget` | `recipe` | Learned (`recipe`). | ack |
| `gather` | `target` | Living node (`target`/`exhausted`); NPC check; profession level (`level`). Completes after `economy.gather_ticks`; moving/attacking/leaving range cancels; node exhausts for everyone. | `gather` start/done/cancel |
| `bank_open` | `target` | NPC check; `banker` flag (`target`). | ack |
| `bank_close` | — | — | ack |
| `bank_deposit` | `uid`, `quantity` 1..1000 | Open banker in range (`bank`); owned, not worn ("Giyilen bir eşyayı çıkarmadan bankaya koyamazsınız!", `equipped`); free bank slot (`bank`). | ack |
| `bank_withdraw` | `uid`, `quantity` 1..1000 | Open banker; uid in bank (`ownership`); enough (`quantity`); instance items move whole (`quantity`); bag space (`inventory`). | ack |
| `bank_currency` | `amount` ≠ 0, ±`currency_cap` | Open banker; positive deposits, negative withdraws; balances within 0..cap (`currency`). NONCANONICAL. | ack |
| `bank_destroy` | `uid` | Open banker; uid in bank (`ownership`); `flags` bit 2 (`destroy`). A bank cell dropped outside every window (004344b0 destroy confirm). Other `bank_*` names → `request`. | ack |
| `zone` | `zone` | Admin only (`permission`); known zone (`zone`); walkable spawn (`collision`). Debug teleport; players change zones by using `<loc>` door NPCs (`interact`). | ack |
| `moderate` | `target`, `operation` (`mute`\|`unmute`\|`kick`) | Admin only, not self (`permission`); unknown operation → `request`. Kick freezes the actor and cancels its trade. | ack; target gets `moderation`/`kick` |

Bank commits write character and bank in one row update (`src/world.cpp:1719-1745`).

## Events

Delivery: **self** = sender only; **AOI** = every player in the same zone within `aoi` of the acting entity, including self (`World::project_event`, `src/world.cpp:378-388`).

| Type | Fields | Delivery | Source |
| --- | --- | --- | --- |
| `ack` | `action` | self | `src/world.cpp:1060,1677` |
| `error` | `code`, `message` (+ `peer`, `revision` for `trade_persistence`) | self | `:402-406,1048` |
| `correction` | `x`, `y`, `reason` (`blocked_destination`\|`collision`) | self | `:817,1459` |
| `dialogue` | `target`, `text`, `quests` (offer for this player, see `quest_accept`), `shop`, `bank` | self | `World::handle` interact |
| `quest` | `quest`, `stage` (`accepted`\|`progress`\|`ready`\|`completed`\|`expired`), `name`, `level`, `progress`, `count`, `text` (completed, optional) | self | quest block (`World::quest_*`); kills credit every rewarded group member |
| `trade` | `peer`, `peer_name`, `initiator`, `phase` (`invited`\|`open`), `revision`, `own`/`other` `{items[{uid,item,name,quantity}], currency, accepted}` | both parties | `:826-852` |
| `trade_invite` | `from`, `phase`, `revision` | recipient | `:1279` |
| `trade_cancel` | — | both parties | `:824` |
| `trade_complete` | `peer` | both parties | `:1316` |
| `group` | `leader` (0 = none), `members` | members | `:872-874,1409` |
| `group_invite` | `from`, `name` | target | `group_invite` action |
| `guild` | `guild` (object or null for the departing member) | affected online characters | `:1370` |
| `guild_invite` | `from`, `name`, `guild` | target | `guild_invite` action |
| `complaint` | `from`, `name`, `target`, `target_name` | online administrators | `complain` action |
| `announce` | `text` (≤256 bytes) | every online player | private web administration `announce` command and restart/shutdown countdown warnings (NONCANONICAL) |
| `chat` | `channel`, `from`, `name`, `text` | channel recipients | `:1376-1400` |
| `emote` | `entity`, `source`, `target`, `clip` | AOI | `:1381` |
| `combat` | `entity`, `source`, `target`, `damage`, `hp`, `owner`, `miss?`, `critical?` (player→NPC; a summon's bite carries the summon id as `entity`/`source`, kill credit to its master); `entity`, `source`, `target`, `damage`, `hp`, `miss?`, `critical?` (hit on a player, or an NPC's bite on a summon). Hit chance = Saldırı/(Saldırı+Savunma) clamped to `formulas.hit_percent`; a `miss` carries `damage` 0 and plays the target's Dodge clip | AOI | `World::swing`/`strike`/`combat` |
| `skill` | `skill`, `kind` (skill target type), `skill_source` (skills.dat `[classindex 0..2, skillno 0..15]`; skillno 15 is Boz Ayı's slot-8 variant record, so observers outside the caster's class can animate and voice it), `entity`, `source`, `target` (+ `damage`, `hp`, `owner` for single-target damage). A Şef (`boss` NPC) casting one of its NONCANONICAL `skills` sends the same event with its NPC id as `entity`/`source`, `kind` `enemy` (target = its victim) or `enemies` (target = itself, area cast); its damage follows as `combat` events and its effect as a harmful buff | AOI | `:1163-1165,1207`, `World::boss_skills` |
| `hit` | `skill`, `entity`, `source`, `target`, `damage`, `hp`, `owner` (area damage, damage over time) | AOI | `:1168,1492` |
| `death` | `entity`, `source`, `target` | AOI | `:906,1003` |
| `resurrect` | `entity`, `source`, `target` | AOI | `:1206,1215,1482` |
| `loot_open` | `target`, `items` (≤9 Ganimet slots: `{item, quantity, prefixes?}` or null when empty) | self | `loot` action |
| `loot` | `target`, `slot`, `item`, `prefixes?` | self | `loot_take` action |
| `repair` | `cost`, `items` | self | `:1238` |
| `reset_offer` | `target`, `reset` (`skills`\|`crafts`), `price` | self | `interact` on a reset machine; the client confirms in a Dikkat box and sends `point_reset` |
| `point_reset` | `reset`, `price` | self | `point_reset` action |
| `notice` | `text` | self | server line shown centred and in chat; the `free_resets` level-up refund ("N seviye olduğunuz için yeteneklerinizi/dirençlerinizi ÜCRETSİZ sıfırlandı. Tekrar dağıtabilirsiniz!") |
| `equip` / `unequip` | `entity`, `source`, `target`, `equipment` (visible array) | AOI | `:1262` |
| `craft` | `entity`, `recipe`, `item`, `quantity` | self | `:1702` |
| `gather` | `phase` (`start`\|`cancel`\|`done`), `entity`, `source`, `target`, `items` (done) | AOI | `:1711,1749,1762` |
| `moderation` | `operation` (`mute`\|`unmute`), `by` | target | `:1421-1422` |
| `kick` | `by` | target | `:1423` |
| `arena` | `text` (system chat line), `arenas`/`stake` (list), `winner` (match end, 0 = draw) | self / every player in the arena | `World::arena*` (`src/world.cpp` activities block) |
| `arena_stake` | `code`, `stake`, `text` (Dikkat confirmation wording, NONCANONICAL) | self | `World::arena` (`arena_stake` action) |

Queue rules (`World::emit`, `src/world.cpp:354-377`): critical = `trade`, `trade_invite`, `trade_complete`, `trade_cancel`, `kick`, `error` with code `trade_persistence` (`:16-20`). A new `trade`/`trade_complete`/`trade_cancel` drops queued `trade`/`trade_invite`. Overflow drops the oldest noncritical event first. A kicked actor receives only `kick` events afterwards.

## Validation limits

| Limit | Value | Code |
| --- | --- | --- |
| Frame body | 2..65,535 bytes | `include/ikv/protocol.hpp:14`, `src/protocol.cpp:205,266` |
| State/action JSON | 1..65,533 bytes, one object, nesting ≤ 32, raw NUL rejected (`\u0000` escape allowed) | `src/protocol.cpp:14-39` |
| Coordinates | ±1,536,000 | `include/ikv/protocol.hpp:15` |
| Snapshot players | ≤ 32 | `src/protocol.cpp:56,227` |
| Connections / players | 64 / 32 | `src/server_main.cpp:39`, `src/world.cpp:683` |
| Hello deadline / graceful close | 5 s / 2 s | `src/server_main.cpp:41,434` |
| Server output per peer | 262,144 bytes pending (`output_limit`) | `src/server_main.cpp:39,133` |
| Actions | 40 per second per connection | `src/server_main.cpp:307-309` |
| Action object | ≤ 8,192 dumped bytes; name ≤ 32 bytes | `src/world.cpp:1055-1057` |
| Event queue | 32 events and 32,768 bytes; one event ≤ 32,768 bytes | `src/world.cpp:359,371` |
| State frame budget | 65,533 bytes | `src/world.cpp:1635,1649,1653` |
| Startup schema proof | own state, stats, catalogs, 5 other group members, a 32+32-item trade + 31 `trade_persistence` notices ≤ 65,533; trade event ≤ 32,768 | `src/world.cpp:288-347` |
| Character state | ≤ 8,192 dumped bytes | `src/world.cpp:567,792` |
| Character name | 1..32 bytes, ≤ 66 dumped JSON bytes | `src/world.cpp:690-691` |
| Appearance | ≤ 384 dumped bytes; index ranges per sex | `src/protocol.cpp:121-148` |
| Equipment / buffs / NPC debuffs | 8 / 8 / 8 | `src/world.cpp:568,1197,920` |
| Inventory | `inventory_slots` ≤ 32 (config 32) | `src/world.cpp:102,593` |
| Character quests | ≤ 20 active (`quest_accept`), ≤ 64 at admission; completed ids in `quests_done` (8 KiB state cap) | `src/world.cpp` quest block |
| Item id / name | ≤ 32 bytes and ≤ 34 dumped / ≤ 128 bytes and ≤ 64 dumped | `src/world.cpp:167-168` |
| Item UID | exactly 32 lowercase hex | `src/world.cpp:760,744` |
| Stack / trade / shop quantity | 1..1,000 | `src/world.cpp:167,585,863,1266` |
| Trade offer | ≤ 32 items; uint64 revision, never wraps | `src/world.cpp:854,1275,1293` |
| Chat | ≤ 512 bytes, 1 per 10 ticks | `src/world.cpp:1374,1382,1400` |
| Group / guild | 6 / 64 members; guild name 2..32 bytes | `src/world.cpp:1404,1333,1339,1349` |
| Contacts per list | `social.contacts_max` ≤ 20 (config 20) | `src/world.cpp:281,1017` |
| Recipe memory | `economy.recipe_limit` ≤ 200 (config 100); recipe catalog ≤ 1,024 | `src/world.cpp:266-267,1686` |
| Bank | `economy.bank_slots` ≤ 128 (config 64) | `src/world.cpp:266,739,1729` |
| Currency / Şan / Klan Parası / Akçe | `currency_cap` ≤ 999,000,000 / 1e9 / 999,000,000 / 999,000,000 | `src/world.cpp:102,993-994` |
| Catalogs | zones 32 (each instanced zone adds ≤ 20 copies with cloned NPCs), collision boxes 64/zone, items 1,024, quests 1,024 (state carries only active quests + visible NPC offers, budgeted per NPC), skills 64, NPCs 1,024, prefixes 2,048 | `src/world.cpp` constructor |
| Client-side | entities ≤ 96, events ≤ 32, inventory ≤ 32, quests ≤ 256, buffs ≤ 8, recipes ≤ 200, bank ≤ `slots` ≤ 128 | `src/client_session.cpp:197-345` |
