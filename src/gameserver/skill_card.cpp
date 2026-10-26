#include "game_session.hpp"
#include "game_server.hpp"
#include "logger.hpp"
#include "combat_util.hpp"
#include "paula_crypt.hpp"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <mutex>

// ─────────────────────────────────────────────────────────────────────────────
//  The Card Master (Raccoon, 2nd job) and its 3rd job the Gambler — 1:1 with the retail GameServer (GameServerRelease),
//  the client's classes verified in Trickster.bin (each class's read function reads the opener's tail).
//
//  The card throws spend the Throw item of the common slot 20 (Player +4608, sub_894600) — a throw card is Type 11
//  SubType 60. Lady Luck (4210) is a passive One Pair reads (its own ESAction 1365 is never cast: a passive's C2S 0xEE
//  cuts the session). Super Hips (4202) is ESAHVUp's (es.cpp FluentWaist). Metamorphosis (ESAWildChange 1248) is the
//  third mutation (skill_cat.cpp mutation_*): its StateType 8 gates Wild Nail, Heavy Hit and Card Soldier (StateType 3).
//  Bodyguard (ESAWhiteKnight 1172), Mass Destruction (ESAKamikaze 1479 -> 1480), Full House (ESAFullHouse 1280 -> 1281)
//  and Four of a Kind's ESAFourCardDo (1496) are ESs of ours (es.cpp); the knock-backs (1221 / 1272 / 1348), Full
//  House's hits (1281) and Card Soldier (1382) are ESs on the monsters (card_es_on, the zone tick in es.cpp).
// ─────────────────────────────────────────────────────────────────────────────

namespace {
float rowf(const EsaRow* r, const char* k) { return r ? static_cast<float>(r->get(k, 0.0)) : 0.0f; }
int   rowi(const EsaRow* r, const char* k) { return r ? static_cast<int>(r->get(k, 0.0)) : 0; }

void put_xy(PacketBuffer& p, int32_t x, int32_t y)
{
    p.write<uint16_t>(static_cast<uint16_t>(x));
    p.write<uint16_t>(static_cast<uint16_t>(y));
}

// The throw selections' payload (ESAUI 7, sub_5BE9A0's a3[9]): [u16 target][u32 item id][u64 uid].
struct ThrowPick {
    uint16_t target{ 0 };
    uint32_t item{ 0 };
    uint32_t uid_lo{ 0 }, uid_hi{ 0 };
};
ThrowPick throw_pick(const std::vector<uint8_t>& pl)
{
    ThrowPick t;
    if (pl.size() >= 2) t.target = static_cast<uint16_t>(pl[0] | (pl[1] << 8));
    if (pl.size() >= 14) {
        for (int k = 0; k < 4; ++k) t.item   |= static_cast<uint32_t>(pl[2 + k]) << (8 * k);
        for (int k = 0; k < 4; ++k) t.uid_lo |= static_cast<uint32_t>(pl[6 + k]) << (8 * k);
        for (int k = 0; k < 4; ++k) t.uid_hi |= static_cast<uint32_t>(pl[10 + k]) << (8 * k);
    }
    return t;
}

struct CHit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };

// x87's __ftol: NaN or out of range -> 0x80000000 (the "integer indefinite").
int32_t ftol87(double v)
{
    if (!(v > -2147483649.0 && v < 2147483648.0)) return INT32_MIN;
    return static_cast<int32_t>(v);
}

// sub_569690 / sub_5726F0 / sub_582F90: a knock-back's end — from `a` (the caster) toward `b` (the target), R away: the
// x step normalized, then the y step normalized with the x ALREADY scaled (retail); (int) of each added to a's (as
// int16). Both at one spot: 0 / 0 -> NaN -> 0x80000000 -> a itself.
void knock_dest(int16_t ax, int16_t ay, int16_t bx, int16_t by, int r, int16_t& ox, int16_t& oy)
{
    float vx = static_cast<float>(static_cast<double>(bx) - static_cast<double>(ax));
    float vy = static_cast<float>(static_cast<double>(by) - static_cast<double>(ay));
    const float l1 = std::sqrt(static_cast<float>(static_cast<double>(vx) * vx + static_cast<double>(vy) * vy));
    vx = static_cast<float>(static_cast<double>(vx) / static_cast<double>(l1) * static_cast<double>(r));
    const float l2 = std::sqrt(static_cast<float>(static_cast<double>(vx) * vx + static_cast<double>(vy) * vy));
    vy = static_cast<float>(static_cast<double>(vy) / static_cast<double>(l2) * static_cast<double>(r));
    ox = static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(ax) + static_cast<uint32_t>(ftol87(vx))));
    oy = static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(ay) + static_cast<uint32_t>(ftol87(vy))));
}

constexpr int kRegenPauseMs = 6000;    // sub_73CEF0: +3188 = 6000 (as game_session.cpp)

// ESAFullHouse's spot (sub_56F860): ONE for the whole server — a function static (unk_EE43A4) and its flag (byte_EE43A0).
std::mutex g_fh_mutex;
bool       g_fh_set = false;
int16_t    g_fh_x = 0, g_fh_y = 0;
} // namespace

