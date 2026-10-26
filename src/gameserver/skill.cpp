#include "game_session.hpp"
#include "game_server.hpp"
#include "client_text.hpp"
#include "logger.hpp"
#include "combat_util.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  Skill use — 1:1 with the retail GameServer (GameServerRelease):
//
//  C2S 0x00EE [u16 skill][u8 level]   Player state machine sub_73D2A0 case 238 -> sub_75EBC0
//      checks, invocation conditions (ESAII) -> a skill with a selection UI (ESAUI != 0) is answered
//      with S2C 241 [u8 2][u16 skill][u8 level] (sub_5ED860): the client opens the target selection.
//      Failures: S2C 239 [u16 reason] (sub_5ED7D0, client "Alert_CancelType%d").
//  C2S 0x00E0 [u8 mode][u16 skill][u8 level][u16 uiKind][u16 size][payload]   case 224 ->
//      Player::InvokeSkill sub_75EEF0: conditions, cool time, the invocation condition object
//      (ESAIC, Player +1204, sub_715980 / sub_5218A0): ESAICChase (ESAIC 2) chases the target into
//      ESAII Range, every ESAIC waits the cast Delay; ready -> S2C 240 [u16 skill][u8 level]
//      (sub_5ED6D0) + the skill (sub_8911A0: MP/HP cost, cool time, the ESAction effect).
//  C2S 0x0107 [u8 mode][u16 skill][u8 level]  case 263: the client closed its selection.
//  C2S 0x034C [u64 uid][u16 skill]  case 844: a skill scroll (Type 15 SubType 69, SkillScroll_Info) casts
//      its own skill at the row's Level through the same UseSkill; the item waits at Player +0xC94 and is
//      used up when the skill's effect state (ESClass 4 / 5 / 6) is made. C2S 0x0351 (case 849): the
//      selected target of such a skill (7001..7052), the body of 0x00E0, into the same InvokeSkill.
//
//  The CLIENT (Trickster.bin) side: sub_BEB290 sends 0xEE and sets the "waiting" bit 0x20 of
//  player+26512, which blocks every further skill/item use (sub_6AD890) until S2C 239 / 240 / 241 /
//  243 / 276 / 279 / 748 clears it (sub_6B4E00) — an unanswered 0xEE locks the skills silently.
//  241 opens ESAUISelectTargetObject (ESAUI 1); clicking a target sends 0xE0 (sub_C0B180) and sets
//  the bit again until 240 / 239.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int     kChaseDefaultRange = 200;   // ESAICChase init sub_522790: +192 = 200
static constexpr int     kChaseGiveUpMs     = 5000;  // ESAICChase tick sub_522BF0: +194 > 5000 -> fail(1)
static constexpr int     kChaseOrderGapMs   = 300;   // ESAICChase start sub_522A60: strategy gap 300
static constexpr uint8_t kSelectionMode     = 2;     // sub_75EBC0 -> sub_5ED860(this, 2, skill, level)

// ESAII column -> invocation condition type (GameInfoMgr sub_524160, case-insensitive).
enum EsaCond {
    kCondNone = 0, kCondDelay, kCondMinLv, kCondMaxLv, kCondMinApLv, kCondMaxApLv, kCondHpCon,
    kCondMpCon, kCondRange, kCondCoolTime, kCondHpRate, kCondMpRate, kCondHeightType,
    kCondProbability, kCondMonDelay
};

