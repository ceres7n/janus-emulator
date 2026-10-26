#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <string>
#include "motion.hpp"
#include "item_db.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  Entity types (Trickster "object types")
// ─────────────────────────────────────────────────────────────────────────────

enum class EntityType : uint8_t {
    Player  = 0,
    Monster = 1,
    NPC     = 2,
    Item    = 3,   // dropped item on ground
    Pet     = 4,
    Summon  = 5,
};

// ─────────────────────────────────────────────────────────────────────────────
//  BaseEntity — all in-world objects
// ─────────────────────────────────────────────────────────────────────────────

struct BaseEntity {
    uint32_t   object_id{ 0 };    // Runtime ID (not DB uid)
    EntityType type{ EntityType::Player };
    int16_t    zone{ 0 };
    int32_t    pos_x{ 0 };
    int32_t    pos_y{ 0 };
    bool       alive{ true };
};

struct PlayerEntity : BaseEntity {
    uint32_t    char_uid{ 0 };
    uint32_t    account_id{ 0 };
    uint32_t    session_id{ 0 };  // the owning GameClientSession's session_id()
    // Reserved — the fields below are never set or read: the live name, class and stats are the
    // GameClientSession's (game_session.cpp fills only zone, pos, char_uid, account_id, session_id).
    char        name[18]{ 0 };
    int16_t     type_id{ 0 };     // char type (Fox, Sheep, Cat, Dragon, etc.)
    int16_t     job{ 0 };

    // Combat stats (reserved, see above)
    int32_t     hp{ 0 };
    int32_t     hp_max{ 1 };
    int32_t     mp{ 0 };
    int32_t     mp_max{ 1 };
    int32_t     money{ 0 };
    uint32_t    exp{ 0 };

    // PMSC base stats
    uint8_t     pts_p{ 0 };
    uint8_t     pts_m{ 0 };
    uint8_t     pts_s{ 0 };
    uint8_t     pts_c{ 0 };
};