// ── ESAction 1150 "Card Strike" (ESACardThrow, init sub_55B4C0; ESAUI 7, ESAIC 2) ─────────────────────────────
//  A generic ES on the caster (ESClass -1, made at once) whose init does the throw. The row ESAction_CardThrow
//  (sub_51B0F0: ApplyRatio +0, Ratio +4, Radius +8 i16). The payload's [u16 target] (zone vtbl+16 kind 3) — none: no
//  object —, we may attack it (sub_5E6D70) and own it (vtbl+164) — else no object; no Throw item (+4608): no object;
//  one of it used up. AP' = (int)((AP + 8 * HV) * Ratio). The block: the opener sub_5EE050 with the object's id +
//  [u16 target][pos us][pos target][u16 the payload's item (as i16)][u64 the payload's uid] (sub_55BDC0; VERIFIED: the
//  client's ESACardThrow 0x4D0E10 reads them); then (sub_55B7D0) the zone's monsters in list order — not dead, our
//  layer, not Type 8 / 12, a shadow one only with the Third Eye — within Radius of the TARGET (sqrtf); such a one once
//  7 were hit ends the walk; one with no owner or ours (our party's: none here): the physical damage sub_5E6040(AP',
//  its DP, ApplyRatio, element 0), its damage times (float)(1 - distance / Radius) (the result and its fatal flag as
//  rolled — retail), its EXP share of that, the damage event, EXP 30003 kind 0. (Players in PvP, sub_55BB60: no PvP.)
//  Then the skill's TM per hit on the target. The object stays as long as its caster is in the zone (its tick
//  sub_55BD80 only tests the caster; no lifetime, no end of its own — retail).
void GameClientSession::esa_card_throw(const SkillDef& d, uint8_t lv, const EsaRow* act)
{
    const auto now = std::chrono::steady_clock::now();
    const uint16_t id = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());   // the ES object (sub_7FD4E0)
    auto no_object = [&]() { server_.entity_mgr().free_oid(id); };
    const ThrowPick pk = throw_pick(su_.payload);
    MonsterEntity* t = pk.target ? server_.entity_mgr().get_monster(pk.target) : nullptr;
    if (!t || t->zone != zone_id_ || !area_target_ok(*t)) { no_object(); return; }   // (a corpse too: no death test)
    if (!may_attack_owned(*t)) { no_object(); return; }
    if (!(throw_item_ ? bag_find_uid(throw_item_) : nullptr)) { no_object(); return; }
    consume_throw_item(1);
    const float ratio  = rowf(act, "Ratio");
    const float apply  = rowf(act, "ApplyRatio");
    const int16_t radius = static_cast<int16_t>(rowi(act, "Radius"));
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_ + 8 * st_hv_) * static_cast<double>(ratio));
    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    t->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_), id);
    o.write<uint16_t>(pk.target);
    put_xy(o, ax, ay);
    put_xy(o, tx, ty);
    o.write<uint16_t>(static_cast<uint16_t>(pk.item));
    o.write<uint32_t>(pk.uid_lo);
    o.write<uint32_t>(pk.uid_hi);
    group.push_back(std::move(o));
    std::vector<CHit> done;
    int hits = 0;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0 || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        const float dd = dist_f(mx, my, tx, ty);
        if (!(static_cast<double>(dd) <= static_cast<double>(radius))) continue;
        if (hits >= 7) break;
        if (owned_by_other(*m)) continue;
        const float fall = static_cast<float>(1.0 - static_cast<double>(dd) / static_cast<double>(radius));
        SkillDamage h = skill_phys_damage(*m, ap2, apply, 0);
        h.dmg = static_cast<int32_t>(static_cast<double>(h.dmg) * static_cast<double>(fall));
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
        ++hits;
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const CHit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now);
    skill_hit_tm(*t, su_.skill);
    // Hardening (not retail): retail keeps every Card Strike object until the caster leaves the zone, so a caster
    // grinding in one map piled up ~1000 ESs and world ids per hour; the previous one (its throw long over) ends
    // here with its S2C 280 and gives its id back — at most one per caster.
    for (size_t i = 0; i < es_.size(); ++i) {
        if (es_[i].kind == EsKind::Timer && es_[i].esaction == d.esaction) {
            es_end(i);
            break;
        }
    }
    es_push_timer(d, lv, id, INT32_MAX);
    LOG_DEBUG("GameClientSession %u: Card Strike at mob %u (AP' %d): %d hit", session_id(), pk.target, ap2, hits);
}

// ── ESAction 1194 "One Pair" (GameServer sub_5CBD30, ESAOnePair; ESAUI 7, ESAIC 2) ────────────────────────────
//  The payload's [u16 target] (kind 3) — none: nothing. The cards = 2; with Lady Luck (4210) learned (sub_54BA80 /
//  sub_8923C0), its learned level's row (ESAction_LuckyGoddess sub_51CEC0: MinCard +10, MaxCard +12): MinCard, or when
//  they differ sub_5CC080(Min, Max + 1) = Min + (Max + 1 - Min) * rand() / 0x8000. No Throw item: nothing; that many
//  used up (sub_894600). AP' = (int)((AP + 8 * HV) * Ratio) — with Lady Luck (int)(AP' * ((float)(cards / 15.0) +
//  1.0)). The block: the opener sub_5EE050 + [u16 target][pos us][pos target][u16 the payload's item][u64 its uid][u16
//  cards] (sub_5CC310; VERIFIED: the client's ESAOnePair 0x4DFC20 reads them); the hit (sub_5CC0B0): our mark
//  (vtbl+132, not on ourselves), sub_5E6040(AP', its DP, ApplyRatio, element 0), its EXP share, the damage event,
//  EXP 30003 kind 3 when we may attack it (vtbl+164). Then the skill's TM per hit.
void GameClientSession::esa_one_pair(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const ThrowPick pk = throw_pick(su_.payload);
    MonsterEntity* t = pk.target ? server_.entity_mgr().get_monster(pk.target) : nullptr;
    if (!t || t->zone != zone_id_) return;
    const LearnedSkill* ll = find_skill(4210);
    int cards = 2;
    if (ll) {
        const SkillDef* s = server_.skill_db().get(4210);
        const EsaRow* lr = s ? server_.esa_db().row(s->esa_table, ll->level) : nullptr;
        const int mn = static_cast<int16_t>(rowi(lr, "MinCard"));
        const int mx = static_cast<int16_t>(rowi(lr, "MaxCard"));
        cards = (mn == mx) ? mn : mn + (mx + 1 - mn) * std::rand() / 0x8000;
    }
    if (!(throw_item_ ? bag_find_uid(throw_item_) : nullptr)) return;
    consume_throw_item(cards);
    int ap2 = static_cast<int>(static_cast<double>(st_ap_ + 8 * st_hv_) * static_cast<double>(rowf(act, "Ratio")));
    if (ll) {
        const float k = static_cast<float>(static_cast<double>(cards) / 15.0);
        ap2 = static_cast<int>(static_cast<double>(ap2) * (static_cast<double>(k) + 1.0));
    }
    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    t->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(pk.target);
    put_xy(o, ax, ay);
    put_xy(o, tx, ty);
    o.write<uint16_t>(static_cast<uint16_t>(pk.item));
    o.write<uint32_t>(pk.uid_lo);
    o.write<uint32_t>(pk.uid_hi);
    o.write<uint16_t>(static_cast<uint16_t>(cards));
    group.push_back(std::move(o));
    mark_monster_owner(*t);
    const SkillDamage h = skill_phys_damage(*t, ap2, rowf(act, "ApplyRatio"), 0);
    const int32_t hp_before = t->hp;
    const bool killed = skill_damage_event(*t, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*t, h.dmg, hp_before, killed, su_.skill, !may_attack_owned(*t), now, 3);
    LOG_DEBUG("GameClientSession %u: One Pair -> mob %u: %d cards, AP' %d dmg=%d%s", session_id(), pk.target, cards, ap2,
              h.dmg, killed ? " [KILLED]" : "");
}