static bool iequals(const std::string& a, const char* b)
{
    const size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

static int esa_cond_type(const std::string& col)
{
    static const char* const kNames[] = { "Delay", "MinLv", "MaxLv", "MinApLv", "MaxApLv", "HpCon", "MpCon",
                                          "Range", "CoolTime", "HpRate", "MpRate", "HeightType", "Probability",
                                          "MonDelay" };
    for (int i = 0; i < 14; ++i)
        if (iequals(col, kNames[i])) return i + 1;
    return kCondNone;
}

// sub_522E90: the first condition of a type in the level's list.
static const double* esa_cond(const EsaRow* row, int type)
{
    if (!row) return nullptr;
    for (const auto& f : row->fields)
        if (esa_cond_type(f.first) == type) return &f.second;
    return nullptr;
}

// The ESActions the ESAction switch sub_5BE9A0 handles itself (its 165 cases); every other one is a
// generic ES (sub_7BEF40 -> sub_524950, es.cpp).
static bool esa_has_handler(int esaction)
{
    static const int kHandled[] = {
        1078, 1080, 1082, 1085, 1086, 1087, 1090, 1094, 1100, 1103, 1121, 1123, 1124, 1131, 1132, 1133,
        1134, 1135, 1136, 1137, 1138, 1139, 1147, 1148, 1156, 1158, 1159, 1163, 1164, 1167, 1168, 1170,
        1174, 1179, 1180, 1181, 1184, 1185, 1187, 1190, 1191, 1193, 1194, 1195, 1196, 1197, 1199, 1214,
        1215, 1216, 1218, 1219, 1220, 1222, 1227, 1229, 1230, 1231, 1234, 1235, 1238, 1239, 1242, 1244,
        1246, 1249, 1256, 1257, 1258, 1259, 1260, 1261, 1262, 1263, 1264, 1265, 1268, 1269, 1271, 1276,
        1277, 1282, 1283, 1284, 1287, 1290, 1291, 1292, 1296, 1297, 1298, 1299, 1307, 1310, 1312, 1313,
        1314, 1317, 1318, 1321, 1323, 1325, 1326, 1327, 1328, 1329, 1330, 1335, 1336, 1343, 1344, 1346,
        1347, 1352, 1356, 1362, 1364, 1365, 1381, 1383, 1393, 1400, 1401, 1402, 1403, 1404, 1405, 1406,
        1407, 1408, 1409, 1410, 1411, 1412, 1413, 1414, 1415, 1416, 1417, 1418, 1419, 1420, 1421, 1452,
        1454, 1455, 1457, 1461, 1464, 1469, 1470, 1472, 1474, 1475, 1477, 1481, 1482, 1484, 1486, 1488,
        1490, 1491, 1493, 1495, 1497,
    };
    return std::binary_search(std::begin(kHandled), std::end(kHandled), esaction);
}

// sub_891A30: failing condition type -> the S2C 239 reason.
static uint16_t cond_reason(int type)
{
    switch (type) {
    case kCondMinLv:   return 1;
    case kCondMaxLv:   return 2;
    case kCondMinApLv: return 3;
    case kCondMaxApLv: return 4;
    case kCondHpCon:   return 5;
    case kCondMpCon:   return 6;
    default:           return 0;
    }
}

// sub_524340: the player's invocation conditions of this level; returns the failing type (0 = ok).
// (MinApLv/MaxApLv compare a Player +790 value no ESAII table of this data uses — not modelled.)
int GameClientSession::skill_conditions(const EsaRow* row) const
{
    if (!row) return 0;
    for (const auto& f : row->fields) {
        const int v = static_cast<int>(f.second);
        switch (esa_cond_type(f.first)) {
        case kCondMinLv:       if (level_ < v) return kCondMinLv;  break;
        case kCondMaxLv:       if (level_ > v) return kCondMaxLv;  break;
        case kCondHpCon:       if (hp_ < v)    return kCondHpCon;  break;       // +444 >= v
        case kCondMpCon:       if (mp_ < v)    return kCondMpCon;  break;       // +448 >= cost (sub_717240: no buff)
        case kCondProbability: if (rnd(100) > v) return kCondProbability; break; // sub_41ED70(100) <= v
        default: break;
        }
    }
    return 0;
}

// S2C 239 (0xEF) [u16 reason] to ourselves (sub_5ED7D0). VERIFIED in the client: the field router
// sub_6F0960 case 239 -> the local player sub_6B4E00 case 239: reads the u16, clears the skill
// "waiting" bits, closes the selection and (reason != 15) shows SkillStrings "Alert_CancelType%d".
void GameClientSession::skill_cancel_reason(uint16_t reason)
{
    PacketBuffer p(static_cast<Opcode>(0x00EF), is_paula());
    p.write<uint16_t>(reason);
    send(p);
    LOG_DEBUG("GameClientSession %u: skill cancelled, reason %u", session_id(), reason);
}

// S2C 243 (0xF3), empty, to ourselves (_fputchar -> sub_60BE50(this, 243)) + the ESAIC reset
// (sub_75F770 -> sub_72C020). Client case 243: clears the waiting bit 0x20 and closes the selection.
void GameClientSession::skill_cancel_using()
{
    PacketBuffer p(static_cast<Opcode>(0x00F3), is_paula());
    send(p);
    su_ = SkillUse{};
}

// The ESAIC "cast start" (ESAICChase in range sub_522790 / on arrival sub_522C50; ESAICNormal on
// entering state 14 sub_5224B0): S2C 0x0019 halt [u16 id][u16 x][u16 y][u8 type] at our current
// position (sub_5F52F0; type = our move type, 1 when standing — sub_41C820/sub_41C800) and S2C 242
// [u16 id][u8 mode][u16 skill][u8 level][u8 targetKind][u16 target] (sub_5ED8F0), both to the area.
// VERIFIED in the client: 0x0019 is the halt of the death block (sub_6F0960 cases 24-27 -> the actor
// move events); 242 -> sub_6F00A0 -> the actor: the local Avatar (sub_6B4E00 case 0xF2) and the
// other characters (sub_6BEC50 case 0xF2) read [u8][u16 skill][u8][u8 kind] + kind 1 [u16 id] /
// kind 2 [u16 x][u16 y], face it and play the skill's cast motion.
void GameClientSession::skill_cast_motion()
{
    const auto now = std::chrono::steady_clock::now();
    int32_t px, py;
    cur_pos(now, px, py);
    const uint8_t halt = motion_.moving(now) ? static_cast<uint8_t>(motion_.type) : 1;
    motion_.rest(px, py);
    pos_x_ = px; pos_y_ = py;
    if (auto* p = server_.entity_mgr().get_player(object_id_)) { p->pos_x = px; p->pos_y = py; }
    const uint16_t self = static_cast<uint16_t>(object_id_);
    {
        PacketBuffer h(static_cast<Opcode>(0x0019), is_paula());
        h.write<uint16_t>(self);
        h.write<uint16_t>(static_cast<uint16_t>(px)); h.write<uint16_t>(static_cast<uint16_t>(py));
        h.write<uint8_t>(halt);
        server_.broadcast_zone(zone_id_, h, 0);
    }
    {
        PacketBuffer c(static_cast<Opcode>(0x00F2), is_paula());
        c.write<uint16_t>(self);
        c.write<uint8_t>(su_.mode);
        c.write<uint16_t>(su_.skill);
        c.write<uint8_t>(su_.level);
        if (su_.target) { c.write<uint8_t>(1); c.write<uint16_t>(su_.target); }   // descriptor kind 1
        else            { c.write<uint8_t>(0); }
        server_.broadcast_zone(zone_id_, c, 0);
    }
}

// ── C2S 0x00EE: use a skill (case 238) ──────────────────────────────────────
void GameClientSession::handle_skill_request(PacketBuffer& pkt)
{
    try {
        const uint16_t sid = pkt.read<uint16_t>();
        const uint8_t  lv  = pkt.read<uint8_t>();
        LOG_DEBUG("GameClientSession %u: C2S 0x00EE use skill %u lv %u", session_id(), sid, lv);

        if (dead_) { skill_cancel_reason(19); return; }      // state 0x16 -> sub_5ED7D0(this, 0x13)
        if (pcond_test(8) && sid != 1110) { skill_cancel_using(); return; }   // the condition 8: sub_75F790
        // Sitting (state 8) -> sub_5ED7D0(this, 10); resting (state 15) -> sub_5ED7D0(this, 9).
        if (rest_state_) { skill_cancel_reason(rest_state_ == 8 ? 10 : 9); return; }
        if (talk_npc_) { skill_cancel_reason(0); return; }   // the talk state 9: sub_5ED7D0(this, 0)
        if (sshop_) { skill_cancel_reason(0); return; }      // the skill master state 25: the same
        // A selection still open (+0x148 flag 1): reset the ESAIC (sub_75F770) and clear the flag.
        if (su_.selecting) su_ = SkillUse{};

        // A skill we do not have or a passive one: the session is dropped (sub_8AA290).
        auto it = std::find_if(skills_.begin(), skills_.end(), [&](const LearnedSkill& s) { return s.id == sid; });
        const SkillDef* d = (it != skills_.end()) ? server_.skill_db().get(sid) : nullptr;
        if (!d) {
            LOG_ERROR("%s: CMD_USE_SKILL:skill not found:%d", char_name_, sid);
            close();
            return;
        }
        if (d->type == 1) {
            LOG_ERROR("%s: CMD_USE_SKILL: skill is Passive: skillid:%d, lv:%d", char_name_, sid, lv);
            close();
            return;
        }
        if (lv <= 0 || lv > 11 || lv != it->level) {
            LOG_INFO("%s: CMD_USE_SKILL: lv is invalid: skillid:%d, lv:%d, skilllv:%d", char_name_, sid, lv, it->level);
            return;
        }
        if (d->skill_option & 0x10) return;                  // never usable this way

        if (drill_.active) drill_stop(1, -1, 0);             // state 3: sub_75C4A0(1, -1, 0) first
        const int r = skill_use(*d, lv);                     // sub_75EBC0
        if (r == 1) su_.selecting = true;                    // +0x148 flag 1 (sub_5225F0); sub_76EC10 (see below)
        else if (r == 3) skill_enter_cast();
    } catch (const std::exception& e) {
        LOG_WARN("GameClientSession %u: C2S 0x00EE skill use aborted: %s", session_id(), e.what());
    } catch (...) {
        LOG_WARN("GameClientSession %u: C2S 0x00EE skill use aborted: unknown exception", session_id());
    }
}

// ── Player::UseSkill (sub_75EBC0), for C2S 0x00EE and a scroll's C2S 0x034C ─────────────────
//  0: failed (S2C 239 sent); 1: a selection UI (S2C 241 sent) — the caller sets the selection flag
//  (+0x148) and sub_76EC10 (Player +3256 = 0: pauses the 5 s "in battle" countdown of the flag 256 a
//  melee swing sets, sub_73D020 / sub_73D060 — that flag is not modelled); 2: fired at once (S2C 240 +
//  the skill, the motion stopped — sub_41C5D0); 3: not ready — skill_enter_cast.
//  SkillOption 128 (Sudden Attack, Poison Assault) needs the hide (Character +452 bit 0) -> else S2C 239
//  [22]. Fired at once, a hidden caster leaves its hide first unless the skill has SkillOption 64 (the
//  limiter's vtbl+60(1286)) — so does state 14's fire (sub_75F140); InvokeSkill's (sub_75EEF0) does not.
//  (Not modelled, no such state yet: the flag 256 with SkillOption 256 -> S2C 239 [23], the condition
//  bits of SkillParam +100 -> [16..21] (sub_891E70).)
int GameClientSession::skill_use(const SkillDef& d, uint8_t lv)
{
    const uint16_t sid = static_cast<uint16_t>(d.id);
    // The invocation conditions of this level (MP, HP, ...).
    const EsaRow* ii = server_.esa_db().row(d.esaii_table, lv);
    if (const int c = skill_conditions(ii)) { skill_cancel_reason(cond_reason(c)); return 0; }
    if ((d.skill_option & 128) && !hiding_) { skill_cancel_reason(22); return 0; }

    if (d.esaui != 0) {
        // A selection UI: the ESAIC reset (sub_72C020) and S2C 241 [u8 2][u16 skill][u8 level] to
        // ourselves; the client opens it (VERIFIED: sub_6F0960 case 241 -> sub_6B4E00 case 241 reads u8,
        // u16, u8 and starts the ESAUI).
        su_ = SkillUse{};
        PacketBuffer p(static_cast<Opcode>(0x00F1), is_paula());
        p.write<uint8_t>(kSelectionMode);
        p.write<uint16_t>(sid);
        p.write<uint8_t>(lv);
        send(p);
        LOG_DEBUG("GameClientSession %u: skill %u lv %u -> selection (S2C 241)", session_id(), sid, lv);
        return 1;
    }
    // No selection UI: the cool time (S2C 239 [12]), then the ESAIC at once (vtbl+16 with mode 2 and
    // no target descriptor). Ready -> S2C 240 + the skill (sub_8911A0), 2; not ready -> 3.
    const auto now = std::chrono::steady_clock::now();
    auto cd = skill_cd_.find(sid);                           // sub_88F820
    if (cd != skill_cd_.end() && now < cd->second) { skill_cancel_reason(12); return 0; }
    su_ = SkillUse{};
    su_.active = true;
    su_.skill  = sid;
    su_.level  = lv;
    su_.mode   = kSelectionMode;
    su_.esaic  = d.esaic;
    if (su_.esaic == 2) su_.range = kChaseDefaultRange;
    if (ii) {
        for (const auto& f : ii->fields) {
            const int t = esa_cond_type(f.first);
            if (su_.esaic == 2 && t == kCondRange) {
                su_.range = static_cast<int16_t>(f.second);
            } else if (t == kCondDelay) {
                su_.delay_ms = static_cast<int16_t>(static_cast<int>(f.second * 1000.0));
                if (su_.delay_ms > 0) su_.flags |= 1;
            }
        }
    }
    if (su_.esaic == 2) su_.fail = 1;                        // ESAICChase::Init: no target object
    if (su_.fail) {
        const uint16_t reason = (su_.fail == 2) ? 13 : 0;
        su_ = SkillUse{};
        skill_cancel_reason(reason);
        return 0;
    }
    if (su_.flags == 0) {
        if (skill_fire_check(d) != -1) { su_ = SkillUse{}; return 0; }   // sub_890B30
        if (!(d.skill_option & 64)) hide_break();                         // (a hidden caster: vtbl+60(1286))
        // (the caller's sub_41C5D0, done before the fire here — the effects read cur_pos anyway)
        skill_stop_motion(now);
        skill_execute(now);
        return 2;
    }
    return 3;
}

// UseSkill's 3, the caller (case 238 / 844): the motion stops (sub_41C5D0), the selection flag (+0x148)
// and state 14, whose entry does the ESAICNormal cast start (sub_5224B0); state 14 replaces the attack
// state.
void GameClientSession::skill_enter_cast()
{
    skill_stop_motion(std::chrono::steady_clock::now());
    skill_state14_enter();
    skill_cast_motion();
    ensure_world_tick();
}

// ChangeState(14) (sub_414B50): the attack state's exit (0x74E874: the attack strategy stops, S2C 0x0020
// to the area — end_attack) and state 14's OnInit (0x74D857): the end conditions 2 / 3 / 8 (none here),
// the queued 30107s dropped (sub_4380B0), the attack target holder (+232) paused (sub_76E4A0(0)) and,
// coming from the attack state (the event's +4 = 1) without a gun (sub_6DA700(54)), the flag 2 of +0x148
// (sub_5225F0): the skill done, state 14's tick takes the attack up again. (The ESAIC's vtbl+24 — the
// cast start / the chase — is the caller's.)
void GameClientSession::skill_state14_enter()
{
    su_.state14       = true;
    su_.resume        = attacking_ && !gun_in_any_slot();
    su_.resume_target = su_.resume ? atk_target_ : 0;
    follow_q_         = FollowAttack{};
    if (attacking_) end_attack(true);
}

// State 14's tick once sub_75F6F0 is done — the skill fired (1) or failed (2) (0x74DA03): with the flag 2
// and an attack target (sub_41C690), the event 30107 [that target (sub_41C650)][2][the attack strategy's
// vtbl+60(1) (sub_41B800: +52 = the Attack2 motion time) + 200] to ourselves; then ChangeState(0), whose
// exit clears the flags 1 | 2 of +0x148 (sub_5101D0(3)). Called before su_ is reset.
void GameClientSession::skill_state14_done()
{
    if (!su_.state14 || !su_.resume || !su_.resume_target) return;
    follow_q_.target = su_.resume_target;
    follow_q_.ms     = server_.char_range().get(type_).attack2_ms + 200;
}

// A damage skill's handler after its block (e.g. sub_5C2110): the caster's +0x148 flag 2 clear
// (sub_560700) and the target not dead (+92 flag 4, sub_41C860) -> the event 30107 [u16 target][u16 2]
// [the attack strategy's vtbl+60(1) + 500] to the caster (sub_417E00): it attacks the target after the
// skill. (With the flag 2 state 14's tick takes the previous attack up instead.) Some handlers also want
// no gun equipped (sub_6DA700(54)) — `no_gun`.
void GameClientSession::skill_follow_after(uint16_t target, bool no_gun)
{
    if (su_.resume) return;
    if (no_gun && gun_in_any_slot()) return;
    const MonsterEntity* m = target ? server_.entity_mgr().get_monster(target) : nullptr;
    if (!m || !m->alive || m->hp <= 0) return;
    follow_q_.target = target;
    follow_q_.ms     = server_.char_range().get(type_).attack2_ms + 500;
}

// The queued event 30107, run once the skill is done (the retail message queue: after state 14's
// ChangeState(0)).
void GameClientSession::skill_run_follow()
{
    const FollowAttack f = follow_q_;
    follow_q_ = FollowAttack{};
    if (f.target) skill_follow_attack(f.target, f.ms);
}

// sub_421E50: running is allowed unless the StateType (+1060) has the bit 1 or 16 and the bag carries
// more than 89% of the max WT (+584).
bool GameClientSession::can_run() const
{
    if (!(state_type_ & 1) && !(state_type_ & 16)) return true;
    const int32_t wt = stat_value(6);
    const uint32_t pct = (wt <= 0) ? 100u : static_cast<uint32_t>(100 * bag_weight_ / wt);
    return pct <= 0x59;
}

// ── Event 30107 (the Player's global handler, 0x73D688): [u16 target][u16 move type][u32 ms] ─────────
//  Ignored while we are dead (+92 flag 4); the target (zone vtbl+16, kind 3) there and not dead ->
//  BeginAttack (sub_75FA00) with the move type 2 — 1 when we may not run (sub_421E50), 2 again outside the
//  normal StateType (sub_718CD0(1)); the target must be attackable (sub_5E6D70: no MonsterParamEx2 Type
//  8 / 12); the motion stops (sub_41C5D0); the attack state retargets (vtbl+140), any other state enters
//  it (vtbl+124 + ChangeState(1): S2C 0x001E) — then the attack strategy's cooldown (+56, sub_41B7E0) =
//  ms: the first swing waits for it. (As C2S 0x001D, the owner test vtbl+164 is not modelled.)
void GameClientSession::skill_follow_attack(uint16_t target, int ms)
{
    if (dead_ || leaving_ || state_ != GameClientSessionState::InWorld) return;
    MonsterEntity* m = server_.entity_mgr().get_monster(target);
    if (!m || m->zone != zone_id_ || !m->alive || m->hp <= 0) return;
    if (const MobDef* md = server_.mob_db().get(static_cast<int>(m->mob_id)))
        if (md->type == 8 || md->type == 12) return;
    uint8_t mt = 2;
    if (!can_run()) mt = 1;
    if (!(state_type_ & 1)) mt = 2;
    const auto now = std::chrono::steady_clock::now();
    skill_stop_motion(now);
    atk_move_type_ = mt;
    if (!attacking_) {
        PacketBuffer ack(static_cast<Opcode>(0x001E), is_paula());
        send(ack);
    }
    drill_ = Drill{};                                        // ChangeState(1) leaves those states
    wh_leave_state();
    skill_shop_leave();
    start_attack(target);
    pl_atk_ready_ = now + std::chrono::milliseconds(ms);
    LOG_DEBUG("GameClientSession %u: 30107 -> attack mob %u (move %u, cooldown %d ms)", session_id(), target, mt, ms);
}

// sub_41C5D0: the move strategy stops where we are.
void GameClientSession::skill_stop_motion(std::chrono::steady_clock::time_point now)
{
    int32_t px, py;
    cur_pos(now, px, py);
    motion_.rest(px, py);
    pos_x_ = px; pos_y_ = py;
    if (auto* p = server_.entity_mgr().get_player(object_id_)) { p->pos_x = px; p->pos_y = py; }
}

// ── C2S 0x034C: use a skill scroll (case 844, 0x741386) ─────────────────────────────────
//  The client (sub_BEB290 with the scroll flag, or for a skill it has not learned) sends [u64 uid][u16
//  skill] for a Type 15 SubType 69 item and does not wait for an answer (no "waiting" bit). Retail: a
//  selection still open is cancelled (sub_75F790: S2C 243); the item by uid; Player +0xC94 = 0; no item
//  or not SubType 69 -> S2C 0x005A (sub_5F6730); its SkillScroll_Info row (sub_636E70); the player's
//  +1636 flag 0x80000000, or the row's skill not the one asked -> S2C 0x005A; the conditions 4, 8 (but
//  for skill 1110) and 0x400 (but a Type 10 item) -> S2C 0x005A (none of those states exists here yet);
//  a temporary entry of the skill (sub_88F120: not a learned one); state 3 -> the drill stops
//  (sub_75C4A0(1, -1, 0)); UseSkill at the row's Level; then, the item's HPCon / MPCon met
//  (sub_73C940): +0xC94 = the item and by the result 1 the selection flag (+ sub_76EC10), 2 the stop,
//  3 the stop + the flag + state 14 — any other result, or HPCon / MPCon not met: S2C 0x005A. The
//  scroll is used up when its skill's effect state is made (es.cpp scroll_use_up). Only the Normal
//  state handles 844 (its jumptable 0073D8E8): the dead / sitting / talking ... states ignore it.
void GameClientSession::handle_skill_scroll(PacketBuffer& pkt)
{
    auto refuse = [&]() {
        PacketBuffer p(static_cast<Opcode>(0x005A), is_paula());
        send(p);
    };
    try {
        const uint64_t uid = pkt.read<uint64_t>();
        const uint16_t sid = pkt.read<uint16_t>();
        LOG_DEBUG("GameClientSession %u: C2S 0x034C skill scroll %llu skill %u", session_id(),
                  static_cast<unsigned long long>(uid), sid);
        if (dead_ || rest_state_ || talk_npc_ || sshop_) return;
        if (su_.selecting) {                                 // (+0x148).HasFlag(1) -> sub_75F790 + clear
            skill_cancel_using();
            su_.selecting = false;
        }
        BagItem* b = bag_find_uid(uid);                      // sub_60C610(+1716, uid)
        scroll_uid_ = 0;                                     // +0xC94 = 0
        const ItemDef* it = b ? server_.item_db().get(b->item_id) : nullptr;
        if (!b || !it || it->sub_type != 69) { refuse(); return; }
        const SkillScrollDef* row = server_.skill_scroll_db().get(b->item_id);
        if (!row || row->skill != sid) { refuse(); return; } // (no row: the retail reads it anyway)
        const uint8_t lv = static_cast<uint8_t>(row->level);
        const SkillDef* d = server_.skill_db().get(sid);     // sub_88F120
        if (!d) {                                            // (7006 / 7023: no SkillParam2 row — a null read)
            LOG_ERROR("%s: skill scroll %u: skill %u not in SkillParam", char_name_, b->item_id, sid);
            refuse();
            return;
        }
        if (drill_.active) drill_stop(1, -1, 0);             // state 3: sub_75C4A0(1, -1, 0)
        const int r = skill_use(*d, lv);                     // sub_75EBC0
        b = bag_find_uid(uid);                               // (the item is still there: nothing used it)
        if (!b || hp_ < it->hp_con || mp_ < it->mp_con) {    // sub_73C940
            if (r == 3) su_ = SkillUse{};                    // (state 14 is never entered)
            refuse();
            return;
        }
        scroll_uid_ = uid;                                   // +0xC94 = the item
        switch (r) {
        case 1: su_.selecting = true; break;                 // sub_5225F0(1) + sub_76EC10
        case 2: break;                                       // sub_41C5D0 (done in skill_use)
        case 3: skill_enter_cast(); break;
        default: refuse(); break;
        }
        LOG_DEBUG("GameClientSession %u: skill scroll %u (%s) -> skill %u lv %u: %d", session_id(), it->id,
                  it->name.c_str(), sid, lv, r);
    } catch (const std::exception& e) {
        LOG_WARN("GameClientSession %u: C2S 0x034C skill scroll aborted: %s", session_id(), e.what());
    } catch (...) {
        LOG_WARN("GameClientSession %u: C2S 0x034C skill scroll aborted: unknown exception", session_id());
    }
}

// ── C2S 0x0351: a scroll skill's selected target (case 849, 0x748390) ─────────────────────
//  The client sends it instead of 0x00E0 for the skills 7001..7052 (sub_C0B180 -> c2s_849: the same
//  body [u8 mode][u16 skill][u8 level][u16 uiKind][u16 size][payload]) and waits (bit 0x20) for S2C
//  240 / 239. Retail: the selection flag cleared; a temporary entry of the skill (sub_88F120); a
//  passive one drops the session (sub_8AA290); then Player::InvokeSkill at the packet's level — no
//  mode check, no learned skill. DEVIATION (anti-cheat): the retail would invoke any skill at any level
//  for anyone here; we require the scroll of the last C2S 844 (Player +0xC94) to cast exactly this
//  skill at its level, else S2C 239 [0] (the client's answer for a refused invocation).
void GameClientSession::handle_skill_scroll_target(PacketBuffer& pkt)
{
    try {
        const uint8_t  mode = pkt.read<uint8_t>();
        const uint16_t sid  = pkt.read<uint16_t>();
        const uint8_t  lv   = pkt.read<uint8_t>();
        const uint16_t kind = pkt.read<uint16_t>();
        const uint16_t size = pkt.read<uint16_t>();
        std::vector<uint8_t> payload;
        for (uint16_t i = 0; i < size && i < 128; ++i) payload.push_back(pkt.read<uint8_t>());
        LOG_DEBUG("GameClientSession %u: C2S 0x0351 scroll skill %u lv %u mode %u uiKind %u size %u",
                  session_id(), sid, lv, mode, kind, size);
        if (dead_ || rest_state_ || talk_npc_ || sshop_) return;

        su_.selecting = false;                               // (+0x148).sub_5101D0(1)
        const SkillDef* d = server_.skill_db().get(sid);     // sub_88F120
        if (!d) {
            LOG_ERROR("%s: CMD_ESACTION:skill not found:%d", char_name_, sid);
            close();
            return;
        }
        if (d->type == 1) {                                  // sub_6E5B50 == 1
            LOG_ERROR("%s: CMD_ESACTION: skill is Passive: skillid:%d, lv:%d", char_name_, sid, lv);
            close();
            return;
        }
        const BagItem* b = scroll_uid_ ? bag_find_uid(scroll_uid_) : nullptr;
        const SkillScrollDef* row = b ? server_.skill_scroll_db().get(b->item_id) : nullptr;
        if (!row || row->skill != sid || static_cast<uint8_t>(row->level) != lv) {
            LOG_ERROR("%s: CMD_ESACTION (scroll): skill %u lv %u without its scroll", char_name_, sid, lv);
            skill_cancel_reason(0);
            return;
        }
        skill_invoke(*d, lv, kind, payload);                 // sub_75EEF0; 2 -> sub_76EC30 (Player +3256 = 1)
    } catch (const std::exception& e) {
        LOG_WARN("GameClientSession %u: C2S 0x0351 scroll target aborted: %s", session_id(), e.what());
    } catch (...) {
        LOG_WARN("GameClientSession %u: C2S 0x0351 scroll target aborted: unknown exception", session_id());
    }
}

// ── C2S 0x0107: the client closed its selection (case 263) ──────────────────
void GameClientSession::handle_skill_select_cancel(PacketBuffer& pkt)
{
    try {
        const uint8_t  mode = pkt.read<uint8_t>();
        const uint16_t sid  = pkt.read<uint16_t>();
        const uint8_t  lv   = pkt.read<uint8_t>();
        LOG_DEBUG("GameClientSession %u: C2S 0x0107 selection cancelled (mode %u skill %u lv %u)",
                  session_id(), mode, sid, lv);
        su_.selecting = false;                               // (+0x148).sub_5101D0(1)
        if (mode == kSelectionMode) skill_cancel_using();    // sub_75F790: S2C 243 + ESAIC reset
    } catch (const std::exception& e) {
        LOG_WARN("GameClientSession %u: C2S 0x0107 selection cancel aborted: %s", session_id(), e.what());
    } catch (...) {
        LOG_WARN("GameClientSession %u: C2S 0x0107 selection cancel aborted: unknown exception", session_id());
    }
}

// ── C2S 0x00E0: the selected target (case 224 -> Player::InvokeSkill sub_75EEF0) ────────────
void GameClientSession::handle_skill_target(PacketBuffer& pkt)
{
    try {
        const uint8_t  mode = pkt.read<uint8_t>();
        const uint16_t sid  = pkt.read<uint16_t>();
        const uint8_t  lv   = pkt.read<uint8_t>();
        const uint16_t kind = pkt.read<uint16_t>();
        const uint16_t size = pkt.read<uint16_t>();
        std::vector<uint8_t> payload;
        for (uint16_t i = 0; i < size && i < 128; ++i) payload.push_back(pkt.read<uint8_t>());
        LOG_DEBUG("GameClientSession %u: C2S 0x00E0 skill %u lv %u mode %u uiKind %u size %u",
                  session_id(), sid, lv, mode, kind, size);

        su_.selecting = false;                               // (+0x148).sub_5101D0(1)
        if (mode != kSelectionMode) return;                  // mode 1: nothing
        if (dead_) return;

        // As case 238: a skill we do not have or a passive one drops the session (sub_8AA290).
        auto it = std::find_if(skills_.begin(), skills_.end(), [&](const LearnedSkill& s) { return s.id == sid; });
        const SkillDef* d = (it != skills_.end()) ? server_.skill_db().get(sid) : nullptr;
        if (!d) {
            LOG_ERROR("%s: CMD_ESACTION:skill not found:%d", char_name_, sid);
            close();
            return;
        }
        if (d->type == 1) {
            LOG_ERROR("%s: CMD_ESACTION: skill is Passive: skillid:%d, lv:%d", char_name_, sid, lv);
            close();
            return;
        }
        if (lv <= 0 || lv > it->level) {
            LOG_INFO("%s: CMD_ESACTION: lv is invalid: lv:%d, skilllv:%d", char_name_, lv, it->level);
            return;
        }
        skill_invoke(*d, lv, kind, payload);                 // sub_75EEF0; 2 -> sub_76EC30 (Player +3256 = 1)
    } catch (const std::exception& e) {
        LOG_WARN("GameClientSession %u: C2S 0x00E0 skill target aborted: %s", session_id(), e.what());
    } catch (...) {
        LOG_WARN("GameClientSession %u: C2S 0x00E0 skill target aborted: unknown exception", session_id());
    }
}

// ── Player::InvokeSkill (sub_75EEF0), for C2S 0x00E0 and a scroll's C2S 0x0351 ─────────────
//  0: failed (S2C 239 sent); 2: fired at once; 3: not ready — Player state 14 entered here (the
//  caller's selection flag + sub_414B50(14)). The ESAIC always gets mode 2.
int GameClientSession::skill_invoke(const SkillDef& d, uint8_t lv, uint16_t kind, const std::vector<uint8_t>& payload)
{
    const uint16_t sid = static_cast<uint16_t>(d.id);
    const EsaRow* ii = server_.esa_db().row(d.esaii_table, lv);
    if (!ii) {
        LOG_INFO("***Player::InvokeSkill- IsValidSkillLevel false : %s, LV(%d)", char_name_, lv);
        skill_cancel_reason(0);
        return 0;
    }
    if (const int c = skill_conditions(ii)) { skill_cancel_reason(cond_reason(c)); return 0; }
    const auto now = std::chrono::steady_clock::now();
    auto cd = skill_cd_.find(sid);                           // sub_88F820: still in its cool time
    if (cd != skill_cd_.end() && now < cd->second) { skill_cancel_reason(12); return 0; }

    // The ESAIC (sub_715980 / sub_5218A0): base init sub_521790 + its conditions (vtbl+36).
    su_ = SkillUse{};
    su_.active  = true;
    su_.skill   = sid;
    su_.level   = lv;
    su_.mode    = kSelectionMode;
    su_.esaic   = d.esaic;
    su_.ui_kind = kind;
    su_.payload = payload;
    if (su_.esaic == 2 || su_.esaic == 3) su_.range = kChaseDefaultRange;   // (ESAICMove sub_523070: 200 too)
    // sub_521690: selection kinds 1/7/8 carry a target object id (descriptor kind 1).
    if ((kind == 1 || kind == 7 || kind == 8) && payload.size() >= 2)
        su_.target = static_cast<uint16_t>(payload[0] | (payload[1] << 8));
    for (const auto& f : ii->fields) {
        const int t = esa_cond_type(f.first);
        if ((su_.esaic == 2 || su_.esaic == 3) && t == kCondRange) {   // ESAICChase sub_5229A0 / ESAICMove sub_5231F0:
            su_.range = static_cast<int16_t>(f.second);                //   +192 = Range
        } else if (t == kCondDelay) {                        // sub_522410: a player's cast delay
            su_.delay_ms = static_cast<int16_t>(static_cast<int>(f.second * 1000.0));
            if (su_.delay_ms > 0) su_.flags |= 1;
        }
    }
    if (su_.esaic == 3) {
        // ESAICMove::Init (sub_523070): the point (sub_523230 — selection kinds 3 / 6 / 10: the payload's [u16 x][u16 y];
        // any other: the log and (0, 0)); farther than Range (d² > Range²) -> the flag 4 (we walk there first), else
        // in range: S2C 0x0019 + 242 (sub_5F52F0 / sub_5ED8F0 — the cast motion; the descriptor of a point is kind 0).
        if ((kind == 3 || kind == 6 || kind == 10) && payload.size() >= 4) {
            su_.dest_x = static_cast<int16_t>(payload[0] | (payload[1] << 8));
            su_.dest_y = static_cast<int16_t>(payload[2] | (payload[3] << 8));
        } else {
            LOG_INFO("ESAICMove::GetDestPosition - invalid m_UI:%s, %d", char_name_, kind);
        }
        int32_t px, py;
        cur_pos(now, px, py);
        if (cmp_r(dist2(px, py, su_.dest_x, su_.dest_y), su_.range) > 0) {
            su_.flags |= 4;
            su_.in_range = false;
        } else {
            su_.in_range = true;
            skill_cast_motion();
        }
    }
    if (su_.esaic == 2) {
        // ESAICChase::Init (sub_522790): the target must exist; in range -> cast start, else chase.
        int32_t mx, my;
        if (!su_.target || !skill_target_pos(su_.target, now, mx, my)) {
            su_.fail = 1;
        } else {
            int32_t px, py;
            cur_pos(now, px, py);
            if (cmp_r(dist2(px, py, mx, my), su_.range) > 0) {
                su_.flags |= 2;
                su_.in_range = false;
            } else {
                su_.in_range = true;
                skill_cast_motion();                         // (no HeightType condition in this data)
            }
        }
    }
    if (su_.fail) {                                          // sub_72C000 -> reason 13 for fail 2, else 0
        const uint16_t reason = (su_.fail == 2) ? 13 : 0;
        su_ = SkillUse{};
        skill_cancel_reason(reason);
        return 0;
    }
    if (su_.flags == 0) {                                    // ready: fire now (returns 2) — no state change:
        if (skill_fire_check(d) != -1) { su_ = SkillUse{}; return 0; }   // an attack goes on (sub_890B30)
        skill_execute(now);
        return 2;
    }
    // Not ready (returns 3): the caller's ChangeState(14) — the attack state's exit and state 14's OnInit,
    // which runs the ESAIC vtbl+24: ESAICChase starts its NormalFollowingStrategy (sub_522A60: range =
    // Range, gap 300, our move type, the target); ESAICNormal does the cast start (sub_5224B0).
    skill_state14_enter();
    if (su_.esaic == 2) {
        su_.following     = true;
        su_.follow_acc_ms = 0;
        su_.move_type     = motion_.moving(now) ? static_cast<uint8_t>(motion_.type) : 0;
    } else if (su_.esaic == 3) {
        // ESAICMove vtbl+24 (sub_5232E0): with the flag 4, the move type — we may run (sub_421E50): standing -> run (2),
        // moving -> the move goes on as it is; we may not -> walk (1) — and MoveTo the point (vtbl+100): S2C 0x0018
        // (sub_5F5240). (In range, the cast motion was done at its init.)
        if (su_.flags & 4) {
            const uint8_t mtype = can_run() ? (motion_.moving(now) ? static_cast<uint8_t>(motion_.type) : 2) : 1;
            int32_t px, py;
            cur_pos(now, px, py);
            apply_move_type_speed(mtype);
            motion_.start(px, py, su_.dest_x, su_.dest_y, cur_speed_, now, mtype);
            pos_x_ = su_.dest_x; pos_y_ = su_.dest_y;
            if (auto* p = server_.entity_mgr().get_player(object_id_)) { p->pos_x = su_.dest_x; p->pos_y = su_.dest_y; }
            broadcast_move(static_cast<uint16_t>(object_id_), px, py, su_.dest_x, su_.dest_y, mtype);
        }
    } else {
        skill_cast_motion();
    }
    ensure_world_tick();
    return 3;
}

// ── Player state 14: the running ESAIC (sub_75F6F0 -> sub_72C1B0) + the chase strategy ─────
void GameClientSession::skill_tick(std::chrono::steady_clock::time_point now, int elapsed_ms)
{
    // The NormalFollowingStrategy (actor +372, sub_421340) of ESAICChase: re-order a move to the
    // target's CURRENT position every gap (300) ms while d² > Range²; within range post 30011(1).
    if (su_.following) {
        int32_t mx, my;
        if (!skill_target_pos(su_.target, now, mx, my)) {
            su_.following = false;                           // 30011(0) -> ESAICChase: fail(1)
            su_.fail = 1;
        } else {
            int32_t px, py;
            cur_pos(now, px, py);
            if (cmp_r(dist2(px, py, mx, my), su_.range) > 0) {
                su_.follow_acc_ms += elapsed_ms;
                if (su_.follow_acc_ms >= kChaseOrderGapMs) {
                    // sub_4211A0: destination = the target's current position, our stored move type
                    // (0 counts as run, sub_42BC10); announced with S2C 0x0018 (sub_5F5240).
                    const uint8_t mtype = ((su_.move_type & 7) == 0) ? 2 : static_cast<uint8_t>(su_.move_type & 7);
                    apply_move_type_speed(mtype);
                    motion_.start(px, py, mx, my, cur_speed_, now, mtype);
                    pos_x_ = mx; pos_y_ = my;
                    if (auto* p = server_.entity_mgr().get_player(object_id_)) { p->pos_x = mx; p->pos_y = my; }
                    broadcast_move(static_cast<uint16_t>(object_id_), px, py, mx, my, su_.move_type);
                    su_.follow_acc_ms = 0;
                }
            } else {
                // 30011(1) -> ESAICChase sub_522C50: in range now; stop the strategy and, if the
                // invocation is not ready yet (the cast delay), do the cast start.
                su_.following = false;
                if (!su_.in_range) {
                    su_.in_range = true;
                    su_.flags &= static_cast<uint16_t>(~2u);
                    if (su_.flags != 0) skill_cast_motion();
                }
            }
        }
    }

    // sub_72C1B0: ready before the tick -> fire; else tick (vtbl+40) and re-check.
    bool ready = (su_.flags == 0) && !su_.fail;
    if (!ready && !su_.fail) {
        if (su_.esaic == 2 && !su_.in_range) {               // ESAICChase sub_522BF0: chase time
            su_.chase_ms += elapsed_ms;
            if (su_.chase_ms > kChaseGiveUpMs) su_.fail = 1;
        } else if (su_.esaic == 3 && !su_.in_range) {        // ESAICMove sub_523430: the walk's time (+194)
            su_.chase_ms += elapsed_ms;
            if (su_.chase_ms > kChaseGiveUpMs) {
                su_.fail = 1;
            } else {
                // within Range of the point: we stop (sub_41C5D0) -> 30008 -> sub_523500: in range, the flag 4 off,
                // S2C 0x0019 + 242 (a natural arrival at the point takes the same 30008 path)
                int32_t px, py;
                cur_pos(now, px, py);
                if (cmp_r(dist2(px, py, su_.dest_x, su_.dest_y), su_.range) <= 0) {
                    su_.in_range = true;
                    su_.flags &= static_cast<uint16_t>(~4u);
                    skill_cast_motion();
                }
            }
        } else if (su_.flags & 1) {                          // sub_522570: the cast delay
            su_.delay_ms -= elapsed_ms;
            if (su_.delay_ms <= 0) su_.flags &= static_cast<uint16_t>(~1u);
        }
        ready = (su_.flags == 0) && !su_.fail;
    }
    if (su_.fail) {                                          // returns 2 -> sub_75F790: S2C 243
        LOG_DEBUG("GameClientSession %u: skill %u invocation failed (%d)", session_id(), su_.skill, su_.fail);
        skill_state14_done();                                // (the tick takes the attack up after a fail too)
        skill_cancel_using();
        skill_run_follow();
        return;
    }
    if (ready) {                                             // returns 1 -> mode 2 -> sub_75F140
        const SkillDef* d = server_.skill_db().get(su_.skill);
        if (d && skill_fire_check(*d) != -1) {               // sub_890B30 (sub_75F6F0 still returns 1)
            skill_state14_done();
            su_ = SkillUse{};
            skill_run_follow();
            return;
        }
        if (d && !(d->skill_option & 64)) hide_break();       // sub_75F140: a hidden caster leaves its hide
        skill_execute(now);
    }
}

// ── sub_890B30: the checks right before a skill fires (UseSkill / InvokeSkill when ready, the state 14
//    fire sub_75F140) — -1 to fire, else the S2C 239 reason (sent here) ─────────────────────────────────
//  The skill's StateType (SkillParam +144) against the player's StateType bits (+1060, sub_718CF0): 1 needs
//  bit 1 (the normal state; else S2C 0x01E4 [14][0][0]), 2 bit 4, 3 bit 8, 5 bit 64, 4 bit 1 or 16 (else
//  0x01E4 [15]); SkillOption 512 not in a map of type 1 / 5 / 6 (0x01E4 [15]); either -> 15 (the client
//  shows no line for 15). Before them (sub_890DD0) a skill with a PreESAICondition row (4105 / 4206 / 4211)
//  needs (float)(HP / MaxHP) >= HpRate and (float)(MP / MaxMP) >= MpRate — else 0x01E4 [10][0][0] and 15.
//  (Not modelled: the condition 8 — but for skill 1110 — and the ESAction's own target check sub_5BD4E0.)
int GameClientSession::skill_fire_check(const SkillDef& d)
{
    if (const PreEsaicDef* pc = server_.pre_esaic_db().get(d.id)) {
        const float hp_rate = static_cast<float>(static_cast<double>(hp_) / static_cast<double>(hp_max_));
        const float mp_rate = static_cast<float>(static_cast<double>(mp_) / static_cast<double>(mp_max_));
        if (hp_rate < pc->hp_rate || !(mp_rate >= pc->mp_rate)) {
            send_system_msg(10, 0, 0);
            skill_cancel_reason(15);
            return 15;
        }
    }
    uint32_t need = 0;
    bool or16 = false;
    switch (d.state_type) {
    case 1: need = 1;  break;
    case 2: need = 4;  break;
    case 3: need = 8;  break;
    case 5: need = 64; break;
    case 4: need = 1; or16 = true; break;
    default: break;
    }
    if (need && !(state_type_ & need) && !(or16 && (state_type_ & 16))) {
        send_system_msg(d.state_type == 1 ? 14 : 15, 0, 0);
        skill_cancel_reason(15);
        return 15;
    }
    if (d.skill_option & 512) {
        const MapDef* z = server_.maps_db().get(zone_id_);
        if (z && (z->type == 1 || z->type == 5 || z->type == 6)) {
            send_system_msg(15, 0, 0);
            skill_cancel_reason(15);
            return 15;
        }
    }
    return -1;
}

// ── The skill fires: S2C 240 + sub_8911A0 ───────────────────────────────────
void GameClientSession::skill_execute(std::chrono::steady_clock::time_point now)
{
    const SkillDef* d = server_.skill_db().get(su_.skill);
    if (!d) { su_ = SkillUse{}; return; }
    const uint16_t sid = su_.skill;
    const uint8_t  lv  = su_.level;
    // (sub_890B30's state checks — StateType, no-town SkillOption 0x200 — are skill_fire_check, run before
    // this; the ESAction's own target test sub_5BD4E0 is not modelled.)

    // S2C 240 [u16 skill][u8 level] to ourselves (sub_5ED6D0). VERIFIED: client case 240 clears the
    // skill bits of player+26512 and closes the selection (it reads no body).
    {
        PacketBuffer p(static_cast<Opcode>(0x00F0), is_paula());
        p.write<uint16_t>(sid);
        p.write<uint8_t>(lv);
        send(p);
    }

    // sub_8911A0: the costs (sub_5244C0: HpCon -> S2C 0x24 [u32 HP], MpCon — through the discount chain of
    // kind 1, sub_717290 (Bolster Ballad) — -> S2C 0x25 [u32 MP], each only when affordable), then the cool
    // time (CoolTime - 0.5 s, sub_88F740).
    const EsaRow* ii = server_.esa_db().row(d->esaii_table, lv);
    if (ii) {
        for (const auto& f : ii->fields) {
            const int t = esa_cond_type(f.first);
            int v = static_cast<int>(f.second);
            if (t == kCondHpCon && v && hp_ >= v) {
                hp_ = std::min(hp_ - v, hp_max_);
                PacketBuffer p(static_cast<Opcode>(0x0024), is_paula());
                p.write<uint32_t>(static_cast<uint32_t>(hp_));
                send(p);
            } else if (t == kCondMpCon && (v = mp_discount(v)) != 0 && mp_ >= v) {
                mp_ = std::min(mp_ - v, mp_max_);
                PacketBuffer p(static_cast<Opcode>(0x0025), is_paula());
                p.write<uint32_t>(static_cast<uint32_t>(mp_));
                send(p);
            }
        }
        // ESABloodyOath on us (+1616) and a dark skill (SkillParam +96 eSkillSymbolType 8) with no cost held (+1618):
        // +1618 = its MpCon (the ESAII condition 7); the HP at most (int)(+1620 HPRatio * that) -> +1616 off.
        if (bloody_on_ && d->symbol_type == 8 && bloody_cost_ == 0) {
            const double* mpc = esa_cond(ii, kCondMpCon);
            bloody_cost_ = static_cast<int16_t>(mpc ? static_cast<int32_t>(*mpc) : 0);
            if (hp_ <= static_cast<int32_t>(static_cast<double>(bloody_hp_) * static_cast<double>(bloody_cost_)))
                bloody_on_ = false;
        }
        if (const double* ct = esa_cond(ii, kCondCoolTime)) {
            const float secs = static_cast<float>(*ct) - 0.5f;
            if (secs > 0.0f)
                skill_cd_[sid] = now + std::chrono::milliseconds(static_cast<int>(secs * 1000.0));
        }
    }
    crazy_add(true);                                         // the Bear's Crazy Hammer gauge: SkillPoint (skill_bear.cpp)

    // The ESAction: its own handler in the switch sub_5BE9A0 (SkillParam +88), else the generic ES
    // (sub_524950). A handled one of a debuff class (4 / 5 / 6) then uses up the scroll at Player +0xC94
    // (the generic ES does it in sub_524690). (UseStyle 0x2000 — the scrolls — also counts in the
    // server's statistics, sub_7B3B60(+6280, 12, 1): not modelled.)
    const EsaRow* act = server_.esa_db().row(d->esa_table, lv);
    bool handled = true;
    switch (d->esaction) {
    case 1078: esa_steel_punch(*d, act, now); break;
    case 1082: esa_magic_arrow(*d, act, now); break;
    case 1085: esa_heal(*d, act, now);        break;
    case 1103: esa_tornado_attack(*d, act, now); break;
    case 1297: esa_fairy_of_magic(*d, act, now); break;
    case 1147: esa_item_detector(*d, act);    break;
    case 1170: esa_basic_detector(*d, act, now); break;
    case 1121: case 1124: case 1179: case 1180: case 1307:
               esa_firework(*d, 0);           break;
    case 1137: case 1181: case 1291:
               esa_firework(*d, 89);          break;
    case 1197: esa_firework(*d, 1);           break;
    case 1222: esa_heart_bomb(*d);            break;
    case 1080: case 1090: case 1094: case 1100: case 1123: case 1191: case 1296: case 1298: case 1299:
               esa_phys_strike(*d, act, now, d->esaction); break;
    case 1086: esa_magic_ring(*d, act, now);  break;
    case 1087: esa_magic_storm(*d, act, now); break;
    case 1328: esa_prevent_slow(*d, lv, act, 1328); break;
    case 1269: esa_prevent_slow(*d, lv, act, 1270); break;   // Strong Mind: ESAInsulatorDo
    case 1313: esa_prevent_slow(*d, lv, act, 1313); break;   // Gas Mask: ESAGasMaskDo
    case 1314: esa_prevent_slow(*d, lv, act, 1314); break;   // Lightning Rod: ESAInsulateArmorDo
    case 1214: esa_heavy_baggage(*d, act, now);      break;
    case 1242: esa_adhesive(*d, lv, act, now);       break;
    case 1326: esa_wide_throw(*d, act, now);         break;
    case 1362: esa_chain_throw(*d, act, now);        break;
    case 1475: esa_up_throw(*d, act, now);           break;
    case 1290: esa_sudden_attack(*d, act, now);      break;
    case 1344: esa_poison_baggage(*d, lv, act, now); break;
    case 1383: esa_poison_assault(*d, lv, act, now); break;
    case 1490: esa_crack_attack(*d, act, now);       break;
    case 1277: esa_reckless(*d, act, now);           break;
    case 1491: esa_bunker(*d, lv, act);              break;
    case 1497: esa_bunker_release(*d);               break;
    case 1139: esa_volley_kick(*d, act, now);        break;
    case 1199: esa_siren(*d, act, now);              break;
    case 1216: esa_rush(*d, act, now);               break;
    case 1329: esa_gellder_chain_hit(*d, act, now);  break;
    case 1364: esa_smell_ex(*d, lv, act, now);       break;
    case 1477: esa_buff_cancler(*d, lv, act, now);   break;
    case 1258: esa_two_chain_attack(*d, act, now);   break;
    case 1346: esa_wild_talon(*d, act, now);         break;
    case 1381: esa_sharp_scream(*d, act, now);       break;
    case 1493: esa_beast(*d, lv, act);               break;
    case 1194: esa_one_pair(*d, act, now);           break;
    case 1220: esa_charging(*d, lv, act, now);       break;
    case 1246: esa_wild_attack(*d, act, now);        break;
    case 1312: esa_mana_convert(*d, act);            break;
    case 1330: esa_lucky_gellder_hit(*d, act, now);  break;
    case 1271: esa_power_charging(*d, lv, act, now); break;
    case 1347: esa_massive_attack(*d, lv, act, now); break;
    case 1495: esa_four_card(*d, lv, act);           break;
    case 1393: esa_card_identify(*d, now);           break;
    case 1452: case 1455:
               esa_gm_cheer(*d);                     break;
    case 1454: case 1457:
               esa_gm_dignity(*d, act, now);         break;
    case 1167: esa_uppercut(*d, act, now);    break;
    case 1195: esa_chain_punch(*d, act, now, false); break;
    case 1327: esa_chain_punch(*d, act, now, true);  break;
    case 1469: esa_stun_cancel(*d, act);      break;
    case 1218: esa_fast_sword(*d, act, now);  break;
    case 1244: esa_power_brandish(*d, act, now); break;
    case 1310: esa_blast_brandish(*d, act, now); break;
    case 1325: esa_wind_brandish(*d, act, now, 5, 5.0); break;
    case 1336: esa_wind_brandish(*d, act, now, 7, 7.0); break;
    case 1261: esa_power_sea_brandish(*d, act, now); break;
    case 1262: esa_ones_other_self(*d, act, now);    break;
    case 1287: esa_fluorescene(*d, lv, act, now);    break;
    case 1482: esa_self_cure(*d, lv, act);           break;
    case 1156: case 1158: case 1257:
               esa_water_ball(*d, act, now);         break;
    case 1263: esa_electric_spear(*d, act, now);     break;
    case 1264: esa_fire_dragon(*d, act, now);        break;
    case 1265: esa_earthquake(*d, act, now);         break;
    case 1284: esa_mustang_wind(*d, act, now);       break;
    case 1484: esa_dimension_crack(*d, lv, act, now); break;
    case 1163: case 1164: case 1187:
               esa_dark_light_bolt(*d, act, now);    break;
    case 1168: esa_first_aid(*d, act);               break;
    case 1190: esa_light_wave(*d, act, now);         break;
    case 1215: esa_gravity(*d, act, now);            break;
    case 1219: esa_primary_cure(*d, act);            break;
    case 1256: esa_shining_ray(*d, act, now);        break;
    case 1323: esa_wide_first_aid(*d, act);          break;
    case 1474: esa_revival(*d, act);                 break;
    case 1259: esa_med_cure(*d, act);                break;
    case 1343: esa_light_weight(*d, act, now);       break;
    case 1486: esa_settle_down(*d, lv, act);         break;
    case 1282: case 1283:
               esa_death_scythe(*d, act, now);       break;
    case 1488: esa_dark_up(*d, lv, act);             break;
    case 1159: esa_earth_divide(*d, act, now);       break;
    case 1184: esa_rock_summons(*d, act, now);       break;
    case 1185: esa_fire_explosion(*d, act, now);     break;
    case 1321: esa_sharp_wind(*d, act, now);         break;
    case 1356: esa_call_lightning(*d, act, now);     break;
    case 1318: esa_fire_burn(*d, lv, act, now);      break;
    case 1472: esa_element_up(*d, lv, act);          break;
    case 1352: esa_earthquake_sword(*d, act, now); break;
    case 1470: esa_electric_sword(*d, act, now); break;
    case 1335: esa_fighting_fire(*d, act, now);  break;
    case 1481: esa_physical_match(*d, act, now); break;
    case 1138: esa_gun_stock(*d, act, now);   break;
    case 1148: esa_power_shot(*d, act, now);  break;
    case 1174: esa_melee_shot(*d, act, now);  break;
    case 1196: esa_unhappy_day(*d, act, now); break;
    case 1249: esa_double_shot(*d, act, now); break;
    // the Bear line (skill_bear.cpp — a reconstruction: the retail switch ends at 1497)
    case 1551: esa_hammer_ap(*d, act, now);          break;
    case 1523: esa_multi_hammer(*d, act, now, 2);    break;
    case 1535: esa_multi_hammer(*d, act, now, 3);    break;
    case 1539: esa_fly_hammer(*d, act, now);         break;
    case 1530: esa_area_hammer(*d, act, now, true);  break;
    case 1546: esa_area_hammer(*d, act, now, false); break;
    default:
        handled = false;
        if (esa_has_handler(d->esaction)) {
            LOG_INFO("GameClientSession %u: skill %u ESAction %d (%s) handler not implemented yet", session_id(),
                     sid, d->esaction, d->esa_table.c_str());
            break;
        }
        // sub_8911A0 / sub_7BEF40: a skill with a DebuffBitCode selecting an object (ESAUI 1) on a monster with
        // a DebuffSet (zone vtbl+16 kind 2) -> sub_891F80; all resisted -> S2C 1247 instead of the ES.
        es_resisted_ = 0;
        if (d->debuff_bits && d->esaui == 1) {
            MonsterEntity* tm = su_.target ? server_.entity_mgr().get_monster(su_.target) : nullptr;
            uint32_t bits = 0;
            if (tm && tm->zone == zone_id_ && monster_debuff_immune(*tm, d->debuff_bits, bits)) {
                send_debuff_resisted(*tm, bits);
                break;
            }
            es_resisted_ = bits;                             // the parts resisted go to the ES (+192)
        }
        es_apply_skill(*d, lv, act);
        es_resisted_ = 0;
        break;
    }
    if (handled) scroll_use_up(d->esclass, d->esaction);
    LOG_DEBUG("GameClientSession %u: skill %u lv %u fired (MP %d/%d)", session_id(), sid, lv, mp_, mp_max_);
    skill_state14_done();                                    // (fired from state 14: its tick's 30107)
    su_ = SkillUse{};                                        // sub_72C020: the ESAIC is reset
    skill_run_follow();                                      // the queued event 30107
}

// ─────────────────────────────────────────────────────────────────────────────
//  Damage skills — the shared pieces of the retail ESAction handlers.
// ─────────────────────────────────────────────────────────────────────────────

// Event 30182 on a target whose resist is over 100 (sub_5E6760 / sub_5E6510): Character vtbl+252
// (sub_718B80) HP += the absorbed part (at most MaxHP) and S2C 719 [u16 id][u32 amount] to the area
// (sub_601660). VERIFIED: the client's field router sub_6F0960 case 719 hands it to the actor
// (sub_49DCE0 case 0x2CF): HP (+452) += amount up to the max (+496) and the heal number.
void GameClientSession::absorb_heal(MonsterEntity& m, int32_t amount)
{
    m.hp = std::min(m.hp + amount, m.hp_max);
    PacketBuffer p(static_cast<Opcode>(0x02CF), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(m.object_id));
    p.write<uint32_t>(static_cast<uint32_t>(amount));
    server_.broadcast_zone(zone_id_, p, 0);
    LOG_DEBUG("GameClientSession %u: mob %u absorbs %d -> HP %d/%d", session_id(), static_cast<unsigned>(m.object_id),
              amount, m.hp, m.hp_max);
}

// sub_5E6760's element -> the target's resist slot (sub_718BE0): 0 PhysicalR (20), 1..128 the
// elements (11..18), 256 GunR (19), 512 none, 1024 slot 21.
static int element_slot(int element)
{
    switch (element) {
    case 0:    return 20;
    case 1:    return 11;
    case 2:    return 12;
    case 4:    return 13;
    case 8:    return 14;
    case 16:   return 15;
    case 32:   return 16;
    case 64:   return 17;
    case 128:  return 18;
    case 256:  return 19;
    case 1024: return 21;
    default:   return -1;
    }
}

// sub_5E6760 on a hit: DamageInfo[3] = the element, dmg -= the target's resist of it (> 100: the
// target absorbs, event 30182 — the heal is absorb_heal, applied by skill_damage_event).
void GameClientSession::skill_resist_hit(const MonsterEntity& m, int element, SkillDamage& h)
{
    const int slot = element_slot(element);
    const int r = (slot >= 0) ? m.resist_slot(slot) : 0;
    int32_t absorbed = 0;
    h.dmg = skill_resist(h.dmg, r, absorbed);
    h.element  = element;
    h.absorbed = absorbed;                                   // event 30182, applied with the damage
}

// sub_5E6040: the physical skill damage of AP' against a monster (the PvE factor sub_5E7050 = 1.0).
//   DamageInfo starts {0, 2 (death type), 1 (miss), 0, 0}; a protected target (+92 bit 1: result 2 — players
//   only) or a frozen one (+0x148 flag 32, ESAChillingPrison: result 1, flags |= 0x200) takes no roll
//   hit  = rand(100) < clamp((int)((AC - HV*2.5 + 40) * 1.2 * ApplyRatio), 20, 100)   (sub_5E5E10)
//   crit = rand(100) < clamp((int)((AC + LK*1.2 - (tLK + tHV)) / 4), 5, 40)            (sub_5E5F80)
//   crit (int)(1.5*AP' - 0.75*DP) >= 1, result 3 / 131; else (int)(AP' - 0.75*DP) >= 0, result 0,
//   |0x80 when it kills (sub_5E4B80 / sub_5E4B30) — then the element's resist (sub_5E6760).
GameClientSession::SkillDamage GameClientSession::skill_phys_damage(const MonsterEntity& m, int ap2, float apply_ratio,
                                                                     int element, int dp_arg)
{
    SkillDamage h;
    h.death_type = 2;
    if (m.skill_flags & 32) { h.flags |= 0x200; return h; }  // a frozen target (+0x148 32): a miss with 0x200
    const int dp = (dp_arg >= 0) ? dp_arg : m.dp;            // the caller's DP argument (the target's +600)
    const int hitc = clampi(static_cast<int>((st_ac_ - m.hv * 2.5 + 40.0) * 1.2000000476837158 * apply_ratio), 20, 100);
    if (rnd(100) >= hitc) return h;
    const int critc = clampi(static_cast<int>((st_ac_ + st_lk_ * 1.2000000476837158 - (m.lk + m.hv)) / 4.0), 5, 40);
    if (rnd(100) < critc) {
        h.dmg = static_cast<int32_t>(static_cast<float>(ap2) * 1.5f - static_cast<float>(dp) * 0.75f);
        if (h.dmg <= 0) h.dmg = 1;
        h.result = (h.dmg < m.hp) ? 3 : 131;
    } else {
        h.dmg = static_cast<int32_t>(static_cast<float>(ap2) * 1.0f - static_cast<float>(dp) * 0.75f);
        if (h.dmg < 0) h.dmg = 0;
        h.result = (h.dmg >= m.hp) ? 0x80 : 0;
    }
    skill_resist_hit(m, element, h);
    return h;
}

// sub_5E53C0: the gun skill damage against a monster (defense factor 30, the PvE factor 1.0).
//   DamageInfo starts {0, 4 (death type), 1 (miss), 256 (the gun element), 0}
//   hit  = rand(100) < clamp((int)((LK - tLK + 90) * 1.2 * ApplyRatio), 20, 100)      (sub_5E5EF0)
//   crit = the skill crit (sub_5E5F80) -> (int)(((AC-49)*20 + AP + 20) * 1.2 * (Ratio * the +1068
//          multiplier — 1.0, ESASharpSense_EX's Ratio while it runs) - HV*30) >= 1, result 3 / 131  (sub_5E75E0)
//   else (int)(float((AC-49)*20 + AP + 20) * Ratio - HV*30) >= 0, result 0 (1 when 0), |0x80 when
//          it kills                                                                  (sub_5E7540)
//   then GunR (sub_5E6760(.., 256): slot 19).
GameClientSession::SkillDamage GameClientSession::skill_gun_damage(const MonsterEntity& m, int gun_ap, float ratio,
                                                                    float apply_ratio)
{
    SkillDamage h;
    h.death_type = 4;
    h.element    = 256;
    if (m.skill_flags & 32) { h.flags |= 0x200; return h; }  // a frozen target (+0x148 32): a miss with 0x200
    const int hv = std::max(m.hv, 0);
    const int hitc = clampi(static_cast<int>((st_lk_ - m.lk + 90) * 1.2000000476837158 * apply_ratio), 20, 100);
    if (rnd(100) >= hitc) return h;
    const int critc = clampi(static_cast<int>((st_ac_ + st_lk_ * 1.2000000476837158 - (m.lk + m.hv)) / 4.0), 5, 40);
    if (rnd(100) < critc) {
        const float crit_mult = gun_skill_mult_;                         // attacker +1068 (ESASharpSense_EX)
        const float v = static_cast<float>(((st_ac_ - 49) * 20.0 + gun_ap + 20.0) * 1.200000047683716 *
                                           static_cast<double>(ratio * crit_mult));
        const float k = static_cast<float>(hv * 30.0);
        int32_t dd = static_cast<int32_t>(static_cast<double>(v) - static_cast<double>(k));
        if (dd <= 0) dd = 1;
        h.dmg    = dd;
        h.result = (h.dmg < m.hp) ? 3 : 131;
    } else {
        const float base = static_cast<float>((st_ac_ - 49) * 20.0 + gun_ap + 20.0);
        const float v = base * ratio;
        const float k = static_cast<float>(hv * 30.0);
        int32_t dd = static_cast<int32_t>(static_cast<double>(v) - static_cast<double>(k));
        if (dd < 0) dd = 0;
        h.dmg    = dd;
        h.result = h.dmg ? 0 : 1;
        if (h.dmg >= m.hp) h.result |= 0x80;
    }
    skill_resist_hit(m, 256, h);
    return h;
}

// The gun's AP (sub_41C150 / the skill handlers): its options 10012 (GunAp, sub_6E8140) else
// ItemParam AP; -1 without a gun in slot 1.
int32_t GameClientSession::equipped_gun_ap() const
{
    const BagItem* gb = equip_[1] ? const_cast<GameClientSession*>(this)->bag_find_uid(equip_[1]) : nullptr;
    const ItemDef* gd = gb ? server_.item_db().get(gb->item_id) : nullptr;
    if (!gb || !gd) return -1;
    int32_t ap = gd->ap;
    bool have = false;
    int32_t sum = 0;
    for (const ItemOption& o : gb->opts)
        if (o.id == 10012) { sum += o.value; have = true; }
    if (have) ap = static_cast<int16_t>(sum);
    return ap;
}

// The skill opener S2C 224 of a damage skill: sub_5EE050 = [u16 caster][u16 caster][u16 0][u16
// ESAction][u8 2][u8 ESClass][u16 skill][u16 EState]; sub_5EDE00 puts the target as the second id.
PacketBuffer GameClientSession::skill_opener(const SkillDef& d, uint16_t second_id, uint16_t es_id) const
{
    PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
    o.write<uint16_t>(static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(second_id);
    o.write<uint16_t>(es_id);                                // an ES class's opener: its object's id
    o.write<uint16_t>(static_cast<uint16_t>(d.esaction));
    o.write<uint8_t>(2);
    o.write<uint8_t>(static_cast<uint8_t>(d.esclass));
    o.write<uint16_t>(su_.skill);
    o.write<uint16_t>(static_cast<uint16_t>(d.estate));
    return o;
}

// The damage event 30002 on the monster (sub_729100): the owner mark on a damaging hit (sub_423B70),
// the HP, then S2C 0x002A [u16 id] + the 28-byte DamageInfo verbatim (sub_5F5940), the HP update
// 0x011E and on a kill 0x002B [u16 id][u8 death type] (sub_5F5B00), appended to the block. The
// DamageInfo's [5] is the attacker's kind (Character::OnMessage sub_715A90 sets it before vtbl+236: 1, a
// player). The message 30330 (Dimension Crack) is the same path with vtbl+240 (sub_728FE0): its damage
// goes out as S2C 1307 (sub_5F58B0; VERIFIED: the client's case 1307 hands [DamageInfo] to the object as
// its 42) — `opcode`.
// sub_5FC080: a monster's line of `situation` (MonSpeechDb, the pick sub_60CF40): S2C 184 [u16 its id][str] to its area
// — into `group` (the damage event's block, sub_729100: before its 0x2A), or at once (mon_speech_now: its battle state
// begins, CommonMonster sub_729E20 state 1). VERIFIED: the client's sub_6F0960 case 184 (the actor's speech balloon).
void GameClientSession::mon_speech(const MonsterEntity& m, int situation, std::vector<PacketBuffer>& group) const
{
    const std::string* s = server_.mon_speech_db().pick(static_cast<int>(m.mob_id), situation);
    if (!s) return;
    const std::string t = to_client_text(*s);
    PacketBuffer p(static_cast<Opcode>(0x00B8), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(m.object_id));
    p.write_bytes(reinterpret_cast<const uint8_t*>(t.c_str()), t.size() + 1);
    group.push_back(std::move(p));
}

void GameClientSession::mon_speech_now(const MonsterEntity& m, int situation)
{
    std::vector<PacketBuffer> one;
    mon_speech(m, situation, one);
    for (PacketBuffer& p : one) server_.broadcast_zone(m.zone, p, 0);
}

bool GameClientSession::skill_damage_event(MonsterEntity& m, const SkillDamage& h, std::vector<PacketBuffer>& group,
                                           uint16_t opcode)
{
    const uint16_t tid = static_cast<uint16_t>(m.object_id);
    if (h.absorbed > 0) absorb_heal(m, h.absorbed);          // 30182 (sub_5E6760, during the damage roll)
    if (!m.alive || m.hp <= 0) return false;                 // (a dead one: its 30002 does nothing, sub_729100)
    bloody_on_dealt(h);                                      // the 30097 to us before the damage (our ESs' notify):
    if (dark_ghost_on_dealt(m, h, group)) return true;       //   ESABloodyOath, ESADarkGhost (its hit may kill it)
    if (!m.alive || m.hp <= 0) return false;
    if (h.result == 3 || h.result == 4) mon_speech(m, 0, group);   // sub_729100: a crit / strong crit, its line
    if (h.dmg > 0) mark_monster_owner(m);
    int32_t hp = m.hp, mx = m.hp_max;
    bool killed = false;
    if (h.dmg > 0) { killed = server_.entity_mgr().damage_monster(tid, h.dmg, hp, mx); if (hp < 0) hp = 0; }
    else if (h.dmg < 0) { m.hp -= h.dmg; hp = m.hp; }      // sub_729100: HP -= damage — a negative one heals, past
                                                             //   MaxHP too (retail; e.g. ESAFullHouseHit not applied)
    {
        PacketBuffer p(static_cast<Opcode>(opcode), is_paula());
        p.write<uint16_t>(tid);
        p.write<int32_t>(h.dmg);                                    // [0] damage
        p.write<int32_t>(h.death_type);                             // [1] death type
        p.write<int32_t>(h.result);                                 // [2] result
        p.write<int32_t>(h.element);                                // [3] element
        p.write<int32_t>(h.flags);                                  // [4] flags
        p.write<int32_t>(1);                                        // [5] the attacker's kind (a player)
        p.write<int32_t>(h.extra);                                  // [6] (+24 u16: e.g. Double Shot's hits)
        group.push_back(std::move(p));
    }
    {
        PacketBuffer u(static_cast<Opcode>(0x011E), is_paula());
        u.write<uint16_t>(tid);
        u.write<int32_t>(hp);
        u.write<uint16_t>(0);
        u.write<uint8_t>(0);
        group.push_back(std::move(u));
    }
    if (killed) {
        PacketBuffer k(static_cast<Opcode>(0x002B), is_paula());
        k.write<uint16_t>(tid);
        k.write<uint8_t>(static_cast<uint8_t>(h.death_type));
        group.push_back(std::move(k));
    }
    return killed;
}

// After the block: the die handler of a kill (the owner's TM sub_891AB0, the loot), this hit's EXP
// (30003, the share taken before the damage — `exp_done` when the handler posted it first), the
// skill's TM per hit (sub_891C80, `tm_skill` 0 = none), the hate entry; a kill ends our follow on it
// and leaves the corpse, else the monster takes us as its target.
void GameClientSession::skill_after_hit(MonsterEntity& m, int32_t dmg, int32_t hp_before, bool killed,
                                        uint16_t tm_skill, bool exp_done, std::chrono::steady_clock::time_point now,
                                        int exp_kind)
{
    const uint16_t tid = static_cast<uint16_t>(m.object_id);
    int32_t tx, ty;
    m.motion.pos(now, tx, ty);
    if (killed) { owner_monster_quest(m); kill_tm_reward(m); server_.drop_monster_loot(m, tx, ty); }
    if (!exp_done) gain_attack_exp(m, dmg, hp_before, exp_kind);
    if (tm_skill) skill_hit_tm(m, tm_skill);
    if (killed) {
        if (attacking_ && atk_target_ == tid) end_attack(true);
        kill_monster_aftermath(m, tid, tx, ty);
    } else {
        engage_monster(m);
    }
}

// sub_891F80: a monster with a DebuffSet (MonsterParamEx2 +2462) against a skill's DebuffBitCode — for each
// bit i of the code the MonsterDebuffResist row's column i: 100, or another non-zero n with rand()%100 >= n,
// resists that bit (into `bits`); 0 or a failed roll does not. True when every bit was resisted (a code of
// 0 is resisted too, retail) — the caller then sends S2C 1247 instead of the effect state. False without a
// DebuffSet row.
bool GameClientSession::monster_debuff_immune(const MonsterEntity& m, uint32_t code, uint32_t& bits)
{
    bits = 0;
    if (!m.debuff_set) return false;
    const DebuffResistDb::Row* row = server_.debuff_resist_db().get(m.debuff_set);
    if (!row) return false;
    bool all = true;
    for (int i = 0; i < 31; ++i) {
        const uint32_t bit = 1u << i;
        if (!(code & bit)) continue;
        const int v = (*row)[i];
        if (v && (v == 100 || rnd(100) >= v)) bits |= bit;
        else all = false;
    }
    return all;
}

// S2C 1247 (sub_60AA00) [u16 target][u32 the resisted bits], to the area of the caster and the target.
// VERIFIED: the client's field router sub_6F0960 case 1247 hands it to the actor (sub_47B250), whose handler
// (sub_6BEC50 case 0x4DF) reads the u32 (sub_474840) and shows the resist.
void GameClientSession::send_debuff_resisted(const MonsterEntity& m, uint32_t bits)
{
    PacketBuffer p(static_cast<Opcode>(0x04DF), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(m.object_id));
    p.write<uint32_t>(bits);
    server_.broadcast_zone(zone_id_, p, 0);
    LOG_DEBUG("GameClientSession %u: boss mob %u resists the debuff (bits 0x%08X)", session_id(),
              static_cast<unsigned>(m.object_id), bits);
}

// sub_5C24E0 after a Steel Punch hit that is not a miss / block (result 1 / 2 / 6 / 8) on a target with HP
// left: our learned passives in the PS limiter (+4504, sub_54BA80) — Faint (1201, sub_5C25C0) puts an
// ESAPowerStun (1144) on it, then Guard Break (1101, sub_5C2710) an ESADefenseParalysis (1141): the action
// {that ESAction, EState 0, ESClass 1, flags 2, the passive's skill and its LEVEL 1 row} through
// sub_524690(.., 1); a monster with a DebuffSet first rolls against the passive's DebuffBitCode (sub_891F80):
// all resisted -> S2C 1247 instead.
void GameClientSession::steel_punch_passives(MonsterEntity& m)
{
    for (const auto& p : { std::pair<int, int>{ 1201, 1144 }, std::pair<int, int>{ 1101, 1141 } }) {
        const LearnedSkill* ls = find_skill(p.first);
        const SkillDef* sd = server_.skill_db().get(p.first);
        if (!ls || ls->level <= 0 || !sd) continue;
        if (!m.alive || m.hp <= 0) return;
        uint32_t bits = 0;
        if (monster_debuff_immune(m, sd->debuff_bits, bits)) { send_debuff_resisted(m, bits); continue; }
        SkillDef dd = *sd;
        dd.esaction = p.second;
        dd.estate   = 0;
        dd.esclass  = 1;
        es_apply_to_monster(dd, 1, server_.esa_db().row(sd->esa_table, 1));
    }
}

// ── ESAction 1078 "Steel Punch" (GameServer sub_5C2110; client ESASteelPunch) ──────────────
// Level row (ESAction_Steel_Punch, loader sub_51A3C0): ApplyRatio, Ratio. The caster marks the target
// (vtbl+132); AP' = (int)(AP * Ratio) (sub_560500) -> sub_5E6040 (element 0: PhysicalR). The command
// block: the opener 224 (sub_5EE050) + [u16 target][pos][pos] (sub_5C24A0) — VERIFIED: the client
// block runner sub_6EBF20 plays an opener 224 through sub_4A8680 (reads the 14 bytes, event 30098 ->
// the factory sub_55CD40 -> ESASteelPunch sub_4B9AD0 reads [u16 target][pos][pos]); then the damage
// event's 0x002A / 0x011E [/ 0x002B]; EXP (30003); the skill's TM per hit (sub_891C80). (Then
// sub_5C24E0: the learned passives Faint 1201 / Guard Break 1101 follow up a hit that is not a miss
// — steel_punch_passives above.)
void GameClientSession::esa_steel_punch(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    const float ratio       = act ? static_cast<float>(act->get("Ratio", 1.0)) : 1.0f;
    const float apply_ratio = act ? static_cast<float>(act->get("ApplyRatio", 1.0)) : 1.0f;
    mark_monster_owner(*m);                                   // Player vtbl+132 (sub_7601C0)
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) * ratio);
    SkillDamage h = skill_phys_damage(*m, ap2, apply_ratio, 0);
    const int32_t hp_before = m->hp;                          // vtbl+264 before 30002

    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    m->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    o.write<uint16_t>(static_cast<uint16_t>(ax)); o.write<uint16_t>(static_cast<uint16_t>(ay));
    o.write<uint16_t>(static_cast<uint16_t>(tx)); o.write<uint16_t>(static_cast<uint16_t>(ty));
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: Steel Punch -> mob %u ratio=%.2f dmg=%d result=%d hp=%d/%d%s", session_id(), tid,
              ratio, h.dmg, h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "");
    skill_follow_after(tid);                                  // 30107 (the flag 2 clear, the target alive)
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, false, now);
    const int r = h.result;                                   // (DamageInfo[2] as it is)
    if (r != 1 && r != 2 && r != 6 && r != 8 && m->alive && m->hp > 0) steel_punch_passives(*m);   // sub_5C24E0
}

