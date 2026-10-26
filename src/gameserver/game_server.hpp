#pragma once
#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <string>
#include <cstdint>
#include "session.hpp"
#include "database.hpp"
#include "config.hpp"
#include "store/char_store.hpp"
#include "store/item_store.hpp"
#include "world/entity_manager.hpp"
#include "world/mob_db.hpp"
#include "world/stat_table.hpp"
#include "world/char_range_db.hpp"
#include "world/maps_db.hpp"
#include "world/skill_db.hpp"
#include "world/esa_db.hpp"
#include "world/mon_spawn_db.hpp"
#include "world/npc_db.hpp"
#include "world/npc_msg_db.hpp"
#include "world/npc_script_db.hpp"
#include "world/boss_mon_db.hpp"
#include "world/item_db.hpp"
#include "world/drop_db.hpp"
#include "world/map_attr.hpp"
#include "party.hpp"
#include "chat_server.hpp"
#include "friend_server.hpp"
#include "world/random_stat_db.hpp"
#include "world/skill_card_db.hpp"
#include "world/item_rebirth_db.hpp"
#include "world/pre_esaic_db.hpp"
#include "world/shop_db.hpp"
#include "world/gun_db.hpp"
#include "world/drill_db.hpp"
#include "world/quest_db.hpp"
#include "world/exchange_shop_db.hpp"
#include "world/compound_db.hpp"
#include "world/skill_shop_db.hpp"
#include "world/present_item_db.hpp"
#include "world/skill_scroll_db.hpp"
#include "world/port_db.hpp"
#include "world/trans_scroll_db.hpp"
#include "world/post_db.hpp"
#include "world/memory_port_db.hpp"
#include "world/mon_speech_db.hpp"
#include "world/pet_speech_db.hpp"
#include "store/post_store.hpp"
#include "world/debuff_resist_db.hpp"
#include "world/refine_db.hpp"
#include "interserver/inter_link.hpp"
#include "interserver/inter_dispatch.hpp"

class GameClientSession;
class SystemClientSession;

// Hardening (not retail): the longest client text we relay to other players (chat, whisper, party chat). The
// client's chat box copies a line into a 248-byte buffer before it sends C2S 54 / 417 / 713, the whisper or the
// party chat (ChatInput sub_8BC100: char Buffer[248], strcpy), so 247 bytes never cut what an unmodified client
// typed; a longer text can only come from a modified one (and above 65 KB it wrapped the S2C frame's u16 length).
constexpr size_t kClientChatMax = 247;
// Cut a client text (CP949: a byte 0x81..0xFE leads a two-byte character) to at most `max` bytes, never in the
// middle of a character.
inline void cut_client_text(std::string& s, size_t max)
{
    if (s.size() <= max) return;
    size_t i = 0;
    while (i < max) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t n = (c >= 0x81 && c <= 0xFE) ? 2 : 1;
        if (i + n > max) break;
        i += n;
    }
    s.resize(i);
}
// The same for a UTF-8 text (the database side, after from_client_text): back to the start of a character.
inline void cut_utf8_text(std::string& s, size_t max)
{
    if (s.size() <= max) return;
    size_t i = max;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    s.resize(i);
}

// cGameServer — the in-world zone server (the retail GameServer, with the SystemServer's community port).
// Accepts the clients the LoginServer handed over at char select (C2S 0x0004 with a game_tickets row), runs
// every zone (movement, combat, items, NPCs, quests, party, friends) and keeps the characters in MariaDB itself
// (CharStore / ItemStore / PostStore). A field change is a reconnect: S2C 154 -> a new connection -> C2S 156.
// The MasterServer link is only a startup handshake (nothing else goes through it).
class cGameServer
{
public:
    explicit cGameServer(asio::io_context& ioc);

    bool init(const Config& cfg);
    void run();
    void stop();