// ── ESAction 1220 "Impelling Rage" (GameServer sub_5CF220, ESACharging; no selection) ────────────────────────
//  The row ESAction_Charging (sub_51BDC0: ApplyRatio +0, Ratio +4, Radius +8, Comment +10). The targets (sub_5CF390):
//  the zone's monsters in list order — not dead, our layer, not Type 8 / 12, a shadow one only with the Third Eye —
//  within Radius of us; such a one once 5 are taken ends the walk; one with no owner or ours (our party's) gets the
//  action {ESAction 1221 ESAChargingHit, EState 0, ESClass -1, flags 2, skill 4205, this row} through sub_524690(..,
//  1) (sub_5CF670; card_es_on). (Players in PvP, sub_5CF710.) Then the block {the opener sub_5EE050, no tail —
//  VERIFIED: the client's ESACharging 0x4E5BE0 reads none} and the use TM.
void GameClientSession::esa_charging(const SkillDef& d, uint8_t lv, const EsaRow* act,
                                     std::chrono::steady_clock::time_point now)
{
    const float radius = static_cast<float>(static_cast<int16_t>(rowi(act, "Radius")));
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    std::vector<MonsterEntity*> picks;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0 || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (!(static_cast<double>(radius) >= static_cast<double>(dist_f(mx, my, cx, cy)))) continue;
        if (picks.size() >= 5) break;
        if (owned_by_other(*m)) continue;
        picks.push_back(m);
    }
    SkillDef dd = d;
    dd.esaction = 1221;
    dd.estate   = 0;
    dd.esclass  = -1;
    for (MonsterEntity* m : picks) es_apply_on(*m, dd, lv, act);
    std::vector<PacketBuffer> group;
    group.push_back(skill_opener(d, static_cast<uint16_t>(object_id_)));
    server_.broadcast_block(zone_id_, group, 0);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Impelling Rage: %zu monster(s)", session_id(), picks.size());
}

// ── ESAction 1246 "Wild Nail" (GameServer sub_5D0A40, ESAWildAttack; ESAUI 1, ESAIC 2; StateType 3) ────────────
//  The selected target (kind 3) — none: nothing; our mark (vtbl+132, not on ourselves). The row ESAction_Rush layout
//  (sub_51BA50: ApplyRatio +0, Ratio +4). AP' = (int)(16 * HV * Ratio) — no AP — against its DP (sub_5E6040, ApplyRatio,
//  element 0); its EXP share; the block {the opener sub_5EE050 + [u16 target][pos us][pos target] (sub_5D0D80;
//  VERIFIED: the client's ESAWildAttack 0x4E7D90 reads them), the damage event, EXP 30003 kind 0}; the 30107
//  follow-up (+0x148 flag 2 clear, the target not dead); the skill's TM per hit.
void GameClientSession::esa_wild_attack(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_) return;
    mark_monster_owner(*t);
    const int ap2 = static_cast<int>(static_cast<double>(16 * st_hv_) * static_cast<double>(rowf(act, "Ratio")));
    const SkillDamage h = skill_phys_damage(*t, ap2, rowf(act, "ApplyRatio"), 0);
    const int32_t hp_before = t->hp;
    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    t->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    put_xy(o, ax, ay);
    put_xy(o, tx, ty);
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*t, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_follow_after(tid);
    skill_after_hit(*t, h.dmg, hp_before, killed, su_.skill, false, now);
    LOG_DEBUG("GameClientSession %u: Wild Nail -> mob %u AP' %d dmg=%d result=%d%s", session_id(), tid, ap2, h.dmg,
              h.result, killed ? " [KILLED]" : "");
}

// ── ESAction 1312 "Flux Capacitor" (GameServer sub_5D6F60, ESAManaConvert; no selection) ─────────────────────
//  The row ESAction_ManaConvert (sub_51C8C0: ApplyRatio +0, Ratio +4, HpRatio +8). The MP gained = (int)(MaxHP * HpRatio
//  * Ratio), the HP left = HP - (int)(MaxHP * HpRatio); only with HP left above 0: MP = min(MP + gain, MaxMP), HP = it,
//  S2C 0x0025 then 0x0024. The block {the opener sub_5EE050 + [u8 done] (VERIFIED: the client's ESAManaConvert
//  0x4FB410 reads a u8)}; done -> the use TM.
void GameClientSession::esa_mana_convert(const SkillDef& d, const EsaRow* act)
{
    const double hp_ratio = static_cast<double>(rowf(act, "HpRatio"));
    const int32_t gain = static_cast<int32_t>(static_cast<double>(hp_max_) * hp_ratio * static_cast<double>(rowf(act, "Ratio")));
    const int32_t left = hp_ - static_cast<int32_t>(static_cast<double>(hp_max_) * hp_ratio);
    int32_t mp = gain + mp_;
    if (mp > mp_max_) mp = mp_max_;
    const bool done = left > 0;
    if (done) {
        mp_ = mp;
        hp_ = left;
        PacketBuffer pm(static_cast<Opcode>(0x0025), is_paula());
        pm.write<uint32_t>(static_cast<uint32_t>(mp_));
        send(pm);
        PacketBuffer ph(static_cast<Opcode>(0x0024), is_paula());
        ph.write<uint32_t>(static_cast<uint32_t>(hp_));
        send(ph);
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint8_t>(done ? 1 : 0);
    group.push_back(std::move(o));
    server_.broadcast_block(zone_id_, group, 0);
    if (done) skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Flux Capacitor %s — HP %d MP %d", session_id(), done ? "converts" : "fails", hp_, mp_);
}

// ── ESAction 1330 "Furious Galder Throw" (GameServer sub_5D9750, ESALuckyGellderHit; ESAUI 8, ESAIC 2) ─────────
//  The selected target (kind 3) we may attack (vtbl+164) — else nothing; a player pays 500 gelder (sub_5D9B70: not
//  enough -> nothing; money -= 500, S2C 0x0029). The luck (sub_5D9A00): v = 100 * rand() / 0x7FFF; 0..14 -> 1, ..24 ->
//  200, ..34 -> 300, ..44 -> 400, ..64 -> 500, ..79 -> 600, ..89 -> 700, ..94 -> 800, ..99 -> 1000, 100 -> 0. AP' =
//  (int)(luck * Ratio) (sub_5D9B30; the row sub_51A3C0: ApplyRatio +0, Ratio +4), the damage sub_5E6040(AP', DP 0,
//  ApplyRatio, element 512); the EXP value min(its HP, the damage) itself (retail, as Galder Thrower). The block {the
//  opener sub_5EE050 + [u16 target][pos us][pos target][u16 luck] (sub_5D9AF0 + sub_53DE60; VERIFIED: the client's
//  ESALuckyGellderHit 0x503BC0 reads them), the damage event, EXP 30003 [0][that][0]}; the skill's TM per hit.
void GameClientSession::esa_lucky_gellder_hit(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_) return;
    if (!may_attack_owned(*t)) return;
    if (500 > money_) return;                                  // sub_5D9B70(500)
    money_ -= 500;
    send_money();
    const int v = 100 * std::rand() / 0x7FFF;
    int luck = 0;
    if (static_cast<unsigned>(v) <= 14u)  luck = 1;
    else if (v >= 15 && v < 25)           luck = 200;
    else if (v >= 25 && v < 35)           luck = 300;
    else if (v >= 35 && v < 45)           luck = 400;
    else if (v >= 45 && v < 65)           luck = 500;
    else if (v >= 65 && v < 80)           luck = 600;
    else if (v >= 80 && v < 90)           luck = 700;
    else if (v >= 90 && v < 95)           luck = 800;
    else if (v >= 95 && v < 100)          luck = 1000;
    float ratio = rowf(act, "Ratio");
    if (ratio < 0.0f) ratio = 0.0f;
    const int ap2 = static_cast<int>(static_cast<double>(luck) * static_cast<double>(ratio));
    const SkillDamage h = skill_phys_damage(*t, ap2, rowf(act, "ApplyRatio"), 512, 0);
    const int32_t raw = std::min(t->hp, h.dmg);
    const int32_t hp_before = t->hp;
    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    t->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    put_xy(o, ax, ay);
    put_xy(o, tx, ty);
    o.write<uint16_t>(static_cast<uint16_t>(luck));
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*t, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    gain_exp_share(*t, raw, 0);
    skill_after_hit(*t, h.dmg, hp_before, killed, su_.skill, true, now);
    LOG_DEBUG("GameClientSession %u: Furious Galder Throw -> mob %u luck %d AP' %d dmg=%d (money %d)%s", session_id(), tid,
              luck, ap2, h.dmg, money_, killed ? " [KILLED]" : "");
}