// ── ESAction 1148 "Power Shot" (GameServer sub_5C8290; ESAIC Chase, ESAUI select an object) ───
// The caster marks the target (vtbl+132). The gun in slot 1 (none -> "Player::POWERSHOT_SKILL_CANCEL")
// and its AP; the gun skill damage sub_5E53C0 (ESAction_PowerShot: ApplyRatio, Ratio); the EXP share;
// ONE bullet (sub_894560(.., 1)); the block {224 opener + [u16 target][pos][pos] (sub_5C85A0), the
// damage event}; EXP; the skill's TM per hit. Client ESAPowerShot sub_4D2AC0 reads [u16][pos][pos].
void GameClientSession::esa_power_shot(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    mark_monster_owner(*m);
    const int32_t gun_ap = equipped_gun_ap();
    if (gun_ap < 0) {
        LOG_INFO("Player::POWERSHOT_SKILL_CANCEL %u,%s", object_id_, char_name_);
        return;
    }
    const float ratio       = act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f;
    const float apply_ratio = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    SkillDamage h = skill_gun_damage(*m, gun_ap, ratio, apply_ratio);
    const int32_t hp_before = m->hp;
    consume_bullets(1);

    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    m->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    o.write<uint16_t>(static_cast<uint16_t>(ax)); o.write<uint16_t>(static_cast<uint16_t>(ay));
    o.write<uint16_t>(static_cast<uint16_t>(tx)); o.write<uint16_t>(static_cast<uint16_t>(ty));
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: Power Shot -> mob %u ap=%d ratio=%.2f dmg=%d result=%d hp=%d/%d%s", session_id(),
              tid, gun_ap, ratio, h.dmg, h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "");
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, false, now);
}

