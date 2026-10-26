#pragma once
#include <chrono>
#include <map>
#include <unordered_map>
#include <vector>
#include "session.hpp"
#include "party.hpp"
#include "world/entity_manager.hpp"
#include "world/skill_db.hpp"
#include "world/esa_db.hpp"
#include "world/item_db.hpp"
#include "world/gun_db.hpp"
#include "world/char_range_db.hpp"
#include "world/npc_msg_db.hpp"
#include "world/npc_script_db.hpp"
#include "world/quest_db.hpp"
#include "world/memory_port_db.hpp"
#include "world/pet_speech_db.hpp"
#include "store/post_store.hpp"

class cGameServer;

// ─────────────────────────────────────────────────────────────────────────────
//  GameClientSession — per-player connection on the map/zone server
// ─────────────────────────────────────────────────────────────────────────────

enum class GameClientSessionState { Handshake, Loading, InWorld, Closed };

class GameClientSession : public Session
{
public:
    GameClientSession(asio::ip::tcp::socket socket, cGameServer& srv)
        : Session(std::move(socket)), server_(srv) {}

    void on_packet(std::shared_ptr<Session> self, PacketBuffer pkt);
    void on_close (std::shared_ptr<Session> self);
    void begin_handshake();
    // Persist progress (autosave / shutdown) — only a loaded character's, and not a field change's old connection
    // (begin_field_change saved the destination; the new connection owns the character: its stale copy would
    // roll the new session's changes back).
    void save_now() { if (holds_char() && !relocated_) save_position(); }
    void party_closed() { server_party_closed(); }   // the connection closed (party.cpp)
    // From our party's share group (party.cpp, on our strand): events 30037 / 30127 (progress.cpp) and the
    // PartyTypeBonusProcessor (0 none, 1 faster recovery, 2..5 +4 points to a line's three stats).
    void party_exp(int32_t amount, uint8_t flag);
    void party_tm(int32_t amount, uint8_t flag);
    void party_type_bonus(uint8_t bonus);
    void party_board_balloon(uint8_t type, const std::string& text);   // the board's head notice (0 = none)

    uint32_t object_id() const { return object_id_; }
    uint32_t char_uid() const { return char_uid_; }
    // The connection's address (set at accept): the Login -> Game ticket and the community login are bound to it.
    void               set_peer_ip(const std::string& ip) { peer_ip_ = ip; }
    const std::string& peer_ip() const { return peer_ip_; }
    // A character is loaded on this connection (its C2S 0x0004 / 156 took a ticket): loading or in the world.
    bool holds_char() const
    {
        return char_uid_ && (state_ == GameClientSessionState::Loading || state_ == GameClientSessionState::InWorld);
    }
    bool relocated() const { return relocated_; }   // the 154 went out: a field change's old connection
    // The SystemServer's "office" of this character (FriendClient +1316, kept from the game servers' reports): its
    // zone, whether it is between maps (a field change running — its zone is already the destination then) and its
    // current point. False: not in a world.
    bool office(int16_t& zone, bool& moving, int32_t& x, int32_t& y) const
    {
        if (state_ != GameClientSessionState::InWorld || !object_id_) return false;
        zone   = zone_id_;
        moving = leaving_;
        cur_pos(std::chrono::steady_clock::now(), x, y);
        return true;
    }
    // The game server's FriendObserveStateAndLevelProcessor (sub_828A70): the Player state 0 / 8 / 15 (normal, sitting,
    // resting) — the friend "office" checks.
    bool office_state_ok() const
    {
        if (state_ != GameClientSessionState::InWorld || leaving_) return false;
        const int st = fsm_state();
        return st == 0 || st == 8 || st == 15;
    }
    int32_t  stat_value(int i) const { return (i >= 0 && i < 12) ? stat_[i] : 0; }   // Player +560 + 4i
    int16_t  level() const { return level_; }
    // A heal on this player (e.g. ESAHeal sub_5C2BB0): HP = min(max, (int)(HP + MaxHP * ratio)); S2C
    // 0x0024 [u32 HP] to it (sub_5F4670). Returns whether it healed something (HP was below the max
    // and the result is above the old HP).
    bool     heal_by_ratio(float ratio);
    int16_t  zone_id()   const { return zone_id_; }
    bool     in_world()  const { return state_ == GameClientSessionState::InWorld && !leaving_; }
    bool     leaving()   const { return leaving_; }
    bool     in_world_or_leaving() const { return state_ == GameClientSessionState::InWorld; }

    // Builds the S2C "remote player appear" packet for THIS player (so another client can render
    // it). Layout reversed byte-exact from the client reader sub_6F02B0 + sub_6CDF30. opcode 8 =
    // plain appear (sector enter); 9 = the same body with the warp-in effect (EnterZone sub_5F6FC0;
    // client sub_6F02B0 a3=1 plays "dung jang"/"etc_warp").
    PacketBuffer build_appear(uint16_t opcode = 0x0008) const;
    // Our pet (pet.cpp): whether we have one, and its appear (op 8 / 9) for the sector enter of another.
    bool         has_pet() const { return pet_.active; }
    PacketBuffer build_pet_appear(uint16_t opcode = 0x0008) const;

private:
    cGameServer&       server_;
    GameClientSessionState  state_{ GameClientSessionState::Handshake };
    uint32_t         account_id_{ 0 };
    uint32_t         char_uid_{ 0 };    // 0 until a C2S 0x0004 / 156 took its ticket (on_packet's gate)
    uint8_t          authority_{ 0 };   // GM/authority level (char_state.authority); gates atcommands
    std::string      peer_ip_;          // the client's address (game_server.cpp accept_next)
    int              dispatch_depth_{ 0 };   // on_packet nesting (the 0x039F wrapper re-posts its inner command)
    std::vector<uint16_t> unhandled_seen_;   // the unknown C2S opcodes already logged at INFO (on_packet, at most 16)

    // Pending seamless field-change (portal / @warp): set when we send RELOCATE (154),
    // consumed when the client replies with CMD_ENTER_ZONE (156).
    bool             warp_pending_{ false };
    int16_t          warp_zone_{ 0 };
    int32_t          warp_x_{ 0 };
    int32_t          warp_y_{ 0 };
    uint32_t         object_id_{ 0 };    // runtime entity ID
    int16_t          zone_id_{ 0 };
    int32_t          pos_x_{ 0 };
    int32_t          pos_y_{ 0 };

    // ── Character data (loaded from DB in load_char_from_db) ──────────────
    char     char_name_[18]{};          // null-terminated, max 17 chars
    // The tiers (characters type/job/hair, type_2/job_2/hair_2, type_3/job_3/hair_3) and the one
    // shown, job_state (Player::SetJobState sub_761250, see apply_job_state):
    int16_t  type_{ 1 };                // the shown model, Player +436 (CharacterInfo id)
    int16_t  job_{ 0 };                 // the class, Player +1024: the highest tier's job (JobTypeInfo id)
    int16_t  base_type_{ 1 };           // characters.type   — the 1st tier's model
    int16_t  type2_{ 0 };               // characters.type_2 — the 2nd tier's model (0 = none)
    int16_t  type3_{ 0 };               // characters.type_3
    uint8_t  job1_{ 0 }, job2_{ 0 }, job3_{ 0 };   // characters.job / job_2 / job_3
    uint8_t  hair2_{ 0 }, hair3_{ 0 };  // characters.hair_2 / hair_3 (hair_: the 1st tier's)
    uint8_t  f_type_{ 0 };             // appearance / gender byte
    uint8_t  pts_p_{ 0 };              // PMSC allocation points (from char_attr)
    uint8_t  pts_m_{ 0 };
    uint8_t  pts_s_{ 0 };
    uint8_t  pts_c_{ 0 };
    int16_t  level_{ 1 };
    int16_t  home_{ 1 };               // return town (retail Player +1656 = characters.home; 1..999, default 1)
    int32_t  exp_{ 0 };
    int32_t  hp_{ 100 };
    int32_t  hp_max_{ 100 };
    int32_t  mp_{ 100 };
    int32_t  mp_max_{ 100 };
    int32_t  money_{ 0 };
    uint8_t  hair_{ 0 };
    uint8_t  job_state_{ 1 };
    // Movement speed. Retail: the CLIENT picks the move type it sends in C2S 0x18 (sub_6AF5B0: 1 =
    // walk when the run toggle is off or when carrying > 89% of the max WT, else 2 = run), and the
    // server's move strategy switches the actor's CURRENT speed by that type (sub_42BC10: 1 walk,
    // 2 run, 3 chase; type 0 counts as 2) and announces a change with S2C 0x01D8 (Player vtbl+92
    // sub_760550). Walk = half the run speed (the user's rule; libconfig MoveSpeedParam has
    // Walk 115 / Run 184). Per-session only; @speed sets the run speed.
    float    run_speed_{ 160.0f };  // run speed (client actor+120 while running)
    float    cur_speed_{ 160.0f };  // current speed (retail +132)
    uint8_t  move_speed_{ 40 };     // = (int)(cur_speed_/4): the appear/E9 speed byte (actor+120 = 4*byte)
    // Sub-stats from char_status
    int16_t  ap_pts_{ 0 };             // Attack Power
    int16_t  ac_pts_{ 0 };             // Accuracy
    int16_t  dx_pts_{ 0 };             // Dexterity
    int16_t  ma_pts_{ 0 };             // Magic Attack
    int16_t  md_pts_{ 0 };             // Magic Defense
    int16_t  da_pts_{ 0 };             // Dodge/Avoidance
    int16_t  lk_pts_{ 0 };             // Luck
    int16_t  dp_pts_{ 0 };             // Defense
    int16_t  hv_pts_{ 0 };             // Evasion
    int16_t  max_hp_pts_{ 0 };
    int16_t  max_mp_pts_{ 0 };
    int16_t  attr_pts_{ 0 };           // Unspent attribute points

    // ── Effective (derived) combat stats, computed by recompute_stats() from the gulim13 table ──
    //  Stat[i] = StatTable[AccPts[i]/4 + 1][i]; AccPts from the P/M/S/C growth × (level-1). These
    //  are what combat actually uses (not the allocated *_pts). MaxHP/MaxMP are hp_max_/mp_max_.
    int32_t  st_ap_{ 1 };              // AP  attack power
    int32_t  st_ac_{ 1 };              // AC  accuracy (hit + firearm dmg)
    int32_t  st_dx_{ 1 };              // DX  (attack speed; lower = faster)
    int32_t  st_da_{ 0 };              // DA  drill ability
    int32_t  st_lk_{ 0 };              // LK  luck (crit + block)
    int32_t  st_dp_{ 0 };              // DP  defense
    int32_t  st_hv_{ 0 };              // HV  evasion
    int16_t  tm_level_{ 0 };           // TM (Master) level — 0..500, persisted in characters.tm_level
    int32_t  tm_exp_{ 0 };             // TM EXP within the TM level (retail +828; characters.tm_exp)
    int32_t  tm_points_{ 0 };          // TM points, +1 per TM level (retail +4592; characters.tm_point)
    // Learned skills (char_skills), sent at login as S2C 271. Retail record (5 bytes, server charRec
    // +12094, client GameWorld+0xA69C): u16 skillId, u8 level (-> skill+12 both sides), u16 (stored by
    // uspm_skill_update_skill_level, unused on load). At most 80 (SetPlayerFromLoginInfo).
    struct LearnedSkill { uint16_t id; uint8_t level; };
    std::vector<LearnedSkill> skills_;
    // Accumulated allocated points per stat (canonical order, +764+2i in retail). Initialised from
    // the P/M/S/C growth × (level-1); the stat-distribution "+" (C2S 0x006C) adds to these from the
    // unspent attr_pts pool. recompute_stats() turns these into the effective stats via gulim13.
    int32_t  accpts_[12]{};

    // ── Melee combat state (server-driven) ────────────────────────────────
    // The real server drives every swing: one C2S 0x001D (attack request) starts the attacker's
    // battle-follow strategy, which walks it into range and attacks until a stop condition (target
    // dead / player moved / target out of chase range). The client never sends a per-swing packet.
    // Both sides are evaluated on the retail world tick (GameTask fps 10 = 100 ms, sub_507F00 ->
    // sub_435FD0(this,10,1)) by world_tick(); tick_gen_ invalidates a stale timer chain.
    uint16_t atk_target_{ 0 };         // the player's melee target (object id, 0 = none)
    bool     attacking_{ false };      // the player's battle-follow strategy is running
    bool     ticking_{ false };
    uint32_t tick_gen_{ 0 };
    std::chrono::steady_clock::time_point last_tick_{};
    bool     dead_{ false };           // player is dead (ignore move/attack; relog or revive to clear)
    // The death's EXP / TM loss (Player +4772 / +4776, sub_76DE10): a beginner's restart gives it back.
    int32_t  lost_exp_{ 0 };
    int32_t  lost_tm_{ 0 };
    // A revive in progress (Player +4768, set by sub_764260): the player stays dead (state 2) until
    // the event 30200 1500 ms later (sub_764460); a second revive meanwhile is refused.
    bool     reviving_{ false };
    // The protection after a revive (Player +92 flag 1 for +3252 ms, sub_73CF80; counted down while
    // alive, sub_73CFC0): an attack on us ends as result 2 (sub_5E5700).
    bool     protected_{ false };
    int16_t  protect_ms_{ 0 };
    // The portal walk (Player +2968 / +2969): a C2S 0x1D4 arms it with its destination, a plain
    // 0x18 disarms it; the zone tick moves us when we stand in a portal with that destination
    // (sub_811DE0). leaving_ = the field change has started (Player +1641 / +1642).
    bool     portal_armed_{ false };
    int16_t  portal_dx_{ 0 }, portal_dy_{ 0 };
    bool     leaving_{ false };
    // The 154 went out (begin_field_change): this connection is the old one of a field change — the
    // reconnected session owns the char, so our close must not save or tell the area, and the C2S that
    // still come here are dropped (on_packet).
    bool     relocated_{ false };
    // The NPC we talk to (Player +0xC9C; set = the talk state 9, npc_talk.cpp).
    uint16_t talk_npc_{ 0 };
    // The normal quests in progress (QuestInv, Player +2212: 30 entries of {+0 used, +1 completed, +4 the
    // run number, +8 qid, +12 QuestInfo}; quest types 1 and 4 of char_quest; quest.cpp).
    struct QuestEntry {
        bool            used{ false };
        bool            done{ false };
        int32_t         count{ 0 };
        int32_t         qid{ 0 };
        const QuestDef* def{ nullptr };
    };
    QuestEntry quests_[30]{};
    // The scenario quests in progress (Player +3112: the same 30-entry QuestInv over ScenarioQuestInfo;
    // quest type 6 of char_quest).
    QuestEntry squests_[30]{};
    // The monster quest (MonsterQuestInv, Player +2228: one entry of {+0 used, +1 completed, +4 the run,
    // +8 qid, +12 grade, +16 the Comment, +144 success, +148 kills, +152 the record (ms), +156 the
    // MonsterQuestInfo}) and its running state (Player +3740: +0 a flag (1), +1 active, +4 the time
    // left (ms), +8 the 1-second accumulator, +12 the kills, +16 the target, +20 qid, +24 grade;
    // monster_quest.cpp).
    struct MonQuestEntry {
        bool        used{ false };
        bool        done{ false };
        int32_t     count{ 0 };
        int32_t     qid{ 0 };
        int32_t     grade{ -1 };
        std::string comment;
        bool        success{ false };
        int32_t     kills{ 0 };
        int32_t     record{ 0 };
        const MonsterQuestDef* def{ nullptr };
    };
    MonQuestEntry monq_{};
    struct MonQuestState {
        bool    flag{ true };
        bool    active{ false };
        int32_t remain_ms{ 0 };
        int32_t acc_ms{ 0 };
        int32_t kills{ 0 };
        int32_t target{ 0 };
        int32_t qid{ 0 };
        int32_t grade{ 0 };
    };
    MonQuestState mqs_{};
    // The shop state 5 (shop.cpp): the open shop's object id, 0 = none.
    uint16_t shop_{ 0 };
    // The exchange shop state 18 (exchange_shop.cpp).
    bool     exchange_{ false };
    // The compound state 13 (compound.cpp) and its CompndMgr (Player +2752): the request a C2S 227 checked
    // and rolled (compound, the client's type, spins, items) and the roll — the rate shown (+172), the
    // successes (+176), the failures (+180) and the mode (+168 = Player +2920: 3 the rolled results,
    // 0 nothing made, -1 none).
    struct CompoundSession {
        struct Item { uint64_t uid{ 0 }; uint32_t item{ 0 }; int32_t cnt{ 0 }; };
        int32_t  id{ 0 };              // +0
        int16_t  type{ 0 };            // +4: 5 / 6 / 9 / 10 (the client's sub_8C20A0)
        int16_t  spin{ 0 };            // +6
        int16_t  n{ 0 };               // +8
        Item     items[5]{};           // +16
        int32_t  mode{ -1 };           // +168
        int32_t  rate{ -1 };           // +172
        int32_t  success{ -1 };        // +176
        int32_t  fail{ -1 };           // +180
    };
    CompoundSession cmp_{};
    bool     compound_{ false };
    // The Item_Refine / Item_Growth / Reset_Mix UIs (refine.cpp): the Player state 30 / 31 / 32 the
    // NPC script put us in, 0 = none. They swallow moves, sit / rest, equips and the common slot.
    int      refine_ui_{ 0 };
    // The skill master talk state 25 (skill_shop.cpp): the master's object id (Player +0xC9C), 0 = none.
    uint16_t sshop_{ 0 };
    // The skill quests in progress (Player +2472: a QuestInv of ONE entry over QuestInfo — Player::Player
    // sub_7DBCC0(1); char_quest type 3, the Master's SQL type of the GameServer's quest type 7): the
    // masters' change-job quests (CJQuest).
    static constexpr int kSkillQuestCap = 1;
    QuestEntry skquests_[kSkillQuestCap]{};
    // The change-job quest state (char info +209 = characters.change_job_queststate): the job whose
    // change-job quest was completed (CJQuest_Complete), 0 = none; a job change clears it.
    uint8_t  cj_state_{ 0 };
    // Player +2488 / +2492: the quest the last CJQuest_Check / SkillQuest_Check named and the skill
    // SkillQuest_Check named (SkillQuest_Accept takes the quest).
    int16_t  skq_check_qid_{ 0 };
    int32_t  skq_check_skill_{ 0 };
    // The rest states (C2S 0x0040, sub_75C850): Player state 8 sitting / 15 resting, 0 = neither.
    // They set the recovery rates (sub_7654A0) and swallow moves, attacks, pick-ups and skills.
    int      rest_state_{ 0 };
    // The Third Eye (Player +1628): set by the Mind's Eye ES (ESAThirdEye, event 30280), cleared when
    // it runs out (30281) or by a revive. Shadow monsters (ids 16000..16999) and their dropped items
    // exist only for a player who has it.
    bool     third_eye_{ false };
    // Character +452 bit 0: hidden (ESAHiding, S2C 827). Monsters neither take nor keep a hidden player as their
    // target (sub_427CB0 / sub_4313D0 / sub_423770: no ESAOpticalEyes of theirs sees it); a skill without
    // SkillOption 64, a swing, a shot, an NPC talk or a dropped item ends it (the limiter's vtbl+60(1286)).
    bool     hiding_{ false };
    // The player's motion (current position = interpolation of the last segment; pos_x_/pos_y_
    // stay the resting point). Combat measures CURRENT positions like the retail sub_4100C0.
    Motion   motion_;
    uint8_t  atk_move_type_{ 2 };      // move type from C2S 0x001D (strategy's stored type, actor+1036)
    // One retail BattleFollowingStrategy instance (states as sub_41DD40: 0 idle, 1 approach before
    // the first attack, 2 re-evaluate after an attack, 3 backing off, 4 re-chasing, 5 chasing,
    // 6 attacking). The player runs LazyBattleFollowingStrategy (Player::vtbl+40 sub_73A7B0 ->
    // container id 2 -> sub_41F540), the monster NormalBattleFollowingStrategy (CommonMonster
    // ::Initialize -> id 1 -> sub_41F100).
    struct BattleFollow {
        int      state{ 0 };
        int32_t  budget_ms{ 0x7FFFFFFF };   // strategy+60 <- actor ChaseTime (+1056); players: 0x7FFFFFFF
        bool     ordered{ false };          // an order was issued since (re)start (strategy+56 != 0)
        std::chrono::steady_clock::time_point last_order{};   // strategy+56
        int      orders{ 0 };               // strategy+52: orders since the last attack (>2 -> random dir)
    };
    BattleFollow pf_;                  // the player's follow (Lazy)
    // The monsters fighting us, each one's NormalBattleFollowingStrategy (their combat runs in our
    // tick; MonsterEntity::target_oid = us). The hate entry: CommonMonster adds its attacker with a
    // 5000 ms timer (vtbl+124 -> sub_717D40 -> hate list sub_5E9740, ticked down by sub_5E9540) and
    // leaves combat when its hate list is empty (OnProcessState sub_729E20 state 1); our attacks on
    // it set the timer again.
    struct MobEngage {
        uint16_t     eid{ 0 };
        BattleFollow mf;
        int32_t      hate_ms{ 5000 };
    };
    std::vector<MobEngage> mobs_;
    // The player's attack-strategy cooldown (AttackStrategy+56, sub_41B970): after an attack the next
    // one may fire only when this passes; "ready" after AttackStrategy::Init (sub_41B6E0). The
    // monster's own cooldown lives on its MonsterEntity (atk_ready).
    std::chrono::steady_clock::time_point pl_atk_ready_{};
    // HP/MP recovery (retail Player +3164..+3188): the pause after a hit and the two 2 s timers.
    int32_t  regen_pause_ms_{ 0 };     // +3188
    int32_t  hp_regen_ms_{ 0 };        // +3164
    int32_t  mp_regen_ms_{ 0 };        // +3176
    int      hp_full_sends_{ 0 };      // +3172
    int      mp_full_sends_{ 0 };      // +3184