// ── ESAction 1271 "Power Charging" (GameServer sub_5D5160, ESAPowerCharging; no selection) ───────────────────
//  The row ESAction_Charging layout (sub_51BDC0: Radius +8). sub_53E360 around us, at most 5, Radius; each one
//  (sub_5D52A0): a monster with a DebuffSet rolls against 4502's DebuffBitCode (sub_891F80; all resisted -> S2C 1247 —
//  and the action goes on anyway, retail) and gets {ESAction 1272 ESAPowerChargingDo, EState 0, ESClass -1, flags 2,
//  skill 4502, this row, the resisted bits} through sub_524690(.., 1) (card_es_on). Then the block {the opener
//  sub_5EE050, no tail — VERIFIED: the client's ESAPowerCharging 0x4F4E60 reads none} and the use TM.
void GameClientSession::esa_power_charging(const SkillDef& d, uint8_t lv, const EsaRow* act,
                                           std::chrono::steady_clock::time_point now)
{
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    const std::vector<MonsterEntity*> targets = area_monsters(cx, cy, static_cast<int16_t>(rowi(act, "Radius")), 5, now);
    SkillDef dd = d;
    dd.esaction = 1272;
    dd.estate   = 0;
    dd.esclass  = -1;
    for (MonsterEntity* m : targets) {
        uint32_t bits = 0;
        if (monster_debuff_immune(*m, d.debuff_bits, bits)) send_debuff_resisted(*m, bits);
        es_resisted_ = bits;
        es_apply_on(*m, dd, lv, act);
        es_resisted_ = 0;
    }
    std::vector<PacketBuffer> group;
    group.push_back(skill_opener(d, static_cast<uint16_t>(object_id_)));
    server_.broadcast_block(zone_id_, group, 0);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Power Charging: %zu monster(s)", session_id(), targets.size());
}

// ── ESAction 1347 "Heavy Hit" (GameServer sub_5DA900, ESAMassiveAttack; ESAUI 1, ESAIC 2; StateType 3) ─────────
//  The row ESAction_Charging layout (sub_51BDC0: ApplyRatio +0, Ratio +4, Radius +8). The selected target (kind 3) —
//  none: nothing; we may attack it (sub_5E6D70) -> the knock-back (sub_5DABD0: {ESAction 1348 ESAMassiveAttack, EState
//  0, ESClass 1, flags 2, skill 4503, this row} through sub_524690(.., 1); card_es_on) — its block first. AP' =
//  (int)((AP + (float)(HP / 8.0)) * Ratio) against its DP (sub_5E6040, ApplyRatio, element 0), its EXP share; the block
//  {the opener sub_5EE050 + [u16 target][pos us][pos target] (sub_5DAB90; VERIFIED: the client's ESAMassiveAttack
//  0x5077A0 reads them), the damage event}; EXP 30003 kind 0; the use TM. No mark of its own.
void GameClientSession::esa_massive_attack(const SkillDef& d, uint8_t lv, const EsaRow* act,
                                           std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_) return;
    if (area_target_ok(*t)) {                                  // sub_5E6D70 (no death test)
        SkillDef dd = d;
        dd.esaction = 1348;
        dd.estate   = 0;
        dd.esclass  = 1;
        es_apply_on(*t, dd, lv, act);
    }
    const float h8 = static_cast<float>(static_cast<double>(hp_) / 8.0);
    const int ap2 = static_cast<int>((static_cast<double>(st_ap_) + static_cast<double>(h8)) *
                                     static_cast<double>(rowf(act, "Ratio")));
    const SkillDamage h = skill_phys_damage(*t, ap2, rowf(act, "ApplyRatio"), 0);
    const int32_t hp_before = t->hp;
    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    t->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    put_xy(o, ax, ay);
    put_xy(o, tx, ty);
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*t, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*t, h.dmg, hp_before, killed, 0, false, now);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Heavy Hit -> mob %u AP' %d dmg=%d result=%d%s", session_id(), tid, ap2, h.dmg,
              h.result, killed ? " [KILLED]" : "");
}

// ── ESAction 1495 "Four of a Kind" (GameServer sub_5E0180; ESAUI 6 / ESAIC 3: a point) ─────────────────────────
//  The action {ESAction 1496 ESAFourCardDo, EState 0, ESClass -1, flags 2, skill 4505, the row, the payload's point} on
//  us through sub_524690(.., 1) — made at once (es.cpp EsKind::FourCard, four_card_init); then a player's use TM.
void GameClientSession::esa_four_card(const SkillDef& d, uint8_t lv, const EsaRow* act)
{
    SkillDef dd = d;
    dd.esaction = 1496;
    dd.estate   = 0;
    dd.esclass  = -1;
    es_create(dd, lv, act, EsKind::FourCard, false, 2);
    skill_use_tm(su_.skill);
}