// ── ESAction 1138 "Butt Plate" (GameServer sub_5C6AE0, ESAGunStock) ────────────────────────────
// The caster marks the target (vtbl+132); AP' = (int)((8 * (AC + DA) - 392) * Ratio) -> the physical
// skill damage sub_5E6040 with the gun element 256 (GunR); the block {224 opener + [u16 target][pos]
// [pos] (sub_5C6D80), the damage event}; EXP; the skill's TM per hit. Client ESAGunStock sub_4CDAA0
// reads [u16][pos][pos].
void GameClientSession::esa_gun_stock(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    mark_monster_owner(*m);
    const float ratio       = act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f;
    const float apply_ratio = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    const int v = 8 * (st_ac_ + st_da_) - 392;
    const int ap2 = static_cast<int>(static_cast<double>(v) * ratio);
    SkillDamage h = skill_phys_damage(*m, ap2, apply_ratio, 256);
    const int32_t hp_before = m->hp;

    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    m->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    o.write<uint16_t>(static_cast<uint16_t>(ax)); o.write<uint16_t>(static_cast<uint16_t>(ay));
    o.write<uint16_t>(static_cast<uint16_t>(tx)); o.write<uint16_t>(static_cast<uint16_t>(ty));
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: Butt Plate -> mob %u ap'=%d dmg=%d result=%d hp=%d/%d%s", session_id(), tid, ap2,
              h.dmg, h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "");
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, false, now);
}