    Database&      db()         { return db_; }
    CharStore&     char_store()  { return char_store_; }
    PartyServer&   party()       { return party_; }   // the party (party.cpp)
    ChatServer&    chat()        { return chat_; }    // the community chat: whispers (chat_server.cpp)
    FriendServer&  friends()     { return friends_; } // the friend list (friend_server.cpp)
    ItemStore&     item_store()  { return item_store_; }
    PostStore&     post_store()  { return post_store_; }  // the post box's procedures (postbox.cpp)
    EntityManager& entity_mgr() { return entity_mgr_; }
    // The session of a player object id (nullptr when none) — e.g. an ES's caster (+220).
    GameClientSession* player_session(uint32_t oid);
    // The game session of a character (its newest one in the world; a field change's old one, still leaving,
    // when there is no other), or null.
    std::shared_ptr<GameClientSession> session_of_char(uint32_t char_uid);
    // A double login (release spec, GSC-04): every other connection holding that character (loading or in the
    // world, not a field change's old one) is saved and closed now, before the new one loads it. `from_ip` is
    // the new login's address; with `community` the old client's community links from another address go too.
    void kick_char(uint32_t char_uid, uint32_t except_sid, const std::string& from_ip, bool community);
    // The community login's binding: that character is loaded on a game connection from this address.
    bool char_online_from(uint32_t char_uid, const std::string& ip);
    // Effect states on monsters (es.cpp): the lock every change to a monster's ESs takes (the io
    // pool has several threads), the admission (MonsterESLimiter vtbl+40), the ticks, the end and
    // CommonMonster vtbl+160 (base + the ResChain 9 buffs).
    std::recursive_mutex& mob_es_mutex() { return mob_es_mutex_; }
    bool monster_es_admit(MonsterEntity& m, const SkillDef& d);
    void monster_es_tick(int32_t elapsed_ms);
    void monster_es_end(MonsterEntity& m, size_t i);
    void mark_owner_of(MonsterEntity& m, uint32_t caster);   // a caster's vtbl+132(target, 5000) at an ES end
    void knock_land(MonsterEntity& m, const MonsterEntity::Es& e); // a knock-back's landing (sub_7149C0, skill_card.cpp)
    void release_owner(MonsterEntity& m);                    // CommonMonster vtbl+120 (sub_72B400): S2C 366
    void release_owned(uint32_t player);                     // sub_717DB0 on a player: its monsters let go
    uint16_t live_owner(const MonsterEntity& m);             // its owner's id (+1212), 0 = none / gone
    void monster_recompute(MonsterEntity& m);
    // Monster regeneration (regen.cpp): the retail MonsterManager of each map with a MonsterTable.
    const MonSpawnDb& mon_spawn_db() const { return mon_spawn_db_; }
    const NpcDb&      npc_db() const { return npc_db_; }
    const NpcMsgDb&   npc_msg_db() const { return npc_msg_db_; }        // NpcMsg_* (db/npc_msg_db.yml)
    const TeleportDb& teleport_db() const { return teleport_db_; }      // TeleportInfo
    const NpcMoveDb&  npc_move_db() const { return npc_move_db_; }      // NpcMoveInfo
    const NpcCheckLvMsgDb& npc_check_lv_msg_db() const { return npc_check_lv_msg_db_; }
    const QuestDb&    quest_db() const { return quest_db_; }            // QuestInfo / QuestResult_* (quest.cpp)
    const ExchangeShopDb& exchange_shop_db() const { return exchange_shop_db_; }   // (exchange_shop.cpp)
    const CompoundDb& compound_db() const { return compound_db_; }                 // (compound.cpp)
    const SkillShopDb& skill_shop_db() const { return skill_shop_db_; }            // (skill_shop.cpp)
    const PresentItemDb& present_item_db() const { return present_item_db_; }      // (item_use.cpp)
    const SkillScrollDb& skill_scroll_db() const { return skill_scroll_db_; }      // (skill.cpp, item_use.cpp)
    const RandomPortDb&  random_port_db() const { return random_port_db_; }        // (item_use.cpp)
    const MemoryPortDb&  memory_port_db() const { return memory_port_db_; }        // (memory_port.cpp)
    const MonSpeechDb&   mon_speech_db() const { return mon_speech_db_; }          // (S2C 184, the monsters' lines)
    const PetSpeechDb&   pet_speech_db() const { return pet_speech_db_; }          // (S2C 592, the pets' lines)
    const TeleportAdvDb& teleport_adv_db() const { return teleport_adv_db_; }      // (item_use.cpp)
    const TransScrollDb& trans_scroll_db() const { return trans_scroll_db_; }      // (item_use.cpp)
    const PostSetupDb&   post_setup_db()   const { return post_setup_db_; }        // (postbox.cpp)
    const LevelUpGiftDb& levelup_gift_db() const { return levelup_gift_db_; }      // (postbox.cpp)
    int                  post_sync_ms()    const { return post_sync_ms_; }         // conf post_sync_minutes
    const DebuffResistDb& debuff_resist_db() const { return debuff_resist_db_; }  // (es.cpp: sub_891F80)
    int zone_player_count(int16_t zone_id);                // the zone's users (Limit_Teleport)
    // The NPC of a zone by object id (the zone's NPC map lookup, sub_60E670), nullptr if none.
    NpcEntity* zone_npc(int16_t zone, uint16_t oid);
    // The shop (ShopNpc, kind 5) of a zone by object id (zone +0x5E4 -> sub_60E670), nullptr if none.
    NpcEntity* zone_shop(int16_t zone, uint16_t oid);
    // The skill master (SkillShopNpc, kind 14) of a zone by object id (zone +0xE2C -> sub_60E670).
    NpcEntity* zone_skill_shop(int16_t zone, uint16_t oid);
    const ShopDb& shop_db() const { return shop_db_; }
    void monster_removed(const MonsterEntity& m);           // the monster left the world (sub_858B70)
    // Monster AI (monster_ai.cpp): the idle monsters of the zones with players — their
    // NormalWalkStrategy wander and the AggressiveAttackStrategy look-around.
    void monster_ai_tick(int32_t elapsed_ms);
    void monster_walk_tick(MonsterEntity& m, int32_t elapsed_ms, std::chrono::steady_clock::time_point now);
    void monster_aggro_tick(MonsterEntity& m, int32_t elapsed_ms,
                            const std::vector<std::shared_ptr<GameClientSession>>& players);
    void build_regen();                                     // sub_857F10 for every map
    void regen_tick(int32_t elapsed_ms);                    // sub_859AA0 -> sub_859A50 -> sub_859760 / sub_85A3F0
    uint32_t regen_spawn(int16_t zone, int mob, int region, int entry, int slot); // sub_858B00 / sub_858C90
    // CreateMonsterByRegionId (sub_858C90 -> CreateMonster sub_8595E0 -> CreateChr sub_858E40).
    uint32_t create_monster(int16_t zone, int mob, int region, int entry, int slot, int boss);
    MobDb&         mob_db()     { return mob_db_; }
    StatTable&     stat_table() { return stat_table_; }
    ExpTable&      exp_table()  { return exp_table_; }
    TmTable&       tm_table()   { return tm_table_; }
    CharRangeDb&   char_range() { return char_range_; }
    MapsDb&        maps_db()    { return maps_db_; }
    SkillDb&       skill_db()   { return skill_db_; }
    EsaDb&         esa_db()     { return esa_db_; }
    ItemDb&        item_db()    { return item_db_; }
    const RandomStatDb& random_stat_db() const { return random_stat_db_; }
    const RefineDb&     refine_db()      const { return refine_db_; }
    const MaturingDb&   maturing_db()    const { return maturing_db_; }
    const SkillCardDb&  skill_card_db()  const { return skill_card_db_; }
    const ItemRebirthDb& item_rebirth_db() const { return item_rebirth_db_; }
    const PreEsaicDb&   pre_esaic_db()   const { return pre_esaic_db_; }
    const GunDb&        gun_db()         const { return gun_db_; }
    const DrillDb&      drill_db()       const { return drill_db_; }
    const MapItemDb&    map_item_db()    const { return map_item_db_; }
    // sub_435960: the server's running time in ms (timeGetTime() - the start); the drill's timing and
    // its CRC seed (sub_410150, the frame's copy of it) come from it.
    uint32_t uptime_ms() const
    {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time_).count());
    }
    // ── The buried items of each zone (retail MapItemManager, zone +5664; drill.cpp) ──
    // Built with the world (build_map_items; sub_850040 when the zone's ItemPercentage is not 0): the
    // diggable cells are counted and items laid until they cover ItemPercentage % of them, each cell
    // holding one item at a depth that is a multiple of 10 between MinItemDepth and MaxItemDepth.
    bool    map_item_has(int16_t zone, int16_t x, int16_t y);                 // sub_850260
    int16_t map_item_depth(int16_t zone, int16_t x, int16_t y);               // sub_850110 (-1 = none)
    void    map_item_remove(int16_t zone, int16_t x, int16_t y, bool refill); // sub_8504E0 / sub_850530
    // MapItemManager::PopItemByRandom (sub_850890): a type by its rate, then an item of it by the
    // items' rates; type 5 boosts the items `quest_items` lists (an open quest's needs). 0 = none.
    uint32_t map_item_pop(int16_t zone, const std::vector<uint32_t>& quest_items);
    void    map_item_stats(int16_t zone, size_t& laid, int32_t& unblocked);       // (@dig)
    void    map_items_refill(int16_t zone);                 // sub_850570 alone (after a BigBang)
    void    build_map_items();                              // the world build: sub_850040 per zone
    // A player's Crazy Drill state carried across a zone change (the leaving zone's message 3003 writes
    // it, sub_7B05E0; the next zone's player build reads it, sub_7B0520): active -> its remaining time,
    // thresholds and warnings, else its gauge. Not saved: a new login starts empty.
    struct CdmCarry {
        bool     active = false;
        uint32_t drill_uid = 0;
        int32_t  remain_ms = 0, half_ms = 0, quarter_ms = 0;
        bool     warn_half = false, warn_quarter = false, warn_5s = false;
        float    gauge = 0.0f;
        std::chrono::steady_clock::time_point at{};
    };
    void cdm_put(uint32_t char_uid, const CdmCarry& c);
    bool cdm_take(uint32_t char_uid, CdmCarry& out);       // (an entry older than a minute is dropped)
    // A player's monster quest carried across a zone change (the leaving zone's login info: its
    // MonsterQuestInv, sub_40B740's +3740 state block; read back by SetPlayerFromLoginInfo). Not saved:
    // a new login has only the successful quests of char_quest and no running timer.
    struct MonqCarry {
        bool     used = false, done = false, success = false;
        int32_t  count = 0, qid = 0, grade = -1, kills = 0, record = 0;
        bool     flag = true, active = false;            // the state block (+3740 / +3741)
        int32_t  remain_ms = 0, acc_ms = 0, st_kills = 0, target = 0, st_qid = 0, st_grade = 0;
        std::chrono::steady_clock::time_point at{};
    };
    // A player's Crazy Hammer state carried across a zone change (skill_bear.cpp; the client keeps its avatar and
    // the gauge / Crazy mode objects on it, so their ids go along).
    struct CrazyCarry {
        uint16_t gauge_id = 0, do_id = 0;
        int32_t  gauge = 0, max = 0, left_ms = 0, dx = 0, crit = 0, shock_n = 0, shock_r = 0;
        float    ap_ratio = 0.0f, speed = 0.0f;
        std::chrono::steady_clock::time_point at{};
    };
    void crazy_put(uint32_t char_uid, const CrazyCarry& c);
    bool crazy_take(uint32_t char_uid, CrazyCarry& out);   // (an entry older than a minute is dropped)
    void pq_put(uint32_t char_uid, const PartyQuestState& s);     // a party quest across a zone change
    bool pq_take(uint32_t char_uid, PartyQuestState& out);         // (older than a minute: dropped)
    void monq_put(uint32_t char_uid, const MonqCarry& c);
    bool monq_take(uint32_t char_uid, MonqCarry& out);     // (an entry older than a minute is dropped)
    // A player's effect states carried across a zone change (Client::PrepareLeavingZone sub_40B800 ->
    // the ESLimiter's SaveAll sub_5E2210; the next zone's LoadESLimiterAll sub_40E300): each with the
    // state its class saves (es.cpp).
    struct EsCarry {
        int      esaction = 0, estate = 0, esclass = 0;
        uint16_t skill = 0;
        uint8_t  level = 0;
        uint16_t flags = 2;
        int32_t  life_ms = 0, elapsed_ms = 0, end_ms = 0;
        uint32_t added = 0;
        uint8_t  kind = 0;
        int      stat = -1;
        float    ratio = 0.0f;
        int32_t  value = 0;
        bool     countdown = false;
        int32_t  bonus[12] = {};
        int      child = 0;                // an aura's child ESAction, Radius, Team Bolster
        int32_t  radius = 0;
        bool     bolster = false;
        int32_t  count = 0;                // an aura child's party count and row Ratio
        float    base_ratio = 0.0f;
        bool     adder_on = true;          // ESADeadAlive: active, its threshold, its last check
        double   threshold = 0.0;
        double   hp_ratio = 0.0;
        int32_t  last_ms = 0;
        int32_t  hp_drop = 0, mp_drop = 0; // ESABerserk
        int32_t  interval_ms = 0, interval_acc = 0;   // ESAShield's refill timer, ESASaintPromise's (an ESASelfCureDo
                                                      // is never carried: Chakra Balance makes it outside the limiter)
    };
    void es_put(uint32_t char_uid, std::vector<EsCarry> v);
    bool es_take(uint32_t char_uid, std::vector<EsCarry>& out);   // (an entry older than a minute is dropped)
    // A player's disguise carried across a zone change (PrepareLeavingZone writes +6208 when disguised, else
    // +436; the next zone's ICMD_EX_OPTION_LIST_ACK_OK sets it back, sub_769530, and StateType 16).
    void disguise_put(uint32_t char_uid, int32_t model);
    int32_t disguise_take(uint32_t char_uid);                      // 0 = none (or older than a minute)
    // conf `restart_town`: false = "last" (retail: the last town visited), true = "zone" (the town
    // of the zone you died in, maps_db Town; falls back to the last town when the zone has none).
    bool           restart_town_of_zone() const { return restart_town_of_zone_; }

    const std::string& game_ip()   const { return game_ip_; }    // our own address (for RELOCATE 154)
    uint16_t           game_port() const { return self_port_; }

    void broadcast_zone(int16_t zone_id, PacketBuffer& pkt, uint32_t exclude_session = 0);
    void send_to_session(uint32_t sid, const PacketBuffer& pkt);   // (a copy; nothing when it is gone)
    // Run `fn` on a session's strand (nothing when it is gone) — e.g. the party's EXP for a member.
    void post_to_session(uint32_t sid, std::function<void(GameClientSession&)> fn);
    // The community connections (system_session.cpp, the retail SystemServer): the address the client
    // is sent in S2C 0x1585, its one-time login tickets, and sending on one.
    const std::string& system_ip() const   { return system_ip_; }
    uint16_t           system_port() const { return system_port_; }
    uint32_t system_ticket(uint32_t char_uid);                       // a new ticket for that character
    bool     check_system_ticket(uint32_t char_uid, uint32_t ticket);
    void     send_to_system(uint32_t sid, const PacketBuffer& pkt);
    // The megaphone queue (the retail Master's broadcast messages, megaphone.cpp): a TBN Megaphone's message
    // queued (uspm_insert_broadcast_message) -> 0, or the error (101); `wait` = the messages still waiting
    // before it. Every MegaphoneDelay ms the next one goes to every community connection (S2C 801) and is
    // done (uspm_update_broadcast_success).
    int      megaphone_submit(uint32_t char_uid, const std::string& message_utf8, int32_t& wait);
    void     start_megaphone();
    // The system megaphone (the GameServer's 3699 -> OnCCmdSysMegaphone): "[header] island - text" queued as a
    // GM message (flag 0, type 0).
    void     sys_megaphone(const std::string& header_utf8, const std::string& text_utf8);
    void     megaphone_recover();                // uspm_update_broadcast_recovery (the start)

    // Send a COMMAND BLOCK to every player in a zone. The client only plays an action
    // (attack/skill/item) when its packets arrive grouped under one non-zero block id and are
    // then CLOSED (empty 0x079F with the same id) — see PacketBuffer::block_id. This assigns one
    // fresh id to the whole group, sends the group packets then the close to each recipient
    // contiguously (first packet of `group` must be the action opener, e.g. 0x001F for a melee
    // swing). `group` is modified in place (block ids stamped).
    void broadcast_block(int16_t zone_id, std::vector<PacketBuffer>& group, uint32_t exclude_session = 0);
    // Allocate the next block id (1..0x7FFF, wraps to 1; never 0, which means "ordinary packet").
    uint16_t next_block_id();
    // One command block seen differently by its actor and by the rest of the zone (the retail block,
    // sub_8A9F70 .. sub_8A9FD0, stamps every packet sent while it is open and closes it — S2C 0x079F
    // — for each session that got one): `self_session` gets `mine` + CLOSE, every other player of the
    // zone `theirs` + CLOSE (nothing when `theirs` is empty), all under one id. E.g. an item use:
    // the user {0x0097, 0x005B, 0x0024, ...}, the others {0x005B}.
    void broadcast_block_split(int16_t zone_id, uint32_t self_session, std::vector<PacketBuffer>& mine,
                               std::vector<PacketBuffer>& theirs);

    // Entity visibility: on enter, show the newcomer to everyone already in the zone AND show them
    // to the newcomer (mutual op 8 appear). On leave, tell the zone to remove the entity (S2C 0x000F).
    void player_entered(GameClientSession& who);
    void player_left(int16_t zone_id, uint16_t eid, uint32_t exclude_session);

    // A monster killed by a player (drop.cpp): the loot of its die handler (sub_7296E0) — gelder
    // (S2C 617) then items (S2C 12) owned by the monster's owner, placed around (x, y).
    void drop_monster_loot(const MonsterEntity& m, int32_t x, int32_t y);
    // A ground item left the world (expired / picked up): S2C 0x000F to its zone.
    void ground_item_gone(int16_t zone, uint32_t oid);
    // An item a player threw away (Player::OnDropItem -> sub_7FEE10 / sub_7FE1B0): it lands at the
    // target point, at most 80 units from the thrower (sub_8040D0), free for anyone (no owner), and
    // the zone sees it fly from the thrower (S2C 0x000D). Returns the new item's object id.
    uint32_t drop_player_item(int16_t zone, uint16_t thrower, int32_t fx, int32_t fy, int32_t tx, int32_t ty,
                              GroundItemEntity g);
    // An item that pops out of the ground at an actor (a drill's find: sub_7FEF10 -> sub_7FE2F0(…, 1,
    // actor, 4, 6)): on a free cell around (x, y) (spiral inner 4 / outer 6, else (x, y) itself),
    // owned as `g` says, and the zone sees it fly out of the actor (S2C 14). Returns its object id.
    uint32_t pop_up_item(int16_t zone, uint16_t popper, int32_t x, int32_t y, GroundItemEntity g);
    // The BigBang of a Crazy Drill dig (sub_7FE670): each find (one of it, owned by `owner_oid`) lands on
    // a free cell around the digger at (x, y) (spiral inner 4 / outer 6, the batch's cells taken, else
    // (x, y)), and the area sees them fly out of their own cells: S2C 1074 (sub_607740).
    struct BigBangFind { uint32_t item = 0; int16_t from_x = 0, from_y = 0; };
    void bigbang_drop(int16_t zone, uint16_t digger, int32_t x, int32_t y, uint32_t owner_oid,
                      const std::vector<BigBangFind>& finds, bool wedding);
    // The zone attribute grid of a map (dropped items, pick-up walk targets), or nullptr.
    const MapGrid* zone_grid(int16_t zone);

    // World entities (monsters + NPCs): appear them to a player who just entered the zone.
    // Same S2C op 8 as players; only the `kind` field (3=Monster, 2=NPC) + the short class blob
    // differ (reversed byte-exact from the client readers sub_6C35B0 / sub_6C53F0).
    void appear_world_entities_to(GameClientSession& who);
    // Spawn a built monster/NPC appear to everyone already in its zone (used when a mob (re)spawns).
    void broadcast_entity_spawn(uint32_t oid, uint16_t opcode = 0x0008);   // op 8 appear (9 = with the warp-in effect)
    // Load the per-zone spawn tables and populate the world (called once at startup).
    void spawn_world_entities();
    void spawn_shops();                                     // ShopManager::Init (sub_882C90) per zone
    void spawn_skill_shops();                               // SkillShopManager::Init per zone