// ESAFourCardDo's init (sub_5BB710; ESAction_FourCard sub_51E210: ApplyRatio +0, KillNum +4, Ratio +8, Comment +12,
// Radius +16 — the row kept, +316): its caster (+196), +8 = 0; no Throw item -> no object; kept whatever the roll; up to
// 4 monsters around the point within Radius (sub_53E360, their refs +220..), +336 = all four found (the only way it hits);
// 4 of the Throw item used up whatever (sub_894600); the tail [u8 hits][u16 the item's id][u64 its uid][pos the point]
// [u16 n][u16 id]*n (VERIFIED: the client's ESAFourCardDo 0x520E30 reads them). `e.ids` the targets, `e.first` the hit.
bool GameClientSession::four_card_init(ActiveEs& e, std::vector<uint8_t>& tail)
{
    const BagItem* tb = throw_item_ ? bag_find_uid(throw_item_) : nullptr;
    if (!tb) return false;
    const uint32_t item = tb->item_id;
    const uint64_t uid  = tb->uid;
    int16_t px = 0, py = 0;
    if (su_.payload.size() >= 4) {
        px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
        py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
    }
    const auto now = std::chrono::steady_clock::now();
    const std::vector<MonsterEntity*> found = area_monsters(px, py, e.radius, 4, now);
    e.ids.clear();
    for (MonsterEntity* m : found) e.ids.push_back(static_cast<uint16_t>(m->object_id));
    e.first = found.size() >= 4;
    e.stay  = true;
    consume_throw_item(4);
    auto put16 = [&](uint16_t v) { tail.push_back(static_cast<uint8_t>(v)); tail.push_back(static_cast<uint8_t>(v >> 8)); };
    auto put32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(v >> (8 * k))); };
    tail.push_back(e.first ? 1 : 0);
    put16(static_cast<uint16_t>(item));
    put32(static_cast<uint32_t>(uid));
    put32(static_cast<uint32_t>(uid >> 32));
    put16(static_cast<uint16_t>(px));
    put16(static_cast<uint16_t>(py));
    put16(static_cast<uint16_t>(e.ids.size()));
    for (uint16_t id : e.ids) put16(id);
    return true;
}

// ESAFourCardDo's hits, in its block (sub_5BBB10 on each, all four found): AP' = (int)((AP + 16 * DA) * Ratio) against
// its DP (sub_5E6040, ApplyRatio, element 0), its EXP share, the damage event, EXP 30003 kind 0 (the aftermath: `hits`).
void GameClientSession::four_card_hits(const ActiveEs& e, const EsaRow* act, std::vector<PacketBuffer>& group,
                                       std::vector<CounterHit>& hits)
{
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_ + 16 * st_da_) * static_cast<double>(rowf(act, "Ratio")));
    const float apply = rowf(act, "ApplyRatio");
    for (uint16_t id : e.ids) {
        MonsterEntity* m = server_.entity_mgr().get_monster(id);
        if (!m || m->zone != zone_id_) continue;
        const SkillDamage h = skill_phys_damage(*m, ap2, apply, 0);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        hits.push_back({ m, h.dmg, hp_before, killed });
    }
}

// The knock-backs' end (sub_7149C0 -> the zone's sub_7EE300): the monster stands at its spot (`fx` / `fy`). (The sectors
// would tell the players of a changed sector — a Paula map is one sector.)
void cGameServer::knock_land(MonsterEntity& m, const MonsterEntity::Es& e)
{
    m.motion.rest(e.fx, e.fy);
    m.pos_x = e.fx;
    m.pos_y = e.fy;
}

