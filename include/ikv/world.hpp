#pragma once
#include "ikv/protocol.hpp"
#include "ikv/store.hpp"
#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <utility>

namespace ikv {
struct Zone;
class World {
public:
    World(Json config, std::function<void(std::span<const CharacterSave>)> persist);
    void add(uint32_t id, int64_t account_id, int64_t character_id, const Json& record, bool admin = false);
    void remove(uint32_t id);
    void input(uint32_t id, int8_t dx, int8_t dy, uint32_t sequence);
    void destination(uint32_t id, int32_t x, int32_t y, uint32_t sequence);
    void action(uint32_t id, const Json& request);
    void moderation(uint32_t id, bool muted);
    void entitlement(uint32_t id, bool plus); // Plus Paket (accounts.plus), refreshed by the server's session heartbeat
    void set_source_zones(std::map<std::string, std::shared_ptr<const Zone>> zones);
    void advance();
    uint32_t tick() const;
    std::vector<Player> snapshot(uint32_t id) const;
    Json state(uint32_t id);
    Json status() const;
    // Private web administration command (announce/kick/teleport/summon/clock/respawn/settings); throws
    // std::invalid_argument with "code: message" when refused. NONCANONICAL operator tooling.
    Json admin(const std::string& command, const Json& arguments);
    void save_all();
    // Server-side bots (NONCANONICAL, src/world_bots.cpp): peerless actors whose behaviour issues the same handle()/destination()
    // requests a client would, so every range, cooldown, resource and ownership rule applies. They never use the 32 player slots.
    uint32_t add_bot(int64_t account_id, int64_t character_id, const Json& record, const Json& spec); // spec {behaviour, zone?}; returns id
    Json bots() const; // admin status rows
    std::vector<std::pair<int64_t,int64_t>> released_bots(); // (account, character) of bots removed since the last call
    Json bot_character(int level) const; // a fresh bot {name, class_id, appearance, state} with original class/sex/cosmetics
    Json bot_plan(int total) const; // spawn specs from gameplay.json bots.spawn; total < 0: the configured counts, else round-robin
private:
    struct Actor {
        int64_t account_id = 0, character_id = 0;
        Json character, events = Json::array();
        std::array<size_t,32> event_sizes{};
        size_t event_bytes = 0;
        bool admin = false, muted = false, externally_muted = false, kicked = false, bot = false, plus = false;
        int complaints = 0; // Şikayet Et uses this session (guide: Plus 10, others 3)
        int8_t dx = 0, dy = 0;
        uint32_t sequence = 0, attack_ready = 0, chat_ready = 0, skill_ready = 0;
        uint32_t engaged = 0, resurrect_ready = 0; // auto-attack target (approach + repeat); death countdown end
        bool moving = false, looting = false; // looting: engaged target is a corpse to walk to and claim
        std::vector<std::pair<int32_t,int32_t>> route;
        size_t waypoint = 0;
        std::map<std::string, uint32_t> cooldowns;
        Json bank; // Banka: persisted beside (not inside) state, written only by bank actions
        uint32_t banker = 0, gathering = 0, gather_ready = 0; // open bank NPC; Toplama node and completion tick
        uint8_t posture = 0; // held RP clip (social.postures: sit/lay) shown to late joiners; cleared by moving, fighting, dying
    };
    struct Npc {
        Json definition;
        int32_t x = 0, y = 0;
        int hp = 0;
        uint32_t owner = 0, attack_ready = 0, respawn_at = 0;
        std::map<uint32_t, int64_t> threat;
        size_t waypoint = 0;
        std::vector<std::pair<int32_t,int32_t>> route;
        int32_t goal_x = 0, goal_y = 0;
        size_t route_waypoint = 0;
        bool route_planned = false;
        bool returning = false; // leashed: walks home ignoring aggro
        bool wandering = false; // on a classes.dat idlewalktend leg toward goal_x/goal_y
        uint32_t idle_at = 0; // next idlewalktend roll
        bool looted = false;
        Json debuffs = Json::array(); // skill effects on this NPC: stun/speed/dot/vulnerable with expiry and caster
        Json drop; // Ganimet slots filled at death: {currency} until opened, loot table {item,quantity}, rolled instance; null = taken
        uint32_t master = 0, expires = 0, command = 0; // summon (Boz Ayı/Tesla Küresi/Çağrı): caster, end tick, commanded target
        std::vector<uint32_t> skill_ready; // Şef (boss) skill cooldown ends, by definition "skills" index
    };
    struct Trade {
        uint32_t a = 0, b = 0;
        uint64_t revision = 0;
        bool open = false, accept_a = false, accept_b = false;
        Json offer_a = {{"items", Json::array()}, {"currency", 0}};
        Json offer_b = {{"items", Json::array()}, {"currency", 0}};
    };
    using Cell = std::tuple<std::string, int32_t, int32_t>;
    Json config_;
    std::function<void(std::span<const CharacterSave>)> persist_;
    std::map<std::string, std::shared_ptr<const Zone>> source_zones_;
    bool initializing_ = true;
    uint32_t tick_ = 0;
    uint64_t trade_revision_ = 0;
    Json logs_ = Json::array();
    std::map<uint32_t, Actor> actors_;
    std::map<uint32_t, Npc> npcs_;
    std::map<Cell, std::set<uint32_t>> cells_;
    std::map<uint32_t, Trade> trades_;
    std::map<uint32_t, std::vector<uint32_t>> groups_;
    std::map<uint32_t, uint32_t> invites_;
    std::map<uint32_t, Json> guild_invites_;
    uint32_t next_summon_ = 0xf0000000u; // summoned NPC ids, above the configured population
    // Er Meydanı match (guide rehberi "Er Meydanı"): per-clan score, end tick, Klan Parası stakes by actor (/kpsavas), and per
    // activities.arena.areas entry its holding clan plus the clan currently alone inside and for how many ticks.
    struct Hold { int owner = 0, clan = 0; uint32_t ticks = 0; };
    struct Arena { bool stake = false; uint32_t ends = 0; std::map<int,int64_t> score; std::map<uint32_t,int64_t> stakes; std::vector<Hold> holds; };
    std::map<std::string, Arena> arenas_; // running matches by arena zone copy
    std::string arena_zone(int code) const; // Er Meydanı code 1.. -> arena zone or its copy; "" out of range
    void arena(uint32_t id, const std::string& action, const Json& request); // arena_list / arena_join / arena_leave
    void arena_tick(); // empty arenas close, finished matches pay out
    void arena_end(const std::string& zone);
    void arena_say(const std::string& zone, const std::string& text); // "arena" system line to every player in the zone
    void send_home(uint32_t id); // clan headquarters (activities.arena.homes) or the default spawn
    void vacated(const std::string& zone, bool any = false); // an emptied group-zone copy (any: any zone) resets its NPCs
    int karizma(const Json& state) const; // guide Karizma: 0..100, back to 100 two hours after hitting 0
    int arena_power(const Json& state) const; // skill bonus % from the Er Meydanı areas this character's clan holds
    // Administrator settings (NONCANONICAL): XP/drop/passive-regen percent multipliers, zones closed to entry, day-clock shift.
    int xp_rate_ = 100, drop_rate_ = 100, regen_rate_ = 100;
    std::set<std::string> disabled_zones_;
    double clock_offset_ = 0;
    double clock_hour() const;
    void kick(uint32_t id, uint32_t by);
    Actor& actor(uint32_t id);
    void emit(uint32_t id, Json event);
    void reject(uint32_t id, std::string code, std::string message);
    Cell cell(const std::string& zone, int32_t x, int32_t y) const;
    void index(uint32_t id, const std::string& zone, int32_t x, int32_t y, bool insert);
    std::vector<uint32_t> nearby(uint32_t id, int range) const;
    bool passable(const std::string& zone, int32_t x, int32_t y) const;
    bool clear_path(const std::string& zone, int32_t x, int32_t y, int32_t nx, int32_t ny) const;
    std::vector<std::pair<int32_t,int32_t>> route(const std::string& zone, int32_t x, int32_t y, int32_t nx, int32_t ny, int limit = 1000) const;
    std::string instance(uint32_t id, const std::string& zone) const; // zone, or the player's group copy of an instanced zone
    void transfer(uint32_t id, const std::string& zone, int32_t x, int32_t y);
    Json visible_equipment(const Json& state) const;
    // Saved rows carry state.visual (the worn visuals) so the character list can dress its slot actors; never live state.
    void persisted_visual(Json& state) const { state["visual"]=visible_equipment(state); }
    void project_event(uint32_t entity, Json event);
    bool close(uint32_t a, uint32_t b, int range) const;
    Npc& npc(uint32_t player, uint32_t target, const char* kind = "");
    void commit(const std::map<uint32_t, Json>& proposed, const Json* bank = nullptr); // bank: single-entry commit's new bank
    void gather_tick(uint32_t id, Actor& a);
    void economy(uint32_t id, const std::string& action, const Json& request); // Üretim/Toplama/Banka actions
    void give(Json& state, const std::string& item, int quantity, const Json& instance = Json::object()); // instance: prefixes/durability
    // Worn equipment: the weapon (stats need it even when worn out), whether its Eskime is above zero, the summed classes.dat
    // dmgreduction (generator armor = dmgreduction x200) of intact pieces and summed Efsun prefix attributes of intact pieces.
    struct Gear { const Json* weapon = nullptr; bool intact = false; int armor = 0, magic = 0, heal = 0; std::map<int, double> efsun; };
    Gear gear(const Json& state) const;
    // Original Karakter Bilgisi formulas (client 00402f60) over level, class, worn gear, stances and invested resistances.
    // Elements in resistance-row order: Ateş, Buz, Elektrik, Asit, Zehir.
    struct Fighter { double attack = 0, defense = 0, speed = 1, armor = 0, armor_cap = 60, crit = 1, crit_multiplier = 2, min = 0, max = 0, multiplier = 1, crit_armor = 0;
                     std::array<double,5> element{}, resist{}; std::string types = "E"; };
    Fighter fighter(const Json& state, int class_id) const;
    // One attack before the victim's defences: attack < 0 always lands (spells, damage over time); types = classes.dat attacktype letters.
    struct Blow { double attack = -1, physical = 0, crit = 0, multiplier = 2; std::array<double,5> element{}; std::string types; };
    struct Landed { int damage = 0; bool hit = false, critical = false; };
    Blow swing(const Fighter& fighter, const Json& buffs) const; // a player's weapon roll (Silah Hasarı + elements, power buffs)
    Blow spell(int damage, const Json& definition) const; // a skill amount in its school's element (physical otherwise)
    Landed land(const Blow& blow, double defense, double armor, double crit_armor, const std::array<double,5>& resist) const;
    Landed land_npc(const Blow& blow, const Json& definition, const Json& debuffs) const; // classes.dat defense, dmgred K/D/E and resistances; Meteorit armor_break
    void resources(Json& state) const; // max Enerji/Kudret from the level curves plus Maksimum Enerji/Kudret Efsun (6/7)
    void wear(Json& state, bool weapon, int permille) const; // NONCANONICAL durability loss (‰ of maximum) of the weapon or every worn armour piece
    Json roll_drop(const Json& definition, bool certain = false) const; // certain: always drops, first Efsun always rolled (Şef/arena prize)
    void take(Json& state, const std::string& uid, int quantity);
    Json& owned(Json& state, const std::string& uid);
    const Json& owned(const Json& state, const std::string& uid) const;
    void currency(Json& state, int64_t change) const;
    void xp(Json& state, int amount) const;
    void die(uint32_t id, Json& state) const;
    void engage(uint32_t id, Actor& a);
    // A skill's crit/multiplier land with its damage (true when it crit); auto-attack and DoT pass already-landed damage.
    bool combat(uint32_t id, uint32_t target, int damage, Json caster, Json event, double crit = 0, double multiplier = 2);
    void hostile_skill(uint32_t id, uint32_t target, const Json& definition, const std::string& skill, int dot); // NPC or hostile player
    void boss_skills(uint32_t id, Npc& n, const char* on, uint32_t victim); // a Şef's ready "cast" skills, or its "hit" riders on a bitten victim
    void summon(uint32_t id, const std::string& skill, const Json& definition, int rank, int damage);
    void summons(); // per-tick summon fight/expiry pass
    void hit_summon(uint32_t source, uint32_t summon, int damage);
    void walk(uint32_t id, Npc& n, int32_t gx, int32_t gy, int pace, int speed = 0); // one NPC movement step toward a goal; 0 = definition speed
    void idle_walk(uint32_t id, Npc& n, int pace); // classes.dat idlewalktend wander around the spawn point
    bool hostile(uint32_t a, uint32_t b) const; // player-vs-player relation (original 00478090 value 1)
    bool ignores(uint32_t listener, int64_t speaker) const; // Yasaklılar list
    // Hit roll, Zırh/resistances, immunity/absorb/death for a blow on a player; `credit` is the killer for Şan (a summon's master).
    bool strike(uint32_t source, uint32_t victim, const Blow& blow, uint32_t credit = 0, bool* critical = nullptr);
    bool social(uint32_t id, const std::string& action, const Json& request); // contacts/Klan/Kabadayı; false if not a social action
    Trade* trade(uint32_t id);
    void cancel_trade(uint32_t id);
    void trade_event(const Trade& value);
    std::pair<Json,Json> trade_events(const Trade& value) const;
    void validate_offer(uint32_t id, const Json& offer);
    uint32_t group(uint32_t id) const;
    void leave_group(uint32_t id);
    void handle(uint32_t id, const Json& request);
    // Quests (world.cpp quest block). Character state keeps active quests in "quests" {id:{progress[,deadline]}} and
    // completed ids in the sorted array "quests_done"; events are queued by the helpers and emitted after the commit.
    bool quest_done(const Json& state, int source_quest_id) const; // <loc> requiredquestid gate
    Json quests_at(const Npc& npc, const Json& state) const; // offered at this NPC (GB-Anı: re-takeable auto quests)
    Json quest_event(const std::string& quest, const char* stage, int progress = 0) const;
    void quest_kill(Json& state, const Json& enemy, Json& events);
    void quest_finish(Json& state, const std::string& quest, Json& events);
    void quest_expiry(uint32_t id, Actor& a);
    // Bots (src/world_bots.cpp). `errand` is an NPC to reach and `request` the action to send once in interaction range.
    struct Bot {
        std::string behaviour, home; // home: the zone the bot was spawned for (town wander / hunting ground)
        int32_t anchor_x = 0, anchor_y = 0;
        uint32_t next_think = 0, idle_until = 0, chat_ready = 0, quest_ready = 0; // quest_ready: next chore/quest look
        uint32_t leader = 0, errand = 0, stuck = 0, unblock = 0;
        Json request, inbox = Json::array(); // inbox: group/private chat from players (/dismiss)
        std::set<uint32_t> blocked; // doors that refused this bot (quest/GM/death gates) and quarries it could not reach
        uint32_t quarry = 0, misses = 0; // hunt target and how often the engine dropped the approach to it
        int32_t goal_x = 0, goal_y = 0; int64_t goal_best = INT64_MAX; uint32_t stalls = 0; // bot_walk progress toward its goal
        bool resting = false, lost = false, attacked = false, routed = false;
    };
    std::map<uint32_t, Bot> bots_;
    uint32_t next_bot_ = 0x70000000u; // bot actor ids: below INT32_MAX, far above connection ids
    std::vector<std::pair<int64_t,int64_t>> released_bots_;
    uint32_t bot_search_tick_ = UINT32_MAX; // one long bot route search per tick bounds its cost
    std::map<std::string, std::vector<uint32_t>> bot_index_; // "kind:zone" -> configured NPC ids (built on first bot use)
    const std::vector<uint32_t>& bot_npcs(const std::string& kind, const std::string& zone);
    std::vector<std::pair<std::string,uint32_t>> bot_zones(const Bot& b, const std::string& from); // BFS through doors, first door
    void admit(uint32_t id, int64_t account_id, int64_t character_id, const Json& record, bool admin, bool bot);
    void bots_tick();
    void bot_think(uint32_t id, Actor& a, Bot& b);
    bool bot_act(uint32_t id, const Json& request); // handle() as a client would send it; false when refused
    bool bot_walk(uint32_t id, Actor& a, Bot& b, int32_t x, int32_t y); // straight legs; one route() search after no progress
    bool bot_reach(uint32_t id, Actor& a, Bot& b, uint32_t npc, Json request); // walk to an NPC, then send request
    bool bot_travel(uint32_t id, Actor& a, Bot& b, const std::string& zone); // next door toward a zone; false: no path
    bool bot_fight(uint32_t id, Actor& a, Bot& b); // skills, potions and heals while engaged or a groupmate is hurt
    bool bot_upkeep(uint32_t id, Actor& a, Bot& b); // skill points, loot, gear, repair, selling, potions, quests
    void bot_town(uint32_t id, Actor& a, Bot& b);
    void bot_hunt(uint32_t id, Actor& a, Bot& b);
    void bot_follow(uint32_t id, Actor& a, Bot& b);
};
}