struct MonsterEntity : BaseEntity {
    uint32_t    mob_id{ 0 };      // monster template ID (db row)
    // CommonMonster::Initialize (sub_7287E0): +149 = 1 for the monster ids 16000..16999 — a shadow
    // monster, which only a player with the Third Eye (Player +1628, the Mind's Eye ES) fights.
    bool shadow() const { return mob_id >= 16000 && mob_id <= 16999; }
    uint16_t    species{ 0 };     // client render species id (mon### / sub_604320 key) sent in appear
    std::string name;
    int32_t     hp{ 0 };
    int32_t     hp_max{ 1 };
    int16_t     level{ 1 };       // monster level (MonsterParamEx2 Level; exceeds 255, so 16-bit)
    uint16_t    facing{ 0 };      // facing direction (0..255 mapped)
    uint8_t     move_speed{ 20 }; // appear-blob speed byte = (int)(cur_speed/4) (retail sub_7146E0 ->
                                  // sub_60C750); client actor+120 = 4*this; 0 = static
    // Movement (retail CommonMonster::Initialize sub_7287E0): +156 walk, +160 run, +164 chase =
    // (walk+run)/2, +132 current speed = walk at spawn. A typed move order switches the current
    // speed (PathFindingMoveStrategy: 1 walk, 2 run, 3 chase; 0 keeps it) and announces it (0x01D8).
    float       walk_speed{ 0.0f };
    float       run_speed{ 0.0f };
    float       chase_speed{ 0.0f };
    float       cur_speed{ 0.0f };
    int32_t     chase_time_ms{ 3000 }; // ChaseTime budget (actor +1056 <- param +28)
    Motion      motion;           // current server-side movement; pos_x/pos_y = its resting point
    std::chrono::steady_clock::time_point atk_ready{};   // attack-strategy cooldown (+56, sub_41B970)
    // Combat stats, from the mob_db (MonsterParamEx2 *Lv columns = real stats at the mob's level).
    int32_t     ap{ 0 };          // attack power
    int32_t     ac{ 0 };          // accuracy
    int32_t     dx{ 0 };          // dexterity: +100 ms per point on the attack cooldown (sub_41B970)
    int32_t     dp{ 0 };          // defense
    int32_t     hv{ 0 };          // evasion
    int32_t     lk{ 0 };          // luck
    int32_t     da{ 0 };          // DA (stat 7)
    int32_t     ma{ 0 };          // MA (stat 4)
    int32_t     md{ 0 };          // MD (stat 5)
    int         debuff_set{ 0 };  // MonsterParamEx2 DebuffSet (a boss's MonsterDebuffResist row)
    int         acc_formula{ 0 }; // MonsterParamEx2 AccFormula (param +2460, read through +1832)
    // Resistances (%), MonsterParamEx2 order Fire Water Wind Earth Elec Light Dark NoProp Physical
    // Gun Shadow. CommonMonster::Initialize copies them to the actor's base slots 11..21 with
    // Physical/Gun swapped (sub_718A20: slot 19 = GunR, slot 20 = PhysicalR); the effective value
    // (+836+2*slot, read by sub_718BE0) = base * 1.0 (+1632) + buffs (sub_718280).
    int16_t     resist[11]{};
    int16_t     resist_buff[11]{};  // the buffs' part of each (sub_718280; ESABreakBarrier: Physical/Gun)
    int resist_slot(int slot) const
    {
        if (slot >= 11 && slot <= 18) return resist[slot - 11] + resist_buff[slot - 11];
        if (slot == 19) return resist[9] + resist_buff[9];     // GunR
        if (slot == 20) return resist[8] + resist_buff[8];     // PhysicalR
        if (slot == 21) return resist[10] + resist_buff[10];   // ShadowR
        return 0;
    }
    // Effect states on the monster (its MonsterESLimiter, CommonMonster +1856 sub_72BE90: the ESClass
    // lists hold 10,5,5,0,1,0,0,0) — es.cpp. While one is up, the stats above are base + buff:
    // CommonMonster vtbl+160 (sub_716680, kind 3) = base (+704) + ResChain 9 (sub_716820).
    struct Es {
        uint16_t id{ 0 };              // +56 the ES object id
        int      esaction{ 0 };        // +160
        int      estate{ 0 };          // +164
        int      esclass{ 0 };         // +168
        uint16_t skill{ 0 };           // +156
        uint16_t flags{ 2 };           // +158
        int      result{ 1 };          // +180 the apply roll
        bool     warned{ false };      // +185
        int32_t  life_ms{ 0 };         // +188
        int32_t  elapsed_ms{ 0 };      // +8
        int32_t  end_ms{ 0 };          // the class tick's end
        uint32_t added{ 0 };           // the limiter entry +16
        float    ratio{ 0.0f };        // +212 / +196
        float    base_apply{ 0.0f };   // ESACruelFlameDo +200: each burn's ApplyRatio
        float    ratio2{ 0.0f };       // ESAAdhesiveDo +216 LkDRatio (`ratio` = +212 HvDRatio)
        float    ratio3{ 0.0f };       // ESAAdhesiveDo +220 DpDRatio
        int32_t  value{ 0 };           // ESABreakBarrier +208: the resist cut
        uint32_t caster{ 0 };          // +220 the caster's object id (gone -> the ES ends)
        bool     stay{ false };        // +184: kept although not applied (ESASkunkBag)
        int      phase{ 0 };           // ESASkunkBag +228: 0 the throw, 1 the hit, 2 the stun
        int32_t  phase_ms{ 0 };        // ESASkunkBag +220: the phase's time
        uint32_t resisted{ 0 };        // +192: the debuff bits a DebuffSet resisted (sub_891F80) — those parts skip
        int16_t  fx{ 0 }, fy{ 0 };     // ESAElectricField: its spot (the target's at the cast)
        int32_t  pulse_ms{ 0 };        // ESAElectricField +240: its 2000 ms pulse
        uint8_t  level{ 0 };           // the skill level (its row, for a pulse)
        int16_t  bonus[12]{};          // ESAGMCheer*Do's eight stat values (by stat index)
    };
    std::vector<Es> es;
    bool es_on(int esaction) const           // an applied ES of that ESAction is on it
    {
        for (const Es& e : es) if (e.esaction == esaction && e.result == 1) return true;
        return false;
    }
    uint16_t    skill_flags{ 0 };      // Character +0x148: 32 = frozen (ESAChillingPrison: melee on it misses, 0x200)
    // Its layer (+104) is -1 (ESADimensionDo, sub_5B99A0): no player sees it (the sectors' S2C 16 hid it), the
    // area walks skip it (sub_893F00), an entering player gets no appear of it.
    bool        banished{ false };
    bool        base_set{ false };     // base_* hold the base stats (+704) once an ES arrived
    int32_t     base_ap{ 0 }, base_ac{ 0 }, base_dx{ 0 }, base_dp{ 0 }, base_hv{ 0 }, base_lk{ 0 }, base_da{ 0 };
    int32_t     base_ma{ 0 }, base_md{ 0 };
    uint32_t    exp_reward{ 0 };  // MonsterParamEx2 EXP: shared out per hit (vtbl+264 sub_72BC10)
    // Owner (retail +1212): the first player whose attack marks it (sub_72B2F0, only while it has none);
    // owner_hp = its HP at that moment (+2024) — the owner's TM on the kill scales with owner_hp/MaxHP.
    uint32_t    owner_oid{ 0 };
    int32_t     owner_hp{ 0 };
    uint32_t    tm_reward{ 0 };   // TM EXP granted on kill
    // spawn bookkeeping (for respawn): where this instance belongs
    uint32_t    spawn_id{ 0 };    // index into the zone's spawn table (for respawn), 0 = none
    int16_t     regen_entry{ -1 }; // the map MonsterManager entry (MonsterID) that made it, -1 = none
    int8_t      regen_slot{ -1 };  // its region slot (sub_85AC40), counted down when it leaves
    int16_t     boss_entry{ -1 };  // the map's boss entry that spawned it (sub_85A480), -1 = none
    // AI (monster_ai.cpp). The class by MonsterParamEx2 Type (MonsterManager::CreateChr sub_858E40):
    // Type 0/11 TestMonster (attack strategy 2 = Passive), Type 1 AggressiveMonster (strategy 3 =
    // Aggressive, view range 200). target_oid = the player it fights (its combat runs in that
    // player's session); 0 = idle, when its NormalWalkStrategy (+1640) wanders and an aggressive one
    // looks around every 500 ms.
    bool        aggressive{ false };
    int32_t     view_range{ 200 };     // +432 (AggressiveMonster: 200; CommonMonster: aiViewRange)
    uint32_t    target_oid{ 0 };
    uint8_t     walk_state{ 1 };       // NormalWalkStrategy: 1 waiting, 2 walking
    int32_t     walk_timer_ms{ 3000 }; // +48: starts at the min delay (+52 = 3000, sub_433120)
    int32_t     scan_ms{ 0 };          // AggressiveAttackStrategy +8: the 500 ms look-around
    uint64_t    spawn_seq{ 0 };        // the order it joined the world (the zone lists keep it)
    // The condition entries (Character +0x118: 5 x {u32 mask, u8 used}, their OR at +0x140 — sub_5E1130):
    // an action tests the OR (sub_41C6B0): 1 stops MoveTo (sub_717CB0), 2 the attack (DoAttack sub_423770).
    uint32_t    cond[5]{};
    bool        cond_used[5]{};
    bool cond_add(uint32_t mask)             // sub_5E1040: a free entry takes the mask (false: none free)
    {
        for (int i = 0; i < 5; ++i)
            if (!cond_used[i]) { cond[i] = mask; cond_used[i] = true; return true; }
        return false;
    }
    void cond_remove(uint32_t mask)          // sub_5E10A0: the first entry of exactly that mask
    {
        for (int i = 0; i < 5; ++i)
            if (cond_used[i] && cond[i] == mask) { cond_used[i] = false; cond[i] = 0; return; }
    }
    bool cond_has(uint32_t mask) const       // sub_5E0FE0: an entry of exactly that mask
    {
        for (int i = 0; i < 5; ++i) if (cond_used[i] && cond[i] == mask) return true;
        return false;
    }
    bool cond_test(uint32_t mask) const      // sub_41C6B0: the OR has one of the bits
    {
        uint32_t all = 0;
        for (int i = 0; i < 5; ++i) if (cond_used[i]) all |= cond[i];
        return (all & mask) != 0;
    }
};