    // ── Skill use (retail Player state machine sub_73D2A0: cases 238 / 224 / 263) ─────────
    // C2S 0x00EE asks to use a skill; a skill with a selection UI (ESAUI) is answered with S2C 241 and
    // the client returns the chosen target in C2S 0x00E0, which invokes it (Player::InvokeSkill
    // sub_75EEF0) through the skill's invocation condition object (ESAIC, Player +1204): it may have
    // to chase the target into Range (ESAICChase) and/or wait the cast Delay before it fires
    // (Player state 14, ticked by sub_75F6F0); then S2C 240 + the effect (sub_8911A0).
    struct SkillUse {
        bool     selecting{ false };   // Player +0x148 flag 1: S2C 241 sent, waiting for C2S 0x00E0
        bool     active{ false };      // an ESAIC invocation is running (state 14)
        uint16_t skill{ 0 };
        uint8_t  level{ 0 };
        uint8_t  mode{ 0 };            // ESAIC +16 (2 = the client's selection)
        int      esaic{ 0 };           // 2 = ESAICChase, 3 = ESAICMove, otherwise the Normal behaviour
        uint16_t ui_kind{ 0 };         // ESAIC +28: the selection kind of C2S 0x00E0
        uint16_t target{ 0 };          // ESAIC +176 target descriptor (kind 1: object id)
        std::vector<uint8_t> payload;  // ESAIC +44: the selection payload
        uint16_t flags{ 0 };           // ESAIC +24: 1 = cast delay pending, 2 = must chase into range, 4 = must
                                       //   walk into range of the point (ESAICMove)
        int      delay_ms{ 0 };        // ESAIC +184: cast delay (ESAII Delay * 1000, players)
        int      range{ 200 };         // ESAICChase / ESAICMove +192: ESAII Range (default 200)
        bool     in_range{ false };    // ESAICChase / ESAICMove +188
        int      chase_ms{ 0 };        // ESAICChase / ESAICMove +194: time spent chasing / walking (fails past 5000 ms)
        int16_t  dest_x{ 0 }, dest_y{ 0 };   // ESAICMove +196: the chosen point (selection kinds 3 / 6 / 10)
        bool     following{ false };   // the NormalFollowingStrategy (actor +372) is running
        int      follow_acc_ms{ 0 };   // its order timer (+8) against the gap (+84 = 300)
        uint8_t  move_type{ 0 };       // its move type (+80): our move type when the chase started
        int      fail{ 0 };            // ESAIC +12: 1 = target lost / chase timed out, 2 = height check
        bool     state14{ false };     // Player state 14 was entered for it (UseSkill / InvokeSkill 3)
        bool     resume{ false };      // Player +0x148 flag 2: state 14 came from the attack state (1), no gun
        uint16_t resume_target{ 0 };   // that attack's target (the attack target holder +232)
    };
    SkillUse su_;
    // The event 30107 a skill posts to its caster (the attack after the skill), run once the skill is
    // done — the retail message queue: [u16 target][u32 the attack strategy's cooldown ms].
    struct FollowAttack { uint16_t target{ 0 }; int ms{ 0 }; };
    FollowAttack follow_q_;
    // The effect objects our skills leave on others whose work needs us, the caster (our stats, our EXP):
    // in the retail ES objects of their own (a caster ref +204, the target ref +228) that end when the
    // caster is gone (their tick, e.g. sub_54CDA0). Ticked with our world tick.
    struct CastEs {
        uint16_t id{ 0 };              // the ES object id (+56)
        int      esaction{ 0 };
        uint16_t skill{ 0 };
        uint16_t target{ 0 };          // a monster
        int32_t  ap{ 0 };              // the row's AP / power
        float    apply{ 0.0f };        // the row's ApplyRatio
        int      state{ 0 };           // the ES's state machine
        int32_t  elapsed{ 0 };         // +8
        bool     released{ false };    // ESAMagicStormHit +252: its hold (condition 1) is gone
        int      result{ 1 };          // the apply roll (+180): ESAEarthDivideHit is not kept without it
    };
    std::vector<CastEs> cast_es_;
    // Player +3036 cool-time map: skill id -> time it may be used again (sub_88F740 / sub_88F820).
    std::unordered_map<uint16_t, std::chrono::steady_clock::time_point> skill_cd_;
    // Player +0xC94: the item being used — set by a scroll's C2S 844 (the effect state of its skill uses
    // it up) and by C2S 0x0108 (kept by uid here; the retail keeps the item's pointer).
    uint64_t scroll_uid_{ 0 };
    // Player +2936: the custom data of the last C2S 0x0108 (CMD_USE_ITEM_CUSTOM_DATA) with its read
    // position — what an Options 0x8000 Type 15 item reads (a port's destination, a hair, a name, ...).
    struct UseData {
        std::vector<uint8_t> b;
        size_t pos{ 0 };
        size_t left() const { return pos < b.size() ? b.size() - pos : 0; }
        uint8_t  u8()  { return left() >= 1 ? b[pos++] : 0; }
        uint16_t u16() { uint16_t v = 0; if (left() >= 2) { v = static_cast<uint16_t>(b[pos] | (b[pos + 1] << 8)); pos += 2; } return v; }
        uint32_t u32() { uint32_t v = 0; if (left() >= 4) { for (int k = 0; k < 4; ++k) v |= static_cast<uint32_t>(b[pos + k]) << (8 * k); pos += 4; } return v; }
        uint64_t u64() { uint64_t v = 0; if (left() >= 8) { for (int k = 0; k < 8; ++k) v |= static_cast<uint64_t>(b[pos + k]) << (8 * k); pos += 8; } return v; }
        // sub_43AD60: a C string at the cursor (false: nothing left — the retail null)
        bool str(std::string& out)
        {
            if (!left()) return false;
            out.clear();
            while (pos < b.size()) { const char c = static_cast<char>(b[pos++]); if (!c) break; out += c; }
            return true;
        }
    };
    UseData  use_data_;
    // The disguise (item_use.cpp): Player +6208 the model others are shown (0 here = our own type) and
    // +6204 disguised (+6208 != the type, sub_769530); Player +1060 StateType bits (sub_718CF0): 1 normal,
    // 2 | 16 disguised.
    int32_t  shown_model_{ 0 };
    bool     disguised_{ false };
    uint32_t state_type_{ 1 };
    // Character +0x118: the condition entries (5 x {u32 mask, u8 used}; their OR, sub_5E1130). An action
    // tests the OR (sub_41C6B0): 8 a skill (C2S 238 — but Clinch Release 1110 — and a scroll's C2S 844), 32 a
    // move (C2S 24 / 468), 64 the attack request (C2S 29), 128 a pick-up (C2S 33), 0x4E8 the drill (C2S 44).
    uint32_t pcond_[5]{};
    bool     pcond_used_[5]{};
    bool pcond_add(uint32_t mask)          // sub_5E1040
    {
        for (int i = 0; i < 5; ++i)
            if (!pcond_used_[i]) { pcond_[i] = mask; pcond_used_[i] = true; return true; }
        return false;
    }
    void pcond_remove(uint32_t mask)       // sub_5E10A0
    {
        for (int i = 0; i < 5; ++i)
            if (pcond_used_[i] && pcond_[i] == mask) { pcond_used_[i] = false; pcond_[i] = 0; return; }
    }
    bool pcond_test(uint32_t mask) const   // sub_41C6B0
    {
        uint32_t all = 0;
        for (int i = 0; i < 5; ++i) if (pcond_used_[i]) all |= pcond_[i];
        return (all & mask) != 0;
    }

