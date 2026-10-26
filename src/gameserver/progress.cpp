#include "game_session.hpp"
#include "game_server.hpp"
#include "logger.hpp"
#include <algorithm>
#include <climits>

// ─────────────────────────────────────────────────────────────────────────────
//  EXP and TM — 1:1 with the retail GameServer (GameServerRelease):
//
//  Every attack of a player on a monster (melee AttackStrategy::DoAttack sub_41B970, a skill's
//  ESAction such as sub_5C2110):
//    1. the monster's share for this hit, vtbl+264 (CommonMonster sub_72BC10), taken BEFORE the
//       damage: min(dmg, HP) * EXP / MaxHP (32-bit integer math);
//    2. the damage event 30002 on the monster (HP, S2C 0x2A, a kill -> the die handler sub_7296E0,
//       whose owner gets the TM: sub_891AB0);
//    3. event 30003 on the attacker (sub_5E71D0: a player with weapon category 0/2) -> Player
//       sub_73D2A0 @0x7431C8: gain = sub_5E4CE0 -> sub_5E4C00, AddExp (vtbl+168 sub_716A80); no
//       level-up -> S2C 0x406 [u32 gain] (sub_5F4E10).
//  The owner (+1212) is the first player who marks the monster (sub_72B2F0, only while it has no
//  owner): melee on a damaging hit (the monster's 30002 handler sub_423B70, dmg > 0), a skill before
//  its damage (Player vtbl+132 sub_7601C0); the HP at that moment is kept (+2024).
//
//  In a party (party.cpp) the EXP of 30003 and the owner's TM go through the share group's reward
//  handler (events 30037 / 30127 to each member), and the rates of a member carry the party bonus and
//  its share (S2C 886 / 887 show the bonus).
//
//  Client (Trickster.bin), all VERIFIED: sub_6F0960 routes 0x26 / 0x27 / 0x11D / 0x406 / 0xF5 to the
//  local player sub_6B4E00: 0x406 adds the u32 to the EXP (+912), 0x26 sets it, 0x27 is the level
//  up, 0x11D [u8][u32] sets the TM EXP (+728), 0xF5 [u8 ftype][u16 TM lv][u32 TM exp][u16 TM pts]
//  is the TM level up (ftype picks the "skill_gong/magic/gam gak/mae ruk tm lv up" effect). S2C 365
//  [u16 monster][u16 owner] -> the monster actor (Character handler sub_49DCE0 case 365).
// ─────────────────────────────────────────────────────────────────────────────

// GameInfoMgr sub_62ACF0: the EXP divisor per 30003 kind (+119492[kind], read by sub_5E73F0) —
// kind 0 the normal hit, 1 e.g. Punch / Warm Up (sub_5D60C0 / sub_5D6930).
static constexpr int kExpDivisor[7] = { 5, 4, 1, 4, 1, 1, 1 };
static constexpr int kMaxLevel        = 400; // AddExp sub_716A80
static constexpr int kMaxTmLevel      = 500; // AddTMExp sub_716C30

// The owner mark (sub_72B2F0) — only while the monster has no owner (sub_4269F0).
void GameClientSession::mark_monster_owner(MonsterEntity& m)
{
    if (m.owner_oid) return;
    m.owner_oid = object_id_;
    m.owner_hp  = m.hp;                                  // +2024 = +444
    // S2C 365 (sub_5EE580): [u16 monster][u16 owner], to the area of both.
    PacketBuffer p(static_cast<Opcode>(0x016D), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(m.object_id));
    p.write<uint16_t>(static_cast<uint16_t>(object_id_));
    server_.broadcast_zone(zone_id_, p, 0);
    LOG_DEBUG("GameClientSession %u: owns mob %u (HP %d/%d)", session_id(), static_cast<unsigned>(m.object_id),
              m.owner_hp, m.hp_max);
}

// Event 30003 on the attacker: this hit's EXP.
void GameClientSession::gain_attack_exp(const MonsterEntity& m, int32_t dmg, int32_t hp_before, int exp_kind,
                                        bool skip_zero_share)
{
    if (dead_) return;
    // vtbl+264 (sub_72BC10): min(dmg, HP) * EXP / MaxHP — an int32 product, as the retail imul.
    int32_t share = 0;
    if (m.hp_max > 0) {
        const int32_t part = std::max(0, std::min(dmg, hp_before));
        const int32_t prod = static_cast<int32_t>(static_cast<uint32_t>(part) * m.exp_reward);
        share = prod / m.hp_max;
    }
    if (skip_zero_share && share <= 0) return;           // a handler that posts 30003 only for a share > 0
    gain_exp_share(m, share, exp_kind);
}