// ── ESAction 1196 "Lucky Fist" (GameServer sub_5CB900, ESAUnHappyDay) ─────────────────────────
// Clears the caster's +0x148 flag 2; AP' = (int)((8*LK + the target's MaxHP*0.1) * Ratio) -> the
// physical skill damage sub_5E6040 (element 0); the damage event 30002 (sub_414CB0 runs it at once:
// its 0x002A / 0x011E [/ 0x002B] go out BEFORE the block, unblocked — retail order), then the block:
// the opener 224 + [u16 target][pos][pos] (sub_5CBCF0; client ESAUnHappyDay sub_4E0E80 reads
// [u16][pos][pos]); EXP; the 30107 follow-up (no gun); the skill's TM per hit.
void GameClientSession::esa_unhappy_day(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    su_.resume = false;                                       // sub_5101D0(+0x148, 2)
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    const float ratio       = act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f;
    const float apply_ratio = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    const int ap2 = static_cast<int>((static_cast<double>(8 * st_lk_) +
                                      static_cast<double>(m->hp_max) * 0.1000000014901161) * ratio);
    SkillDamage h = skill_phys_damage(*m, ap2, apply_ratio, 0);
    const int32_t hp_before = m->hp;

    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    m->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> dmg;
    const bool killed = skill_damage_event(*m, h, dmg);
    for (PacketBuffer& p : dmg) server_.broadcast_zone(zone_id_, p, 0);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    o.write<uint16_t>(static_cast<uint16_t>(ax)); o.write<uint16_t>(static_cast<uint16_t>(ay));
    o.write<uint16_t>(static_cast<uint16_t>(tx)); o.write<uint16_t>(static_cast<uint16_t>(ty));
    group.push_back(std::move(o));
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: Lucky Fist -> mob %u ap'=%d dmg=%d result=%d hp=%d/%d%s", session_id(), tid, ap2,
              h.dmg, h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "");
    skill_follow_after(tid, true);
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, false, now);
}