    // ── Effect states (es.cpp) ─────────────────────────────────────────────
    // A buff an ESAction leaves on its target: in the retail a zone object of kind 11 with its own
    // object id (ESAction class, factory sub_527F90 keyed by the ESAction), initialised by
    // sub_50F910 + the class's vtbl+116, ticked with the zone, ended by vtbl+28 (sub_50F770), and
    // held by the target's ESLimiter (Player +2340, PlayerESLimiter: per-ESClass lists).
    enum class EsKind : uint8_t {
        None,
        StatRatio,      // a StatusValueAdder: stat += (base + items) * Ratio (ESAAcUp, ESADaUp, ...)
        StatValue,      // a StatusValueAdder: stat += the row's int value (ESAAgilityUp)
        PriMarksman,    // ESAPriMarksManShip: AC += AP / 20.0
        CustomizingGun, // ESACustomizingGun: the gun's AP x the learned 3503 level's Ratio (sub_5E4FD0)
        FastLoad,       // ESAFastLoad: the FastShot interval x Ratio / 100 (case 477)
        SharpSense,     // ESASharpSense: Player +1064 = Ratio, the gun power shot's multiplier
        ThirdEye,       // ESAThirdEye (Mind's Eye): Player +1628 while it runs (events 30280 / 30281)
        Boiling,        // ESABoiling (1391, "끓는 냄비"): the Crazy Drill gauge boost (CCrazyDrillMgr +64)
        ScrollShell,    // ESAStatUpScroll (1387): lives 1 s, puts an ESAStatUpScrollDo on the caster
        ScrollStatUp,   // ESAStatUpScrollDo (1388): its 12 stat bonuses (CalculateType 1 / 2)
        Marker,         // ESAPreventWarp / Stun / ExDieScroll (1397..1399): looked up by others (es_find)
        Light,          // ESALightScroll (1423): the client's light radius in dark maps
        WideHealing,    // ESAWideHealingScroll (1424): heals at its start, then just stays
        PartyDetect,    // ESAPartyItemDetectorScroll (1425): the buried items around a chosen point
        Anniversary,    // ESAFirstAnniversary (1223): LK += (base + items) * Ratio, from its second 1 on
        SkillResist,    // ESAPreventSlowDo / ESAInsulatorDo / ESAGasMaskDo / ESAInsulateArmorDo (1328 / 1270 / 1313 /
                        //   1314): the skill resist map (+1256) += value for (char)2215 / 4301 / 4010 / 2113 + 5115
        Aura,           // ESAHeartBeat / ESAManaFastRecover / ESABigShield (1125 / 1127 / 1129): every 400 ms
                        //   its child on each party member within Radius
        AuraChild,      // their ESAHeartBeatDo (AP) / ESAManaFastRecoverDo (MP recovery) / ESABigShieldDo (DP)
        Timer,          // an object whose init did its work (ESAWeaponThrow, ESAGellderHit): it just lives
        MagicReflect,   // ESAMagicReflect (1097): sends back part of a magic (death type 1 / 3) hit on us
        Dash,           // ESADash (1142): a run at speed 600 to the chosen point
        Guard,          // ESAGuard (1309): turns the next Count blows on us into nothing (result 6)
        DeadAlive,      // ESADeadAlive (1351): AP up while HP is low (checked every 3 s)
        Berserk,        // ESABerserk (1171): AP x ApRatio, no control (condition 1256), attacks around itself,
                        //   HP / MP drained every second
        FlashCut,       // ESAFlashAttacking / ESAPowerFlashAttacking (1275 / 1367): a run at 600 / 750 to the
                        //   target's spot, then the hit (ESAction 1276)
        EnergySword,    // ESAEnergySword (1368): the attack ranges + Range while a weapon is in slot 1
        SelfCure,       // ESASelfCureDo (1483): Heal HP every Interval; a damage event on us ends it
        ThunderBolt,    // ESAThunderBolt (1157): Electro Attack's hits, then its Electroshock at 1000 ms
        WindSlash,      // ESAWindSlash (1161): Wind Blade's hits around us, 1500 ms
        WhirlWind,      // ESAWhirlWind (1186): every 2000 ms an ESAWhirlWindDo hit on those around us, while we stand
        ElemShield,     // ESAWaterShield / Electric / Fire / Earth / WindShield (1247 / 1250..1253): a resist + Value
        ElementUp,      // ESAElementUpDo (1473): Fire..Elec + their own * Ratio / 100 (Elemental Boost)
        Field,          // ESAFireField / ESABreakdownGround (1353 / 1358): a field that puts its Do on those in it
        GaleBreak,      // ESAGaleBreak (1357): after Seconds, one hit on those at the point
        Counter,        // ESAWaterCounter .. ESAWindCounter (1337..1341): hits back whoever hits us (its shield on)
        ElemArea,       // ESAWaterArea .. ESAWindArea (1369 / 1371 / .. / 1377): its Do (a resist cut) on those in it
        Bind,           // ESABind (1154): we stand bound (condition 10), those around stunned (ESABindDo 1155)
        DarkBarrier,    // ESADarkBarrier (1254): DarkR + Value, part of a blow on us sent back
        LightShield,    // ESALightShield (1255): LightR + Value, MD / DP + 16 * (MA - 49) * their ratios
        HolyShield,     // ESAShield (1333): a shield that takes the blows on us (S2C 921), refilled
        BloodyOath,     // ESABloodyOath (1360): dark magic + MARatio, paid in HP (S2C 924)
        SaintPromise,   // ESASaintPromise (1361 / 1386): HP + MaxHP * HPRatio every 3000 ms
        SkyRay,         // ESASkyRay (1285, Searing Light): light hits around the target, then 3000 ms of protection
        SaintOath,      // ESASaintOath (1379, Sacred Oath): a 1 s shell that puts ESASaintPromise 1386 on the party
        SettleDown,     // ESASettleDownDo (1487, Refreshing Dew): our cool times run 1 / Ratio as fast
        DarkGhost,      // ESADarkGhost (1342, Dark Commando): a second dark hit on each dark one of ours
        DarkWhisper,    // ESADarkWhisper (1380, Ghostly Whisper): dark hits around the spot every 2 s while we stand
        DarkUp,         // ESADarkUpDo (1489, Dark Assimilation): Dark + Dark * Ratio / 100
        PowerThrow,     // ESAPowerThrow (1146, Precise Pitch): up for Seconds — the throws read its Ratio
        Hiding,         // ESAHiding (1286, Hide): Character +452 bit 0 (S2C 827) while it runs
        Mine,           // ESAMine / ESAMineMaster (1192 / 1267): armed past 3200 ms, the first monster in Radius
                        //   of its point sets it off (ESAction 1193 / 1268)
        Bunker,         // ESABunkerDo (1492, Bunker): a shield of 15 * DA * Ratio taking the blows (S2C 921), DP +
                        //   15 * DA * DefRatio, no MoveTo (condition 1)
        OpticalEyes,    // ESAOpticalEyes (1289, Revealing Eye): every 400 ms the hidden ones around us (S2C 828)
        SharpSenseEx,   // ESASharpSense_EX (1385, Doctor's Sharp Sense): Player +1068 = Ratio (the gun skills' crit)
        Mutation,       // ESAWildMutation / ESABeastDo (1208 / 1494, Evolution / Beast Instinct): five base stats
                        //   rewritten, StateType 4 / 64, MP drained every 2000 ms
        SharpClaw,      // ESASharpClaw (1245, Fatal Wound): a first hit on the target, then a bleed every 2000 ms
        Subcutaneous,   // ESASubcutaneous (1311, Sumo Suit): the blows on us lose (HV * Ratio + 20) / 3 % (at most 50 %)
        FluentWaist,    // ESAFluentWaist (1166, Super Hips' part of Dodge Master): the condition 2048 — no crits on us
                        //   from attackers less than 100 levels above
        WhiteKnight,    // ESAWhiteKnight (1172, Bodyguard): every 400 ms the party around get ESAWhiteKnightDo
        WhiteKnightDo,  // ESAWhiteKnightDo (1173): the blows on a member lose half (the bodyguard's share goes nowhere)
        Kamikaze,       // ESAKamikaze (1479, Mass Destruction): the condition 8, past AfterBoomTime the boom
        KamikazeDo,     // ESAKamikazeDo (1480): the condition 0x80000 for PotionBanTime (no packets)
        FullHouse,      // ESAFullHouse (1280): past 1000 ms ESAFullHouseHit on those around the server-wide spot
        FourCard,       // ESAFourCardDo (1496, Four of a Kind): four monsters around the point or nothing
        GMCheer,        // ESAGMCheerAttackDo / ESAGMCheerDefenceDo (1453 / 1456): eight stats up (flat, or % of the base)
        BearStat,       // the Bear line's buffs (skill_bear.cpp, a reconstruction): stat += (base + items) * Ratio
                        //   + value (DPRevision); MuscleUp / MindUp / WillShield / StrongDefense / AngerEnergy /
                        //   StrongForearm / GoodDefence (1524 / 1525 / 1527 / 1534 / 1540 / 1541 / 1542), and
                        //   BestShield (1544: value = the equipment DP * DpRatio / PDpRatio %)
        HammerWheel     // ESAHammerWheel (1547, Hammer Spin; skill_bear.cpp): every TicTime ms a hit on those around us
    };
    struct ActiveEs {
        uint16_t id{ 0 };              // +56 the ES object id
        int      esaction{ 0 };        // +160
        int      estate{ 0 };          // +164
        int      esclass{ 0 };         // +168
        uint16_t skill{ 0 };           // +156
        uint8_t  level{ 0 };
        uint16_t flags{ 2 };           // +158: 2, | 8 when SkillOption & 1
        int      result{ 1 };          // +180 the apply roll (sub_510060): 1 applied, 2 ApplyRatio failed, 3 resisted
        bool     stay{ false };        // +184: kept although not applied (sub_5275E0(this, 1))
        bool     warned{ false };      // +185: the 10 s warning (S2C 850) went out
        bool     limited{ false };     // held by the ESLimiter (sub_524690's third path)
        int32_t  life_ms{ 0 };         // +188 the lifetime countdown (sub_5275B0)
        int32_t  elapsed_ms{ 0 };      // +8
        int32_t  end_ms{ 0 };          // the class tick's own end: elapsed >= this
        uint32_t added{ 0 };           // the ESLimiter entry +16: when it was added (the oldest is evicted)
        EsKind   kind{ EsKind::None };
        int      stat{ -1 };           // the StatusValueAdder's stat (canonical index)
        float    ratio{ 0.0f };        // +212 (float classes) / +196 / +200
        int32_t  value{ 0 };           // +212 (int classes)
        bool     countdown{ false };   // the class counts its time down (ESAStatUpScroll +196 / ...Do +212):
                                       //   it ends once past its time (elapsed > end), not at it
        int32_t  bonus[12]{};          // ESAStatUpScrollDo +216..+260: the bonus of each stat
        bool     adder_on{ true };     // its StatusValueAdder is in the chain (ESAFirstAnniversary +212:
                                       //   only from its tick past 1000 ms) / an aura child's +216 (active)
        // Aura: its pulse timer (+204), its child's ESAction, the row's Radius (+10), Team Bolster (4103,
        // ESABigShield +196); AuraChild: the party count of the last refresh (+212), the row's Ratio
        // (+220) — `ratio` is the current one (+224) —, its aura gone with a zone change (no caster).
        int32_t  pulse_ms{ 0 };
        int      child{ 0 };
        int32_t  radius{ 0 };
        bool     bolster{ false };
        int32_t  count{ 0 };
        float    base_ratio{ 0.0f };
        bool     orphan{ false };
        uint16_t caster{ 0 };          // the object whose skill made it (an aura child's caster ref +220): us or
                                       //   another player (es_create's `caster`)
        // ESADeadAlive: the HP threshold (+248, HPPercent / 100), the HP ratio of the last check (+256), the
        // time of that check (+264).
        double   threshold{ 0.0 };
        double   hp_ratio{ 0.0 };
        int32_t  last_ms{ 0 };
        // ESABerserk: its HpDrop / MpDrop (+246 / +248), the idle time (+224) and the target timer (+228).
        int32_t  hp_drop{ 0 };
        int32_t  mp_drop{ 0 };
        int32_t  idle_ms{ 0 };
        int32_t  seek_ms{ 0 };
        // ESAFlashAttacking: the target (+224 / +256), the run not started yet (+264), the power version
        // (ESAPowerFlashAttacking); `value` packs the target's spot (+248), `ratio` our speed before (+260).
        uint16_t target{ 0 };
        bool     first{ false };
        bool     power{ false };
        // ESASelfCureDo: the heal interval (+200, ms) and its timer (+204); `value` = Heal (+208).
        int32_t  interval_ms{ 0 };
        int32_t  interval_acc{ 0 };
        // ESAThunderBolt: the targets (+228, at most 10, the selected one first), which took damage (+196) and
        // the Electroshock follow-up done (+206).
        std::vector<uint16_t> ids;
        std::vector<uint8_t>  hit;
        bool     shock_done{ false };
        // ESAFireField / ESABreakdownGround / ESAGaleBreak: the spot (+212 / +196).
        int16_t  fx{ 0 }, fy{ 0 };
        // ESADarkBarrier: the blow to send back (+196, a DamageInfo), its attacker (+248), pending (+224) and its
        // 200 ms timer (+228). ESAShield: the shield (+228) and its max (+232) in `value` / `count`.
        int32_t  dinfo[7]{};
        uint16_t other{ 0 };
    };
    std::vector<ActiveEs> es_;
    int32_t  buff_[12]{};              // Player +656: the ResChain 9 (StatusValueAdder) result per stat
    // Character +924 + 2i: the element values the ESs add (the shields 1247 / 1250..1253, ESAElementUpDo ...),
    // part of elem_slot; the client keeps the same array (avatar +824, S2C 826).
    int16_t  elem_es_[22]{};
    uint32_t es_resisted_{ 0 };        // the DebuffSet bits a monster resisted of the skill being cast (-> the ES +192)
    // ESABloodyOath on us (Player +1616 on, +1620 HPRatio, +1624 MARatio; +1618 the MP cost of the dark skill in
    // use, sub_8911A0): dark magic does + MARatio, paid with HP = that cost * HPRatio at its first damage.
    bool     bloody_on_{ false };
    float    bloody_hp_{ 0.0f };
    float    bloody_ma_{ 0.0f };
    int16_t  bloody_cost_{ 0 };
    // Character +1256: the resistance to a skill, keyed by (char)skill id (sub_5EBA70 adds, sub_5EBB40
    // takes back and drops an entry at <= 0); the apply roll of a skill's ES on us reads it (sub_890AE0 ->
    // sub_718CB0).
    std::map<int8_t, int32_t> skill_resist_;
    void skill_resist_add(int skill, int32_t v) { skill_resist_[static_cast<int8_t>(skill)] += v; }
    void skill_resist_sub(int skill, int32_t v)
    {
        auto it = skill_resist_.find(static_cast<int8_t>(skill));
        if (it == skill_resist_.end()) return;
        it->second -= v;
        if (it->second <= 0) skill_resist_.erase(it);
    }

    // ── Inventory (retail Player +1716: a list of up to 300 items, no slots — the client lays them
    //    out; it keys them by uid) ──────────────────────────────────────────────────────────────
    struct BagItem {
        uint64_t uid{ 0 };             // char_inventory.uid
        uint32_t item_id{ 0 };
        uint16_t count{ 0 };
        uint8_t  kind{ 1 };            // ItemParam Class (1 common, 2 rare)
        uint8_t  rare[4]{};            // a rare item's 4 bytes (+48..+51; byte 3 = compound slots)
        std::vector<ItemOption> opts;  // a rare item's options (char_item_option)
    };
    std::vector<BagItem> bag_;
    // A bag change record (retail 28-byte record, sub_771C60 / sub_6DB7C0; S2C 0x00A0): type 1 = the
    // item `uid` now has `count` (a stack grew), 2/4 = a new item (2 had a uid, 4 got one from the DB).
    struct BagRecord {
        uint8_t  type{ 4 };            // 1 = the item `uid` now has `count`, 2/4 = a new item, 3 = removed
        uint8_t  kind{ 1 };
        uint32_t item_id{ 0 };
        uint64_t uid{ 0 };
        uint16_t count{ 0 };
        uint8_t  rare[4]{};
        std::vector<ItemOption> opts;  // a new rare item's options (sent after it in S2C 0x00A0)
    };
    int32_t  bag_weight_{ 0 };         // the inventory's weight (+68): sum of Weight x count
    int32_t  st_maxwt_{ 0 };           // MaxWT (Player +584 = stat 6)
    // The pick-up (Player state 4 + its target ref +1664): set by C2S 0x0021; done when the walk to
    // the item ends (event 30008, posted by the move strategy) or at once when already near.
    struct Pickup { bool active{ false }; uint16_t item{ 0 }; };
    Pickup   pick_;
    // The drill state (Player state 3, DrillState; drill.cpp): set by C2S 0x002C, the dig begins when the
    // walk to the point ends (event 30008), the client's result (C2S 0x0031) or cancel (0x0030), a hit
    // (30002) or any other action ends it.
    struct Drill {
        bool     active{ false };      // state 3
        bool     started{ false };     // +4: OnStopMove passed, the dig runs
        int16_t  x{ 0 }, y{ 0 };       // +0x67C: the point
        uint16_t seed{ 0 };            // +0xC40: the result CRC's seed (even; 0 once a result came)
        uint32_t start_ms{ 0 };        // +0xC44: when the dig began (server ms, sub_435960)
    };
    Drill    drill_;
    int16_t  drill_cell_x_{ 0 }, drill_cell_y_{ 0 };   // +0xC48: the cell of the last dig (one probe EXP per cell)
    int32_t  drill_depth_sum_{ 0 };    // +0x1414: the depths reported
    int32_t  drill_finds_{ 0 };        // +0x1418: the items found
public:
    // The Crazy Drill Mode (CCrazyDrillMgr, Player +6212; drill.cpp): a gauge the digs fill (skill 5019
    // "Crazy Excavation" or the wedding flag), full -> CDM_Seconds of instant digs (Player::DoDrill)
    // with the BigBang. Carried across a zone change (sub_7B05E0 / sub_7B0520), not saved.
    struct CrazyDrill {
        bool     active{ false };      // +0
        bool     announce{ false };    // +1: S2C 50 [11] on the next tick
        bool     warn_half{ false };   // +2
        bool     warn_quarter{ false };// +3
        bool     warn_5s{ false };     // +4
        float    gauge{ 0.0f };        // +8
        int32_t  duration_ms{ 0 };     // +12
        int32_t  half_ms{ 0 };         // +16
        int32_t  quarter_ms{ 0 };      // +20
        int32_t  cooldown_ms{ 0 };     // +24: the 200 ms between instant digs
        int32_t  grade{ 0 };           // +32
        uint32_t drill_uid{ 0 };       // +40: its drill (the uid's low 32 bits)
        float    gauge_max{ 120.0f };  // +44
        float    stress_bonus{ 0.0f }; // +48
        int32_t  remain_ms{ 0 };       // +52
        float    ratio_bang{ 0.05f };  // +56: the BigBang chance
        bool     wedding{ false };     // +60
        float    boost{ 0.0f };        // +64: the Boiling buff's StressUp (ESAction 1391)
    };
private:
    CrazyDrill cdm_;
    // m_SyncFlag 0xC0020000 (Player +1636): an item drop is waiting for the DB (hItemDrop sets it,
    // ICMD_DROP_ACK OK/CANCEL clears it); a drop meanwhile is refused with S2C 0x0045.
    bool     drop_sync_{ false };
    // ── The warehouse (retail Player +1804 WHInventory; warehouse.cpp): tabs of up to 80 slots with
    //    the ids 2, 21..29 built from char_state.wh_slots (sub_6F2BA0, at most 800); each tab is an
    //    item list like the bag (WHTab +12, capacity 80), read from the DB on the first opening.
    struct WhTab {
        uint8_t  id{ 0 };              // +4: 2, 21..29 (item_where in the DB)
        int      slots{ 0 };           // +8: its share of the slot count
        bool     sent{ false };        // +12+84: its list went to the client (S2C 137's flag)
        int32_t  weight{ 0 };          // +12+68: Weight x count of its items
        std::vector<BagItem> items;    // +12
    };
    std::vector<WhTab> wh_tabs_;
    int      wh_slots_{ 0 };           // WHInventory +108: the slot count
    int      wh_use_{ 0 };             // Player +5060 WHUseType (1 = an NPC's, 2 = MyCamp's)
    bool     wh_state_{ false };       // the Player state 10 (WarehouseState)
    bool     wh_loading_{ false };     // m_SyncFlag 0x800000: tabs still being read (no tabs at all: for good)
    // ── The NeoWarehouse, the account's bank (Player +1920 / +1984; bank.cpp): an item list of capacity
    //    80 that the DB fills with the account's first 24 items, its gelder, the flag of its list having
    //    gone to the client, and the Player state 11 (NeoWarehouseState).
    std::vector<BagItem> bank_;
    int32_t  bank_weight_{ 0 };
    int32_t  bank_money_{ 0 };
    bool     bank_sent_{ false };
    bool     bank_state_{ false };
    // A place an item can be in for the warehouse moves (sub_75DAF0: 1 = the bag, 3 = the bank, else a tab).
    struct WhPlace {
        std::vector<BagItem>* items{ nullptr };
        int32_t* weight{ nullptr };
        int      cap{ 0 };             // the list's capacity (sub_6DABE0): the bag 300, a tab / the bank 80
        uint8_t  where{ 0 };           // item_where: 1 the bag, 3 the bank, else the tab id
        WhTab*   tab{ nullptr };
    };
    struct WhReq { uint64_t uid{ 0 }; uint32_t item{ 0 }; int cnt{ 0 }; };   // the request's item (16 bytes)
    // ── Equipment (retail Player +2120): 23 slots that reference items of the bag by uid (an equipped
    //    item stays in the bag, char_inventory.equip_slot = its slot; S2C 0x00E9 carries the 23 uids).
    uint64_t equip_[23]{};
    bool     equip_sync_{ false };     // m_SyncFlag 0xC2000000: hItemEquipment waiting for the DB
    int32_t  stat_[12]{};              // the final stats (Player +560): base (gulim13) + equipment