private:
    // drop.cpp
    uint16_t drop_amount(const DropItem& e) const;                                       // rare -> 1
    void roll_item_drops(const MobDef& d, std::vector<DropLoot>& loot) const;            // sub_729220
    void roll_sub_tables(int roll, const DropList& dl, int rolls, std::vector<DropLoot>& loot) const; // sub_72B920
    void roll_sub_table(const DropSubTable& t, int rolls, std::vector<DropLoot>& loot) const;        // sub_72BA00
    // sub_7FF180: place each item around (x, y) and tell the zone (S2C `opcode`).
    void place_drops(const MonsterEntity& m, const std::vector<DropLoot>& loot, uint32_t owner_oid,
                     bool drop_flag, uint8_t item_event_flag, int inner, int outer, uint16_t opcode,
                     int32_t x, int32_t y);
    void appear_ground_items_to(GameClientSession& who);   // op 10 for each item of the zone
    void start_item_tick();                                 // 100 ms ground-item state machines
    void item_tick();
    // drill.cpp: a zone's buried items (MapItemManager +8: a map of the cell {x, y} -> the depth; held
    // here as one u16 per cell of the grid, kNoItem = none — nothing walks the retail map in order).
    struct ZoneMapItems {
        static constexpr uint16_t kNoItem = 0xFFFF;
        bool    init = false;
        int16_t w = 0, h = 0;
        int32_t unblocked = 0;                              // +104 UnblockTileCount
        size_t  laid = 0;                                   // the map's size
        std::vector<uint16_t> depth;                        // [y * w + x]
        uint16_t* at(int16_t cx, int16_t cy)                // (a cell off the grid holds nothing)
        {
            if (cx < 0 || cy < 0 || cx >= w || cy >= h || depth.empty()) return nullptr;
            return &depth[static_cast<size_t>(cy) * static_cast<size_t>(w) + static_cast<size_t>(cx)];
        }
    };
    ZoneMapItems& zone_map_items(int16_t zone);             // sub_850040 on the first use
    void map_items_fill(int16_t zone, ZoneMapItems& z);     // sub_850570

    void accept_next();
    void accept_system();                                   // the community port (system_session.cpp)
    bool open_acceptor(asio::ip::tcp::acceptor& a, const asio::ip::address& ip, uint16_t port, const char* what);
    void on_session_close(std::shared_ptr<Session> s);
    void start_heartbeat_loop();
    void start_autosave();      // periodic persist of all in-world players (survives hard kill)
    void save_all();            // persist every connected session now (autosave / shutdown)
    void connect_master();      // dial the MasterServer hub + handshake (SVR_LOGIN 3000)