// ── ESAction 1249 "Double Shot" (GameServer sub_5D1420) ───────────────────────────────────────
// The gun in slot 1 and a loaded magazine, else nothing. Two gun skill shots (sub_5D1660: each
// sub_5E53C0 with Ratio / ApplyRatio, against the same HP), each kept as a record {u32 dmg, u32
// result, u16 -1} (sub_5205F0: a miss / no damage keeps {0, result}; the target's guard ESs 1333 /
// 1492 / 1309 / 1463 would soak a hit — monsters have none here); the damage = the hits' sum. The
// opener is sub_5EDE00's ([caster][TARGET][0]...); the client's ESADoubleShot (sub_4E8430) reads
// [u16 target] + 2 x [u32][u32][u16] — the target id is written here as the client needs it (the
// recovered server build writes only the records). One damage event (sub_5D1760): DamageInfo
// {sum, 4, 0, 256, 0x8000, 0, +24 = the number of hits}, its EXP first; then 2 bullets; the caster's
// mark (vtbl+132) after; the skill's TM per hit.
void GameClientSession::esa_double_shot(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    const int32_t gun_ap = equipped_gun_ap();
    if (gun_ap < 0 || !mag_.item) return;
    const float ratio       = act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f;
    const float apply_ratio = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    struct Rec { int32_t dmg; int32_t result; uint16_t v; } rec[2];
    int32_t total = 0;
    int hits = 0;
    for (int i = 0; i < 2; ++i) {
        const SkillDamage s = skill_gun_damage(*m, gun_ap, ratio, apply_ratio);
        if (s.absorbed > 0) absorb_heal(*m, s.absorbed);        // 30182 per shot
        if (s.dmg <= 0 || s.result == 1 || s.result == 2) {
            rec[i] = { 0, s.result, 0xFFFF };
        } else {
            total += s.dmg;
            ++hits;
            rec[i] = { s.dmg, s.result, 0xFFFF };
        }
    }
    SkillDamage h;
    h.dmg        = total;
    h.death_type = 4;
    h.result     = 0;
    h.element    = 256;
    h.flags      = 0x8000;
    h.extra      = hits;
    const int32_t hp_before = m->hp;
    gain_attack_exp(*m, total, hp_before);                    // sub_5D1760: 30003 before 30002

    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, tid);
    o.write<uint16_t>(tid);
    for (const Rec& r : rec) {
        o.write<int32_t>(r.dmg);
        o.write<int32_t>(r.result);
        o.write<uint16_t>(r.v);
    }
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    consume_bullets(2);
    mark_monster_owner(*m);                                   // vtbl+132 after the damage
    LOG_DEBUG("GameClientSession %u: Double Shot -> mob %u ap=%d shots %d/%d + %d/%d = %d hp=%d/%d%s", session_id(),
              tid, gun_ap, rec[0].dmg, rec[0].result, rec[1].dmg, rec[1].result, total, m->hp, m->hp_max,
              killed ? " [KILLED]" : "");
    skill_after_hit(*m, total, hp_before, killed, su_.skill, true, now);
}

// ── The physical single-target strikes (sub_5E6040, element 0) ─────────────────────────────
//  1080 Burning Rave (sub_5C2860, ESATwinHit) / 1100 Upper Smash (sub_5C4650, ESAUpperAttack —
//    it first clears the caster's +328 flag 2): AP' = (int)(AP * Ratio) (sub_560500); no mark first;
//    the opener sub_5EE050 + [u16 target][pos][pos] (sub_5C2B70 / sub_5C49D0).
//  1123 Final Blow (sub_5C5580, ESAStrikeOfSoul): the caster marks the target (vtbl+132); AP' =
//    (int)((AP + (MaxHP - HP) / 10.0) * Ratio); the same opener + tail (sub_5C5910).
//  1296 Punch (sub_5D60C0) / 1299 Warm Up (sub_5D6930) / 1298 Stone Shower (sub_5D6600): the mark; AP' =
//    Ratio2 + Ratio1 * min(level, 30) + AP (ESAction_Punch / _WarmingUp / _StoneThrowing, loader
//    sub_51C800: int Ratio1 +4, int Ratio2 +8); the opener sub_5EDE00 ([caster][TARGET][0]...) with no
//    tail; EXP kind 1.
//  1090 Stone Strike (sub_5C2DC0, ESAStoneHit): no mark; AP' = (int)(20 * (DA + 1) * Ratio); the opener
//    sub_5EE050 + [u16 target][pos][pos] (sub_5C31D0).
//  1094 Power Blow (sub_5C3210, ESAStrike): no mark; Ratio += the learned Titanium Wrist's (4011, a
//    "Surpport" passive) row Ratio (sub_50F140: +4), ApplyRatio = its row's (sub_8938A0; 0 / not learned
//    -> the Power Blow row's); AP' = (int)(AP * Ratio) (sub_560500); the opener + [u16 target][pos][pos]
//    (sub_5C35F0).
//  Then the damage event, EXP (30003), the 30107 follow-up (Stone Strike and Stone Shower only without a
//  gun, sub_6DA700(54)), the skill's TM per hit. The client's classes (ESATwinHit, ESAUpperAttack,
//  ESAStrikeOfSoul, ESAStoneHit sub_4BB540, ESAStrike sub_4BBD20) read [u16][pos][pos]; ESAPunch /
//  ESAWarmingUp / ESAStoneThrowing (sub_4F9F90) read nothing.
void GameClientSession::esa_phys_strike(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now,
                                        int variant)
{
    if (variant == 1100) su_.resume = false;                  // sub_5101D0(+0x148, 2)
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    float apply_ratio = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    int ap2 = 0;
    bool tail = true;
    int exp_kind = 0;
    bool no_gun = false;
    bool follow = true;
    switch (variant) {
    case 1080:
    case 1100:
        ap2 = static_cast<int>(static_cast<double>(st_ap_) * (act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f));
        break;
    case 1191:                                               // Piercing Wave (sub_5CB180): the mark, no 30107
        mark_monster_owner(*m);
        ap2 = static_cast<int>(static_cast<double>(st_ap_) * (act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f));
        follow = false;
        break;
    case 1090: {
        const float ratio = act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f;
        const int v = 20 * (st_da_ + 1);
        ap2 = static_cast<int>(static_cast<double>(v) * ratio);
        no_gun = true;
        break;
    }
    case 1094: {
        float ratio = act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f;
        float apply = 0.0f;
        if (const LearnedSkill* tw = find_skill(4011)) {
            const SkillDef* td = server_.skill_db().get(4011);
            const EsaRow* r = (td && tw->level > 0) ? server_.esa_db().row(td->esa_table, tw->level) : nullptr;
            if (r) {
                if (td->support) ratio = ratio + static_cast<float>(r->get("Ratio", 0.0));
                apply = static_cast<float>(r->get("ApplyRatio", 0.0));
            }
        }
        if (apply > 0.0f) apply_ratio = apply;
        ap2 = static_cast<int>(static_cast<double>(st_ap_) * ratio);
        break;
    }
    case 1123: {
        mark_monster_owner(*m);
        const int missing = hp_max_ - hp_;
        const float ratio = act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f;
        ap2 = static_cast<int>((static_cast<double>(st_ap_) + static_cast<double>(missing) / 10.0) * ratio);
        break;
    }
    case 1296:
    case 1298:
    case 1299: {
        mark_monster_owner(*m);
        const int lv = std::min<int>(level_, 30);
        const int r1 = act ? static_cast<int>(act->get("Ratio1", 0.0)) : 0;
        const int r2 = act ? static_cast<int>(act->get("Ratio2", 0.0)) : 0;
        ap2 = r2 + r1 * lv + st_ap_;
        tail = false;
        exp_kind = 1;
        no_gun = (variant == 1298);
        break;
    }
    default:
        return;
    }
    SkillDamage h = skill_phys_damage(*m, ap2, apply_ratio, 0);
    const int32_t hp_before = m->hp;

    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    m->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, tail ? static_cast<uint16_t>(object_id_) : tid);
    if (tail) {
        o.write<uint16_t>(tid);
        o.write<uint16_t>(static_cast<uint16_t>(ax)); o.write<uint16_t>(static_cast<uint16_t>(ay));
        o.write<uint16_t>(static_cast<uint16_t>(tx)); o.write<uint16_t>(static_cast<uint16_t>(ty));
    }
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: ESAction %d -> mob %u ap'=%d dmg=%d result=%d hp=%d/%d%s", session_id(), variant,
              tid, ap2, h.dmg, h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "");
    if (follow) skill_follow_after(tid, no_gun);              // 30107
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, false, now, exp_kind);
}

// sub_5E74E0: the magic formula — (int)((int)(float(base * (MA - offset) / 100) - float(MD * 0.9)) *
// factor), at least 0.
static int32_t magic_formula(int base, int ma, int md, float factor)
{
    const float v6 = static_cast<float>(static_cast<double>(base) * 1.0 * (static_cast<double>(ma) / 100.0));
    const float v7 = static_cast<float>(static_cast<double>(md) * 0.8999999761581421);
    const int32_t inner = static_cast<int32_t>(static_cast<double>(v6) - static_cast<double>(v7));
    const int32_t v5 = static_cast<int32_t>(static_cast<double>(inner) * factor);
    return v5 < 0 ? 0 : v5;
}

// sub_5E6270: the magic skill damage against a monster (the PvE factor 1.0).
//   DamageInfo starts {0, 3 (death type), 1 (miss), 0, 0}; protected / frozen: as sub_5E6040
//   hit  = rand(100) < clamp((int)((LK - tLK + 90) * 1.2 * ApplyRatio), 20, 100)      (sub_5E5EF0)
//   crit = the skill crit (sub_5E5F80): magic((int)(base * 1.1), MA - offset, MD), result 3 / 131
//   else magic(base, MA - offset, MD), result 0 (128 when it kills)            (sub_5E74E0)
//   then the element's resist (sub_5E6760); element 64 (dark) then adds (int)(+1624 * damage) while the caster's
//   +1616 (ESABloodyOath) is on.
GameClientSession::SkillDamage GameClientSession::skill_magic_damage(const MonsterEntity& m, int base, float apply_ratio,
                                                                      int ma_offset, int element)
{
    SkillDamage h;
    h.death_type = 3;
    if (m.skill_flags & 32) { h.flags |= 0x200; return h; }  // a frozen target (+0x148 32): a miss with 0x200
    const int hitc = clampi(static_cast<int>((st_lk_ - m.lk + 90) * 1.2000000476837158 * apply_ratio), 20, 100);
    if (rnd(100) >= hitc) return h;
    const int critc = clampi(static_cast<int>((st_ac_ + st_lk_ * 1.2000000476837158 - (m.lk + m.hv)) / 4.0), 5, 40);
    const int ma = stat_[4] - ma_offset;
    if (rnd(100) < critc) {
        h.dmg    = magic_formula(static_cast<int>(static_cast<double>(base) * 1.100000023841858), ma, m.md, 1.0f);
        h.result = (h.dmg < m.hp) ? 3 : 131;
    } else {
        h.dmg    = magic_formula(base, ma, m.md, 1.0f);
        h.result = (h.dmg < m.hp) ? 0 : 128;
    }
    skill_resist_hit(m, element, h);
    if (element == 64 && bloody_on_)                         // sub_5E76B0 / sub_5E76D0: + (int)(+1624 * damage)
        h.dmg += static_cast<int32_t>(static_cast<double>(bloody_ma_) * static_cast<double>(h.dmg));
    return h;
}

// ── ESAction 1082 "Mana Arrow" (GameServer sub_5C3630, ESAMagicArrow) ──────────────────────────
// A player: the arrows = the learned Arrow Rush (2007, a "Surpport" passive) row's Count (i16 +4,
// else 1, sub_50EFE0), its Ratio (+8, sub_50F140) and ApplyRatio (+0, sub_8938A0) — without Arrow
// Rush both are 1.0 (the Mana Arrow row's own ApplyRatio is not used); the caster enters state 46.
// Each arrow: the magic damage sub_5E6270((int)(AP * Ratio), .., ApplyRatio, MA offset 49, element
// 128 (NoPropR)), its EXP share, the block {224 opener + [u16 target][pos][pos] (sub_5C3980), the
// damage event}, and EXP kind 1 only when the share is above 0. Then the skill's TM per hit.
void GameClientSession::esa_magic_arrow(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    int   arrows = 1;
    float ratio  = 0.0f;
    float apply  = 0.0f;
    if (const LearnedSkill* ar = find_skill(2007)) {
        const SkillDef* sd = server_.skill_db().get(2007);
        if (ar->level > 0 && sd && sd->support) {
            if (const EsaRow* r = server_.esa_db().row(sd->esa_table, ar->level)) {
                arrows = static_cast<int16_t>(static_cast<int>(r->get("Count", 1.0)));
                ratio  = static_cast<float>(r->get("Ratio", 0.0));
                apply  = static_cast<float>(r->get("ApplyRatio", 0.0));
            }
        }
    }
    if (ratio <= 0.0f) { ratio = 1.0f; apply = 1.0f; }
    const int32_t ap = act ? static_cast<int32_t>(act->get("AP", 0.0)) : 0;
    const int base = static_cast<int>(static_cast<double>(ap) * ratio);
    // (the loop goes on after a kill: the remaining arrows' openers still fly; the dead monster's
    // 30002 does nothing and its EXP share is 0)
    bool killed_any = false;
    int32_t kx = 0, ky = 0;
    for (int i = 0; i < arrows; ++i) {
        SkillDamage h = skill_magic_damage(*m, base, apply, 49, 128);
        const int32_t hp_before = m->alive ? m->hp : 0;
        int32_t ax, ay, tx, ty;
        cur_pos(now, ax, ay);
        m->motion.pos(now, tx, ty);
        std::vector<PacketBuffer> group;
        PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(tid);
        o.write<uint16_t>(static_cast<uint16_t>(ax)); o.write<uint16_t>(static_cast<uint16_t>(ay));
        o.write<uint16_t>(static_cast<uint16_t>(tx)); o.write<uint16_t>(static_cast<uint16_t>(ty));
        group.push_back(std::move(o));
        bool killed = false;
        if (m->alive) killed = skill_damage_event(*m, h, group);
        server_.broadcast_block(zone_id_, group, 0);
        LOG_DEBUG("GameClientSession %u: Mana Arrow %d/%d -> mob %u base=%d dmg=%d result=%d hp=%d/%d%s", session_id(),
                  i + 1, arrows, tid, base, h.dmg, h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "");
        if (killed) {
            killed_any = true;
            kx = tx; ky = ty;
            owner_monster_quest(*m);
            kill_tm_reward(*m);
            server_.drop_monster_loot(*m, kx, ky);
        }
        gain_attack_exp(*m, h.dmg, hp_before, 1, true);
    }
    skill_hit_tm(*m, su_.skill);
    if (killed_any) {
        if (attacking_ && atk_target_ == tid) end_attack(true);
        kill_monster_aftermath(*m, tid, kx, ky);
    } else {
        engage_monster(*m);
    }
}

// A heal on this player (ESAHeal sub_5C2BB0 on a player target): v = (int)(HP + MaxHP * Ratio);
// healed = v > HP and HP < MaxHP; HP = min(v, MaxHP); S2C 0x0024 [u32 HP] to it (sub_5F4670).
bool GameClientSession::heal_by_ratio(float ratio)
{
    int32_t v = static_cast<int32_t>(static_cast<double>(hp_) + static_cast<double>(hp_max_) * ratio);
    bool healed = false;
    if (v > hp_) healed = hp_ < hp_max_;
    if (v > hp_max_) v = hp_max_;
    hp_ = v;
    PacketBuffer p(static_cast<Opcode>(0x0024), is_paula());
    p.write<uint32_t>(static_cast<uint32_t>(hp_));
    send(p);
    return healed;
}

// ── ESAction 1085 "Cure" (GameServer sub_5C2BB0, ESAHeal; ESAUI select an object) ─────────────
// The target object (ourselves, another player, or a monster): HP = min(MaxHP, (int)(HP + MaxHP *
// Ratio)) (ESAction_Heal: ApplyRatio, Ratio), S2C 0x0024 to it; the block's opener 224 + [u16
// target][pos][pos][u8 healed] (sub_5C2D70; client ESAHeal sub_4BAB60 reads them and plays "heal");
// a player caster gets the skill's TM per hit only when it healed something.
void GameClientSession::esa_heal(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    const float ratio = act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f;
    int32_t tx, ty;
    if (!tid || !skill_target_pos(tid, now, tx, ty)) return;
    bool healed = false;
    int target_level = 1;
    if (tid == static_cast<uint16_t>(object_id_)) {
        healed = heal_by_ratio(ratio);
        target_level = level_;
    } else if (MonsterEntity* m = server_.entity_mgr().get_monster(tid)) {
        int32_t v = static_cast<int32_t>(static_cast<double>(m->hp) + static_cast<double>(m->hp_max) * ratio);
        if (v > m->hp) healed = m->hp < m->hp_max;
        if (v > m->hp_max) v = m->hp_max;
        m->hp = v;                                           // (a monster has no client for 0x0024)
        target_level = m->level;
    } else if (GameClientSession* s = server_.player_session(tid)) {
        healed = s->heal_by_ratio(ratio);
        target_level = s->level();
    } else {
        return;
    }
    int32_t ax, ay;
    cur_pos(now, ax, ay);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    o.write<uint16_t>(static_cast<uint16_t>(ax)); o.write<uint16_t>(static_cast<uint16_t>(ay));
    o.write<uint16_t>(static_cast<uint16_t>(tx)); o.write<uint16_t>(static_cast<uint16_t>(ty));
    o.write<uint8_t>(healed ? 1 : 0);
    group.push_back(std::move(o));
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: Cure -> %u ratio %.2f healed=%d", session_id(), tid, ratio, healed ? 1 : 0);
    if (healed) skill_hit_tm_level(target_level, su_.skill);
}

// ── ESAction 1297 "Magical Soul" (GameServer sub_5D63D0, ESAFairyOfMagic) ─────────────────────
// The caster marks the target (vtbl+132); base = AP + Ratio2 — the binary computes min(level, 30) *
// Ratio1 and then two NOPs at 0x5D648F drop that term (a patch of the Paula server, kept); the magic
// damage sub_5E6270(base, .., ApplyRatio, MA offset 0, element 128); the opener sub_5EDE00 ([caster]
// [TARGET][0]...) with no tail (client ESAFairyOfMagic reads none); the damage event; EXP; the
// skill's TM per hit.
void GameClientSession::esa_fairy_of_magic(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    mark_monster_owner(*m);
    const float apply_ratio = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    const int r2 = act ? static_cast<int>(act->get("Ratio2", 0.0)) : 0;
    const int base = r2 + st_ap_;
    SkillDamage h = skill_magic_damage(*m, base, apply_ratio, 0, 128);
    const int32_t hp_before = m->hp;
    std::vector<PacketBuffer> group;
    group.push_back(skill_opener(d, tid));
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: Magical Soul -> mob %u base=%d dmg=%d result=%d hp=%d/%d%s", session_id(), tid,
              base, h.dmg, h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "");
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, false, now);
}