    // ── The pet (retail Pet, object kind 4, Player +1712; pet.cpp): made by the pet item of slot 8
    //    (PetItemHelper), it follows us (PetFollowingStrategy) and leaves with the item or with us.
    struct Pet {
        bool        active{ false };
        uint32_t    oid{ 0 };          // +56: its object id (the world IdManager)
        uint64_t    item{ 0 };         // the pet item it came from
        uint16_t    species{ 0 };      // +436: its CharacterInfo id (sub_6DFB90 of the item id)
        char        name[64]{};        // +60 (a fixed array: another session's sector enter reads it)
        Motion      motion;            // its position; motion.tx/ty = the destination (+120)
        float       cur_speed{ 0.0f }; // +132 (Pet::Init: 0)
        bool        moving{ false };   // a move order runs: its end is the event 30008
        // PetFollowingStrategy (+372, id 2): the move type (+80, vtbl+48), the far time (+8)
        uint8_t     move_type{ 2 };
        int32_t     far_ms{ 0 };
        // The Pet FSM (sub_738960): 0 normal, 1 its owner fights, 2 its owner is dead
        int         state{ 0 };
        int32_t     idle_ms{ 0 };      // +1668: how long it has stood still
        int32_t     idle_end_ms{ 0 };  // +1672: rand(8000..20000) — then S2C 63 [0]
        int32_t     last_x{ 0 }, last_y{ 0 };   // +1664: where it stood
        int32_t     run_seen{ 0 };     // +1676: the owner's move type it last took (2 = run: the speech 4)
        int32_t     run_hold_ms{ 0 };  // +1680: 10000 after the speech 4, then it follows the owner again
        // Its speech (sub_738F50 / sub_738FF0): PetParam (+1660), silent in a map of Type 14 (+1684), the
        // pending situation (+1688), the pause running (+1692) for how long (+1696) of how much (+1700).
        const PetSpeechDb::Param* speech{ nullptr };
        bool        silent{ false };
        int         say_pending{ 0 };
        bool        say_on{ false };
        int32_t     say_ms{ 0 };
        int32_t     say_pause_ms{ 1000 };
    };
    Pet      pet_;
    // ── The player-to-player trade (retail ItemExc / ItemExchangeMgr, Player FSM state 6; trade.cpp) ──
    struct TradeSlot {                 // an offered slot: C2S 101's 16-byte record {u64 uid, u32 id, u32 count}
        uint64_t uid{ 0 };
        uint32_t item_id{ 0 };
        int32_t  count{ 0 };
        BagItem  item;                 // its bag item when it was offered (the S2C 101 view, the exchange)
        bool set() const { return uid || item_id; }   // sub_50CD60
    };
    struct TradeSide {                 // ItemExchange +24 / +136
        uint32_t sid{ 0 };             // the player's session
        uint16_t oid{ 0 };             // the player
        uint32_t char_uid{ 0 };
        bool     flag{ false };        // +108: C2S 101's u8
        int32_t  money{ 0 };           // +104: the gelders offered
        bool     checked{ false };     // +109
        bool     agreed{ false };      // +110
        TradeSlot slot[6];
        int32_t  weight{ 0 }, max_wt{ 0 }, purse{ 0 };   // its bag weight, MaxWT and gelders at its last update
        std::vector<BagRecord> recs;   // the exchange's records of its bag (hItemExchange::Send)
        int32_t  settled{ 0 };         // its gelders after the exchange
        std::vector<std::pair<uint64_t, uint16_t>> before;   // its bag (uid, count) when its records were made
    };
    struct TradeEx {
        uint16_t id{ 0 }; TradeSide side[2]; int first{ -1 };
        bool exec{ false };            // hItemExchange::Send runs: the offers are fixed (trade_execute)
        bool committed{ false };       // USPM_ItemTrade_User is done (trade_exec_other)
    };
    static std::mutex                  trade_mx_;
    static std::map<uint16_t, TradeEx> trades_;
    static uint16_t                    trade_next_;
    bool     trade_{ false };          // Player FSM state 6
    uint16_t trade_opposite_{ 0 };     // +0xC7C: the other player (RefT)
    int32_t  trade_id_{ -1 };          // +0xA80: the exchange (the zone's ItemExchangeMgr), -1 = none
    bool     trade_sync_{ false };     // m_SyncFlag 0xC4000000: the exchange's DB step runs
    // PlayerRecoveryAmountAcc (Player +2724, sub_739710), in the ResChain 2 while a pet item with options
    // 24000 / 24001 (its HPRecoveryRate / MPRecoveryRate) is on: +2740 / +2744.
    bool     pet_regen_on_{ false };
    float    pet_regen_hp_{ 1.0f };
    float    pet_regen_mp_{ 1.0f };

    // ── Guns and their bullets (gun.cpp). The common slot 20 is not one of the 23 slots: a loaded
    //    bullet stack (Type 29) is the magazine, a Throw item (Type 16, or a Type 11 SubType 60 card)
    //    the throw slot; the DB keeps either in equip_slot 20 and S2C 0x00E9 lists it there.
    struct Magazine {                  // Player +4596 (sub_79DE70 / reset sub_76CCA0)
        int      per_shot{ 1 };        // [0] bullets fired per shot
        int      loaded{ 0 };          // [1] the stack's count when it was loaded
        uint64_t item{ 0 };            // [2] the loaded stack; 0 = empty (sub_5803A0)
    };
    Magazine mag_;
    uint64_t throw_item_{ 0 };         // Player +4608: the equipped Throw item / throw card
    uint16_t gun_seed_{ 0 };           // Player +0xB9E: the CRC seed of the next shot (S2C 0x0200)
    int      gun_hacks_{ 0 };          // Player +0x1BCC: refused shots in a row (3 -> cut)
    uint32_t last_shot_tick_{ 0 };     // Player +0x13F0: the tick of the last shot (sub_765480)
    // Player +1032, the next attack's mode: 5 = the requested shot is a power shot (gun.cpp); 3 / 4 =
    // a crit / strong crit charged by a missed melee swing (sub_5E5700), landed and cleared by the
    // next swing whatever its rolls.
    int      atk_power_{ 0 };
    float    power_shot_mult_{ 1.0f }; // Player +1064: the gun power shot's multiplier (ESASharpSense)
    float    gun_skill_mult_{ 1.0f };  // Player +1068: the gun skills' crit multiplier (ESASharpSense_EX, sub_5E53C0)
    // The base stats (+704) are gulim13[accpts / 4 + 1][i], written by sub_7164C0 whenever the points change; a
    // mutation (ESAWildMutation / ESABeastDo) rewrites five of them and its end copies the saved record back
    // (sub_800440), which then holds until the points change again (base_fixed_*). mut_record_ / mut_saved_: the
    // zone's mutation record of us (sub_764850 -> sub_800510: the model, the base stats, the equipment).
    bool     base_fixed_{ false };
    int32_t  base_fixed_val_[12]{};
    int32_t  base_fixed_pts_[12]{};
    bool     mut_record_{ false };
    int32_t  mut_saved_[12]{};
    // GunAttackStrategy (Player attack strategy 5 while a gun is in slot 1): a requested shot is
    // fired once (no cooldown) and the strategy stops on its next tick (+60 = 3 -> vtbl+40), which
    // makes the player leave the attack state (S2C 0x0020). `gun_start_` = +60 = 1: the strategy
    // (re)enters its attack state on the next tick (S2C 0x0019 halt + the follow from state 1).
    bool     gun_start_{ false };
    bool     gun_stop_{ false };
    bool     melee_enter_{ false };    // NormalAttackStrategy +60 = 1: enter the attack state after the cooldown