public:
    void shutdown_save() { save_all(); }   // called on graceful stop

    asio::io_context&       ioc_;
    asio::ip::tcp::acceptor acceptor_;
    Database                db_;
    CharStore               char_store_{ db_ };
    PartyServer             party_{ *this };
    ChatServer              chat_{ *this };
    FriendServer            friends_{ *this };
    ItemStore               item_store_{ db_ };
    PostStore               post_store_{ db_ };
    EntityManager           entity_mgr_;
    MobDb                   mob_db_;
    StatTable               stat_table_;
    ExpTable                exp_table_;
    TmTable                 tm_table_;    // TM EXP per TM level (db/tm_level_db.yml)
    CharRangeDb             char_range_;
    MapsDb                  maps_db_;
    SkillDb                 skill_db_;
    EsaDb                   esa_db_;      // ESAII / ESAUI / ESAction tables (db/esa_db.yml)
    ItemDb                  item_db_;     // the item tables (db/item_db.yml)
    DropDb                  drop_db_;     // monster drop lists + sub-tables (db/mon_item_*_db.yml)
    MapAttrDb               map_attr_;    // zone attribute grids (.md3 layer 1, conf map_path)
    RandomStatDb            random_stat_db_;  // rare items' random stats + compound slots (db/random_stat_*.yml)
    RefineDb                refine_db_;       // refine levels / materials / support items (db/item_refine_*.yml)
    MaturingDb              maturing_db_;     // mature compound targets / materials / aids (db/maturing_*, equip_property)
    SkillCardDb             skill_card_db_;   // skill card items (db/skill_card_db.yml, Item_SkillCardParam)
    ItemRebirthDb           item_rebirth_db_; // resurrection items (db/item_rebirth_db.yml, Item_RebirthParam)
    PreEsaicDb              pre_esaic_db_;    // skills' HP / MP preconditions (db/pre_esaic_condition_db.yml, PreESAICondition)
    GunDb                   gun_db_;         // guns' gauge + attack ranges (db/gun_db.yml, Item_GunParam2)
    DrillDb                 drill_db_;       // drills + grade EXP + stress points (db/drill_db.yml, ...)
    MapItemDb               map_item_db_;    // what a drill digs up per map (db/map_item_db.yml, ...)
    std::unordered_map<int16_t, ZoneMapItems> map_items_;
    std::recursive_mutex    map_items_mutex_;
    std::unordered_map<uint32_t, CdmCarry> cdm_carry_;
    std::mutex              cdm_carry_mutex_;
    std::unordered_map<uint32_t, MonqCarry> monq_carry_;
    std::mutex              monq_carry_mutex_;
    std::unordered_map<uint32_t, std::pair<PartyQuestState, std::chrono::steady_clock::time_point>> pq_carry_;
    std::mutex              pq_carry_mutex_;
    std::unordered_map<uint32_t, CrazyCarry> crazy_carry_;
    std::mutex              crazy_carry_mutex_;
    struct EsCarryList { std::vector<EsCarry> v; std::chrono::steady_clock::time_point at{}; };
    std::unordered_map<uint32_t, EsCarryList> es_carry_;
    std::mutex              es_carry_mutex_;
    std::unordered_map<uint32_t, std::pair<int32_t, std::chrono::steady_clock::time_point>> disguise_carry_;
    std::mutex              disguise_carry_mutex_;
    std::chrono::steady_clock::time_point start_time_{ std::chrono::steady_clock::now() };
    bool                    restart_town_of_zone_{ false };

    std::unordered_map<uint32_t, std::shared_ptr<GameClientSession>> sessions_;
    std::mutex sessions_mutex_;
    asio::ip::tcp::acceptor system_acceptor_;
    std::string             system_ip_   { "127.0.0.1" };
    uint16_t                system_port_ { 8003 };
    std::unordered_map<uint32_t, std::shared_ptr<SystemClientSession>> system_sessions_;
    std::unordered_map<uint32_t, uint32_t> system_tickets_;   // char uid -> its last ticket
    std::mutex system_mutex_;
    std::atomic<uint32_t> next_session_id_{ 1 };   // (both acceptors take ids from it)
    uint16_t   block_counter_{ 0 };   // command-block id source (next_block_id); 1..0x7FFF, wraps to 1

    asio::steady_timer heartbeat_timer_;
    asio::steady_timer accept_retry_timer_;          // an accept failed: listen again shortly (accept_next)
    asio::steady_timer system_accept_retry_timer_;   // (the same for the community port)
    asio::steady_timer item_timer_;     // ground-item tick (100 ms, like the retail GameTask)
    asio::steady_timer mega_timer_;     // the megaphone queue (MasterServer.ini MegaphoneDelay)
    std::string        mega_island_{ "Paula" };   // the island's name in a system message (conf megaphone_island_name)
    int                mega_delay_ms_{ 18000 };
    void               megaphone_tick();
    std::chrono::steady_clock::time_point item_tick_last_{};
    std::recursive_mutex mob_es_mutex_;
    MonSpawnDb           mon_spawn_db_;
    NpcDb                npc_db_;
    NpcMsgDb             npc_msg_db_;
    TeleportDb           teleport_db_;
    NpcMoveDb            npc_move_db_;
    NpcCheckLvMsgDb      npc_check_lv_msg_db_;
    QuestDb              quest_db_;       // db/quest_db.yml, quest_result_db.yml, random_result_item_db.yml
    ExchangeShopDb       exchange_shop_db_; // db/exchange_shop_db.yml, exchange_shop_table_db.yml
    CompoundDb           compound_db_;      // db/compound_db.yml, compounder_db.yml
    SkillShopDb          skill_shop_db_;    // db/skill_shop_db.yml, skill_shop_item_db.yml, limit_skill_master_db.yml, skill_master_db.yml
    PresentItemDb        present_item_db_;  // db/present_item_db.yml
    SkillScrollDb        skill_scroll_db_;  // db/skill_scroll_db.yml
    RandomPortDb         random_port_db_;   // db/random_port_db.yml
    MemoryPortDb         memory_port_db_;   // db/memory_port_db.yml (Item_MemoryPortParam)
    MonSpeechDb          mon_speech_db_;    // db/mon_speech_db.yml (MonsterParamEx2.Speech + MonSpeech_*)
    PetSpeechDb          pet_speech_db_;    // db/pet_param_db.yml (PetParam) + db/pet_speech_db.yml (PetSpeech_*)
    TeleportAdvDb        teleport_adv_db_;  // db/teleport_adv_db.yml
    TransScrollDb        trans_scroll_db_;  // db/trans_scroll_db.yml
    PostSetupDb          post_setup_db_;    // db/post_setup_db.yml
    LevelUpGiftDb        levelup_gift_db_;  // db/post_levelup_gift_db.yml
    int                  post_sync_ms_{ 900000 };   // the post box's sync (retail dword_ADF858, 15 min)
    DebuffResistDb       debuff_resist_db_; // db/monster_debuff_resist_db.yml
    ShopDb               shop_db_;        // Shop_* tables + their item lists (db/shop_db.yml, shop_item_db.yml)
    BossMonDb            boss_mon_db_;
    struct RegenSlot  { int region = 0, avr = 0, max = 0, count = 0; };
    struct RegenEntry { int mob = 0; float ratio = 0.0f; std::vector<RegenSlot> slots; };
    // A boss entry of the map's MonsterManager (sub_857F10: {+0 the last death, +4 the minute count,
    // +8 alive, +12 spawned, +16 the BossMonsterTable row}).
    struct BossEntry  { BossMonRow row; int minutes = 0; int alive = 0;
                        std::chrono::steady_clock::time_point last_death{}; };
    struct ZoneRegen  { int16_t zone = 0; int avr_num = 0; int ratio_sum = 0; int alive = 0;
                        std::vector<RegenEntry> entries;
                        std::vector<BossEntry> bosses; int32_t boss_ms = 60001; };
    std::vector<ZoneRegen>               regen_;
    std::unordered_map<int16_t, size_t>  regen_index_;
    std::recursive_mutex                 regen_mutex_;

    // Inter-server link to the MasterServer
    paula::inter::Dispatcher                 master_disp_;
    std::shared_ptr<paula::inter::InterLink> master_link_;
    std::string master_ip_   { "127.0.0.1" };
    uint16_t    master_port_ { 9010 };
    std::string game_ip_     { "127.0.0.1" };   // address the client is connected to (RELOCATE 154)
    uint16_t    self_port_   { 8002 };
};