struct NpcRow;   // world/npc_db.hpp
struct ShopRow;  // world/shop_db.hpp
struct SkillShopRow;  // world/skill_shop_db.hpp
struct NpcEntity : BaseEntity {
    uint32_t    npc_id{ 0 };      // db row id
    uint16_t    species{ 0 };     // client render species id (npc### / mon###) sent in appear
    std::string name;
    uint16_t    facing{ 0 };      // facing direction
    uint8_t     npc_type{ 0 };    // reserved, never set: the kind is row / shop / sshop below
    const NpcRow* row{ nullptr }; // its NpcTalk record (retail Npc +1680; db/npc_db.yml)
    // A shop (retail ShopNpc, object kind 5) instead of a talk NPC: its Shop_* record (+1636).
    const ShopRow* shop{ nullptr };
    // A skill / job master (retail SkillShopNpc, object kind 14): its SkillShopTalk_* record (+1800).
    const SkillShopRow* sshop{ nullptr };
};

// An item lying on the ground (retail ItemActor). Its item (+152) is {u64 uid, u32 id, u16 count}
// of kind `kind` (ItemParam Class: 1 common, 2 rare). State machine sub_71C760, ticked with the
// elapsed ms (+8): state 1 (owned — only `owner_oid` may take it) lasts 30000 ms, then state 0
// (free, owner cleared); a free item is destroyed after 90000 ms, a gelder pile (ItemParam
// Type 30) after 60000 ms. Each state change restarts the timer (sub_414B50).
struct GroundItemEntity : BaseEntity {
    uint32_t item_id{ 0 };
    uint64_t uid{ 0 };
    uint16_t count{ 1 };
    uint8_t  kind{ 1 };           // ItemParam Class (the kind byte on the wire)
    uint8_t  rare[4]{};           // a rare item's 4 bytes (a player's dropped rare keeps its own)
    std::vector<ItemOption> opts; // a dropped rare item's options (it keeps its uid and DB row)
    int      item_type{ 0 };      // ItemParam Type (30 = gelder)
    uint16_t dropper{ 0 };        // the actor that dropped it (+208)
    uint32_t owner_oid{ 0 };      // the owner (+184), 0 = none
    uint8_t  event_flag{ 0 };     // +149: the u16 after the position in its appear (op 10)
    bool     drop_flag{ false };  // +232 (init struct byte +0x24 non-zero)
    bool     owned{ false };      // state 1
    int32_t  timer_ms{ 0 };       // +8
};