// The 30003 handler with the value the poster gave (most give the share above; ESAWeaponThrow /
// ESAGellderHit give min(damage, HP) itself).
void GameClientSession::gain_exp_share(const MonsterEntity& m, int32_t share, int exp_kind)
{
    if (dead_) return;
    // sub_5E4C00 (kind 0): share / 5 + share * f1 * f2 — f1 = 0.25 and f2 = mobLv/myLv when the monster
    // is the lower level, else 1.0; at least 1 (sub_5E4CE0), so even a miss is worth 1.
    const int mob_lv = std::max<int>(1, m.level);
    const int my_lv  = std::max<int>(1, level_);
    const float f1 = (mob_lv >= my_lv) ? 1.0f : 0.25f;
    const float f2 = (mob_lv < my_lv) ? static_cast<float>(static_cast<double>(mob_lv) / my_lv) : 1.0f;
    const int   kind = (exp_kind >= 0 && exp_kind <= 3) ? exp_kind : 0;   // sub_5E4CE0: kinds 0..3 here
    const float v  = static_cast<float>(static_cast<double>(share) / kExpDivisor[kind] +
                                        static_cast<double>(share) * f1 * f2);
    const int32_t gain = (v < 1.0f) ? 1 : static_cast<int32_t>(v);
    // sub_716F40: our party's reward handler takes it (event 30037 to our share group).
    if (server_.party().reward(char_uid_, gain, static_cast<uint8_t>(kind <= 3 ? 1 : 0), false)) return;
    if (!add_exp(gain, true)) {
        PacketBuffer p(static_cast<Opcode>(0x0406), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(gain));
        send(p);
    }
}

// Event 30037 (Player sub_73D2A0 @0x7429F1): EXP our share group handed us — the rates base (+5352) is
// it, AddExp (our party bonus and share apply); no level-up -> S2C 0x0026 [u32 EXP] (sub_5F4DA0).
void GameClientSession::party_exp(int32_t amount, uint8_t flag)
{
    (void)flag;                                          // (the rate slot 4's switch: no such bonus here)
    if (!add_exp(amount, true)) {
        PacketBuffer p(static_cast<Opcode>(0x0026), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(exp_));
        send(p);
    }
}

// Event 30127 (@0x742A75): TM from our share group — AddTMExp with our rates; no TM level-up -> S2C
// 0x011D [u8 0][u32 TM EXP] (sub_5ED650(this, 0)).
void GameClientSession::party_tm(int32_t amount, uint8_t flag)
{
    (void)flag;
    if (!add_tm_exp(amount, true)) {
        PacketBuffer p(static_cast<Opcode>(0x011D), is_paula());
        p.write<uint8_t>(0);
        p.write<uint32_t>(static_cast<uint32_t>(tm_exp_));
        send(p);
    }
}

// The rates (Player +5352, vtbl+180 sub_765C00 -> sub_76EA70 / vtbl+176 sub_765BD0 -> sub_79EAE0):
// (int)((float)((base + every active slot's (int)(base * its rate)) * 1.0) * our share) — the slot 0 is
// the party bonus (+ 0.005), the share 1.0 outside a party; premium / event slots are not modelled.
int32_t GameClientSession::rate_exp(int32_t base) const
{
    const PartyShare s = server_.party().share_of(char_uid_);
    float sum = 0.0f;
    if (s.exp_rate > 0.0f) sum = static_cast<float>(static_cast<double>(static_cast<int32_t>(static_cast<double>(base) *
                                                                                          s.exp_rate)) + sum);
    const float v = static_cast<float>((static_cast<double>(base) + sum) * 1.0);
    return static_cast<int32_t>(static_cast<double>(v) * static_cast<double>(s.exp_pct));
}

int32_t GameClientSession::rate_tm(int32_t base) const
{
    const PartyShare s = server_.party().share_of(char_uid_);
    float sum = 0.0f;
    if (s.tm_rate > 0.0f) sum = static_cast<float>(static_cast<double>(static_cast<int32_t>(static_cast<double>(base) *
                                                                                         s.tm_rate)) + sum);
    const float v = static_cast<float>((static_cast<double>(base) + sum) * 1.0);
    return static_cast<int32_t>(static_cast<double>(v) * static_cast<double>(s.tm_pct));
}