    // ── Handlers ──────────────────────────────────────────────────────────
    void handle_session_ready    (PacketBuffer& pkt);
    void handle_session_ack      (PacketBuffer& pkt);
    void handle_auth             (PacketBuffer& pkt);
    void handle_enter_confirm    (PacketBuffer& pkt);   // C2S 0x0005 — client in-world ready
    void handle_move             (PacketBuffer& pkt);
    void handle_portal_move      (PacketBuffer& pkt);                // C2S 0x1D4: walk into a portal
    void walk                    (uint16_t sx, uint16_t sy, uint16_t dx, uint16_t dy, uint8_t mt);
    void portal_tick             (std::chrono::steady_clock::time_point now);
    void take_portal             (int zone_to, uint32_t conn_id);
    bool leave_field             (int zone_to);                      // S2C 0x1B + 153 + 21 (sub_5F7A20)
    // NPC talk (npc_talk.cpp)
    void handle_npc_talk         (PacketBuffer& pkt);                // C2S 0x6F
    void handle_npc_next         (PacketBuffer& pkt);                // C2S 0x72
    void handle_npc_end          (PacketBuffer& pkt);                // C2S 0x75
    void handle_npc_script       (PacketBuffer& pkt);                // C2S 0x7F
    const NpcMsgTable* npc_messages(const NpcEntity& n) const;
    void send_npc_talk           (const NpcEntity& n, int index);    // S2C 112
    void send_npc_message        (const NpcEntity& n, int index, int qid);  // S2C 115 / 116
    int  npc_precheck            (const NpcEntity& n);
    int  npc_script              (NpcEntity& n, int func, const uint8_t* data, size_t size, int& quest_slot);
    int  npc_teleport_check      (const NpcEntity& n, int tidx);
    int  npc_teleport_start      (const NpcEntity& n, int tidx);
    int  npc_move_check          (const NpcMoveInfo& mi);            // sub_6D85F0
    int  npc_move_complete       (NpcEntity& n, int first_arg);      // Move_Complete
    void npc_move_after          (int zone, int32_t x, int32_t y, int delay_ms = 2000); // the move (30083) later
    void owner_monster_quest     (const MonsterEntity& m);           // the owner's sub_729410 on a kill
    void exit_npc_dialog         ();                                 // S2C 117 + state 0
    void player_zone_move        (int zone);                         // the player's message 18 (case 18)
    int  npc_give_item           (NpcEntity& n, const uint8_t* args, size_t size);     // Give_Item
    int  npc_take_item           (NpcEntity& n, const uint8_t* args, size_t size);     // Take_Item
    int  npc_limit_teleport      (NpcEntity& n, int tidx, const uint8_t* args, size_t size, int limit);
    int  npc_random_move         (const uint8_t* args, size_t size);                   // Random_Move
    int  npc_exchange_shop_start (NpcEntity& n);                     // ExchangeShop_Start
    void handle_exchange_info    (PacketBuffer& pkt);                // C2S 251
    void handle_exchange         (PacketBuffer& pkt);                // C2S 254
    void handle_exchange_close   ();                                 // C2S 257
    bool given_merge             (std::vector<QuestItem>& given) const;   // one entry per uid (exchange / compound)
    // ── Skill / job masters (skill_shop.cpp) ──
    void handle_skill_shop_open  (PacketBuffer& pkt);                // C2S 486
    void handle_skill_shop_next  (PacketBuffer& pkt);                // C2S 489
    void handle_skill_shop_script(PacketBuffer& pkt, bool params);   // C2S 496 / 497
    void handle_skill_shop_list  (PacketBuffer& pkt, bool sell);     // C2S 503 / 505
    void handle_skill_shop_close ();                                 // C2S 492 / 502
    void handle_skill_master     (PacketBuffer& pkt);                // C2S 560
    void skill_shop_leave        ();                                 // another state replaces 25
    void send_skill_shop_msg     (const NpcEntity& n, int index);   // sub_5FD580: S2C 490 / 491
    void send_skill_shop_buy_list(const NpcEntity& n);              // S2C 500 (sub_5FD870)
    void send_skill_shop_sell_list(const NpcEntity& n);             // S2C 501 (sub_5FD970)
    void send_skill_master_list  (const NpcEntity& n);              // S2C 557 (sub_5FE5A0)
    int  skill_shop_script       (NpcEntity& n, int func, const uint8_t* data, size_t size, bool params);
    void load_skill_quests       ();                                 // the login list (char_quest type 3)
    void send_skill_quest_list   ();                                 // S2C 296 (sub_5F6CB0)
    int  cj_quest_check          (const NpcEntity& n, bool second);  // CJQuest_Check / CJQuest2_Check (sub_7EA110)
    int  cj_quest_accept         (NpcEntity& n, const uint8_t* args, size_t size, int cnt);   // sub_7EA340
    int  cj_quest_complete       (NpcEntity& n, const uint8_t* args, size_t size, int cnt);   // sub_7EA4B0
    int  skill_quest_accept      (const uint8_t* args, size_t size, int cnt);                 // sub_7E3970
    void handle_change_job       (PacketBuffer& pkt);                // C2S 569
    void change_job              (NpcEntity& n);                     // hItemChangeJob (sub_78A0A0 / sub_78A210)
    // ── Compounds (compound.cpp) ──
    int  npc_compound_start      (NpcEntity& n);                     // Compound_Start (sub_7E3190)
    void compound_enter          ();                                 // state 13 (the session reset)
    void compound_leave          ();                                 // another state replaces 13
    void handle_compound_info    (PacketBuffer& pkt);                // C2S 221
    void handle_compound_prob    (PacketBuffer& pkt);                // C2S 227
    void handle_compound         (PacketBuffer& pkt);                // C2S 186
    void handle_compound_open    ();                                 // C2S 187
    void handle_compound_close   ();                                 // C2S 192
    // ── Item refine / mature compound / reset (refine.cpp) ──
    int  npc_refine_start        ();                                 // Item_Refine (func 71, sub_7E5380)
    int  npc_growth_start        ();                                 // Item_Growth (func 72, sub_7E53A0)
    int  npc_resetmix_start      ();                                 // Reset_Mix (func 73, sub_7E53C0)
    void refine_ui_enter         (int state);                        // state 30 / 31 / 32
    void refine_ui_leave         ();                                 // another state replaces them
    void handle_refine           (PacketBuffer& pkt);                // C2S 696 (hItemRefine)
    void handle_repair           (PacketBuffer& pkt);                // C2S 699 (hItemRepair)
    void handle_growth           (PacketBuffer& pkt);                // C2S 704 (hItemGrowthCompound)
    void handle_resetmix         (PacketBuffer& pkt);                // C2S 734 (hItemGrowthCompoundReset)
    void handle_refine_close     (uint16_t op);                      // C2S 702 / 707 / 737 -> state 0
    const ItemOption* item_option(const BagItem& b, int type, int16_t id) const;   // sub_6E82B0-like
    // ── The warehouse (warehouse.cpp) ──
    void wh_build                (int slots);                        // SetChrStateInfo: the tabs
    WhTab* wh_tab                (int id);                           // sub_6F2890
    int  npc_warehouse_start     (int type);                         // Warehouse_Start (sub_7E2E30)
    void send_warehouse_tab      (WhTab& t);                         // S2C 137 (sub_5F9710)
    void handle_wh_add           (PacketBuffer& pkt);                // C2S 139
    void handle_wh_remove        (PacketBuffer& pkt);                // C2S 142
    void handle_wh_exchange      (PacketBuffer& pkt);                // C2S 745
    void handle_wh_close         ();                                 // C2S 138
    void wh_hit                  ();                                 // state 10, event 30002
    void wh_leave_state          ();                                 // another state replaces 10
    bool wh_place                (int id, WhPlace& p);               // sub_75DAF0
    void wh_load_tab             (WhTab& t);                         // DB 4109/4106: the tab's items
    bool wh_has                  (const WhPlace& p, const WhReq& q) const;            // HasAll sub_50CD80
    bool wh_take_record          (const WhPlace& p, const WhReq& q, BagRecord& r) const; // sub_6DBF00
    void wh_put_record           (const WhPlace& p, BagRecord& r) const;              // sub_6DB7C0
    bool wh_can_take             (const WhPlace& p, const WhReq& q) const;            // sub_6DB080
    bool wh_ammo_item            (uint32_t item_id) const;                            // sub_778800
    bool wh_db_move              (const WhPlace& from, BagRecord& fr, const WhPlace& to, BagRecord& tr); // DB
    void wh_apply                (WhPlace& p, const BagRecord& r);                    // sub_6DBA50
    // ── The NeoWarehouse, the account's bank (bank.cpp) ──
    int  npc_bank_start          ();                                 // NeoWarehouse_Start (sub_7E3060)
    void bank_load               ();                                 // DB 4112 -> 4113 -> 4106 -> 4107
    void send_bank               ();                                 // S2C 196 (sub_5F9990)
    void handle_bank_add         (PacketBuffer& pkt);                // C2S 198
    void handle_bank_remove      (PacketBuffer& pkt);                // C2S 201
    void handle_bank_money       (PacketBuffer& pkt, bool add);      // C2S 204 / 207
    void handle_bank_close       ();                                 // C2S 197
    bool bank_db_move            (const WhPlace& from, BagRecord& fr, const WhPlace& to, BagRecord& tr); // DB 4029
    void npc_end_talk            ();
    // Quests (quest.cpp)
    QuestEntry* quest_find       (int qid);                          // sub_7DBDA0
    QuestEntry* quest_add        (int qid, int count);               // sub_7DBFC0
    bool quest_remove            (int qid);                          // sub_7DC020
    void load_quests             ();                                 // the login list (char_quest)
    void send_quest_list         ();                                 // S2C 234
    void send_quest_add          (const QuestEntry& e);              // S2C 132
    void send_quest_remove       (int qid);                          // S2C 133
    void send_npc_cancel         ();                                 // S2C 116 (empty) + the talk ends
    std::vector<uint32_t> quest_items() const;                       // sub_7DC070 (the drill's type-5 boost)
    const QuestDef* npc_quest_def(const NpcEntity& n, int qid) const; // sub_60CA40
    int  quest_check             (NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt);  // Quest_Check
    int  quest_accept            (NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt, bool once); // Quest_Accept / Quest_Once_Accept
    int  quest_complete          (NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt, bool history); // Quest_Complete / Quest_NH_Complete
    int  quest_check_state       (NpcEntity& n, int qid, int num, const uint8_t* args, size_t size, int cnt,
                                  int sql_type = 1);                 // CheckQuestState / S_CheckQuestState
    QuestEntry* qinv_find        (QuestEntry* inv, int qid, int cap = 30);   // sub_7DBDA0 on a QuestInv of `cap`
    QuestEntry* qinv_add         (QuestEntry* inv, int qid, int count, bool scenario, int cap = 30); // sub_7DBFC0
    bool qinv_remove             (QuestEntry* inv, int qid, int cap = 30);   // sub_7DC020
    void send_scenario_quest_list();                                 // S2C 833
    int  squest_check            (NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt);  // S_Quest_Check
    int  squest_accept           (NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt);  // S_Quest_Accept
    int  squest_complete         (NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt);  // S_Quest_Complete
    int  quest_active            (int qid);                          // ActiveQuestID (sub_7E5E10)
    bool quest_items_ok          (const QuestDef& d, bool any);      // sub_6DB140 / sub_6DB1A0
    std::vector<BagRecord> quest_bag_records(std::vector<QuestItem> taken, const std::vector<QuestItem>& adds,
                                             int* new_items = nullptr, bool apply = true); // sub_6DC0E0
    // The quest stored procedures (MasterServer QuestStore), statement by statement on char_quest /
    // char_quest_history; `type` is the SQL quest type.
    int  db_quest_check_history  (int type, int qid, int max_count, int monster_id, int& com_count);
    int  db_quest_take           (int type, int qid, int max_count, int monster_id, int& com_count);
    int  db_quest_completed      (int type, int qid, int monster_id, int kill_count, int record_time, bool history);
    bool db_quest_history_count  (int type, int qid, int monster_id, int check_count);
    bool db_quest_done_today     (int type, int qid, int monster_id);
    void db_quest_cancel         (int qid, int monster_id);
    void db_quest_monster_allkill(int qid, int monster_id, int kill_count, int record_time); // uspm_quest_monster_allkill
    void db_quest_monster_update (int qid, int monster_id, int kill_count);                  // uspm_quest_monster_update
    // Monster quests (monster_quest.cpp)
    MonQuestEntry* monq_find     (int qid);                          // sub_7D9EA0
    MonQuestEntry* monq_add      (int qid, int count, int level);    // sub_7DA620 (grade by the level)
    bool monq_remove             (int qid);                          // sub_7DA9C0
    void load_monster_quest      ();                                 // the login list (char_quest types 2 / 5)
    void monq_restore_carry      ();                                 // a zone change's carried state
    void monq_carry_out          ();                                 // the state the next zone takes
    // ── The party quests (party_quest.cpp) ──
    PartyQuestState pq_;                                             // CPartyQuestInventory (one entry + its timer)
    struct PqParty {                                                 // CPartyManager's checks of the master
        uint16_t party{ 0 };
        uint32_t master{ 0 };
        int      count{ 0 };                                         // the members (sub_86A560)
        bool     same_zone{ false };                                 // sub_86A1B0
        bool     all_found{ true };
        int      min_level{ 400 }, max_level{ 0 };                   // sub_86A2E0 / sub_86A420
        std::vector<std::string> busy;                               // sub_869A20 (their names: S2C 876)
        std::vector<uint32_t> uids;                                  // the members found
    };
    PqParty pq_party();
    bool pq_busy() const;
    void pq_send_time(int32_t a, int32_t b);                         // S2C 1194
    void pq_send_kills(uint16_t total, uint16_t target, const std::vector<PqRec>& recs);   // S2C 1195
    int  pq_check(NpcEntity& n, int qid);                            // NpcScript 153..157 (sub_7E1B20)
    int  pq_accept(NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt);     // 158..162 (sub_7E1CC0)
    int  pq_complete(NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt);   // 163..167 (sub_7E1F00)
    void pq_tick(int elapsed_ms);                                    // sub_75E5B0
    void pq_on_kill(uint32_t mob);                                   // sub_7295F0
    void pq_insert();                                                // hInsertPartyQuest (USPM_Complete_PartyQuest)
    int  db_pq_reward(int qid, uint64_t uid, int& com);              // USPM_Reward_PartyQuest
    void pq_load();                                                  // USPM_GetList_PartyQuest (the login)
    void pq_carry_out();
    void pq_restore_carry();
public:
    struct PqMember { int16_t zone{ 0 }; int level{ 0 }; bool busy{ false }; std::string name; };
    PqMember pq_member() const;                                      // (read by the party's master)
    bool pq_add(uint64_t uid, int qid, uint8_t members, uint16_t monster, uint16_t target, int64_t start,
                int32_t limit, uint16_t com);                        // AddPartyQuest's member part
    void pq_update(int qid, uint16_t total, std::vector<PqRec> recs);   // PartyOrderUpdateQuest (sub_75E430)
    void pq_cancelled(int qid, uint32_t why);                        // PartyOrderCancelQuest (sub_86B2D0)
    void pq_party_state(int qid, uint64_t uid, uint8_t members, uint16_t monster, uint16_t target, int64_t start,
                        int32_t limit, uint16_t com);                // a zone enter / login back (sub_86B810)
private:
    void send_monster_quest_list ();                                 // S2C 235
    void send_monster_quest_add  (const MonQuestEntry& e);           // S2C 210
    void send_monster_quest_remove(int qid);                         // S2C 211
    void monq_tick               (int32_t elapsed_ms);               // Player::ProcessMonsterQuest (sub_75E780)
public:
    void monq_on_kill            (uint32_t mob_id);                  // CommonMonster::ProcessMonsterQuest (sub_729410)
private:
    void monq_on_death           ();                                 // Player::OnObjMsg_Die's monster quest part
    void handle_monq_return      (PacketBuffer& pkt);                // C2S 1040 (the return portal)
    const MonsterQuestDef* npc_monster_quest_def(const NpcEntity& n, int qid) const; // sub_60CAF0
    int  monq_check              (NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt);
    int  monq_accept             (NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt, bool once);
    int  monq_complete           (NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt, bool history);
    int  monq_check_state        (NpcEntity& n, int qid, int num, const uint8_t* args, size_t size, int cnt);
    bool bag_has_item            (uint32_t item_id) const;
    // Skill use (skill.cpp)
    void handle_skill_request    (PacketBuffer& pkt);             // C2S 0x00EE [u16 skill][u8 lv]
    void handle_skill_target     (PacketBuffer& pkt);             // C2S 0x00E0 the selected target
    void handle_skill_select_cancel(PacketBuffer& pkt);           // C2S 0x0107 the selection was cancelled
    void handle_skill_scroll     (PacketBuffer& pkt);             // C2S 0x034C [u64 uid][u16 skill] a scroll (case 844)
    void handle_skill_scroll_target(PacketBuffer& pkt);           // C2S 0x0351 a scroll skill's target (case 849)
    int  skill_use               (const SkillDef& d, uint8_t lv); // Player::UseSkill sub_75EBC0 -> 0/1/2/3
    void skill_enter_cast        ();                              // UseSkill's 3: the stop + state 14 (cast start)
    void skill_state14_enter     ();                              // state 1's exit + state 14's OnInit (0x74D857)
    void skill_state14_done      ();                              // state 14's tick after the fire (0x74DA03)
    void skill_follow_after      (uint16_t target, bool no_gun = false); // a skill's 30107 when +0x148 flag 2 is clear
    void skill_follow_attack     (uint16_t target, int ms);       // event 30107 (0x73D688): BeginAttack + cooldown
    void skill_run_follow        ();                              // the queued 30107
    bool can_run                 () const;                        // sub_421E50
    void skill_stop_motion       (std::chrono::steady_clock::time_point now); // sub_41C5D0
    int  skill_fire_check        (const SkillDef& d);             // sub_890B30: -1 or the S2C 239 reason (sent)
    int  skill_invoke            (const SkillDef& d, uint8_t lv, uint16_t kind,
                                  const std::vector<uint8_t>& payload);   // Player::InvokeSkill sub_75EEF0 -> 0/2/3
    void handle_skill_level_up   (PacketBuffer& pkt);             // C2S 0x01E1 raise a learned skill (case 481)
    bool can_level_up_skill      (int skill, int level);          // Player::CanLevelUpSkill sub_760930
    void skill_tick              (std::chrono::steady_clock::time_point now, int elapsed_ms); // state 14
    int  skill_conditions        (const EsaRow* row) const; // sub_524340 -> the failing condition type
    void skill_cancel_reason     (uint16_t reason);              // S2C 239 [u16 reason]
    void skill_cancel_using      ();                             // S2C 243 + reset the ESAIC
    void skill_cast_motion       ();                             // ESAIC stop + S2C 242 (cast start)
    void skill_execute           (std::chrono::steady_clock::time_point now); // S2C 240 + sub_8911A0
    void esa_steel_punch         (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1078 (sub_5C2110)
    void esa_power_shot          (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1148 (sub_5C8290)
    void esa_melee_shot          (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1174 (sub_5C9770)
    void esa_gun_stock           (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1138 (sub_5C6AE0)
    void esa_unhappy_day         (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1196 (sub_5CB900)
    void esa_double_shot         (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1249 (sub_5D1420)
    // The Bear line (skill_bear.cpp — a reconstruction: no server we have runs these ESActions).
    void esa_hammer_ap           (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // 1551 Energetic Hammer
    void esa_multi_hammer        (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now, int hits); // 1523 / 1535
    void esa_fly_hammer          (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // 1539 Flying Hammer
    void esa_area_hammer         (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now, bool quake); // 1530 / 1546
    void quake_paralysis         (MonsterEntity& m, const SkillDef& d, const EsaRow* act); // 1531 QuakeHammerDo
    int32_t weapon_stat          (int stat) const;                // the slot-1 item's bonus of one stat
    void hammer_wheel_pulse      (const ActiveEs& w);             // Hammer Spin's hit on those around us
    const EsaRow* hammer_wheel_row(const SkillDef& d, uint8_t level, const EsaRow* act) const; // 4909's row when learned
    void bear_party_spread       (const SkillDef& d, uint8_t level, int32_t value); // Party Guard / Best Shield on the party
    void bear_party_es           (uint16_t skill, uint8_t level, uint16_t caster, int32_t value); // (on the member's strand)
    float spin_speed_{ 0.0f };                                    // Hammer Spin's RunSpeed while it turns (0 = none)
    // The Crazy Hammer chain (4801 + its passives 4804 / 4806 / 4908 / 4910; skill_bear.cpp): the gauge the client
    // holds while 4801 is learned (ESACrazyHammer 1532) and the Crazy mode it fills into (ESACrazyHammerDo 1533),
    // with the values the mode took from the learned rows when it started.
    struct CrazyHammer {
        uint16_t gauge_id{ 0 };    // the gauge's ES object id on the client (0 = no gauge)
        int32_t  gauge{ 0 };       // its fill
        int32_t  max{ 0 };         // GaugeP
        uint16_t do_id{ 0 };       // Crazy mode's ES object id (0 = off)
        int32_t  left_ms{ 0 };     // Crazy mode's time left
        float    ap_ratio{ 0.0f }; // 4801 Ratio + 4806 Ratio: the normal attack's AP + (base + items) * this
        int32_t  dx{ 0 };          // 4801 DX: the attack cooldown's DX is -dx
        int32_t  crit{ 0 };        // 4908 CriticalRatio * 100: the crit chance's + (before its 90 cap)
        float    speed{ 0.0f };    // 4804 RunSpeed: the run / walk speed * (1 + this)
        int32_t  shock_n{ 0 };     // 4910 KillNum / Radius: the shockwave around a landed blow's target
        int32_t  shock_r{ 0 };
    };
    CrazyHammer crazy_;
    const EsaRow* learned_row    (int skill, uint8_t* level = nullptr) const; // a learned skill's ESAction row
    void crazy_init              ();                              // the gauge when 4801 is learned (login / learn)
    void crazy_add               (bool skill);                    // AtPoint (a landed blow) / SkillPoint (a skill)
    void crazy_start             ();                              // the gauge full: Crazy mode (1533)
    void crazy_end               (bool send_end);                 // its time over / death: S2C 280, speeds back
    void crazy_tick              (int elapsed_ms);
    void crazy_carry_out         ();                              // a zone change: the state goes along
    void crazy_restore_carry     ();                              // ... and is taken back (156)
    int  crazy_swing_ap          () const;                        // the normal attack's AP (Crazy mode or not)
    void crazy_shockwave         (const MonsterEntity& main, int32_t mx, int32_t my, int ap,
                                  std::chrono::steady_clock::time_point now); // 4910
    // A damage skill's DamageInfo (the 28 bytes of S2C 0x002A, sub_5F5940).
    struct SkillDamage {
        int32_t dmg{ 0 };
        int32_t death_type{ 2 };       // [1]
        int32_t result{ 1 };           // [2] 0 hit, 1 miss, 3 crit, |0x80 fatal
        int32_t element{ 0 };          // [3] (set by sub_5E6760 on a hit)
        int32_t flags{ 0 };            // [4]
        int32_t extra{ 0 };            // [6] (+24 u16)
        int32_t absorbed{ 0 };         // a resist over 100: the target heals this (event 30182)
    };
    void absorb_heal             (MonsterEntity& m, int32_t amount); // 30182: HP += amount, S2C 719
    SkillDamage skill_phys_damage (const MonsterEntity& m, int ap2, float apply_ratio, int element,
                                   int dp_arg = -1);              // sub_5E6040 (its DP argument; -1 = the target's)
    SkillDamage skill_gun_damage  (const MonsterEntity& m, int gun_ap, float ratio, float apply_ratio); // sub_5E53C0
    SkillDamage skill_magic_damage(const MonsterEntity& m, int base, float apply_ratio, int ma_offset,
                                   int element);                 // sub_5E6270
    void esa_magic_arrow         (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1082 (sub_5C3630)
    void skill_resist_hit        (const MonsterEntity& m, int element, SkillDamage& h); // sub_5E6760
    int32_t equipped_gun_ap      () const;                        // the slot-1 gun's AP (10012 else ItemParam)
    PacketBuffer skill_opener    (const SkillDef& d, uint16_t second_id, uint16_t es_id = 0) const; // S2C 224 header (sub_5EE050 / sub_5EDE00)
    void mon_speech              (const MonsterEntity& m, int situation, std::vector<PacketBuffer>& group) const; // S2C 184
    void mon_speech_now          (const MonsterEntity& m, int situation); // ... to its area at once
    bool skill_damage_event      (MonsterEntity& m, const SkillDamage& h, std::vector<PacketBuffer>& group,
                                  uint16_t opcode = 0x002A);      // 30002 (30330: S2C 1307)
    void skill_after_hit         (MonsterEntity& m, int32_t dmg, int32_t hp_before, bool killed, uint16_t tm_skill,
                                  bool exp_done, std::chrono::steady_clock::time_point now, int exp_kind = 0);
    void esa_phys_strike         (const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now,
                                  int variant);                  // 1080 / 1100 / 1123 / 1296 / 1299
    void skill_use_tm            (uint16_t skill);                // sub_891DC0: the skill's SkillExp as TM
    void skill_hit_tm            (const MonsterEntity& m, uint16_t skill); // sub_891C80: TM per skill hit
    void skill_hit_tm_level      (int target_level, uint16_t skill);       // sub_891C80 by the target's level
    bool skill_target_pos        (uint16_t id, std::chrono::steady_clock::time_point now, int32_t& x, int32_t& y);
    void esa_tornado_attack      (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1103 (sub_5C4A10)
    void esa_heal                (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1085 (sub_5C2BB0)
    void esa_fairy_of_magic      (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1297 (sub_5D63D0)
    void esa_item_detector       (const SkillDef& d, const EsaRow* act);       // ESAction 1147 (sub_5C7F40)
    void esa_firework            (const SkillDef& d, int mask);  // 1121 / 1124 / 1137 / 1179 / 1180 / 1181 / 1197 / 1291 / 1307
    void esa_heart_bomb          (const SkillDef& d);            // 1222 (sub_5C5280)
    void esa_basic_detector      (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1170 (sub_5C9430)
    bool area_target_ok          (const MonsterEntity& m) const;  // the area skills' monster test (sub_5C3E10)
    bool monster_debuff_immune   (const MonsterEntity& m, uint32_t code, uint32_t& bits); // sub_891F80
    void send_debuff_resisted    (const MonsterEntity& m, uint32_t bits);                // S2C 1247 (sub_60AA00)
    void steel_punch_passives    (MonsterEntity& m);              // sub_5C24E0: Faint 1201 / Guard Break 1101
    void skill_hit_tm_obj        (uint16_t id, uint16_t skill);   // sub_891C80 on any object of ours
    void esa_magic_ring          (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1086 (sub_5C39C0)
    void esa_magic_storm         (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1087 (sub_5C4110)
    void esa_prevent_slow        (const SkillDef& d, uint8_t lv, const EsaRow* act,
                                  int es_action);                 // ESAction 1328 / 1269 / 1313 / 1314 (sub_5D8B50 ...)
    void esa_reckless            (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1277 (Doctor's Berserk, sub_5D5410)
    void esa_bunker              (const SkillDef& d, uint8_t lv, const EsaRow* act); // ESAction 1491 (sub_5DFF40)
    void esa_bunker_release      (const SkillDef& d);             // ESAction 1497 (sub_5E0020)
    void optical_scan            (ActiveEs& e);                   // ESAOpticalEyes' look-around (sub_573D90)
    void esa_uppercut            (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1167 (sub_5C8D90)
    void esa_chain_punch         (const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now,
                                  bool doubling);                // 1195 (sub_5CB400) / 1327 (sub_5D8630)
    void esa_stun_cancel         (const SkillDef& d, const EsaRow* act);        // ESAction 1469 (sub_5DDCD0)
    void esa_sharp_brandish      (const SkillDef& d, uint8_t lv, const EsaRow* act); // ESASharpBrandish 1145 (sub_55FA10)
    // The Warrior's sword strikes (skill_power.cpp).
    std::vector<MonsterEntity*> area_monsters(int32_t cx, int32_t cy, int radius, int max,
                                              std::chrono::steady_clock::time_point now); // sub_53E360 (monsters)
    int16_t elem_slot            (int i) const;                   // Character +836 + 2i of ours (sub_718280)
    void esa_fast_sword          (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1218 (sub_5CEB50)
    void esa_power_brandish      (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1244 (sub_5D0180)
    void esa_blast_brandish      (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1310 (sub_5D6C40)
    void esa_wind_brandish       (const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now,
                                  int cap, double wind_mult);    // ESAction 1325 (sub_5D8100) / 1336 (sub_5D9BF0)
    void esa_earthquake_sword    (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1352 (sub_5DC340)
    void esa_electric_sword      (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1470 (sub_5DDE00)
    void es_electric_stun        (MonsterEntity& m, const SkillDef& d, const EsaRow* act); // ESAElectricSwordDo 1471
    // The Champion (skill_power.cpp).
    void esa_fighting_fire       (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1335 (sub_5DA160)
    void esa_physical_match      (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1481 (sub_5DF0B0)
    void esa_flash_hit           (const ActiveEs& e, bool applied);            // ESAction 1276 (sub_5D5AA0)
    // The Gladiator (skill_power.cpp).
    void esa_power_sea_brandish  (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1261 (sub_5D46D0)
    void esa_ones_other_self     (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1262 (sub_5D3A20)
    void esa_fluorescene         (const SkillDef& d, uint8_t lv, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1287 (sub_5D44C0)
    void es_fluorescene_do       (MonsterEntity& m, const SkillDef& d, const EsaRow* act); // ESAFluoresceneDo 1288
    void esa_self_cure           (const SkillDef& d, uint8_t lv, const EsaRow* act); // ESAction 1482 (sub_5DF3F0)
    int  chi_sword_range         () const;                        // ESAEnergySword's Range while it is on
    void elem_es_add             (int slot, int16_t v);           // +924 += v, S2C 826 [1][slot][+924] (sub_602E90)
    void element_up_apply        (const ActiveEs& e, bool on);    // ESAElementUpDo sub_5B75D0
    bool chi_sword_on            () const;                        // +0x148 flag 64 (ESAEnergySword on)
    void es_on_damaged           ();                              // the ESs' hook of a damage event on us
    void es_run_to               (int16_t px, int16_t py, float speed,
                                  std::chrono::steady_clock::time_point now);  // a dash ES's run (sub_55C320)
    void esa_weapon_throw        (const SkillDef& d, uint8_t lv, const EsaRow* act); // ESAWeaponThrow 1111 (sub_552810)
    void esa_gellder_hit         (const SkillDef& d, uint8_t lv, const EsaRow* act); // ESAGellderHit 1116 (sub_553D00)
    void es_push_timer           (const SkillDef& d, uint8_t lv, uint16_t id, int32_t ms); // the object of such an ES
    void consume_throw_item      (int n);                         // sub_894600: n of the Throw item (+4608)
    void cast_es_tick            (int elapsed_ms);                // our CastEs' state machines
    void cast_es_end             (size_t i);                      // vtbl+28: S2C 280 + the class's vtbl+100
    void cast_es_end_all         ();                              // we leave: they end
    void storm_hit_damage        (CastEs& e);                     // ESAMagicStormHit sub_54CF60
    // The buried items a detector finds around (x, y) (sub_543A40 / sub_5C8070 / sub_5C9550): at most 50.
    std::vector<std::pair<int16_t, int16_t>> detect_buried(int16_t x, int16_t y, int radius, int reach);
    // Effect states (es.cpp)
    void es_apply_skill          (const SkillDef& d, uint8_t level, const EsaRow* act); // sub_524950 / sub_524690
    void es_apply_to_monster     (const SkillDef& d, uint8_t level, const EsaRow* act); // sub_524F60 on a monster
    void es_apply_on             (MonsterEntity& m, const SkillDef& d, uint8_t level, const EsaRow* act); // sub_524690
    bool may_attack_owned        (const MonsterEntity& m) const;  // Player vtbl+164 (sub_76CF50)
    bool owned_by_other          (const MonsterEntity& m) const;  // an area skill skips it (owner, sub_716EF0)
    // The Bard / Spiritualist (skill_bard.cpp).
    void esa_water_ball          (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1156 (sub_5C7180)
    void passive_follow_es       (MonsterEntity& m, int passive, int esaction); // sub_5C7500 / sub_55F480
    void esa_earth_divide        (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1159 (sub_5C7970)
    void esa_rock_summons        (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1184 (sub_5C9FC0)
    void esa_fire_explosion      (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1185 (sub_5CCCD0)
    void whirlwind_pulse         (const ActiveEs& e);             // ESAWhirlWind's 2000 ms hits (sub_5662E0)
    bool bard_debuff_on          (MonsterEntity& m, MonsterEntity::Es& e, const SkillDef& d, const EsaRow* act);
                                                                  // 1315 / 1316 / 1355's own inits
    void tesla_hit               (const MonsterEntity::Es& e);    // ESAElectricField's pulse -> ESAction 1317
    void esa_sharp_wind          (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1321 (sub_5D77E0)
    void esa_call_lightning      (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1356 (sub_5DB4D0)
    void esa_fire_burn           (const SkillDef& d, uint8_t lv, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1318 (sub_5D7420)
    void esa_element_up          (const SkillDef& d, uint8_t lv, const EsaRow* act); // ESAction 1472 (sub_5DE390)
    void element_up_from         (uint16_t caster, uint16_t skill, uint8_t lv, int16_t zone); // ... its 1473 on us
    void burn_hit                (uint16_t mob, int32_t dmg, uint16_t skill); // ESAFireBurnDo's burn (sub_5B6A50)
    void field_pulse_es          (size_t i);                      // ESAFireField / ESABreakdownGround's 1000 ms pulse
    void field_end_children      (const ActiveEs& e);             // their end: the Do on each one ends
    void field_burn              (uint16_t mob, uint8_t level);   // ESAFireFieldDo's burn (sub_5847B0)
    void gale_break_hit          (ActiveEs& e);                   // ESAGaleBreak's hit (sub_586D50)
    void esa_electric_spear      (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1263 (sub_5D4AC0)
    void esa_fire_dragon         (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1264 (sub_5D4DE0)
    void esa_earthquake          (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1265 (sub_5D4F90)
    void esa_mustang_wind        (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1284 (sub_5D5800)
    void esa_dimension_crack     (const SkillDef& d, uint8_t lv, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1484 (sub_5DF510)
    bool dimension_do_on         (MonsterEntity& m, MonsterEntity::Es& e);   // ESADimensionDo's init (sub_5B99A0)
    void elem_area_pulse         (size_t i);                      // ESAWaterArea..'s 1000 ms pulse (sub_589380)
    void elem_area_end           (const ActiveEs& e);             // their end (sub_5899F0): the Do on each one ends
    bool area_do_on              (MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act); // ESA*AreaDo (sub_58A890)
    // The blessings' handlers of a damage event on us (sub_581070 ..: at the head of Character::OnMessage 30002):
    // each counter's hit on the attacker into `group` (S2C 923), its aftermath into `hits`.
    struct CounterHit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };
    // The Sorcerer / Priest / Dark Lord (skill_dragon.cpp).
    void esa_dark_light_bolt     (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1163 / 1164 / 1187
    bool cruel_flame_on          (MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act); // 1178 (sub_561090)
    void cruel_burn              (uint16_t mob, uint16_t es_id);  // ESACruelFlameDo's burn (sub_5614B0)
    void esa_first_aid           (const SkillDef& d, const EsaRow* act);       // ESAction 1168 (sub_5C92A0)
    void esa_light_wave          (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1190 (sub_5CA8F0)
    void esa_gravity             (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1215 (sub_5CDF40)
    bool unable_recovery_on      (MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act); // 1332 (sub_526BB0)
    void esa_primary_cure        (const SkillDef& d, const EsaRow* act);       // ESAction 1219 (sub_5CF880)
    bool cure_from               (uint16_t caster, const std::vector<int>& list, const SkillDef& d,
                                  uint16_t skill);                // Basic Healing / Med. Cure from another, on our
                                                                  //   strand (true: cured)
    void esa_shining_ray         (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1256 (sub_5CF9F0)
    void esa_wide_first_aid      (const SkillDef& d, const EsaRow* act);       // ESAction 1323 (sub_5D7E80)
    void grace_heal              (PacketBuffer o, uint16_t caster, uint16_t skill, float ratio, int32_t ma_part,
                                  int16_t zone);                  // ... its heal on us
    void esa_revival             (const SkillDef& d, const EsaRow* act);       // ESAction 1474 (sub_5DE620)
    void bind_children           (ActiveEs& e);                   // ESABind's ESABindDo on those around (sub_534410)
    bool bind_do_on              (MonsterEntity& m, MonsterEntity::Es& e);   // ESABindDo's init (sub_55AF80)
    bool dark_slow_on            (MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act); // 1322 (sub_57F7F0)
    void barrier_send_back       (MonsterEntity& m, const int32_t dinfo[7], std::vector<PacketBuffer>* group,
                                  std::vector<CounterHit>* hits);  // ESADarkBarrier's 30002 back
    void bloody_on_dealt         (const SkillDamage& h);          // ESABloodyOath's 30097 handler (sub_5865C0)
    bool dark_ghost_on_dealt     (MonsterEntity& m, const SkillDamage& h, std::vector<PacketBuffer>& group); // sub_5839C0
    void esa_med_cure            (const SkillDef& d, const EsaRow* act);       // ESAction 1259 (sub_5D33B0)
    void esa_light_weight        (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1343 (sub_5DAC70)
    void esa_settle_down         (const SkillDef& d, uint8_t lv, const EsaRow* act); // ESAction 1486 (sub_5DFA50)
    void esa_death_scythe        (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1282 / 1283
    void esa_dark_up             (const SkillDef& d, uint8_t lv, const EsaRow* act); // ESAction 1488 (sub_5DFC10)
    bool fear_on                 (MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act); // ESAFearDo 1279
    void dark_whisper_pulse      (size_t i);                      // ESADarkWhisper's 2000 ms hits (sub_58BF90)
    void es_on_item_use          ();                              // the message 87 (C2S 0x57) to our ESs
    // The Entertainer / Prima Donna (skill_cat.cpp).
    void esa_volley_kick         (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1139 (sub_5C6DC0)
    void esa_multi_gellder_hit   (const SkillDef& d, uint8_t lv, const EsaRow* act); // ESAMultiGellderHit 1165 (sub_55DC80)
    void esa_siren               (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1199 (sub_5CC380)
    void esa_rush                (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1216 (Beast Claw, sub_5CE8D0)
    void esa_gellder_chain_hit   (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1329 (sub_5D9280)
    void esa_smell_ex            (const SkillDef& d, uint8_t lv, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1364 (Deadly Funk, sub_5DBD20)
    void esa_buff_cancler        (const SkillDef& d, uint8_t lv, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1477 (Aura Shatter, sub_5DEE50)
    bool buff_cancler_on         (MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act); // ESABuffCanclerDo 1478
    void buff_cancler_hit        (uint16_t mob, uint16_t es_id);  // its pulse (sub_5B7E80)
    bool wink_on                 (MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act); // ESAWinkOfCharm 1273
    void esa_two_chain_attack    (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1258 (2 Hit Combo, sub_5D2E30)
    void esa_wild_talon          (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1346 (Raging Nail, sub_5DA540)
    void esa_sharp_scream        (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1381 (sub_5DCD10)
    void esa_beast               (const SkillDef& d, uint8_t lv, const EsaRow* act); // ESAction 1493 (sub_5E00A0)
    bool mutation_start          (ActiveEs& e, const EsaRow* act); // InitMutation / InitBeast / InitChange
    bool mutation_end            (const ActiveEs& e);             // EndMutation / EndBeast / EndChange
    void mutation_rebase         ();                              // the mutations' UpdatePlayerBaseStat (vtbl+108)
    void send_base_stats         (bool change_order = false);     // S2C 784 [5]{[stat][base]} (sub_602380)
    int32_t base_stat            (int i) const;                   // +704 + 4i (base_fixed_ / gulim13)
    void sharp_claw_bleed       (ActiveEs& e);                   // its 2000 ms bleed (sub_56A1E0)
    // The Card Master / Gambler (skill_card.cpp).
    void esa_card_throw          (const SkillDef& d, uint8_t lv, const EsaRow* act); // ESACardThrow 1150 (sub_55B4C0)
    void esa_one_pair            (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1194 (sub_5CBD30)
    void esa_charging            (const SkillDef& d, uint8_t lv, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1220 (Impelling Rage, sub_5CF220)
    void esa_wild_attack         (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1246 (Wild Nail, sub_5D0A40)
    void esa_mana_convert        (const SkillDef& d, const EsaRow* act);       // ESAction 1312 (Flux Capacitor, sub_5D6F60)
    void esa_lucky_gellder_hit   (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1330 (sub_5D9750)
    void esa_power_charging      (const SkillDef& d, uint8_t lv, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1271 (sub_5D5160)
    void esa_massive_attack      (const SkillDef& d, uint8_t lv, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1347 (Heavy Hit, sub_5DA900)
    void esa_four_card           (const SkillDef& d, uint8_t lv, const EsaRow* act); // ESAction 1495 (sub_5E0180)
    bool four_card_init          (ActiveEs& e, std::vector<uint8_t>& tail);   // ESAFourCardDo's init (sub_5BB710)
    void four_card_hits          (const ActiveEs& e, const EsaRow* act, std::vector<PacketBuffer>& group,
                                  std::vector<CounterHit>& hits);              // its hits (sub_5BBB10)
    bool card_es_on              (MonsterEntity& m, MonsterEntity::Es& e, const SkillDef& d, const EsaRow* act);
                                                                  // the 1221 / 1272 / 1348 / 1281 / 1382 inits
    void soldier_hit             (uint16_t mob, uint16_t es_id);  // ESASummonSoldier's hit (sub_58C9E0)
    void kamikaze_boom           (const ActiveEs& e);             // ESAKamikaze's boom (sub_5B85A0)
    void self_damage_event       (SkillDamage h, std::chrono::steady_clock::time_point now); // 30002 on us from us
    void full_house_mark         ();                              // ESAFullHouse's server-wide spot (sub_56F860)
    void full_house_burst        (const ActiveEs& e);             // its hits past 1000 ms (sub_56FA20)
    void player_die_packets      (std::vector<PacketBuffer>& group, int32_t pcx, int32_t pcy, uint8_t death_type = 0);
    void player_on_die           (int32_t pcx, int32_t pcy);      // Player::OnDie (sub_73BFA0)
    bool es_base_tick            (size_t i, int elapsed_ms);      // sub_50FF90 on es_[i] (false: it ended)
    // The skills of every class (skill_gm.cpp).
    void esa_card_identify       (const SkillDef& d, std::chrono::steady_clock::time_point now); // 1393 (sub_5DD600)
    void esa_gm_cheer            (const SkillDef& d);             // ESAction 1452 / 1455 GM's Support (sub_5DD6C0)
    void esa_gm_dignity          (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1454 / 1457 (sub_5DD720)
    void gm_cheer_from           (uint16_t caster, int skill, int16_t zone); // its Do made on us (on our strand)
    void gm_cheer_update         ();                              // sub_597210: recompute + S2C 283 [8]
    bool gm_cheer_on             (MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act); // its Do on a monster
    // The Treasure Hunter / Thief Master (skill_thief.cpp).
    void esa_heavy_baggage       (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1214 (sub_5CD6C0)
    void esa_adhesive            (const SkillDef& d, uint8_t lv, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1242 (sub_5D0DC0)
    void esa_wide_throw          (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1326 (sub_5D8D80)
    void esa_chain_throw         (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1362 (sub_5DC6A0)
    void esa_up_throw            (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1475 (sub_5DE6E0)
    void esa_sudden_attack       (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1290 (sub_5D5CE0)
    void esa_poison_baggage      (const SkillDef& d, uint8_t lv, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1344 (sub_5DB030)
    void esa_poison_assault      (const SkillDef& d, uint8_t lv, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1383 (sub_5DD020)
    void esa_crack_attack        (const SkillDef& d, const EsaRow* act,
                                  std::chrono::steady_clock::time_point now);  // ESAction 1490 (sub_5DFCF0)
    bool mine_tick               (size_t i, int elapsed_ms);      // ESAMine / ESAMineMaster sub_565600 (false: it ended)
    void mine_detonate           (const ActiveEs& mine, MonsterEntity& m);   // ESAction 1193 / 1268 (sub_5CD450 / sub_5D37A0)
    bool poison_do_on            (MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act); // 1350 / 1384 inits
    void poison_hit              (uint16_t mob, uint16_t es_id);  // ESAPoisonBaggageDo / ESAPoisonAssaultDo pulses
    bool bear_debuff_on          (MonsterEntity& m, MonsterEntity::Es& e, const SkillDef& d, const EsaRow* act); // 1526 / 1537 / 1543
    void mole_hammer_hit         (uint16_t mob, uint16_t es_id);  // ESABbongHammer's pulse (skill_bear.cpp)
    void hide_set                (bool on);                       // Character +452 bit 0 + S2C 827 (sub_570DA0 / sub_570FC0)
    void hide_break              ();                              // a hidden one's limiter vtbl+60(1286)
    float precise_pitch_ratio    (bool retail_inverted) const;    // Precise Pitch's (3101) Ratio while 1146 is up
    int32_t throw_item_count     () const;                        // the Throw item's amount (Player +4608 vtbl+48)
    int32_t bag_weight_capped    () const;                        // min(the bag's weight, MaxWT) (+1716 / +584)
    float cd_ratio_{ 1.0f };           // Player +3036 + 68: the cool times' speed (ESASettleDownDo, sub_88F9C0)
    void counter_on_hit          (MonsterEntity& m, const int32_t in[7], size_t k, std::vector<PacketBuffer>& group,
                                  std::vector<CounterHit>& hits);
    bool counter_damage_event    (MonsterEntity& m, const SkillDamage& h, std::vector<PacketBuffer>& group); // 30248
    bool es_limiter_admit        (int esclass, int esaction, uint16_t skill); // PlayerESLimiter vtbl+40 (sub_5E1CE0)
    bool es_create               (const SkillDef& d, uint8_t level, const EsaRow* act, EsKind kind, bool limited,
                                  uint16_t flags, int32_t init_life = 0, uint16_t caster = 0);
                                                                  // zone sub_7FD4E0: false = no object; `caster` = another
                                                                  //   player's object id when its skill made it on us
    void es_aura_pulse           (size_t i);                      // an aura's 400 ms pulse (e.g. sub_555C00)
    void aura_child_from         (uint16_t caster, uint16_t skill, uint8_t level, int child, int32_t life_ms,
                                  int32_t count, int16_t zone);   // ... on us: its event, or the child made
    std::vector<GameClientSession*> party_in_radius(int32_t radius, size_t max);   // sub_58B9F0 (us included)
    void whiteknight_cut         (const ActiveEs& e, int32_t& dmg, int32_t flags); // ESAWhiteKnightDo's 30002
    void party_action_from       (uint16_t caster, uint16_t skill, uint8_t level, int esaction,
                                  int16_t zone);                  // 1386 / 1388 / 1487 on us
    bool es_berserk_seek         (ActiveEs& e, int elapsed_ms);   // ESABerserk sub_561CD0 (false: it ended)
    bool es_berserk_drain        (ActiveEs& e);                   // ESABerserk sub_562450 (false: it ends)
    int32_t regen_amount         (int kind, int32_t rate) const;  // ResChain 2 (sub_717030): HP 0 / MP 1
    int32_t mp_discount          (int32_t cost) const;            // the discount chain of kind 1 (sub_717290)
    void es_scroll_do            (const SkillDef& d, uint8_t level, const EsaRow* act); // ESAStatUpScroll's Do (1388)
    void es_scroll_update        (const ActiveEs& e);             // ESAStatUpScrollDo sub_58E210: recompute + S2C 283
    void scroll_use_up           (int esclass, int esaction);     // a scroll's ES made: UseItem on Player +0xC94
    void es_tick                 (int elapsed_ms);                // each ES's vtbl+56 with the zone's elapsed time
    void es_end                  (size_t i);                      // vtbl+28 (sub_50F770): S2C 280 + the class's end
    void es_end_all              ();                              // the player leaves the world
    void es_carry_out            (int zone_to);                   // a zone change: the limiter's ESs go along (sub_5E2210)
    void es_restore_carry        ();                              // ... and are made again (sub_5E2360)
    void es_end_death_lists      ();                              // OnDie: the ESLimiter lists 0, 1 and 7 end
    void third_eye_on            ();                              // event 30280 (Character sub_715A90)
    void third_eye_off           ();                              // event 30281: off, and a shadow target is dropped
    const ActiveEs* es_find      (int esaction) const;            // PlayerESLimiter vtbl+84 (sub_5E2740)
    bool es_remove_esaction      (int esaction);                  // PlayerESLimiter vtbl+60 (sub_5E28A0)
    int32_t es_stat_bonus        (int stat, float base_item) const; // ResChain 9 (sub_716820)
    void es_stat_update          (int stat);                      // vtbl+160 recompute + S2C 283 [1][stat][bonus]
    void es_light_shield_update  ();                              // sub_56CE40: recompute + S2C 283 [2][5][..][10][..]
    void kill_monster_aftermath  (MonsterEntity& m, uint16_t eid, int32_t x, int32_t y); // corpse + end follows
    const LearnedSkill* find_skill(int skill) const;              // sub_8923C0: a learned skill or null
    bool can_learn_skill         (int skill);                     // Player::CanLearnSkill sub_760760 (S2C 0x01E4 on a no)
    bool learn_skill             (int skill, int level, int tm_points); // sub_5FD100 + sub_8922B0 + sub_5F22F0
    void send_system_msg         (uint8_t type, uint16_t a, uint16_t b); // S2C 0x01E4 [u8][u16][u16] (sub_5FD2E0)
    // EXP / TM (progress.cpp)
    void mark_monster_owner      (MonsterEntity& m);              // owner mark (sub_72B2F0) + S2C 365
    void gain_attack_exp         (const MonsterEntity& m, int32_t dmg, int32_t hp_before,
                                  int exp_kind = 0, bool skip_zero_share = false); // event 30003 (first field = the kind)
    void gain_exp_share          (const MonsterEntity& m, int32_t share, int exp_kind); // event 30003 with its value
    bool add_exp                 (int32_t amount, bool rates = false); // AddExp sub_716A80 (true = handled/level-up)
    void grant_levels            (int gained);                    // level-up vtbl+232 (sub_75D390) + S2C 0x27
    void kill_tm_reward          (const MonsterEntity& m);        // the owner's TM on a kill (sub_891AB0)
    void owner_kill_tm           (int32_t owner_hp, int32_t hp_max, int32_t tm_reward, int mob_level); // (as the owner)
    bool add_tm_exp              (int32_t amount, bool rates = false); // AddTMExp sub_716C30 (true = TM level-up)
    int32_t rate_exp             (int32_t base) const;            // the rates (+5352): party bonus + share
    int32_t rate_tm              (int32_t base) const;
    void send_tm_level_up        ();                              // S2C 0x00F5 (sub_5ED5A0)
    // Inventory / pick-up (inventory.cpp)
    void load_inventory          ();                              // char_inventory -> bag_ (+ weight)
    void send_item_list          ();                              // S2C 0x00E7 at login (sub_5F69D0)
    void handle_pickup           (PacketBuffer& pkt);             // C2S 0x0021 [u16 item][u8 moveType]
    void pickup_tick             (std::chrono::steady_clock::time_point now); // the walk ended -> 30008
    void do_pickup               ();                              // Player state 4, event 30008
    int  acceptable_count        (const ItemDef& d, int count) const;   // Player::GetAcceptableCount
    bool can_add_item            (const ItemDef& d, int count) const;   // inventory sub_6DAEA0
    // The record + bag/DB update of an item entering the bag (sub_6DB7C0): `from` = the ground item it
    // comes from (a rare item a player threw away keeps its uid, bytes and options).
    BagRecord bag_add            (const ItemDef& d, uint16_t count, const GroundItemEntity* from = nullptr);
    BagRecord bag_new            (const ItemDef& d, uint16_t count, const GroundItemEntity* from = nullptr); // never stacks
    void send_bag_record         (const BagRecord& r);            // S2C 0x00A0 (sub_5F7160)
    void write_bag_record        (PacketBuffer& p, const BagRecord& r) const; // the record (sub_60C220)
    // NPC shops (shop.cpp)
    void handle_shop_open        (PacketBuffer& pkt);             // C2S 0x0046 [u16 shop] (Player case 70)
    void handle_shop_list        (PacketBuffer& pkt, bool sell);  // C2S 0x00A4 / 0x00A6 (shop state cases 164 / 166)
    void handle_shop_buy         (PacketBuffer& pkt);             // C2S 0x0048 (shop state case 72)
    void handle_shop_sell        (PacketBuffer& pkt);             // C2S 0x039F inner 76 (shop state case 76)
    void handle_shop_close       ();                              // C2S 0x004F (shop state case 79)
    int32_t shop_buy_price       (const ShopRow& s, uint32_t item_id, bool round = true) const; // sub_720160 / sub_721480
    int32_t shop_sell_price      (const ShopRow& s, const BagItem& b) const;   // sub_720210
    void send_money              ();                              // S2C 0x0029 [u32 money]
    // Drilling (drill.cpp)
    void handle_drill_start      (PacketBuffer& pkt);             // C2S 0x002C [u16 x][u16 y][u8 moveType] (case 44)
    void handle_drill_cancel     (PacketBuffer& pkt);             // C2S 0x0030 [u16 cnt][u16] (state 3 case 48)
    void handle_drill_end        (PacketBuffer& pkt);             // C2S 0x0031 CMD_END_DRILL (state 3 case 49)
    void drill_tick              (std::chrono::steady_clock::time_point now); // the walk ended -> 30008
    void drill_on_stop_move      ();                              // DrillState::OnStopMove (state 3, 30008)
    void drill_stop              (int reason, int life, uint32_t item); // sub_75C4A0: S2C 50 + 51, state 0
    void drill_interrupted       ();                              // state 3, event 30002: hit while drilling
    void drill_exp               (int grade, int depth);          // event 30003 kind 6 (sub_5E7450)
    BagItem* equipped_drill      ();                              // slot 7, in the bag (sub_76ECB0 + sub_60C610)
    void drill_do_crazy          ();                              // Player::DoDrill (sub_769640): a CDM dig
    bool cdm_enabled             () const;                        // +60 or skill 5019 (sub_7B0490 / sub_54BA80)
    void cdm_add                 (float add);                     // sub_7AFC80: the gauge
    void cdm_activate            (int how);                       // sub_7AFA20
    void cdm_setup               ();                              // sub_7AFFC0: the drill's CDM values
    void cdm_tick                (int elapsed_ms);                // sub_7AFE50 (the Player tick)
    void cdm_on_death            ();                              // sub_7B0230 (Player::OnDie)
    void cdm_on_equip_drill      (const BagItem& drill);          // DrillItemHelper::OnEquip (sub_6E6EF0)
    int  cdm_gauge_percent       () const;                        // S2C 371's last byte
    void cdm_restore_carry       ();                              // sub_7B0520 after a zone change
    BagItem* bag_find_uid        (uint64_t uid);
    BagItem* bag_find_id         (uint32_t item_id);              // the first item of that id (sub_60D480)
    BagRecord bag_decrease       (const BagItem& b, uint16_t count); // sub_6E4460 / sub_6DB8D0: the record
    void bag_apply               (const BagRecord& r);            // sub_6DBA50: the record on the bag (+ weight)
    void handle_secure_command   (PacketBuffer& pkt);             // C2S 0x039F [u16 op][cstr pwd][payload]
    void handle_drop_item        (PacketBuffer& pkt);             // inner 0x0043 CMD_DROP_ITEM (hItemDrop)
    // Equipment (equip.cpp)
    bool is_equipped             (uint64_t uid) const;
    int  equip_slot_for          (const ItemDef& d) const;        // ConvertToEquipType + accessory slots (sub_6DA520)
    bool verify_equip            (const BagItem& b, const ItemDef& d, int slot) const; // Player::VerifyEquipItem
    void handle_equip_item       (PacketBuffer& pkt);             // C2S 0x0051 CMD_EQUIP_ITEM [u64 uid][u8 slot]
    void handle_release_item     (PacketBuffer& pkt);             // C2S 0x0054 CMD_RELEASE_ITEM [u64 uid][u8 slot]
    void handle_equip_common     (PacketBuffer& pkt);             // C2S 0x0205 CMD_EQUIP_COMMON_ITEM
    void handle_release_common   (PacketBuffer& pkt);             // C2S 0x0208 CMD_RELEASE_COMMON_ITEM
    bool verify_equip_common     (const BagItem& b, const ItemDef& d, int slot) const; // sub_762720
    bool verify_release_common   (const BagItem& b, const ItemDef& d, int slot) const; // sub_762EF0
    void equip_n_release         (int slot, const std::vector<uint64_t>& release, uint64_t equip_uid); // sub_763FD0
    void death_unequip           ();                              // OnDie: a unique item of slots 0..2 may come off
    void gun_equip_changed       (bool gun_in, bool gun_out);     // the gun's equip effect (sub_717E70 / sub_760600)
    void equip_bonus             (int32_t out[12]) const;         // sub_718750 over the equipped items
    void write_equip_appearance  (PacketBuffer& p) const;         // sub_6DA070 + sub_73AF20
    // Item use (item_use.cpp)
    void handle_use_item         (PacketBuffer& pkt);             // C2S 0x0057 [u64 uid] (Player case 87)
    bool use_item                (BagItem& b, const ItemDef& d);  // Player::UseItem sub_75D850
    int  use_item_helper         (BagItem& b, const ItemDef& d);  // sub_6DFA70: the Type's helper -> 0/1/2/3
    int  use_skill_card          (BagItem& b, const ItemDef& d);  // UseSkilCardlHelper::Use (sub_6E7460)
    int  use_rebirth_item        (BagItem& b, const ItemDef& d);  // UseItemHelper::Use (sub_6E1AE0) case 29
    int  use_warehouse_slot_item (BagItem& b, const ItemDef& d);  // case 27: hItemAddWarehouseSlot (sub_6E44B0)
    int  use_item_helper_15      (BagItem& b, const ItemDef& d);  // UseItemHelper::Use (sub_6E1AE0): the Type 15 items
    int  use_present_item        (BagItem& b, const ItemDef& d);  // case 22: a gift box (hPresentItem sub_6E3F60)
    int  use_scroll_item         (BagItem& b, const ItemDef& d);  // SubType 69: a skill scroll / seal (sub_6E59C0)
    void handle_use_item_custom  (PacketBuffer& pkt);             // C2S 0x0108 [u64 uid][i16 n][n bytes] (case 264)
    int  use_reset_status        (BagItem& b, const ItemDef& d, bool all); // Effects 24 / 26 (hItemResetStatus)
    int  use_reset_graph         (BagItem& b, const ItemDef& d);  // Effect 25 (hItemResetGrowthGraph)
    int  use_hair_dye            (BagItem& b, const ItemDef& d);  // Effect 15 (hItemHairColor)
    int  use_char_type_change    (BagItem& b, const ItemDef& d);  // Effect 21 (hItemChangeChrType, Befana's elixir)
    int  use_megaphone_booster   (BagItem& b, const ItemDef& d);  // Effect 32 (hItemMegaphoneBooster, megaphone.cpp)
    int  use_rename_item         (BagItem& b, const ItemDef& d);  // Effect 31 (hItemChangeCharName)
    int  use_memory_port         (BagItem& b, const ItemDef& d);  // Effect 34 (hItemMemoryport, memory_port.cpp)
    const MemoryPortDb::Info* memory_port_info(uint64_t uid);     // sub_789AB0: a bag item's Item_MemoryPortParam
    void handle_memory_port_open (PacketBuffer& pkt);             // C2S 817 CMD_MEMORY_PORT_OPEN
    void handle_memory_port_add  (PacketBuffer& pkt);             // C2S 820 CMD_MEMORY_PORT_ADD
    void handle_memory_port_delete(PacketBuffer& pkt);            // C2S 823 CMD_MEMORY_PORT_DELETE
    int  use_reset_skill         (BagItem& b, const ItemDef& d);  // Effect 23 (hItemResetSkill)
    int  use_disguise_kit        (BagItem& b, const ItemDef& d);  // SubType 71 ('G'): TransScrollInfo
    void set_disguise_model      (int model);                     // sub_769530
    void disguise_end            (uint8_t effect);                // sub_7695B0 / sub_76AA00's body: S2C 1026 + 582
    void handle_disguise_cancel  (PacketBuffer& pkt);             // C2S 0x0401 (case 1025)
    void build_char_blob         (uint8_t blob[69]) const;        // the 69-byte char info (S2C 0x02A7 / 649)
    int  use_port_item           (BagItem& b, const ItemDef& d, bool custom); // Effects 19 / 28 / 14 / 20 / 39
    void item_port               (const BagItem& b, const ItemDef& d, int zone, int16_t x, int16_t y);
    bool zone_no_port            (int zone);                      // MapInfoEx Option 128: no item port from it
    void item_effect_block       (const BagItem& b, const ItemDef& d, const std::vector<uint8_t>& effect,
                                  std::vector<PacketBuffer> mine_after); // 0x0097 + 0x005B (+ ...) block
    // Guns (gun.cpp)
    const ItemDef* slot_item     (int slot) const;                // the item def in an equipment slot (sub_41C7D0)
    bool gun_in_hand             () const;                        // slot 1 holds a SubType 54 item (strategy 5)
    bool gun_in_any_slot         () const;                        // a SubType 54 item in 4/9/1/22 (sub_6DA700)
    const GunDef* equipped_gun   () const;                        // the gun in slot 1 (SubType 54) and its GunParam
    bool has_ps_ability          (int ability) const;             // m_ESLimiter (+4504) holds it (sub_5255F0)
    CharRange attack_range       () const;                        // actor +1040/+1044/+1048 (a gun's or CharacterInfo's)
    void handle_gun_seed         ();                              // C2S 0x01FF -> S2C 0x0200 [u16 seed]
    void handle_gun_attack       (PacketBuffer& pkt);             // C2S 0x01DD CMD_BEGIN_ATTACK_GUN
    void send_attack_cancel      (uint32_t why);                  // S2C 0x0136 [u32] (sub_5F59F0)
    void gun_follow_tick         (MonsterEntity& m, std::chrono::steady_clock::time_point now); // Normal follow + gun
    bool gun_shot                (MonsterEntity& m, std::chrono::steady_clock::time_point now); // GunAttackStrategy::DoAttack
    void consume_bullets         (int count);                     // sub_894560
    void handle_attack           (PacketBuffer& pkt, Opcode op);  // C2S 0x1D attack request / 0x120 target-info
    void start_attack            (uint16_t target_eid);           // (re)start the player's battle-follow
    void end_attack              (bool send_leave);               // stop the player's follow; send_leave => S2C 0x0020
    void send_target_info        (const MonsterEntity& m);        // answer C2S 0x0120 with S2C 0x0121 (HP/MP)
    void engage_monster          (MonsterEntity& m);              // the hit monster takes us as its target
public:
    void engage_by_aggro         (uint16_t eid);                  // an aggressive monster saw us (monster_ai.cpp)
    // A monster may take us as its target (sub_427CB0: not dead (state 2), in the world, not
    // leaving); our current position.
    bool ai_targetable(int32_t& x, int32_t& y) const
    {
        if (dead_ || leaving_ || state_ != GameClientSessionState::InWorld || !object_id_) return false;
        cur_pos(std::chrono::steady_clock::now(), x, y);
        return true;
    }
    bool third_eye() const { return third_eye_; }                  // Player +1628 (sub_426950)
    bool hiding() const { return hiding_; }                        // Character +452 bit 0 (sub_41C600)
    void poison_pulse(uint16_t mob, uint16_t es_id)              // ESAPoisonBaggageDo / ESAPoisonAssaultDo's pulse
    {
        run_on_strand([this, mob, es_id]() { poison_hit(mob, es_id); });
    }
    void mole_pulse(uint16_t mob, uint16_t es_id)                // ESABbongHammer's pulse (Mole Hammer, skill_bear.cpp)
    {
        run_on_strand([this, mob, es_id]() { mole_hammer_hit(mob, es_id); });
    }
    void buff_cancler_pulse(uint16_t mob, uint16_t es_id)        // ESABuffCanclerDo's pulse
    {
        run_on_strand([this, mob, es_id]() { buff_cancler_hit(mob, es_id); });
    }
    void gm_cheer_pulse(uint16_t caster, int skill, int16_t zone) // a GM's cheer (another player's skill) on us
    {
        run_on_strand([this, caster, skill, zone]() { gm_cheer_from(caster, skill, zone); });
    }
    // A party member's Heart's Grace reaching us (esa_wide_first_aid): run on our strand.
    void grace_heal_pulse(const PacketBuffer& o, uint16_t caster, uint16_t skill, float ratio, int32_t ma_part,
                          int16_t zone)
    {
        run_on_strand([this, o, caster, skill, ratio, ma_part, zone]() { grace_heal(o, caster, skill, ratio, ma_part, zone); });
    }
    // A party member's Sacred Oath / party stat scroll / Refreshing Dew reaching us (party_in_radius): on our strand.
    void party_action_pulse(uint16_t caster, uint16_t skill, uint8_t level, int esaction, int16_t zone)
    {
        run_on_strand([this, caster, skill, level, esaction, zone]() {
            party_action_from(caster, skill, level, esaction, zone);
        });
    }
    // A party member's Elemental Boost reaching us (esa_element_up): run on our strand.
    void element_up_pulse(uint16_t caster, uint16_t skill, uint8_t lv, int16_t zone)
    {
        run_on_strand([this, caster, skill, lv, zone]() { element_up_from(caster, skill, lv, zone); });
    }
    // A party member's aura pulse reaching us (es_aura_pulse): run on our strand.
    void aura_child_pulse(uint16_t caster, uint16_t skill, uint8_t level, int child, int32_t life_ms, int32_t count,
                          int16_t zone)
    {
        run_on_strand([this, caster, skill, level, child, life_ms, count, zone]() {
            aura_child_from(caster, skill, level, child, life_ms, count, zone);
        });
    }
    void soldier_pulse(uint16_t mob, uint16_t es_id)             // ESASummonSoldier's hit, the same way
    {
        run_on_strand([this, mob, es_id]() { soldier_hit(mob, es_id); });
    }
    // ESAElectricField's 2000 ms pulse, from the zone tick (monster_es_tick): run on our strand with a copy.
    void field_pulse(const MonsterEntity::Es& e)
    {
        run_on_strand([this, f = e]() { tesla_hit(f); });
    }
    void field_burn_pulse(uint16_t mob, uint8_t level)          // ESAFireFieldDo's 2000 ms burn, the same way
    {
        run_on_strand([this, mob, level]() { field_burn(mob, level); });
    }
    void burn_pulse(uint16_t mob, int32_t dmg, uint16_t skill)   // ESAFireBurnDo's 2000 ms burn, the same way
    {
        run_on_strand([this, mob, dmg, skill]() { burn_hit(mob, dmg, skill); });
    }
    void drop_mob_attack(uint16_t mob)                           // another's ES set that monster's state 0: its
    {                                                            //   fight with us is over (no packet)
        run_on_strand([this, mob]() { end_mob_attack(mob, false); });
    }
    void cruel_burn_pulse(uint16_t mob, uint16_t es_id)          // ESACruelFlameDo's 2000 ms burn
    {
        run_on_strand([this, mob, es_id]() { cruel_burn(mob, es_id); });
    }
    void skill_use_tm_later(uint16_t skill)                      // a skill's use TM after another's strand did it
    {
        run_on_strand([this, skill]() { skill_use_tm(skill); });
    }
    // Revival (ESAction 1474) on us from another player: the revive with the row's ratios (sub_764260); done -> S2C
    // 760 [u16 us][u16 0] to the area (sub_601C90) and the caster's use TM.
    void revive_by_skill(uint32_t caster, float hp, float mp, float exp, float tmxp, uint16_t skill);
private:
    bool engaged_with            (uint16_t eid) const;
    void end_mob_attack          (uint16_t eid, bool send_leave); // stop that monster's follow; send_leave => mob S2C 0x0020
    void end_all_mob_attacks     (bool send_leave);
    void ensure_world_tick       ();                              // start the 100 ms world tick if idle
    void world_tick              (uint32_t gen);                  // one world tick: recovery + both follows
    void regen_tick              (int elapsed_ms);                // HP/MP recovery (retail sub_73CF10)
    void player_follow_tick      (std::chrono::steady_clock::time_point now);  // LazyBattleFollowingStrategy
    void mob_follow_tick         (std::chrono::steady_clock::time_point now, int elapsed_ms); // Normal…
    bool mob_follow_one          (MobEngage& e, std::chrono::steady_clock::time_point now, int elapsed_ms);
    bool player_order            (MonsterEntity& m, unsigned gap_ms, uint8_t type, std::chrono::steady_clock::time_point now);
    bool mob_order               (MonsterEntity& m, BattleFollow& mf, unsigned gap_ms, uint8_t type,
                                  std::chrono::steady_clock::time_point now);
    uint8_t player_swing         (MonsterEntity& m, std::chrono::steady_clock::time_point now); // one melee swing block -> result
    uint8_t mob_swing            (MonsterEntity& m, std::chrono::steady_clock::time_point now); // the monster swings back -> result
    void cur_pos                 (std::chrono::steady_clock::time_point now, int32_t& x, int32_t& y) const;
    void handle_restart          (Opcode op);                     // death-screen restart: 0x0016 town / 0x02F7 in place
    void handle_action           (PacketBuffer& pkt);             // C2S 0x0040 [u8]: sit / rest / stand / emotion
    bool revive                  (float hp, float mp, float exp, float tmxp); // Player revive (sub_764260)
    void restore_lost            (float exp, float tmxp);         // sub_764100: the death's EXP / TM back
    void revive_done             ();                              // event 30200, 1500 ms after (sub_764460)
    void protect                 (int16_t ms);                    // sub_73CF80(ms, 0): +92 flag 1 for ms
    void protect_tick            (int elapsed_ms);                // the protection countdown (sub_73CFC0)
    void update_return_town      (bool at_login);                 // entering a town zone makes it the return town
    void broadcast_move          (uint16_t eid, int fx, int fy, int tx, int ty, uint8_t type); // S2C 0x0018 to the zone
    void broadcast_speed         (uint16_t eid, float speed);    // S2C 0x01D8 [u16 id][u8 speed/4] to the zone
    void recompute_stats         (bool clamp = true);             // final stats = gulim13[accpts/4+1][i] + equipment
    void init_accpts_from_growth (bool clamp = true);                              // accpts[i] = growth(group)*(level-1), then recompute
    void handle_stat_alloc       (PacketBuffer& pkt);             // C2S 0x006C: spend attr_pts into stats (12×i16 deltas)
    void write_stat_block        (PacketBuffer& p) const;         // the 33-byte stat block (retail sub_75CB40 layout)
    void send_stat_refresh       ();                              // S2C 0x0275 [block] -> client Apply_33byte_StatsBlock
    void send_level_up           ();                              // S2C 0x0027 [block][hp][mp][exp][u8] -> client level-up
    void save_stat_points        ();                              // persist accpts_ + attr_pts_ + tm_level_
    void handle_heartbeat        (PacketBuffer& pkt, Opcode op);
    void handle_chat             (PacketBuffer& pkt, Opcode op); // C2S 54 / 417 / 713; '@' -> atcommand
    void handle_party            (PacketBuffer& pkt, uint16_t op); // C2S 7001..7088 (party.cpp)
    PartyInfo party_info() const;                                 // our party record
    void party_tick(int elapsed_ms);                              // S2C 7040 every 500 ms
    void server_party_closed();
    int  party_sync_ms_{ 0 };
    uint8_t party_bonus_{ 0 };                                    // the party type bonus (party_type_bonus)
    uint8_t board_type_{ 0 };                                     // the board balloon over us (+3464, flag 7)
    std::string board_text_;                                      // its text (+3468)
    bool mega_on_{ false };                                       // the Mic's head notice (condition 8, megaphone.cpp)
    std::string mega_text_;                                       // its text (Player +3519, 161 chars)
    int  fsm_state               () const;                        // the Player FSM state as modelled (0 / 8 / 15 ...)
    void head_notice_end         ();                              // the conditions 2 / 3 / 8 end (sub_73CCD0 ...)
    void handle_megaphone        (PacketBuffer& pkt);             // C2S 800 CMD_MEGAPHONE (the Mic)
    int  acc_points(int i) const;                                 // accumulated points + the party's +4 (sub_717170)
    void send_points();                                           // S2C 0x006D [unspent][12 x points] (sub_5F4FF0)
    // ── The trade (trade.cpp) ─────────────────────────────────────────────
    void handle_trade            (PacketBuffer& pkt, uint16_t op); // C2S 94 / 97 / 99 / 100 / 101 / 102 / 171..179
    bool trade_blocks            (PacketBuffer& pkt, uint16_t op); // state 6: the commands it does not handle
    void trade_got_request       (uint16_t from, const std::string& name);  // S2C 94 + the link (+0xC7C)
    void trade_join              (uint16_t from);                 // event 30025 (the asked one said yes)
    void trade_joined            (uint16_t from, uint16_t id);    // event 30026
    void trade_event             (uint16_t from, int ev);         // events 30027 / 30028 / 30029
    void trade_leave             ();                              // out of state 6 (ChangeState 0)
    void trade_cancel            ();                              // 30029 to the other, S2C 100, no exchange, state 0
    void trade_reset             ();                              // sub_762100
    void trade_update            (PacketBuffer& pkt);             // C2S 101 (ItemExchangeMgr::Update)
    void trade_check             (bool on);                       // C2S 102 (sub_84D650)
    void trade_view              (const TradeEx& ex, int me, uint16_t my_over, uint16_t other_over,
                                  uint16_t result, PacketBuffer& p) const;   // S2C 101 (sub_5FB780)
    int  trade_over_weight       (const TradeSide& me, const TradeSide& other) const;   // sub_73B970
    void trade_records           (const TradeSide& me, const TradeSide& other, std::vector<BagRecord>& out); // sub_6DC330
    void trade_send              (uint16_t from);                 // the other's C2S 176 on us, the first checker
    int  trade_execute           ();                              // hItemExchange::Send, player1's half (ours)
    void trade_exec_other        (uint16_t from, uint16_t id);    // the other's half + the DB (USPM_ItemTrade_User)
    void trade_exec_done         (uint16_t from, uint16_t id, bool ok);   // ICMD_EXCHANGE_ACK_OK on the executor
    void trade_apply             (const std::vector<BagRecord>& recs, int32_t money, uint16_t opcode); // sub_84D740 + S2C 104 / 105
    void trade_close             ();                              // our connection closes: a trade in its DB step settles
    // ── The post box (postbox.cpp: Player +7528 CPostBox, hPostBox* requests; the Master's cPostStore is the
    //    PostStore the requests call at once) ──────────────────────────────────────────────────────
    struct PostEntry {
        PostRow  row;
        bool     detail{ false };      // its detail came (CPostNormal / CPostGift vtbl+24)
        bool     done{ false };        // +20: nothing left to take
        int32_t  money{ 0 };           // CPostNormal +208: its gelders
        std::vector<PostItemRow>   items;
        std::vector<PostOptionRow> opts;
    };
    std::map<uint32_t, PostEntry> post_box_;   // by PostUID: the three lists (received / sent / gift box)
    int32_t  post_sync_acc_{ 0 };              // +7528: the sync timer (sub_76BF70)
    void post_login              ();              // CMD_CLIENT_LOGIN / 156: MCmd 3156 PostCount -> S2C 1228
    void post_tick               (int elapsed_ms);
    void post_sync               ();              // sub_76C290 (MCmd 3139) -> sub_76C100: S2C 1229
    bool post_insert             (const PostRow& r);   // CPostBox::Insert + InsertPostSeq (100 a list)
    uint64_t post_oldest_seq     (int list) const;     // sub_7C5A00: 0 received, 1 sent, 2 the gift box
    uint64_t post_newest_seq     () const;             // sub_7C5C00
    void post_send_chunks        (uint16_t op, const PacketBuffer& payload);   // sub_7C5CB0 (1920-byte parts)
    void post_write_row          (PacketBuffer& p, const PostRow& r) const;
    void post_error              (uint16_t op, uint32_t code);   // S2C 1232 .. 1244 [u32 code]
    void handle_post_list        (PacketBuffer& pkt);   // C2S 1230: hPostBoxSelectList (sub_76BFB0)
    void handle_post_detail      (PacketBuffer& pkt);   // C2S 1233: hPostBoxSelectDetailInfo (sub_76C050)
    void handle_post_send        (PacketBuffer& pkt);   // C2S 1239: hPostBoxSendPost (sub_76C2F0)
    void handle_post_recv        (PacketBuffer& pkt);   // C2S 1236: hPostBoxRecvPost (sub_76C3A0)
    void handle_post_delete      (PacketBuffer& pkt);   // C2S 1242: hPostBoxDeletePost (sub_76C450)
    void post_level_up           (int level);           // sub_76BE80: the level's GM gifts (MCmd 3153)
    // ── The pet (pet.cpp) ─────────────────────────────────────────────────
    static uint16_t pet_chr_type(uint32_t item_id);               // sub_6DFB90: a pet item's CharacterInfo id
    void pet_load(bool to_self);                                  // PetItemHelper::OnLoad (sub_6E6B10): op 8
    void pet_equipped(const BagItem& b, const ItemDef& d);        // PetItemHelper::OnEquip (sub_6E6C30): op 9
    void pet_released();                                          // PetItemHelper::OnRelease (sub_6E6DF0)
    bool pet_create(const BagItem& b, const ItemDef& d, bool in_zone);   // Shell::CreatePet (sub_7FD700)
    void pet_destroy(bool to_self);                               // vtbl+28: S2C 0x000F to the area, id back
    void pet_regen_set(const BagItem& b);                         // +2740 / +2744 from options 24000 / 24001
    void pet_tick(std::chrono::steady_clock::time_point now, int elapsed_ms);   // Pet::OnProcess
    void pet_set_state(int state);                                // sub_414B50 on the Pet FSM (S2C 63)
public:
    void pet_speech(int situation);                               // sub_738F50: a situation 1..29 for the pet's line
private:
    void pet_speech_tick(int elapsed_ms);                         // sub_738FF0: the pause, the pending line said
    void pet_say(int type);                                       // sub_738900 + sub_5FBF70: S2C 592 to the area
    bool pet_move_to(int32_t x, int32_t y, uint8_t type, std::chrono::steady_clock::time_point now); // vtbl+100
    void pet_send_move(std::chrono::steady_clock::time_point now); // sub_5F5240: S2C 0x0018 to the area
    void pet_follow_step(std::chrono::steady_clock::time_point now);   // BaseFollowingStrategy sub_4211A0
    void pet_move_end(std::chrono::steady_clock::time_point now);  // PetFollowingStrategy sub_4215C0 (30008)
    void run_atcommand           (const std::string& text);    // GM commands (@item/@warp/@speed/...)
    void apply_move_type_speed   (uint8_t type);                // current speed by move type (+ S2C 0x01D8 if it changed)
    void set_current_speed       (float speed);                 // set cur_speed_/move_speed_ and announce a change (0x01D8)
    void send_server_message     (const std::string& msg);     // GM feedback chat line (S2C 57)
    void handle_enter_zone_cmd   (PacketBuffer& pkt);          // C2S 156: client entered the new field
    void begin_field_change      (int16_t zone, int32_t x, int32_t y,  // send RELOCATE 154 (seamless move)
                                  uint8_t change_type = 0);

    // ── Helpers ───────────────────────────────────────────────────────────
    void apply_job_state();                    // sub_761250: the shown model + the class from the tiers
    uint8_t shown_hair() const;                // Player +440: the shown tier's hair
    void enter_zone(int16_t zone_id, int32_t x, int32_t y);
    bool load_char_from_db();  // returns false on failure (session already closed)
    // The Login -> Game ticket (game_tickets): take (check + delete) the one of a C2S 0x0004 / 156 -> the
    // ticket's account; write a fresh one before we make the client reconnect (S2C 154).
    bool take_game_ticket(uint32_t uid, uint32_t& account);
    void issue_game_ticket();
    void save_position();

    // ── Responses ─────────────────────────────────────────────────────────
    void send_session_open();
    void send_map_enter_ack();
    void send_session_param_1();   // 0x1585 — triggers client's C2S_SESSION_READY (0x1581)
    void send_session_param_2();   // 0x1586 — session parameter 2 (dummy)
    void send_party_init();        // 0x1773 — party data full update (0 members — prevents SoulSeed crash)
    void send_social_init();       // 0x2AFA — social/pairing system init (0 entries)
    void send_player_spawn();      // 0x0007 PLAYER_SPAWN_LOGIN (KR) — renders the player
    void send_entity_id_echo();    // 0x03A5 ENTITY_ID_ECHO — binds spawned entity to "me"
    void send_world_spawn();       // full KR Phase 1 + Phase 2 in-world spawn sequence (Zuri spec)
    void send_map_change();        // 0x017C — loads the actual map/field (fixes NULL-field camera crash)
};