// The monster ESs of the Card Master / Gambler — inits through sub_524690 (es_apply_on hands them here). False: no
// object (the id goes back).
bool GameClientSession::card_es_on(MonsterEntity& m, MonsterEntity::Es& e, const SkillDef& d, const EsaRow* act)
{
    const auto now = std::chrono::steady_clock::now();
    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    m.motion.pos(now, tx, ty);
    e.caster     = object_id_;
    e.elapsed_ms = 0;
    auto opener = [&](const std::vector<uint8_t>& tail) {
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(static_cast<uint16_t>(e.esaction));
        o.write<uint8_t>(2);
        o.write<uint8_t>(static_cast<uint8_t>(e.esclass));
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(static_cast<uint16_t>(e.estate));
        for (uint8_t b : tail) o.write<uint8_t>(b);
        return o;
    };
    auto stop = [&]() {
        int32_t x, y;
        m.motion.pos(now, x, y);
        m.motion.rest(x, y);
        m.pos_x = x; m.pos_y = y;
    };
    const bool applied = (e.result == 1);

    if (d.esaction == 1221 || d.esaction == 1272) {
        // ESAChargingHit (1221, Impelling Rage; sub_568FF0) / ESAPowerChargingDo (1272, Power Charging; sub_572080), the
        // action carrying the skill's row (ApplyRatio +0, Ratio +4, Radius +8): the caster (+196), +8 = 0, +220 = the
        // target's spot; the knock-back's end (sub_569690: R = Radius from us through it). Applied: +220 = how far it
        // gets (sub_893F50, walk_free); the push = (int16)(int)sqrtf(its square); the damage sub_5E6040((int)(our HP *
        // (push + 40) * Ratio / 800), its DP, ApplyRatio, element 0) (Player +444), its EXP share; 1272: the DebuffSet
        // resisted bit 2 -> +220 = its spot; no damage, or it neither walks nor runs (+156 / +160 0.0) -> its spot; the
        // block {S2C 224 sub_5EDE00 [us][target][ES id]... + [u8 +180][pos +220] (VERIFIED: the clients' ESAChargingDo
        // 0x4E6030 / ESAPowerChargingDo 0x4F5180 read them), the damage event, EXP 30003 kind 3 when we may attack it};
        // damage -> it stops and takes the condition 1 (1221 sub_569900; 1272 sub_572970: not with the bit 1 resisted
        // or a Prevent Stun 1398 — monsters have none). Not applied: [u8][pos its spot], the event of a miss {0, 0, 1,
        // ..}. Then our mark with 5000 (vtbl+132). Not kept unapplied. Its tick (sub_569740 / sub_5727A0, es.cpp): past
        // 1000 / 3000 ms it lands at +220 and ends; its end: the condition 1 off on a live one.
        const bool power = (d.esaction == 1272);
        e.fx = static_cast<int16_t>(tx);
        e.fy = static_cast<int16_t>(ty);
        int16_t dx = 0, dy = 0;
        knock_dest(static_cast<int16_t>(ax), static_cast<int16_t>(ay), static_cast<int16_t>(tx), static_cast<int16_t>(ty),
                   static_cast<int16_t>(rowi(act, "Radius")), dx, dy);
        std::vector<PacketBuffer> group;
        SkillDamage h;
        int32_t hp_before = m.hp;
        bool killed = false;
        if (applied) {
            int16_t fx = 0, fy = 0;
            MapGrid::walk_free(server_.zone_grid(zone_id_), static_cast<int16_t>(tx), static_cast<int16_t>(ty), dx, dy, fx, fy);
            e.fx = fx; e.fy = fy;
            const int16_t vx = static_cast<int16_t>(fx - static_cast<int16_t>(tx));
            const int16_t vy = static_cast<int16_t>(fy - static_cast<int16_t>(ty));
            const int16_t push = static_cast<int16_t>(static_cast<int>(std::sqrt(static_cast<float>(vy * vy + vx * vx))));
            const int32_t base = hp_ * (push + 40);
            const int ap2 = static_cast<int>(static_cast<double>(base) * static_cast<double>(rowf(act, "Ratio")) / 800.0);
            h = skill_phys_damage(m, ap2, rowf(act, "ApplyRatio"), 0);
            if (power && (e.resisted & 2)) { e.fx = static_cast<int16_t>(tx); e.fy = static_cast<int16_t>(ty); }
            if (h.dmg == 0 || (m.walk_speed == 0.0f && m.run_speed == 0.0f)) {
                e.fx = static_cast<int16_t>(tx);
                e.fy = static_cast<int16_t>(ty);
            }
            std::vector<uint8_t> tail{ static_cast<uint8_t>(e.result) };
            tail.push_back(static_cast<uint8_t>(e.fx)); tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(e.fx) >> 8));
            tail.push_back(static_cast<uint8_t>(e.fy)); tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(e.fy) >> 8));
            group.push_back(opener(tail));
            hp_before = m.hp;
            killed = skill_damage_event(m, h, group);
        } else {
            std::vector<uint8_t> tail{ static_cast<uint8_t>(e.result) };
            tail.push_back(static_cast<uint8_t>(tx)); tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(tx) >> 8));
            tail.push_back(static_cast<uint8_t>(ty)); tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(ty) >> 8));
            group.push_back(opener(tail));
            SkillDamage miss;
            miss.dmg = 0; miss.death_type = 0; miss.result = 1;
            skill_damage_event(m, miss, group);
        }
        server_.broadcast_block(zone_id_, group, 0);
        m.es.push_back(e);
        if (applied) {
            skill_after_hit(m, h.dmg, hp_before, killed, 0, !may_attack_owned(m), now, 3);
            if (h.dmg && (!power || !(e.resisted & 1))) { stop(); m.cond_add(1); }
        }
        mark_monster_owner(m);
        return true;
    }

    if (d.esaction == 1348) {
        // ESAMassiveAttack (1348, Heavy Hit's; sub_582CD0): the caster (+196), +8 = 0, +220 = the target's spot; the
        // knock-back's end (sub_582F90, R = the row's Radius). Applied and it walks or runs -> +220 = how far it gets
        // (walk_free). The block {S2C 224 sub_5EDE00 [us][target][ES id][1348][2][ESClass 1][4503][0] + [u8 +180][pos
        // +220] (VERIFIED: the client's ESAMassiveAttackDo 0x507DE0 reads them)}; our mark (vtbl+132). No damage, no
        // stop. Its tick (sub_583040, es.cpp): not applied, or its caster gone -> the end; past 1000 ms it lands at +220.
        e.fx = static_cast<int16_t>(tx);
        e.fy = static_cast<int16_t>(ty);
        int16_t dx = 0, dy = 0;
        knock_dest(static_cast<int16_t>(ax), static_cast<int16_t>(ay), static_cast<int16_t>(tx), static_cast<int16_t>(ty),
                   static_cast<int16_t>(rowi(act, "Radius")), dx, dy);
        if (applied && (m.walk_speed != 0.0f || m.run_speed != 0.0f)) {
            int16_t fx = 0, fy = 0;
            MapGrid::walk_free(server_.zone_grid(zone_id_), static_cast<int16_t>(tx), static_cast<int16_t>(ty), dx, dy, fx, fy);
            e.fx = fx; e.fy = fy;
        }
        std::vector<uint8_t> tail{ static_cast<uint8_t>(e.result) };
        tail.push_back(static_cast<uint8_t>(e.fx)); tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(e.fx) >> 8));
        tail.push_back(static_cast<uint8_t>(e.fy)); tail.push_back(static_cast<uint8_t>(static_cast<uint16_t>(e.fy) >> 8));
        std::vector<PacketBuffer> group;
        group.push_back(opener(tail));
        server_.broadcast_block(zone_id_, group, 0);
        m.es.push_back(e);
        mark_monster_owner(m);
        return true;
    }

    if (d.esaction == 1281) {
        // ESAFullHouseHit (1281; sub_56FD70 — the action carries Full House's row: ApplyRatio +0, Ratio +4, Second +8):
        // the caster (+196), +8 = 0, +220 = 1000 * Second, +224 = 0; AP' = (int)((AP + 8 * HV + 8 * LK) * Ratio), the
        // damage sub_5E6040(AP', its DP, ApplyRatio, element 0), its EXP share, +224 = its result (a byte); the block
        // {S2C 224 sub_5EDE00 [us][target][ES id][1281][2][ESClass 1][4501][0] + [u8 applied] (VERIFIED: the client's
        // ESAFullHouseHit 0x4F2E10 reads a u8); applied: the damage event, EXP 30003 kind 0 when we may attack it; not:
        // the event of {(int)((AP' - OUR DP) * 0.1), .., result 0} (retail)}; applied: the skill's TM per hit, then a
        // hit neither missed / blocked / 6 / 8 nor resisted (bit 1) stops it with the condition 15 (sub_570270). Its
        // tick (sub_5701F0, es.cpp): past +220 or the caster gone, the end; its end: the condition 15 off (that hit).
        e.end_ms = 1000 * static_cast<int16_t>(rowi(act, "Second"));
        e.life_ms = e.end_ms;
        const int ap2 = static_cast<int>(static_cast<double>(st_ap_ + 8 * st_hv_ + 8 * st_lk_) *
                                         static_cast<double>(rowf(act, "Ratio")));
        SkillDamage h = skill_phys_damage(m, ap2, rowf(act, "ApplyRatio"), 0);
        e.value = h.result & 0xFF;
        std::vector<PacketBuffer> group;
        group.push_back(opener({ static_cast<uint8_t>(applied ? 1 : 0) }));
        if (!applied) {
            h.result = 0;
            h.dmg    = static_cast<int32_t>(static_cast<double>(ap2 - st_dp_) * 0.1000000014901161);
        }
        const int32_t hp_before = m.hp;
        const bool killed = skill_damage_event(m, h, group);
        server_.broadcast_block(zone_id_, group, 0);
        m.es.push_back(e);
        if (applied) {
            skill_after_hit(m, h.dmg, hp_before, killed, e.skill, !may_attack_owned(m), now, 0);
            const int r = e.value;
            if (r != 2 && r != 1 && r != 6 && r != 8 && !(e.resisted & 1)) {
                bool prevent = false;
                for (const MonsterEntity::Es& o : m.es) if (o.esaction == 1398) { prevent = true; break; }
                if (!prevent) { stop(); m.cond_add(15); }
            }
        } else {
            skill_after_hit(m, h.dmg, hp_before, killed, 0, true, now, 0);
        }
        return true;
    }

    if (d.esaction == 1382) {
        // ESASummonSoldier (1382, Card Soldier; sub_58C520 — on the selected monster; ESAction_SummonSoldier sub_51D170:
        // DamageRatio +4, MinCard +8, MaxCard +10, ApplyRatio +12, Comment +16 — +0 left 0.0: always applied): the
        // target (+212) and us (+240); we may attack it (sub_5E6D70) — else no object; our mark (vtbl+132); we may own
        // it (vtbl+164) — else no object; no Throw item -> the block {S2C 224 sub_5EDE00 ... + [u8 +180][u16 3]} (the
        // client's "Alert_C604_ErrorMsg3") and no object; a Throw item not of SubType 60 (a throw card, sub_524EE0) ->
        // no object; one used up; +200 = DamageRatio, +204 = ApplyRatio, +208 = (int)((MaxCard - MinCard + 1) * (rand() /
        // 32767.0) + MinCard) — MaxCard + 1 on a rand() of 32767 (retail) —, +196 = 3000, +198 = 0; the block {the
        // opener + [u8 +180][u16 0][u16 +208][u16 target][u32 the item's id][u64 its uid]} (VERIFIED: the client's
        // ESASummonSoldier 0x515090 reads them); applied -> a player's use TM. Its tick (sub_58C9E0, es.cpp): +196 -=
        // the time; at <= 1000 once the hit (soldier_hit); at <= 0 the end.
        if (!area_target_ok(m)) return false;
        mark_monster_owner(m);
        if (!may_attack_owned(m)) return false;
        const BagItem* tb = throw_item_ ? bag_find_uid(throw_item_) : nullptr;
        if (!tb) {
            std::vector<PacketBuffer> group;
            group.push_back(opener({ static_cast<uint8_t>(e.result), 3, 0 }));
            server_.broadcast_block(zone_id_, group, 0);
            return false;
        }
        const ItemDef* idf = server_.item_db().get(tb->item_id);
        if (!idf || idf->sub_type != 60) return false;
        const uint32_t item = tb->item_id;
        const uint64_t uid  = tb->uid;
        consume_throw_item(1);
        e.ratio      = rowf(act, "DamageRatio");
        e.base_apply = rowf(act, "ApplyRatio");
        const int mn = static_cast<int16_t>(rowi(act, "MinCard"));
        const int mx = static_cast<int16_t>(rowi(act, "MaxCard"));
        const double rr = static_cast<double>(std::rand()) / 32767.0;
        e.value    = static_cast<int32_t>(static_cast<double>(mx - mn + 1) * rr + static_cast<double>(mn));
        e.phase_ms = 3000;
        e.phase    = 0;
        std::vector<uint8_t> tail{ static_cast<uint8_t>(e.result), 0, 0 };
        auto put16 = [&](uint16_t v) { tail.push_back(static_cast<uint8_t>(v)); tail.push_back(static_cast<uint8_t>(v >> 8)); };
        auto put32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(v >> (8 * k))); };
        put16(static_cast<uint16_t>(e.value));
        put16(static_cast<uint16_t>(m.object_id));
        put32(item);
        put32(static_cast<uint32_t>(uid));
        put32(static_cast<uint32_t>(uid >> 32));
        std::vector<PacketBuffer> group;
        group.push_back(opener(tail));
        server_.broadcast_block(zone_id_, group, 0);
        m.es.push_back(e);
        if (applied) skill_use_tm(e.skill);
        LOG_DEBUG("GameClientSession %u: Card Soldier on mob %u (card %d)", session_id(),
                  static_cast<unsigned>(m.object_id), e.value);
        return true;
    }
    return false;
}