// AddExp (Character vtbl+168 sub_716A80): `rates` (a4 = 0) takes the amount through the rates (vtbl+180,
// rate_exp), else it is added as it is (a4 = 1: quests, restores — vtbl+172, * 1.0); then every level
// reached costs its GetChrNextExp (sub_719A80, gulim14). Returns true when the gain was handled by a
// level-up (or at the cap, where nothing is added) — the caller then sends no 0x406.
bool GameClientSession::add_exp(int32_t amount, bool rates)
{
    if (level_ >= kMaxLevel) return true;
    if (rates) amount = rate_exp(amount);
    exp_ += amount;
    uint32_t next = server_.exp_table().next_exp(level_);
    if (next == 0 || exp_ < static_cast<int32_t>(next)) return false;
    int lv = level_ + 1;
    for (;;) {
        exp_ -= static_cast<int32_t>(next);
        next = server_.exp_table().next_exp(lv);
        if (!next) {
            LOG_ERROR("Character::AddExp - Cannot GetChrNextExp of user(%s) currentLevel:%d, failedLevel:%d",
                      char_name_, level_, lv);
            return true;
        }
        if (exp_ < static_cast<int32_t>(next)) break;
        if (++lv >= kMaxLevel) { lv = kMaxLevel; exp_ = 0; break; }
    }
    const int gained = lv - level_;
    level_ = static_cast<int16_t>(lv);
    grant_levels(gained);
    LOG_INFO("%s : levelup:level:%d, exp:%d", char_name_, level_, exp_);
    return true;
}

// The level-up (Player vtbl+232 sub_75D390 -> sub_717780 grant + distributor sub_717590): every stat
// slot += its growth weight per level (P->AP/AC/DX, M->MaxMP/MA/MD, S->MaxWT/DA/LK, C->MaxHP/DP/HV),
// 4 unspent points per level, full HP/MP, then S2C 0x0027 (sub_5F4EF0). (The per-level extras —
// level skills sub_764820/sub_602100, the level-31 event, quests — are not modelled yet.)
void GameClientSession::grant_levels(int gained)
{
    if (gained <= 0) return;
    const int gw[12] = { pts_p_, pts_p_, pts_p_, pts_m_, pts_m_, pts_m_,
                         pts_s_, pts_s_, pts_s_, pts_c_, pts_c_, pts_c_ };
    for (int i = 0; i < 12; ++i) accpts_[i] += gw[i] * gained;
    attr_pts_ = static_cast<int16_t>(attr_pts_ + 4 * gained);
    recompute_stats();
    hp_ = hp_max_; mp_ = mp_max_;
    send_level_up();
    pet_speech(8);                         // sub_75D390: our pet's line, after S2C 0x0027
    save_position();
    for (int lv = level_ - gained + 1; lv <= level_; ++lv)
        post_level_up(lv);                 // sub_75D390 -> sub_76BE80: the level's GM gift (postbox.cpp)
    if (level_ == 400) {                   // sub_75D390: StringTable ReachTheMaxLv through the system megaphone
        char msg[256];
        std::snprintf(msg, sizeof(msg), "%s님께서 400레벨을 달성하셨습니다. 모두 축하해 주세요.", char_name_);
        server_.sys_megaphone("트릭스터", msg);   // (ChangeJobStrings Trickster)
    }
}

// sub_891BC0: the TM multiplier by level difference (monster - us), in steps of 0.2 per 10 levels,
// clamped to ±1.8; a negative value divides.
static float tm_level_factor(int my_level, int mob_level)
{
    const int d = mob_level - my_level;
    float f;
    if (d < 0) {
        f = static_cast<float>(static_cast<double>((d - 5) / 10) * 0.2000000029802322 - 1.0);
        if (f < -1.799999952316284) return -1.8f;
    } else {
        f = static_cast<float>(static_cast<double>((d + 5) / 10) * 0.2000000029802322 + 1.0);
        if (f > 1.799999952316284) return 1.8f;
    }
    return f;
}