// ── ESAction 1103 "Tornado Bomb" (GameServer sub_5C4A10, ESATornadoAttack) ───────────────────
// sub_5C4CD0: AP' = the row's AP (int +4, ESAction_TornadoAttack) + AP against the target's DP (a
// guard ES 1492 would replace it — monsters have none); three physical hits sub_5E6040 (ApplyRatio,
// element 0), a hit's damage divided by 3; each kept as {u32 dmg, u32 result, u16 -1} (sub_5205F0,
// a miss {0, result}); the sum. The block: the opener 224 (sub_5EE050) + [pos caster][u16 target]
// [pos target] + 3 records (sub_5C4C30; client ESATornadoAttack reads exactly that), then ONE damage
// event (sub_5C4E10): DamageInfo {sum, 2, 0, 0, 0x8000, 0, +24 = the hits}, its EXP first. The
// skill's TM is the use TM (sub_891DC0).
void GameClientSession::esa_tornado_attack(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    const float apply_ratio = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    const int ap2 = (act ? static_cast<int>(act->get("AP", 0.0)) : 0) + st_ap_;
    struct Rec { int32_t dmg; int32_t result; uint16_t v; } rec[3];
    int32_t total = 0;
    int hits = 0;
    for (int i = 0; i < 3; ++i) {
        SkillDamage s = skill_phys_damage(*m, ap2, apply_ratio, 0);
        if (s.absorbed > 0) absorb_heal(*m, s.absorbed);        // 30182 per hit
        if (s.dmg > 0 && s.result != 1 && s.result != 2) s.dmg /= 3;
        if (s.dmg <= 0 || s.result == 1 || s.result == 2) {
            rec[i] = { 0, s.result, 0xFFFF };
        } else {
            total += s.dmg;
            ++hits;
            rec[i] = { s.dmg, s.result, 0xFFFF };
        }
    }
    SkillDamage h;
    h.dmg        = total;
    h.death_type = 2;
    h.result     = 0;
    h.element    = 0;
    h.flags      = 0x8000;
    h.extra      = hits;
    const int32_t hp_before = m->hp;
    gain_attack_exp(*m, total, hp_before, 3);                 // sub_5C4E10: 30003 kind 3, before 30002

    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    m->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(static_cast<uint16_t>(ax)); o.write<uint16_t>(static_cast<uint16_t>(ay));
    o.write<uint16_t>(tid);
    o.write<uint16_t>(static_cast<uint16_t>(tx)); o.write<uint16_t>(static_cast<uint16_t>(ty));
    for (const Rec& r : rec) {
        o.write<int32_t>(r.dmg);
        o.write<int32_t>(r.result);
        o.write<uint16_t>(r.v);
    }
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: Tornado Bomb -> mob %u ap'=%d hits %d/%d/%d = %d hp=%d/%d%s", session_id(), tid,
              ap2, rec[0].dmg, rec[1].dmg, rec[2].dmg, total, m->hp, m->hp_max, killed ? " [KILLED]" : "");
    skill_follow_after(tid);                                  // 30107
    skill_use_tm(su_.skill);
    skill_after_hit(*m, total, hp_before, killed, 0, true, now);
}

// ── ESAction 1174 "Shooting Spree" (GameServer sub_5C9770; no selection: around the caster) ────
// Needs the gun in slot 1 and a loaded magazine (+4604), else "Player::MELEESHOT_SKILL_CANCEL".
// Level row (ESAction_MeleeShot, loader sub_51B700): ApplyRatio +0, Ratio +4, Radius +8 (i16),
// MonNum +12. The whole skill succeeds on sub_893840(ApplyRatio); then sub_5C9A10 walks the zone's
// objects in order and takes each attackable one (no other player's master, not CharacterInfo type
// 8 / 12, players only with PvP) within sqrtf(dx²+dy²) <= Radius (sub_42F1B0), at most min(10,
// MonNum, the bullets in the loaded stack); that many bullets go (sub_894560). The command block:
// 224 opener + [u8 success][u16 n][u16 target]*n (client ESAMeleeShot sub_4D8730 reads them), then
// per target (sub_5C9DF0) the gun skill damage sub_5E53C0 (Ratio, ApplyRatio) with its EXP share
// first, the damage event and EXP (30003). A success gives the skill's TM (sub_891DC0).
void GameClientSession::esa_melee_shot(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const int32_t gun_ap = equipped_gun_ap();
    const BagItem* mag = mag_.item ? bag_find_uid(mag_.item) : nullptr;
    if (gun_ap < 0 || !mag) {
        LOG_INFO("Player::MELEESHOT_SKILL_CANCEL %u,%s", object_id_, char_name_);
        return;
    }
    const float   ratio       = act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f;
    const float   apply_ratio = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    const int16_t radius      = static_cast<int16_t>(act ? static_cast<int>(act->get("Radius", 0.0)) : 0);
    const int32_t mon_num     = act ? static_cast<int32_t>(act->get("MonNum", 0.0)) : 0;
    const int32_t bullets     = mag->count;

    const bool success = rnd(100) < clampi(static_cast<int>(static_cast<double>(apply_ratio) * 100.0), 0, 100);
    int32_t ax, ay;
    cur_pos(now, ax, ay);
    std::vector<uint16_t> targets;
    if (success) {
        std::vector<uint32_t> oids = server_.entity_mgr().world_entities_in_zone(zone_id_);
        std::sort(oids.begin(), oids.end());
        for (uint32_t oid : oids) {
            if (targets.size() >= 10 || static_cast<int32_t>(targets.size()) >= mon_num ||
                static_cast<int32_t>(targets.size()) >= bullets)
                break;
            MonsterEntity* m = server_.entity_mgr().get_monster(oid);
            if (!m || !m->alive || m->zone != zone_id_) continue;
            // sub_5C9A10: a monster another player owns (+1212, outside our share group) is skipped, so are
            // MonsterParamEx2 Type 8 / 12 (sub_718610 / sub_718710) and a shadow monster unless we
            // have the Third Eye (sub_4268F0 / sub_426950) — all before the range check.
            if (owned_by_other(*m)) continue;
            if (const MobDef* md = server_.mob_db().get(static_cast<int>(m->mob_id)))
                if (md->type == 8 || md->type == 12) continue;
            if (m->shadow() && !third_eye_) continue;
            int32_t mx, my;
            m->motion.pos(now, mx, my);
            const int dx = static_cast<int16_t>(ax) - static_cast<int16_t>(mx);
            const int dy = static_cast<int16_t>(ay) - static_cast<int16_t>(my);
            const float d2 = static_cast<float>(dy * dy + dx * dx);
            if (std::sqrt(d2) <= static_cast<double>(radius)) targets.push_back(static_cast<uint16_t>(oid));
        }
    }
    consume_bullets(static_cast<int>(targets.size()));

    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint8_t>(success ? 1 : 0);
    o.write<uint16_t>(static_cast<uint16_t>(targets.size()));
    for (uint16_t t : targets) o.write<uint16_t>(t);
    group.push_back(std::move(o));
    struct Hit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };
    std::vector<Hit> hits;
    for (uint16_t tid : targets) {
        MonsterEntity* m = server_.entity_mgr().get_monster(tid);
        if (!m || !m->alive) continue;
        const SkillDamage h = skill_gun_damage(*m, gun_ap, ratio, apply_ratio);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        hits.push_back({ m, h.dmg, hp_before, killed });
        LOG_DEBUG("GameClientSession %u: Shooting Spree -> mob %u dmg=%d result=%d hp=%d/%d%s", session_id(), tid, h.dmg,
                  h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "");
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : hits) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now);
    if (success) skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Shooting Spree success=%d targets=%zu (radius %d, max %d, bullets %d)",
              session_id(), success ? 1 : 0, targets.size(), radius, mon_num, bullets);
}

// ─────────────────────────────────────────────────────────────────────────────
//  The detectors — the buried items (drill.cpp, MapItemManager) a player is shown on the ground.
// ─────────────────────────────────────────────────────────────────────────────

// sub_543A40 / sub_5C8070 / sub_5C9550: a grid of 16-unit steps over the square of side 2 * radius around
// (x, y) — its corner (x - radius, y - radius) put back by its int16 % 32 (a multiple of 32), + 16 —,
// (2 * radius) / 16 steps each way, rows of increasing x then the next y; a point counts when its zone
// cell (sub_434300) has byte 0 = 0 and a ground nature other than 6 and an item lies there no deeper than
// `reach` (sub_850110); at most 50.
std::vector<std::pair<int16_t, int16_t>> GameClientSession::detect_buried(int16_t x, int16_t y, int radius, int reach)
{
    std::vector<std::pair<int16_t, int16_t>> found;
    int16_t sx = static_cast<int16_t>(x - static_cast<int16_t>(radius));
    int16_t sy = static_cast<int16_t>(y - static_cast<int16_t>(radius));
    const int16_t ox = static_cast<int16_t>(sx % 32), oy = static_cast<int16_t>(sy % 32);
    sx = static_cast<int16_t>(sx - ox + 16);
    sy = static_cast<int16_t>(sy - oy + 16);
    const int steps = static_cast<int16_t>(2 * radius / 16);
    const MapGrid* g = server_.zone_grid(zone_id_);
    int16_t cx = sx, cy = sy;
    for (int i = 0; i < steps; ++i) {
        for (int j = 0; j < steps; ++j) {
            const bool open = g && g->at(cx / 16, cy / 16) == 0 && g->nature_of(cx, cy) != 6;
            const int16_t depth = server_.map_item_depth(zone_id_, cx, cy);
            if (open && depth > -1 && depth <= reach) {
                found.emplace_back(cx, cy);
                if (found.size() >= 50) return found;
            }
            cx = static_cast<int16_t>(cx + 16);
        }
        cx = sx;
        cy = static_cast<int16_t>(cy + 16);
    }
    return found;
}

namespace {
// sub_893840: rand()%100 < clamp((int)(ratio * 100.0), 0, 100).
bool detect_chance(float ratio)
{
    const int roll = rnd(100);
    return roll < clampi(static_cast<int>(static_cast<double>(ratio) * 100.0), 0, 100);
}
// The detectors' tail: [u8 applied] + when applied [u16 n]{[u16 x][u16 y]} and, with n > 0, [u16 seconds].
void detect_tail(PacketBuffer& o, bool applied, const std::vector<std::pair<int16_t, int16_t>>& pts, int16_t seconds)
{
    o.write<uint8_t>(applied ? 1 : 0);
    if (!applied) return;
    o.write<uint16_t>(static_cast<uint16_t>(pts.size()));
    for (const auto& p : pts) { o.write<uint16_t>(static_cast<uint16_t>(p.first)); o.write<uint16_t>(static_cast<uint16_t>(p.second)); }
    if (!pts.empty()) o.write<uint16_t>(static_cast<uint16_t>(seconds));
}
} // namespace

// ── ESAction 1147 "Item Detector" (sub_5C7F40, ESAItemDetector; ESAUI 6: a point) ─────────────────
//  The row (loader sub_51B3B0): ApplyRatio +0, DepthRatio +4, Seconds +8, Radius +10. The ApplyRatio roll
//  (sub_893840 — no resist); the reach (int)(DA * DepthRatio * 0.1), or 300 when ApplyRatio > 0.9 (every
//  row of this data); the found points around the chosen point (the payload's [u16 x][u16 y],
//  sub_C0BD20); S2C 224 with ES id 0 (sub_5EDC10) to the caster ONLY, in a block: [me][me][0][1147][2]
//  [ESClass][skill][EState] + [u8 applied][u16 n]{x, y}[u16 Seconds]. No TM. (VERIFIED: the client's
//  ESAItemDetector sub_4D3EA0 reads that tail, marks the points and says "Speech_FindItems" /
//  "Speech_NoFindItems", or "Alert_FailedDetection" when not applied.)
void GameClientSession::esa_item_detector(const SkillDef& d, const EsaRow* act)
{
    const float apply = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    const float depth_ratio = act ? static_cast<float>(act->get("DepthRatio", 0.0)) : 0.0f;
    const int16_t seconds = static_cast<int16_t>(act ? static_cast<int>(act->get("Seconds", 0.0)) : 0);
    const int radius = static_cast<int16_t>(act ? static_cast<int>(act->get("Radius", 0.0)) : 0);
    const bool applied = detect_chance(apply);
    int reach = static_cast<int>(static_cast<double>(stat_value(7)) * depth_ratio * 0.1000000014901161);
    if (static_cast<double>(apply) > 0.8999999761581421) reach = 300;
    int16_t px = 0, py = 0;
    if (su_.payload.size() >= 4) {
        px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
        py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
    }
    const auto pts = applied ? detect_buried(px, py, radius, reach) : std::vector<std::pair<int16_t, int16_t>>{};
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    detect_tail(o, applied, pts, seconds);
    std::vector<PacketBuffer> mine, theirs;
    mine.push_back(std::move(o));
    server_.broadcast_block_split(zone_id_, session_id(), mine, theirs);
    LOG_DEBUG("GameClientSession %u: Item Detector at (%d,%d) radius %d reach %d: applied %d, %zu found", session_id(),
              px, py, radius, reach, applied ? 1 : 0, pts.size());
}

// ── ESAction 1170 "Basic Detector" (sub_5C9430, ESABasicDetector) ───────────────────────────────
//  The row (loader sub_51B550): ApplyRatio +0, Seconds +4, Radius +6, Depth +8. The ApplyRatio roll; the
//  found points around the caster with the reach Depth; S2C 224 as 1147's (ES id 0, to the caster only,
//  the same tail and client class); the skill's TM (sub_891DC0).
void GameClientSession::esa_basic_detector(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const float apply = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    const int16_t seconds = static_cast<int16_t>(act ? static_cast<int>(act->get("Seconds", 0.0)) : 0);
    const int radius = static_cast<int16_t>(act ? static_cast<int>(act->get("Radius", 0.0)) : 0);
    const int reach  = static_cast<int16_t>(act ? static_cast<int>(act->get("Depth", 0.0)) : 0);
    const bool applied = detect_chance(apply);
    int32_t cx, cy;
    cur_pos(now, cx, cy);                                    // sub_4100C0
    const auto pts = applied ? detect_buried(static_cast<int16_t>(cx), static_cast<int16_t>(cy), radius, reach)
                             : std::vector<std::pair<int16_t, int16_t>>{};
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    detect_tail(o, applied, pts, seconds);
    std::vector<PacketBuffer> mine, theirs;
    mine.push_back(std::move(o));
    server_.broadcast_block_split(zone_id_, session_id(), mine, theirs);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Basic Detector at (%d,%d) radius %d reach %d: applied %d, %zu found", session_id(),
              cx, cy, radius, reach, applied ? 1 : 0, pts.size());
}

// ─────────────────────────────────────────────────────────────────────────────
//  The event items' ESActions (the scrolls 7008..7022, ESClass 6) whose handler only shows them.
// ─────────────────────────────────────────────────────────────────────────────

// 1121 / 1124 / 1179 / 1180 / 1307 (sub_5C4F50 / sub_5C4FB0 / sub_5C50A0 / sub_5C5100 / sub_5C5520): S2C 224
// [me][me][0][ESAction][2][ESClass][skill][EState] with no tail (sub_5EDE00 with the empty buffer
// unk_EE3478), in a block, to the area. 1137 / 1181 / 1291 (sub_5C5010 / sub_5C5160 / sub_5C5490): the
// payload's [u16 id] looked up in the zone (vtbl+16 sub_7FCCD0 with the mask 89: players, shops, NPCs, skill
// masters — no monsters); none -> nothing; else the same 224 with that target. 1197 (sub_5C51F0): mask 1,
// players only. (VERIFIED: their client classes — ESAFireOfPerry, ESAWhitePearl, ESALovelyChoco, ...
// — read no tail.)
void GameClientSession::esa_firework(const SkillDef& d, int mask)
{
    const uint16_t self = static_cast<uint16_t>(object_id_);
    uint16_t target = self;
    if (mask) {
        const uint16_t id = (su_.payload.size() >= 2) ? static_cast<uint16_t>(su_.payload[0] | (su_.payload[1] << 8)) : 0;
        bool found = false;
        if (mask & 1) {
            if (id == self) found = true;
            else if (GameClientSession* s = server_.player_session(id)) found = (s->zone_id() == zone_id_);
        }
        if (!found && (mask & 0x08)) found = server_.zone_shop(zone_id_, id) != nullptr;
        if (!found && (mask & 0x10)) found = server_.zone_npc(zone_id_, id) != nullptr;
        if (!found && (mask & 0x40)) found = server_.zone_skill_shop(zone_id_, id) != nullptr;
        if (!found) {
            LOG_DEBUG("GameClientSession %u: event skill %u: target %u not found", session_id(), su_.skill, id);
            return;
        }
        target = id;
    }
    std::vector<PacketBuffer> group;
    group.push_back(skill_opener(d, target));
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: event skill %u (ESAction %d) -> %u", session_id(), su_.skill, d.esaction, target);
}