// ESASummonSoldier's hit (sub_58C9E0, its +196 down to 1000): AP' = (int)((AP + 8 * HV) * DamageRatio * (float)(card /
// 6.0 + 0.1)) against its DP (sub_5E6040, +204 ApplyRatio, element 0), its EXP share, the block {the damage event},
// EXP 30003 kind 0 (sub_5E71D0 only: no vtbl+164 here).
void GameClientSession::soldier_hit(uint16_t mob, uint16_t es_id)
{
    if (leaving_ || state_ != GameClientSessionState::InWorld) return;
    MonsterEntity* m = server_.entity_mgr().get_monster(mob);
    if (!m || m->zone != zone_id_) return;
    int32_t card = 0;
    float ratio = 0.0f, apply = 0.0f;
    {
        std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
        const MonsterEntity::Es* e = nullptr;
        for (const MonsterEntity::Es& x : m->es) if (x.id == es_id) { e = &x; break; }
        if (!e) return;
        card  = e->value;
        ratio = e->ratio;
        apply = e->base_apply;
    }
    const auto now = std::chrono::steady_clock::now();
    const float k = static_cast<float>(static_cast<double>(card) / 6.0 + 0.1000000014901161);
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_ + 8 * st_hv_) * static_cast<double>(ratio) *
                                     static_cast<double>(k));
    const SkillDamage h = skill_phys_damage(*m, ap2, apply, 0);
    const int32_t hp_before = m->hp;
    std::vector<PacketBuffer> group;
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*m, h.dmg, hp_before, killed, 0, false, now, 0);
    LOG_DEBUG("GameClientSession %u: Card Soldier (card %d) hits mob %u AP' %d dmg=%d%s", session_id(), card,
              static_cast<unsigned>(mob), ap2, h.dmg, killed ? " [KILLED]" : "");
}