// ─────────────────────────────────────────────────────────────────────────────
//  EntityManager
//  Maintains all in-world entities, assigns runtime object_ids.
//
//  Threading: the id pool and the zone queries take mutex_, but get_player / get_monster / get_npc /
//  get_entity do not, and callers read and write the entities (HP, position, effect states) in place
//  through the pointers they return, from session strands and from the 100 ms world tick alike. That is
//  safe only because the GameServer runs ONE io thread (io_threads: 1, the v0.1 default), so all of
//  those handlers run one after another. Do not raise io_threads before the world state gets its own
//  strand or lock.
// ─────────────────────────────────────────────────────────────────────────────

class EntityManager
{
public:
    // 0 is reserved for "no entity"
    static constexpr uint32_t INVALID_OID = 0;

    // The object ids: the retail IdManager of the world (GameWorld+26832, built with 0x10000 ids and
    // id 0 taken at once — sub_7F3B10(…, 0x10000, 1)). NewId (sub_411510) takes the head of the free
    // list; DropId (sub_7F3D80) puts an id back at its TAIL, so a freed id returns only after every
    // other free one. Every world object (player, monster, NPC, ground item, effect state) takes its
    // id there: the client knows an object by this u16.
    EntityManager();

    uint32_t spawn_player(PlayerEntity ent);
    uint32_t spawn_monster(MonsterEntity ent);
    uint32_t spawn_npc(NpcEntity ent);
    void     despawn(uint32_t object_id);
    // An object id for a world object kept outside these maps (an effect state, retail kind 11: its
    // id comes from the same pool as every other object, sub_411250(dword_F5C078)); give it back
    // with free_oid when the object ends.
    uint32_t alloc_oid() { std::lock_guard<std::mutex> lock(mutex_); return new_id_locked(); }
    void     free_oid(uint32_t oid) { std::lock_guard<std::mutex> lock(mutex_); drop_id_locked(oid); }

    // Despawn only if the player entity at `oid` is still owned by `session_id`.
    // A seamless warp reconnects (new session) and takes over the SAME stable oid while the old
    // connection is still closing; its on_close must not then delete the entity the new session
    // now owns. Returns true if it actually removed the entity.
    bool despawn_player_if_owner(uint32_t oid, uint32_t session_id);