// 1222 "Heart Spring" (sub_5C5280): the tail [u16 n]{u16 id} — the zone's players (zone +28, in its order) in our
// party within 200 (sub_5C5350: sub_716EF0 — our share group, us included; none: ourselves —, the same layer,
// sub_42F1B0 <= 200.0; no death test) — and S2C 224 [me][me][0]... (sub_5EE050), in a block, to the area.
// (VERIFIED: the client's ESAHeartBomb reads them.)
void GameClientSession::esa_heart_bomb(const SkillDef& d)
{
    const uint16_t self = static_cast<uint16_t>(object_id_);
    const auto now = std::chrono::steady_clock::now();
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    std::vector<uint16_t> ids;
    for (uint32_t oid : server_.entity_mgr().players_in_zone(zone_id_)) {
        GameClientSession* s = server_.player_session(oid);
        if (!s || !s->object_id_ || !s->in_world()) continue;
        if (s != this && !server_.party().same_group(char_uid_, s->char_uid_)) continue;
        int32_t x, y;
        s->cur_pos(now, x, y);
        if (static_cast<double>(dist_f(x, y, cx, cy)) <= 200.0) ids.push_back(static_cast<uint16_t>(s->object_id_));
    }
    PacketBuffer o = skill_opener(d, self);
    o.write<uint16_t>(static_cast<uint16_t>(ids.size()));
    for (uint16_t id : ids) o.write<uint16_t>(id);
    std::vector<PacketBuffer> group;
    group.push_back(std::move(o));
    server_.broadcast_block(zone_id_, group, 0);
}

// The current position of a skill's target object: a live monster of our zone, ourselves, or another
// player of our zone (its resting point). False when it is gone.
bool GameClientSession::skill_target_pos(uint16_t id, std::chrono::steady_clock::time_point now, int32_t& x, int32_t& y)
{
    if (id == static_cast<uint16_t>(object_id_)) { cur_pos(now, x, y); return true; }
    if (MonsterEntity* m = server_.entity_mgr().get_monster(id)) {
        if (m->zone != zone_id_ || !m->alive) return false;
        m->motion.pos(now, x, y);
        return true;
    }
    if (PlayerEntity* p = server_.entity_mgr().get_player(id)) {
        if (p->zone != zone_id_) return false;
        x = p->pos_x; y = p->pos_y;
        return true;
    }
    return false;
}

// sub_891DC0: a skill (2..21009) used by a player outside a battle zone (+4684 = 0) gives its
// SkillExp as TM exp below TM 500; no TM level-up -> S2C 0x011D [u8 FType][u32 TM exp] (sub_5ED650).
void GameClientSession::skill_use_tm(uint16_t skill)
{
    if (skill <= 1 || skill >= 21010) return;
    const SkillDef* d = server_.skill_db().get(skill);
    if (!d || tm_level_ >= 500) return;
    if (!add_tm_exp(d->skill_exp, true)) {
        PacketBuffer p(static_cast<Opcode>(0x011D), is_paula());
        p.write<uint8_t>(f_type_);
        p.write<uint32_t>(static_cast<uint32_t>(tm_exp_));
        send(p);
    }
}

// sub_891C80: a skill hit by a player: n = target level - our level; |n| >= 50 -> (SkillExp + lv *
// SkillExp / 10) / 2, else (100 - |n|) * (lv * SkillExp / 10 + SkillExp) / 100 (integer math), as
// TM exp below TM 500; no TM level-up -> S2C 0x011D [u8 FType][u32 TM exp].
void GameClientSession::skill_hit_tm(const MonsterEntity& m, uint16_t skill)
{
    skill_hit_tm_level(m.level, skill);
}

void GameClientSession::skill_hit_tm_level(int target_level, uint16_t skill)
{
    if (skill <= 1 || skill >= 21010) return;
    const SkillDef* d = server_.skill_db().get(skill);
    if (!d) return;
    const int se = d->skill_exp;
    const int n  = target_level - level_;
    int tm;
    if (n <= -50 || n >= 50) tm = (se + level_ * se / 10) / 2;
    else                     tm = (100 - std::abs(n)) * (level_ * se / 10 + se) / 100;
    if (tm_level_ >= 500) return;
    if (!add_tm_exp(tm, true)) {
        PacketBuffer p(static_cast<Opcode>(0x011D), is_paula());
        p.write<uint8_t>(f_type_);
        p.write<uint32_t>(static_cast<uint32_t>(tm_exp_));
        send(p);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Learning a skill (retail Player::CanLearnSkill sub_760760 + the learn sub_5FD100 / sub_8922B0 /
//  sub_5F22F0), used by the skill card items (UseSkilCardlHelper, item_use.cpp).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// The classes are the JobTypeInfo ids the client, the DB and the char record use: 1..9 the 1st tier
// (9 = Animal Lover), 10..18 the 2nd (18 = Animal Trainer), 19..36 the 3rd (36 = Zoologist). The
// recovered GameServer's own tree (sub_893990 card type -> class, sub_893B70 class -> parent)
// predates the Animal Lover line and numbers the classes 1..8 / 9..16 / 17..33 — the same classes
// without the gap (JobTypeInfo DataPath job009..job033.pal belong to JobType 10..35, the Animal Lover
// line has job100..102.pal), which the cards' own Job text (Item_SkillCardParam) confirms for every
// card type, and names the three card types sub_893990 does not know: 43 Animal Lover, 44 Animal
// Trainer, 45 Zoologist.
int tree_to_job(int c) { return c <= 8 ? c : (c <= 16 ? c + 1 : c + 2); }

// sub_893990 (+ 43..45): a skill card's class type -> the class it stands for.
int card_class(int t)
{
    int c = 0;
    switch (t) {
    case 2: c = 1;   break; case 3: c = 2;   break; case 4: c = 9;   break; case 5: c = 10;  break;
    case 6: c = 17;  break; case 7: c = 18;  break; case 8: c = 20;  break; case 9: c = 19;  break;
    case 12: c = 3;  break; case 13: c = 4;  break; case 14: c = 11; break; case 15: c = 12; break;
    case 16: c = 21; break; case 17: c = 22; break; case 18: c = 25; break; case 19: c = 23; break;
    case 20: c = 24; break; case 23: c = 5;  break; case 24: c = 6;  break; case 25: c = 13; break;
    case 26: c = 14; break; case 27: c = 26; break; case 28: c = 27; break; case 29: c = 29; break;
    case 30: c = 28; break; case 33: c = 7;  break; case 34: c = 8;  break; case 35: c = 15; break;
    case 36: c = 16; break; case 37: c = 30; break; case 38: c = 31; break; case 39: c = 33; break;
    case 40: c = 32; break;
    case 43: return 9;
    case 44: return 18;
    case 45: return 36;
    default: return 0;
    }
    return tree_to_job(c);
}

// sub_893B70 (+ the Animal Lover line): the class a class comes from (the 1st tier are the roots).
int parent_class(int j)
{
    if (j >= 10 && j <= 17) return j - 9;
    if (j == 18) return 9;
    if (j == 36) return 18;
    if (j < 19 || j > 35) return 0;
    switch (j - 2) {                                    // the tree's 3rd tier, 17..33
    case 17: case 18: return tree_to_job(9);
    case 19: case 20: return tree_to_job(10);
    case 21: case 22: return tree_to_job(11);
    case 23: case 24: case 25: return tree_to_job(12);
    case 26: case 27: return tree_to_job(13);
    case 28: case 29: return tree_to_job(14);
    case 30: case 31: return tree_to_job(15);
    case 32: case 33: return tree_to_job(16);
    default: return 0;
    }
}

// sub_893CB0: the player's class (Player +1024) or one of its parents is the card type's class, and
// the TM level (+824) is at least `need`.
bool class_allows(int cls, int tm_level, int card_type, int need)
{
    for (; cls; cls = parent_class(cls))
        if (cls == card_class(card_type)) return tm_level >= need;
    return false;
}

} // namespace

const GameClientSession::LearnedSkill* GameClientSession::find_skill(int skill) const
{
    for (const LearnedSkill& s : skills_) if (s.id == skill) return &s;
    return nullptr;
}

// S2C 0x01E4 [u8 type][u16 a][u16 b] (sub_5FD2E0; client case 484 -> sub_6EEA70 shows the message).
void GameClientSession::send_system_msg(uint8_t type, uint16_t a, uint16_t b)
{
    PacketBuffer p(static_cast<Opcode>(0x01E4), is_paula());
    p.write<uint8_t>(type);
    p.write<uint16_t>(a);
    p.write<uint16_t>(b);
    send(p);
}

// Player::CanLearnSkill (sub_760760): the skill's card (SkillParam SkillCardID) must exist; its class
// requirement (SCardType/TMLV or SubSCardType/SubTMLV; none when both types are 0) — else message 5/9;
// the TM points for AffectionPoint — else 5/2; the pre-skills at their levels — else 5/7; the
// exclusive skill pairs (sub_760A20 / sub_760A90) — else 5/8. Messages carry the card's skill.
bool GameClientSession::can_learn_skill(int skill)
{
    const SkillDef* sd = server_.skill_db().get(skill);
    const SkillCardDef* c = sd ? server_.skill_card_db().get(static_cast<uint32_t>(sd->card)) : nullptr;
    if (!c) return false;
    const uint16_t sk = static_cast<uint16_t>(c->affection_skill);
    const bool any_class = !c->scard_type && !c->sub_scard_type;
    if (!any_class && !class_allows(job_, tm_level_, c->scard_type, c->tmlv) &&
        !class_allows(job_, tm_level_, c->sub_scard_type, c->sub_tmlv)) {
        send_system_msg(5, sk, 9);
        return false;
    }
    if (tm_points_ < c->affection_point) { send_system_msg(5, sk, 2); return false; }
    for (int i = 0; i < 3; ++i) {
        if (!c->pre_skill[i]) continue;
        const LearnedSkill* p = find_skill(c->pre_skill[i]);
        if (!p || p->level < c->pre_skill_lv[i]) { send_system_msg(5, sk, 7); return false; }
    }
    // sub_760A20 / sub_760A90: the skills of an exclusive pair or ring exclude each other.
    auto has = [&](int s) { return find_skill(s) != nullptr; };
    bool ok = true;
    switch (skill) {
    case 2102: ok = !has(2106) && !has(2108); break;
    case 2104: ok = !has(2108) && !has(2110); break;
    case 2106: ok = !has(2102) && !has(2110); break;
    case 2108: ok = !has(2102) && !has(2104); break;
    case 2110: ok = !has(2104) && !has(2106); break;
    case 2202: ok = !has(2204); break;
    case 2204: ok = !has(2202); break;
    default: break;
    }
    if (!ok) { send_system_msg(5, sk, 8); return false; }
    return true;
}

// The learn itself: S2C 0x020B [u16 skill][u8 level][u16 TM points] to us (sub_5FD100 -> client
// sub_6B4E00 case 0x20B: takes the TM points, adds the skill, "level up" effect, "LearnSkill") and
// S2C 0x01DF [u16 user] to the area (the others' effect); the skill joins the list (sub_8922B0 —
// only one that SkillParam has) and the DB (sub_5F22F0, message 4501 -> char_skills).
// (A passive skill (SkillParam Type 1) also enters the PS limiter as an effect state, sub_525220; here
// es_stat_bonus and has_ps_ability read the learned skills directly.)
bool GameClientSession::learn_skill(int skill, int level, int tm_points)
{
    PacketBuffer own(static_cast<Opcode>(0x020B), is_paula());
    own.write<uint16_t>(static_cast<uint16_t>(skill));
    own.write<uint8_t>(static_cast<uint8_t>(level));
    own.write<uint16_t>(static_cast<uint16_t>(tm_points));
    send(own);
    PacketBuffer area(static_cast<Opcode>(0x01DF), is_paula());
    area.write<uint16_t>(static_cast<uint16_t>(object_id_));
    server_.broadcast_zone(zone_id_, area, 0);

    if (!server_.skill_db().get(skill)) return false;
    auto it = std::find_if(skills_.begin(), skills_.end(), [&](const LearnedSkill& s) { return s.id == skill; });
    if (it != skills_.end()) it->level = static_cast<uint8_t>(level);
    else skills_.push_back({ static_cast<uint16_t>(skill), static_cast<uint8_t>(level) });
    server_.db().execute("INSERT INTO char_skills (char_uid, skill_id, level) VALUES (%u, %d, %d) "
                         "ON DUPLICATE KEY UPDATE level=%d", char_uid_, skill, level, level);
    LOG_INFO("GameClientSession %u: learned skill %d level %d (TM points -%d -> %d)", session_id(), skill, level,
             tm_points, tm_points_);
    if (skill == 4801) crazy_init();                     // the Bear's Crazy Hammer gauge (skill_bear.cpp)
    return true;
}

// Player::CanLevelUpSkill (sub_760930): a learned skill that has a card (SkillParam SkillCardID +68)
// and a level, can be raised (bLevelUp +44); the new level is at most MaxSKLV (+46, else S2C 0x01E4
// [5][skill][5]) and exactly one above; levels 2..10 need any TM point left, level 11 (the master
// level) MasterTMPoint (+48).
bool GameClientSession::can_level_up_skill(int skill, int level)
{
    const LearnedSkill* s = find_skill(skill);
    const SkillDef* d = server_.skill_db().get(skill);
    if (!s || !d || !d->card) return false;
    if (!s->level) return false;
    if (!d->level_up) return false;
    if (level > d->max_level) {
        send_system_msg(5, static_cast<uint16_t>(skill), 5);
        return false;
    }
    if (level - s->level != 1) return false;
    if (level == 11) return tm_points_ >= d->master_tm;
    if (level >= 11) return false;
    return tm_points_ > 0;
}

// C2S 0x01E1 [u16 skill][u8 new level][u16 TM points] (Player case 481; client skill window
// sub_B18400 "_SKILL_Level UP": the next level of a skill below 10, for 1 TM point, while the player
// has any): CanLevelUpSkill, else S2C 0x01E3 [the same 5 bytes] (sub_5FD2C0). Then the TM points the
// request names go (Player +4592 — the retail takes the client's figure as it is), S2C 0x020C [the 5
// bytes] to us (client sub_6B4E00: the skill's level, the TM points, the level-up sound) and S2C
// 0x01E2 [u16 user][u16 FType][u16 level] to the area (sub_5FD1E0; client sub_6CCBF0 plays
// "skill_<type> sk lv up" / "sk master" on the user), the skill's level (sub_76E620), the DB
// (sub_5F2490, message 4504). (A passive — SkillParam Type 1 — is re-registered in the ESLimiter:
// has_ps_ability reads the learned skills directly.)
void GameClientSession::handle_skill_level_up(PacketBuffer& pkt)
{
    try {
        uint8_t rec[5];
        for (uint8_t& b : rec) b = pkt.read<uint8_t>();             // sub_7AC9C0: exactly 5 bytes
        const int skill = rec[0] | (rec[1] << 8);
        const int level = rec[2];
        const int cost  = rec[3] | (rec[4] << 8);
        auto reply = [&](uint16_t op) {
            PacketBuffer p(static_cast<Opcode>(op), is_paula());
            p.write_bytes(rec, sizeof(rec));
            send(p);
        };
        // Hardening (not retail): retail subtracts the client's cost and lets this opcode reach level 11;
        // the client window only asks levels 2..10 for 1 point, so cost 0 or a level-11 request is a forgery
        // (free levels, mastery without the SkillMasterInfo item — mastery stays C2S 560 at a master).
        constexpr int kLevelUpCost = 1;
        if (level >= 11 || cost != kLevelUpCost) {
            reply(0x01E3);
            const char* forged = level < 11 ? "TM cost" : cost != kLevelUpCost ? "level and TM cost" : "level";
            LOG_INFO("GameClientSession %u: skill %d level %d TM cost %d refused (forged %s: the window asks levels "
                     "2..10 for %d)", session_id(), skill, level, cost, forged, kLevelUpCost);
            return;
        }
        if (!can_level_up_skill(skill, level)) {
            reply(0x01E3);
            LOG_INFO("GameClientSession %u: skill %d level %d refused (TM points %d)", session_id(), skill, level,
                     tm_points_);
            return;
        }
        tm_points_ -= kLevelUpCost;                                  // the server's cost; the 0x020C echo holds the same 1
        reply(0x020C);
        PacketBuffer area(static_cast<Opcode>(0x01E2), is_paula());
        area.write<uint16_t>(static_cast<uint16_t>(object_id_));
        area.write<uint16_t>(f_type_);                               // Player +1028: the FType
        area.write<uint16_t>(static_cast<uint16_t>(level));
        server_.broadcast_zone(zone_id_, area, 0);

        auto it = std::find_if(skills_.begin(), skills_.end(), [&](const LearnedSkill& s) { return s.id == skill; });
        if (it == skills_.end()) { reply(0x01E3); return; }
        it->level = static_cast<uint8_t>(level);
        server_.db().execute("UPDATE char_skills SET level=%d WHERE char_uid=%u AND skill_id=%d", level, char_uid_,
                             skill);
        save_stat_points();                                          // the TM points
        LOG_INFO("GameClientSession %u: skill %d -> level %d (TM points -%d -> %d)", session_id(), skill, level,
                 kLevelUpCost, tm_points_);
    } catch (const std::exception& e) {
        LOG_WARN("GameClientSession %u: C2S 0x01E1 skill level-up aborted: %s", session_id(), e.what());
    } catch (...) {
        LOG_WARN("GameClientSession %u: C2S 0x01E1 skill level-up aborted: unknown exception", session_id());
    }
}