// ESAKamikaze's boom (sub_5B85A0, past AfterBoomTime): up to KillNum (at most 7) monsters around us within Radius
// (sub_53E360); our loss = HP - (int)(HP * HpRemainRatio), theirs = (int)(our loss * Ratio). First the damage event on
// us from us ({our loss, death type 2, 0, ..}: self_damage_event), then each one's — {theirs, 2, 0, 0, flags 0x1000}
// (no roll, no resist) — with its EXP share, EXP 30003 kind 0, then the event; no block. Then ESAKamikazeDo (sub_5B8910:
// {1480, EState 0, ESClass -1, flags 1, skill 4211, the row} on us through sub_524690(.., 1)).
void GameClientSession::kamikaze_boom(const ActiveEs& e)
{
    const auto now = std::chrono::steady_clock::now();
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    int kill_num = e.count;
    if (kill_num > 7) kill_num = 7;                            // (v35: seven slots)
    const std::vector<MonsterEntity*> targets = area_monsters(cx, cy, e.radius, kill_num, now);
    const int32_t loss = hp_ - static_cast<int32_t>(static_cast<double>(hp_) * static_cast<double>(e.base_ratio));
    const int32_t dmg  = static_cast<int32_t>(static_cast<double>(loss) * static_cast<double>(e.ratio));
    SkillDamage self;
    self.dmg = loss; self.death_type = 2; self.result = 0;
    self_damage_event(self, now);
    for (MonsterEntity* m : targets) {
        if (!m || m->zone != zone_id_) continue;
        SkillDamage h;
        h.dmg = dmg; h.death_type = 2; h.result = 0; h.flags = 0x1000;
        const int32_t hp_before = m->hp;
        std::vector<PacketBuffer> group;
        const bool killed = skill_damage_event(*m, h, group);
        server_.broadcast_block(zone_id_, group, 0);
        skill_after_hit(*m, h.dmg, hp_before, killed, 0, false, now, 0);
    }
    LOG_DEBUG("GameClientSession %u: Mass Destruction — we lose %d, %zu monster(s) take %d", session_id(), loss,
              targets.size(), dmg);
    if (const SkillDef* sd = server_.skill_db().get(4211)) {
        SkillDef dd = *sd;
        dd.esaction = 1480;
        dd.estate   = 0;
        dd.esclass  = -1;
        const EsaRow* row = server_.esa_db().row(sd->esa_table, e.level);
        es_create(dd, e.level, row, EsKind::KamikazeDo, false, 1);
    }
}

// The damage event 30002 on us from ourselves (ESAKamikaze's boom -> Character::OnMessage -> Player vtbl+236 sub_73BDB0):
// (our ESs' handlers of the limiter's vtbl+72 first — none of a Card Master's / Gambler's own takes a blow; a party
// member's Bodyguard does: half of a blow with no flags, whiteknight_cut) the recovery pause (sub_73CEF0), HP - damage —
// none left: the Scroll of the Resolute (1399) spares us once at 1 HP (the damage HP - 1) —, S2C 0x002A [us][DamageInfo,
// its [5] our kind 1] (sub_5F5940), then a death (vtbl+248 with the death type: 0x0019 / 0x002B / 0x0020 and OnDie).
void GameClientSession::self_damage_event(SkillDamage h, std::chrono::steady_clock::time_point now)
{
    if (dead_) return;
    for (const ActiveEs& e : es_)
        if (e.limited && e.kind == EsKind::WhiteKnightDo) whiteknight_cut(e, h.dmg, h.flags);
    regen_pause_ms_ = kRegenPauseMs;
    int32_t left = hp_ - h.dmg;
    if (left <= 0) {
        left = 0;
        if (es_remove_esaction(1399)) {
            left  = 1;
            h.dmg = hp_ - 1;
        }
    }
    hp_ = left;
    if (static_cast<double>(hp_max_) * 0.3 >= static_cast<double>(hp_)) pet_speech(6);   // HP <= 30%: our pet's line
    const bool fatal = (hp_ <= 0);
    const bool was_drilling = drill_.active;
    int32_t pcx, pcy;
    cur_pos(now, pcx, pcy);
    std::vector<PacketBuffer> group;
    {
        PacketBuffer p(static_cast<Opcode>(0x002A), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        p.write<int32_t>(h.dmg);
        p.write<int32_t>(h.death_type);
        p.write<int32_t>(h.result);
        p.write<int32_t>(h.element);
        p.write<int32_t>(h.flags);
        p.write<int32_t>(1);
        p.write<int32_t>(h.extra);
        group.push_back(std::move(p));
    }
    if (fatal) player_die_packets(group, pcx, pcy, static_cast<uint8_t>(h.death_type));
    server_.broadcast_block(zone_id_, group, 0);
    es_on_damaged();
    if (fatal) { player_on_die(pcx, pcy); return; }
    if (was_drilling) drill_interrupted();
    wh_hit();
}

// ESAFullHouse's tick before its second (sub_56F860): the server-wide spot takes our position when unset.
void GameClientSession::full_house_mark()
{
    std::lock_guard<std::mutex> lk(g_fh_mutex);
    if (g_fh_set) return;
    int32_t x, y;
    cur_pos(std::chrono::steady_clock::now(), x, y);
    g_fh_x = static_cast<int16_t>(x);
    g_fh_y = static_cast<int16_t>(y);
    g_fh_set = true;
}

// ESAFullHouse past 1000 ms (sub_56F860): applied and the spot set -> up to 5 monsters around the SPOT within Radius
// (sub_53E360) each get ESAFullHouseHit (sub_56FA20: a DebuffSet's roll on 4501's code — all resisted: S2C 1247, and
// the action goes on, retail — {1281, EState 0, ESClass 1, flags 2, skill 4501, the row, the resisted bits} through
// sub_524690(.., 1)); the spot unset.
void GameClientSession::full_house_burst(const ActiveEs& e)
{
    bool set = false;
    int16_t sx = 0, sy = 0;
    {
        std::lock_guard<std::mutex> lk(g_fh_mutex);
        set = g_fh_set;
        sx = g_fh_x; sy = g_fh_y;
        g_fh_set = false;
    }
    if (e.result != 1 || !set) return;
    const SkillDef* sd = server_.skill_db().get(e.skill);
    if (!sd) return;
    const EsaRow* row = server_.esa_db().row(sd->esa_table, e.level);
    const auto now = std::chrono::steady_clock::now();
    const std::vector<MonsterEntity*> targets = area_monsters(sx, sy, e.radius, 5, now);
    SkillDef dd = *sd;
    dd.esaction = 1281;
    dd.estate   = 0;
    dd.esclass  = 1;
    for (MonsterEntity* m : targets) {
        uint32_t bits = 0;
        if (monster_debuff_immune(*m, sd->debuff_bits, bits)) send_debuff_resisted(*m, bits);
        es_resisted_ = bits;
        es_apply_on(*m, dd, e.level, row);
        es_resisted_ = 0;
    }
    LOG_DEBUG("GameClientSession %u: Full House at (%d,%d): %zu monster(s)", session_id(), sx, sy, targets.size());
}