// The die handler's TM part (sub_7296E0 -> sub_891AB0(owner)): the monster's owner gets it — on its own
// strand when it is someone else (a party mate may land the kill).
void GameClientSession::kill_tm_reward(const MonsterEntity& m)
{
    if (!m.owner_oid || m.hp_max <= 0) return;
    const int32_t owner_hp = m.owner_hp, hp_max = m.hp_max, tm = m.tm_reward;
    const int     mob_lv   = m.level;
    if (m.owner_oid == object_id_) { owner_kill_tm(owner_hp, hp_max, tm, mob_lv); return; }
    GameClientSession* o = server_.player_session(m.owner_oid);
    if (o) o->run_on_strand([o, owner_hp, hp_max, tm, mob_lv]() { o->owner_kill_tm(owner_hp, hp_max, tm, mob_lv); });
}

// sub_891AB0: MonsterParamEx2 TM * (the HP the owner took it at / MaxHP) (sub_892080), times or divided
// by the level factor (+5356 = it); our party's reward handler takes it (sub_716F90: event 30127 to the
// share group), else AddTMExp (rates); no TM level up -> S2C 0x11D [u8 0][u32 TM exp] (sub_5ED650).
void GameClientSession::owner_kill_tm(int32_t owner_hp, int32_t hp_max, int32_t tm_reward, int mob_level)
{
    const float fraction = static_cast<float>(static_cast<double>(owner_hp) / static_cast<double>(hp_max));
    const float factor   = tm_level_factor(level_, mob_level);
    const int   base     = static_cast<int>(fraction * static_cast<double>(tm_reward));
    const int32_t tm = (factor <= 0.0f) ? static_cast<int32_t>(static_cast<double>(base) / -factor)
                                        : static_cast<int32_t>(static_cast<double>(base) * factor);
    if (server_.party().reward(char_uid_, tm, 1, true)) return;
    if (!add_tm_exp(tm, true)) {
        PacketBuffer p(static_cast<Opcode>(0x011D), is_paula());
        p.write<uint8_t>(0);
        p.write<uint32_t>(static_cast<uint32_t>(tm_exp_));
        send(p);
    }
    LOG_DEBUG("GameClientSession %u: kill TM +%d (TM %d, exp %d/%d)", session_id(), tm, tm_level_, tm_exp_,
              server_.tm_table().next(tm_level_));
}

// AddTMExp (sub_716C30): nothing at TM 500 (reported as handled); `rates` (a4 = 0) takes the amount
// through the rates (vtbl+176, rate_tm); every TM level reached costs its NextTmxp (sub_719AB0,
// TM_LEVEL2) and runs the TM level-up (vtbl+196 sub_75D6F0: +1 TM point, S2C 0xF5). Returns true on a TM
// level-up.
bool GameClientSession::add_tm_exp(int32_t amount, bool rates)
{
    if (tm_level_ >= kMaxTmLevel) return true;
    int32_t next = server_.tm_table().next(tm_level_);
    if (next <= 0) return false;                         // (table not loaded)
    if (rates) amount = rate_tm(amount);
    tm_exp_ += amount;
    bool up = false;
    while (tm_exp_ >= next) {
        up = true;
        ++tm_level_;
        tm_exp_ -= next;
        next = server_.tm_table().next(tm_level_);
        if (next <= 0) {
            LOG_ERROR("Error TMLevleUp : Name : %s, TmLevel : %d, TmExp : %d", char_name_, tm_level_, tm_exp_);
            next = INT_MAX;
        }
        if (tm_level_ >= kMaxTmLevel) { tm_level_ = kMaxTmLevel; tm_exp_ = 0; }
        ++tm_points_;                                    // sub_75D6F0: ++(+4592)
        send_tm_level_up();
        pet_speech(9);                                   // sub_75D6F0: our pet's line, after S2C 0xF5
        LOG_INFO("%s : tmlevelup:ftype:%d, level:%d, tmxp:%d", char_name_, f_type_, tm_level_, tm_exp_);
    }
    if (up) save_stat_points();
    return up;
}

// S2C 0xF5 (sub_5ED5A0): [u8 ftype][u16 TM level][u32 TM exp][u16 TM points]. The ftype (Player +1028 =
// characters.f_type: 0 Power, 1 Magic, 2 Sense, 3 Charm) picks the client's TM level-up effect.
void GameClientSession::send_tm_level_up()
{
    PacketBuffer p(static_cast<Opcode>(0x00F5), is_paula());
    p.write<uint8_t>(f_type_);
    p.write<uint16_t>(static_cast<uint16_t>(tm_level_));
    p.write<uint32_t>(static_cast<uint32_t>(tm_exp_));
    p.write<uint16_t>(static_cast<uint16_t>(tm_points_));
    send(p);
}