    PlayerEntity*  get_player (uint32_t oid);
    MonsterEntity* get_monster(uint32_t oid);
    NpcEntity*     get_npc    (uint32_t oid);
    BaseEntity*    get_entity (uint32_t oid);

    // Apply damage to a monster. Clamps HP at 0. Returns true if this hit killed it (HP reached 0
    // and it was alive). out_hp/out_max receive the post-hit values for the S2C HP-bar update.
    bool damage_monster(uint32_t oid, int32_t amount, int32_t& out_hp, int32_t& out_max);

    // Returns all entity OIDs in a zone (for ENTITY_LIST_UPDATE)
    std::vector<uint32_t> entities_in_zone(int16_t zone_id) const;

    // Returns all player OIDs in a zone
    std::vector<uint32_t> players_in_zone(int16_t zone_id) const;

    // Returns the non-player (monster + npc) OIDs in a zone — the set to appear to an entering player.
    std::vector<uint32_t> world_entities_in_zone(int16_t zone_id) const;
    // The monsters of a zone (the monster AI).
    std::vector<uint32_t> monsters_in_zone(int16_t zone_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<uint32_t> out;
        for (auto& [oid, m] : monsters_) if (m.zone == zone_id) out.push_back(oid);
        return out;
    }

    // The monsters of a zone in the order they joined it — the retail zone's monster list (zone +4224,
    // a linked list walked from its head: the area skills take their targets in that order).
    std::vector<uint32_t> monsters_in_zone_ordered(int16_t zone_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<uint64_t, uint32_t>> v;
        for (auto& [oid, m] : monsters_) if (m.zone == zone_id) v.emplace_back(m.spawn_seq, oid);
        std::sort(v.begin(), v.end());
        std::vector<uint32_t> out;
        out.reserve(v.size());
        for (auto& p : v) out.push_back(p.second);
        return out;
    }

    // The monsters whose owner (+1212) is that player (its RefT back-references, Character +232).
    std::vector<uint32_t> monsters_owned_by(uint32_t owner) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<uint64_t, uint32_t>> v;
        for (auto& [oid, m] : monsters_) if (owner && m.owner_oid == owner) v.emplace_back(m.spawn_seq, oid);
        std::sort(v.begin(), v.end());
        std::vector<uint32_t> out;
        for (auto& p : v) out.push_back(p.second);
        return out;
    }

    // The monsters with an effect state on them (es.cpp's tick).
    std::vector<uint32_t> monsters_with_es() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<uint32_t> out;
        for (auto& [oid, m] : monsters_) if (!m.es.empty()) out.push_back(oid);
        return out;
    }

    // ── Ground items (copies are returned: the tick may remove an item at any time) ──
    uint32_t spawn_ground_item(GroundItemEntity ent);
    std::vector<GroundItemEntity> ground_items_in_zone(int16_t zone_id) const;
    bool get_ground_item(uint32_t oid, GroundItemEntity& out) const;
    // A pick-up takes `count` of the item (ItemActor count -= count, or the whole item leaves the
    // world when count >= its amount). False if the item is no longer there; `removed` = it left.
    bool take_ground_item(uint32_t oid, uint16_t count, bool& removed);
    // Advance every ground item's state machine by `elapsed_ms`; the ones destroyed (free time
    // over) are removed and returned so the caller can tell their zone (S2C 0x000F).
    void tick_ground_items(int32_t elapsed_ms, std::vector<GroundItemEntity>& destroyed);

private:
    std::unordered_map<uint32_t, PlayerEntity>  players_;
    std::unordered_map<uint32_t, MonsterEntity> monsters_;
    std::unordered_map<uint32_t, NpcEntity>     npcs_;
    std::unordered_map<uint32_t, GroundItemEntity> items_;
    std::unordered_map<uint32_t, uint32_t>      char_oid_;   // char_uid -> stable player oid
    uint64_t spawn_seq_{ 0 };                                // MonsterEntity::spawn_seq
    std::deque<uint16_t> free_ids_;                          // the IdManager free list (head = next)
    std::vector<bool>    id_used_;                           // IdItem +16
    uint32_t new_id_locked();
    void     drop_id_locked(uint32_t oid);
    mutable std::mutex mutex_;
};
