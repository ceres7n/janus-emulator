#include "game_session.hpp"
#include "game_server.hpp"
#include "logger.hpp"
#include "combat_util.hpp"
#include "paula_crypt.hpp"
#include <algorithm>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
//  The Entertainer (Cat, 2nd job) and its 3rd job the Prima Donna — 1:1 with the retail GameServer
//  (GameServerRelease), the client's classes verified in Trickster.bin (each class's read function reads the
//  opener's tail). Evolution (ESAWildMutation 1208) and Beast Instinct (ESABeastDo 1494) are mutations: five base
//  stats rewritten (+704), StateType 4 / 64 — the skills of StateType 2 (Beast Claw, Fatal Wound, Raging Nail, Sharp
//  Scream, Beast Instinct) need the bit 4. Team Bolster (4103) is a passive ESABigShield reads (es.cpp). Fatal Wound
//  (ESASharpClaw 1245) and Sumo Suit (ESASubcutaneous 1311) are effect states of ours (es.cpp).
// ─────────────────────────────────────────────────────────────────────────────

namespace {
float rowf(const EsaRow* r, const char* k) { return r ? static_cast<float>(r->get(k, 0.0)) : 0.0f; }
int   rowi(const EsaRow* r, const char* k) { return r ? static_cast<int>(r->get(k, 0.0)) : 0; }

void put_xy(PacketBuffer& p, int32_t x, int32_t y)
{
    p.write<uint16_t>(static_cast<uint16_t>(x));
    p.write<uint16_t>(static_cast<uint16_t>(y));
}

struct CHit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };

// sub_5205F0 for a monster target (no ESAShield / ESAGuard / 1463 / 1492 of a player to soak a blow): a miss / block /
// no damage records {0, result, -1}; else it adds up (+dmg, a hit more) and records {dmg, result, -1}.
struct CRec { int32_t dmg; int32_t result; uint16_t info; };
void crec_add(int32_t dmg, int32_t result, int32_t& total, uint16_t& hits, std::vector<CRec>& recs)
{
    if (dmg <= 0 || result == 1 || result == 2) { recs.push_back({ 0, result, 0xFFFF }); return; }
    total += dmg;
    ++hits;
    recs.push_back({ dmg, result, 0xFFFF });
}
} // namespace

// ── ESAction 1139 "Volley Kick" (GameServer sub_5C6DC0, ESAVolleyKick; ESAUI 1) ───────────────────────────────
//  The selected target (kind 3) — none: nothing; our mark (vtbl+132, not on ourselves). The row ESAction_Volley_Kick
//  (sub_51AEC0: ApplyRatio +0, Ratio +4). AP' = (int)((16 * HV + AP) * Ratio * 0.75) against its DP (sub_5E6040,
//  ApplyRatio, element 0), its EXP share; the block {the opener sub_5EE050 + [u16 target][pos us][pos target] (sub_5C7140;
//  VERIFIED: the client's ESAVolleyKick 0x4CE260 reads them), the damage event, EXP 30003 kind 0}; the 30107 follow-up
//  (the flag 2 clear, the target alive); the skill's TM per hit.
void GameClientSession::esa_volley_kick(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    mark_monster_owner(*t);
    const int ap2 = static_cast<int>(static_cast<double>(16 * st_hv_ + st_ap_) * static_cast<double>(rowf(act, "Ratio")) * 0.75);
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
    LOG_DEBUG("GameClientSession %u: Volley Kick -> mob %u AP' %d dmg=%d result=%d%s", session_id(), tid, ap2, h.dmg,
              h.result, killed ? " [KILLED]" : "");
}

// ── ESAction 1165 "Galder Barrage" (ESAMultiGellderHit, init sub_55DC80; ESAUI 8: an object + an amount) ─────────
//  A generic ES on the caster (ESClass -1, made at once) whose init does the work. The row ESAction_MultiGellder
//  (sub_51B420: ApplyRatio +0, Ratio +4, Radius +8): +196 = Ratio, +200 = ApplyRatio, +204 = (float)Radius. The
//  payload's [u16 target] (kind 3) we may attack (sub_5E6D70) — else no object; its [u32 money] not above 0: the log
//  "ESAMultiGellderHit::Money - invalid" and no object; a player pays 150 gelder (sub_55E560: 150 more than its money ->
//  no object; else money -= 150 and S2C 0x0029); then the owner test (vtbl+164) — no: no object (the 150 are gone:
//  retail). +212 = 2000 (its lifetime), +232 = AP' = (int)(150 * Ratio). The block: the opener sub_5EE050 with the
//  object's id + [u16 target][pos us][pos target] (sub_55E500; VERIFIED: the client's ESAMultiGellderHit 0x4D4710 reads
//  them); then (sub_55E1B0) the zone's monsters in list order — not dead, our layer, not Type 8 / 12, a shadow one only
//  with the Third Eye, within Radius of the TARGET (Radius >= sqrtf), at most 7, no owner or ours: each the physical
//  damage sub_5E6040(AP', DP 0, ApplyRatio, element 512), its EXP share, the damage event, EXP 30003 kind 0. Then the
//  skill's TM per hit on the target. The object lives 2000 ms (sub_55DF80).
void GameClientSession::esa_multi_gellder_hit(const SkillDef& d, uint8_t lv, const EsaRow* act)
{
    const auto now = std::chrono::steady_clock::now();
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !area_target_ok(*t)) return;   // sub_5E6D70 (a corpse too: no death test)
    const std::vector<uint8_t>& pl = su_.payload;
    int32_t money_arg = 0;
    if (pl.size() >= 6)
        for (int k = 0; k < 4; ++k) money_arg |= static_cast<int32_t>(pl[2 + k]) << (8 * k);
    if (money_arg <= 0) {
        LOG_INFO("!!!!!!!!!!!!!!! ESAMultiGellderHit::Money - invalid :%s", char_name_);
        return;
    }
    if (150 > money_) return;                                  // sub_55E560(150)
    money_ -= 150;
    send_money();
    if (!may_attack_owned(*t)) return;                         // vtbl+164 (after paying: retail)
    const uint16_t id = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());
    const float ratio  = rowf(act, "Ratio");
    const float apply  = rowf(act, "ApplyRatio");
    const float radius = static_cast<float>(static_cast<int16_t>(rowi(act, "Radius")));
    const int ap2 = static_cast<int>(static_cast<double>(150) * static_cast<double>(ratio));
    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    t->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_), id);
    o.write<uint16_t>(tid);
    put_xy(o, ax, ay);
    put_xy(o, tx, ty);
    group.push_back(std::move(o));
    std::vector<CHit> done;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0 || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (!(static_cast<double>(radius) >= static_cast<double>(dist_f(mx, my, tx, ty)))) continue;
        if (done.size() >= 7) break;
        if (owned_by_other(*m)) continue;
        const SkillDamage h = skill_phys_damage(*m, ap2, apply, 512, 0);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const CHit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now);
    skill_hit_tm(*t, su_.skill);
    es_push_timer(d, lv, id, 2000);
    LOG_DEBUG("GameClientSession %u: Galder Barrage at mob %u (AP' %d): %zu hit, money %d", session_id(), tid, ap2,
              done.size(), money_);
}

// ── ESAction 1199 "Siren Song" (GameServer sub_5CC380, ESASiren; ESAUI 1) ─────────────────────────────────────
//  The selected target (kind 3) — none: nothing. The row ESAction_Siren (sub_51B960: ApplyRatio +0, Ratio +4, Radius +8
//  i16, MonNum +12, Angle +16). The targets (sub_5CC540): the zone's monsters in list order — not dead, our layer, not
//  Type 8 / 12, a shadow one only with the Third Eye — within Radius of the TARGET; such a one once MonNum are taken
//  stops the walk; the target itself is taken, any other only within ±Angle of our line to the target (sub_893D30) and
//  with no owner or ours (players in PvP, sub_5CC8D0). The block: the opener sub_5EE050 + [u16 target][u16 n][u16 id]*n
//  (VERIFIED: the client's ESASiren 0x4E14D0 reads them); each one (sub_5CCAE0, looked up as a monster then a player):
//  AP' = (int)((our HP * 0.1 + our DP * 0.5) * Ratio) against its DP (sub_5E6040, ApplyRatio, element 0), its EXP share,
//  the damage event, EXP 30003 kind 0. No TM (retail).
void GameClientSession::esa_siren(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    const float   apply   = rowf(act, "ApplyRatio");
    const float   ratio   = rowf(act, "Ratio");
    const int16_t radius  = static_cast<int16_t>(rowi(act, "Radius"));
    const int     mon_num = rowi(act, "MonNum");
    const float   angle   = rowf(act, "Angle");
    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    t->motion.pos(now, tx, ty);
    std::vector<uint16_t> ids;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0 || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (!(static_cast<double>(dist_f(mx, my, tx, ty)) <= static_cast<double>(radius))) continue;
        if (static_cast<int>(ids.size()) >= mon_num) break;
        if (oid != tid) {
            if (!server_in_line(static_cast<int16_t>(ax), static_cast<int16_t>(ay), static_cast<int16_t>(tx),
                                static_cast<int16_t>(ty), static_cast<int16_t>(mx), static_cast<int16_t>(my), angle))
                continue;
            if (owned_by_other(*m)) continue;
        }
        ids.push_back(static_cast<uint16_t>(oid));
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    o.write<uint16_t>(static_cast<uint16_t>(ids.size()));
    for (uint16_t v : ids) o.write<uint16_t>(v);
    group.push_back(std::move(o));
    const int ap2 = static_cast<int>((static_cast<double>(hp_) * 0.1000000014901161 + static_cast<double>(st_dp_) * 0.5) *
                                     static_cast<double>(ratio));
    std::vector<CHit> done;
    for (uint16_t v : ids) {
        MonsterEntity* m = server_.entity_mgr().get_monster(v);
        if (!m) continue;
        const SkillDamage h = skill_phys_damage(*m, ap2, apply, 0);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const CHit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now);
    LOG_DEBUG("GameClientSession %u: Siren Song at mob %u (AP' %d): %zu hit", session_id(), tid, ap2, ids.size());
}

// ── ESAction 1216 "Beast Claw" (GameServer sub_5CE8D0, ESARush; ESAUI 1; StateType 2) ─────────────────────────
//  The selected target (kind 3) — none: nothing; our mark (vtbl+132, not on ourselves). The row ESAction_Rush
//  (sub_51BA50: ApplyRatio +0, Ratio +4). AP' = (int)(AP * Ratio) (sub_560500) against its DP, ApplyRatio, element 0;
//  its EXP share; the block {the opener sub_5EE050 + [u16 target][pos us][pos target] (sub_5CEB10; VERIFIED: the
//  client's ESARush 0x4E3FB0 reads them), the damage event, EXP 30003 kind 0}; the skill's TM per hit. No 30107.
void GameClientSession::esa_rush(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    mark_monster_owner(*t);
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) * static_cast<double>(rowf(act, "Ratio")));
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
    skill_after_hit(*t, h.dmg, hp_before, killed, su_.skill, false, now);
    LOG_DEBUG("GameClientSession %u: Beast Claw -> mob %u AP' %d dmg=%d result=%d%s", session_id(), tid, ap2, h.dmg,
              h.result, killed ? " [KILLED]" : "");
}

// ── ESAction 1329 "Fist Full of Galders" (GameServer sub_5D9280, ESAGellderChainHit; ESAUI 8) ──────────────────
//  The selected target (kind 3) we may attack (vtbl+164) — else nothing. The row ESAction_GellderChainHit (sub_51C690:
//  ApplyRatio +0, Ratio +4, Count +10). The hits = min(Count, 4); each (sub_5D9540) the physical damage of AP' =
//  (int)(Ratio * 100) against DP 0 (sub_5E6040, ApplyRatio, element 0), recorded (sub_5205F0) — not divided. Our +0x148
//  flag 2 cleared (sub_5101D0). The block: the opener sub_5EE050 + [u16 target][pos us][pos target][u16 hits]{[u32
//  damage][u32 result][u16 -1]}*hits (sub_5D9480; VERIFIED: the client's ESAGellderChainHit 0x5030D0 reads them), one
//  damage event (sub_5D9610): {the sum, death type 2, result 0, element 0, flags 0x8000, [6] = the hits}, its EXP share,
//  30003 kind 0, 30002. Then money -= 100 * hits — not checked, it may go below 0 (retail, sub_73B280) — and S2C 0x0029,
//  the skill's TM per hit.
void GameClientSession::esa_gellder_chain_hit(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    if (!may_attack_owned(*t)) return;
    int hits_n = static_cast<int16_t>(rowi(act, "Count"));
    if (hits_n > 4) hits_n = 4;
    const int ap2 = static_cast<int>(static_cast<double>(rowf(act, "Ratio")) * 100.0);
    const float apply = rowf(act, "ApplyRatio");
    int32_t total = 0;
    uint16_t hits = 0;
    std::vector<CRec> recs;
    for (int i = 0; i < hits_n; ++i) {
        const SkillDamage h = skill_phys_damage(*t, ap2, apply, 0, 0);
        if (h.absorbed > 0) absorb_heal(*t, h.absorbed);
        crec_add(h.dmg, h.result, total, hits, recs);
    }
    su_.resume = false;                                        // sub_5101D0(+0x148, 2)
    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    t->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    put_xy(o, ax, ay);
    put_xy(o, tx, ty);
    o.write<uint16_t>(static_cast<uint16_t>(hits_n));
    for (const CRec& r : recs) {
        o.write<uint32_t>(static_cast<uint32_t>(r.dmg));
        o.write<uint32_t>(static_cast<uint32_t>(r.result));
        o.write<uint16_t>(r.info);
    }
    group.push_back(std::move(o));
    SkillDamage c;
    c.dmg        = total;
    c.death_type = 2;
    c.result     = 0;
    c.flags      = 0x8000;
    c.extra      = hits;
    const int32_t hp_before = t->hp;
    const bool killed = skill_damage_event(*t, c, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*t, c.dmg, hp_before, killed, 0, false, now);
    money_ -= 100 * hits_n;                                    // sub_73B280(-100 * hits)
    send_money();
    skill_hit_tm(*t, su_.skill);
    LOG_DEBUG("GameClientSession %u: Fist Full of Galders -> mob %u: %d hit(s), %d damage, money %d%s", session_id(), tid,
              hits_n, total, money_, killed ? " [KILLED]" : "");
}

// ── ESAction 1364 "Deadly Funk" (GameServer sub_5DBD20, ESAsmellEX; ESAUI 6 / ESAIC 3: a point) ─────────────────
//  The row ESAction_smellEX (sub_51ABB0: ApplyRatio +0, Seconds +4, Comment +6, Radius +8) kept with the payload's point
//  and us. The zone's monsters in list order (sub_5DC0A0) — on our layer, not Type 8 / 12, a shadow one only with the
//  Third Eye — no death test: corpses count (retail) — within Radius of the point; such a one once 5 are taken stops the
//  walk; one with no owner or ours gets (sub_5DBF40) — a monster with a DebuffSet first rolls against Skunk Pouch's
//  (4010) DebuffBitCode (all resisted -> S2C 1247) — Skunk Pouch's action {its ESAction (1119 ESASkunkBag), EState,
//  ESClass, skill 4010, flags 2, OUR row} through sub_524690(.., 1). (Players in PvP, sub_5DBE00.) No packet, no TM.
void GameClientSession::esa_smell_ex(const SkillDef& d, uint8_t lv, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    (void)d;
    int16_t px = 0, py = 0;
    if (su_.payload.size() >= 4) {
        px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
        py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
    }
    const SkillDef* s4010 = server_.skill_db().get(4010);
    if (!s4010) return;
    SkillDef sk_do = *s4010;                                   // the action's flags are 2 (not sub_8911A0's)
    sk_do.skill_option &= ~(1 | 512);
    const SkillDef* sk = &sk_do;
    const float radius = static_cast<float>(static_cast<int16_t>(rowi(act, "Radius")));
    std::vector<MonsterEntity*> picks;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (!(static_cast<double>(radius) >= static_cast<double>(dist_f(mx, my, px, py)))) continue;
        if (picks.size() >= 5) break;
        if (owned_by_other(*m)) continue;
        picks.push_back(m);
    }
    for (MonsterEntity* m : picks) {
        uint32_t bits = 0;
        if (monster_debuff_immune(*m, sk->debuff_bits, bits)) { send_debuff_resisted(*m, bits); continue; }
        es_resisted_ = bits;
        es_apply_on(*m, *sk, lv, act);
        es_resisted_ = 0;
    }
    LOG_DEBUG("GameClientSession %u: Deadly Funk at (%d,%d): %zu monster(s)", session_id(), px, py, picks.size());
}

// ── ESAction 1477 "Aura Shatter" (GameServer sub_5DEE50, ESABuffCancler; ESAUI 6 / ESAIC 3) ──────────────────────
//  The row ESAction_BuffCancler (sub_51DCB0: ApplyRatio +0, Count +4, KillNum +8, Ratio +12, RemoveRatio +16, Delay +20,
//  Comment +24, Radius +28). sub_53E360 around the payload's point, at most KillNum, Radius; each one with an ES in its
//  limiter's list 0 (vtbl+88(0)) gets (sub_5DEF80) — a DebuffSet roll on 4111's code first (all resisted -> S2C 1247) —
//  the action {ESAction 1478, EState 0, ESClass -1, flags 2, skill 4111, the row}: its ESABuffCanclerDo
//  (buff_cancler_on). Then the use TM. No packet of its own (VERIFIED: the client's ESABuffCancler 0x51E1F0 reads none).
void GameClientSession::esa_buff_cancler(const SkillDef& d, uint8_t lv, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    int16_t px = 0, py = 0;
    if (su_.payload.size() >= 4) {
        px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
        py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
    }
    const std::vector<MonsterEntity*> targets = area_monsters(px, py, rowi(act, "Radius"), rowi(act, "KillNum"), now);
    SkillDef dd = d;
    dd.esaction = 1478;
    dd.estate   = 0;
    dd.esclass  = -1;
    size_t n = 0;
    for (MonsterEntity* m : targets) {
        bool list0 = false;
        {
            std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
            for (const MonsterEntity::Es& x : m->es)
                if (x.esclass == 0) { list0 = true; break; }
        }
        if (!list0) continue;
        uint32_t bits = 0;
        if (monster_debuff_immune(*m, d.debuff_bits, bits)) { send_debuff_resisted(*m, bits); continue; }
        es_resisted_ = bits;
        es_apply_on(*m, dd, lv, act);
        es_resisted_ = 0;
        ++n;
    }
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Aura Shatter at (%d,%d): %zu of %zu with a list-0 ES", session_id(), px, py, n,
              targets.size());
}

// ESABuffCanclerDo (1478, sub_5B7D20; the action carries Aura Shatter's row): the target required; it (+236) and us
// (+212); +196 = (int)(Delay * 1000), +200 = Count, +208 = ApplyRatio, +204 = (int)((16 * HV + AP) * Ratio * 3 / 4),
// +8 = 0; kept whatever the roll (+184); the block {S2C 224 sub_5EDE00 [us][target][ES id][1478][2][ESClass -1][4111][0],
// no tail}. No mark, no TM. Its tick (es.cpp): every Delay a pulse (buff_cancler_hit).
bool GameClientSession::buff_cancler_on(MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act)
{
    e.flags      = 2;
    e.phase_ms   = static_cast<int32_t>(static_cast<double>(rowf(act, "Delay")) * 1000.0);
    e.phase      = rowi(act, "Count");
    e.base_apply = rowf(act, "ApplyRatio");
    e.value      = static_cast<int32_t>(static_cast<double>(16 * st_hv_ + st_ap_) * static_cast<double>(rowf(act, "Ratio")) *
                                        3.0 / 4.0);
    e.elapsed_ms = 0;
    e.stay       = true;
    e.end_ms     = INT32_MAX;
    e.life_ms    = 0;
    e.warned     = true;
    {
        std::vector<PacketBuffer> group;
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(1478);
        o.write<uint8_t>(2);
        o.write<uint8_t>(0xFF);
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(0);
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    m.es.push_back(e);
    return true;
}

// ESABuffCanclerDo's pulse (sub_5B7E80): the target's last list-0 ES (the limiter's vtbl+96 sub_5E3690: from the end, one
// without a parent first — ours have none) — none: no Count left (+200 = 0). Else the physical damage sub_5E6040(+204, its
// DP, +208, element 0), its EXP share, EXP 30003 kind 0 when we may attack it — before 30002 —, DamageInfo flags |= 0x4000,
// the damage event, Count - 1; a hit neither missed nor blocked takes that ES off (the limiter's vtbl+60).
void GameClientSession::buff_cancler_hit(uint16_t mob, uint16_t es_id)
{
    if (leaving_ || state_ != GameClientSessionState::InWorld) return;
    MonsterEntity* m = server_.entity_mgr().get_monster(mob);
    if (!m || m->zone != zone_id_) return;
    int32_t value = 0;
    float apply = 0.0f;
    int entry = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
        MonsterEntity::Es* e = nullptr;
        for (MonsterEntity::Es& x : m->es) if (x.id == es_id) { e = &x; break; }
        if (!e) return;
        for (size_t k = m->es.size(); k-- > 0;)
            if (m->es[k].esclass == 0) { entry = m->es[k].esaction; break; }
        if (!entry) { e->phase = 0; return; }
        value = e->value;
        apply = e->base_apply;
    }
    if (!m->alive || m->hp <= 0) return;
    const auto now = std::chrono::steady_clock::now();
    SkillDamage h = skill_phys_damage(*m, value, apply, 0, m->dp);
    h.flags |= 0x4000;
    const int32_t hp_before = m->hp;
    std::vector<PacketBuffer> group;
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*m, h.dmg, hp_before, killed, 0, !may_attack_owned(*m), now, 0);
    std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
    for (MonsterEntity::Es& x : m->es) if (x.id == es_id) { --x.phase; break; }
    if (h.dmg <= 0 || h.result == 1 || h.result == 2) return;
    for (size_t k = 0; k < m->es.size(); ++k)
        if (m->es[k].esaction == entry) { server_.monster_es_end(*m, k); break; }
    LOG_DEBUG("GameClientSession %u: Aura Shatter takes ES %d off mob %u", session_id(), entry, static_cast<unsigned>(mob));
}

// ESAWinkOfCharm (1273, Entrancing Wink; sub_56ED40, loader sub_51BEF0: ApplyRatio +0, Ratio +4, Second +8): +200 = 0,
// +196 = 1000 * Second; the payload's target is its owner; the lifetime +196; the block {S2C 224 sub_5EDE00 [us][target]
// [ES id][1273][2][ESClass 1][4301][0] + [u8 +180]} (VERIFIED: the client's ESAWinkOfCharm 0x4F0F90 reads a u8); a player
// caster's TM per hit; then (sub_56EF40) the target stops (sub_41C5D0) and takes the condition 0x2000 — whatever the
// roll (the base tick ends a not applied one on its next tick; its end takes the condition off). (0x2000 forbids the
// monster's flagged skills, sub_891E70 — no monster skills here.) No mark.
bool GameClientSession::wink_on(MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act)
{
    e.end_ms     = 1000 * static_cast<int16_t>(rowi(act, "Second"));
    e.life_ms    = e.end_ms;
    e.elapsed_ms = 0;
    {
        std::vector<PacketBuffer> group;
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(1273);
        o.write<uint8_t>(2);
        o.write<uint8_t>(static_cast<uint8_t>(e.esclass));
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(static_cast<uint16_t>(e.estate));
        o.write<uint8_t>(static_cast<uint8_t>(e.result));
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    m.es.push_back(e);
    skill_hit_tm(m, e.skill);
    const auto now = std::chrono::steady_clock::now();
    int32_t x, y;
    m.motion.pos(now, x, y);
    m.motion.rest(x, y);
    m.pos_x = x; m.pos_y = y;
    m.cond_add(0x2000);
    LOG_DEBUG("GameClientSession %u: Entrancing Wink on mob %u (ES %u, result %d, %d ms)", session_id(),
              static_cast<unsigned>(m.object_id), e.id, e.result, e.end_ms);
    return true;
}

// ── ESAction 1258 "2 Hit Combo" (GameServer sub_5D2E30, ESATwoChainAttack; ESAUI 1) ───────────────────────────
//  The selected target (kind 3) — none: nothing; Power Blow (4001) and Volley Kick (4101) learned (levels > 0) — else
//  nothing. The row ESAction_TwoChainAttack (sub_51BA50: ApplyRatio +0, Ratio +4). Two rolls (sub_5D3110) against its
//  DP (sub_5E6040, ApplyRatio, element 0): AP1 = (int)(((Power Blow's level + 20) * 0.2 - 3) * AP * Ratio), AP2 =
//  (int)(((Volley Kick's level + 20) * 0.2 - 3) * (16 * HV + AP) * Ratio * 0.75), recorded (sub_5205F0). The block: the
//  opener sub_5EDE00 [us][TARGET][0]... + {[u32 damage][u32 result][u16 -1]}*2 (sub_5D30A0; VERIFIED: the client's
//  ESATwoChainAttack 0x4EEDB0 reads them), one damage event (sub_5D3270): {the sum, 2, 0, 0, 0x8000, [6] = the hits},
//  its EXP share, 30003 kind 0, 30002. Then the 30107 follow-up and the skill's TM per hit.
void GameClientSession::esa_two_chain_attack(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    const LearnedSkill* pb = find_skill(4001);
    const LearnedSkill* vk = find_skill(4101);
    if (!pb || !vk || pb->level <= 0 || vk->level <= 0) return;
    const float ratio = rowf(act, "Ratio");
    const float apply = rowf(act, "ApplyRatio");
    const double ap_r = static_cast<double>(st_ap_) * static_cast<double>(ratio);
    int ap[2];
    ap[0] = static_cast<int>((static_cast<double>(pb->level + 20) * 0.2000000029802322 - 3.0) * ap_r);
    ap[1] = static_cast<int>((static_cast<double>(vk->level + 20) * 0.2000000029802322 - 3.0) *
                             static_cast<double>(16 * st_hv_ + st_ap_) * static_cast<double>(ratio) * 0.75);
    int32_t total = 0;
    uint16_t hits = 0;
    std::vector<CRec> recs;
    for (int i = 0; i < 2; ++i) {
        const SkillDamage h = skill_phys_damage(*t, ap[i], apply, 0, t->dp);
        if (h.absorbed > 0) absorb_heal(*t, h.absorbed);
        crec_add(h.dmg, h.result, total, hits, recs);
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, tid);
    for (const CRec& r : recs) {
        o.write<uint32_t>(static_cast<uint32_t>(r.dmg));
        o.write<uint32_t>(static_cast<uint32_t>(r.result));
        o.write<uint16_t>(r.info);
    }
    group.push_back(std::move(o));
    SkillDamage c;
    c.dmg        = total;
    c.death_type = 2;
    c.result     = 0;
    c.flags      = 0x8000;
    c.extra      = hits;
    const int32_t hp_before = t->hp;
    const bool killed = skill_damage_event(*t, c, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_follow_after(tid);
    skill_after_hit(*t, c.dmg, hp_before, killed, su_.skill, false, now);
    LOG_DEBUG("GameClientSession %u: 2 Hit Combo -> mob %u AP' %d / %d: %u hit, %d damage%s", session_id(), tid, ap[0],
              ap[1], hits, total, killed ? " [KILLED]" : "");
}

// ── ESAction 1346 "Raging Nail" (GameServer sub_5DA540, ESAWildTalon; ESAUI 1; StateType 2) ───────────────────
//  The selected target (kind 3) — none: nothing; our mark (vtbl+132, not on ourselves). The row ESAction_WildTalon
//  (sub_51CA60: ApplyRatio +0, Ratio +4, DisDefenceRatio +8 i16). AP' = (int)((16 * HV + AP) * Ratio * 0.5) against DP'
//  = (int)(its DP * DisDefenceRatio / 100), sub_5E6040 with ApplyRatio 1.0 (not the row's: retail), element 0; its EXP
//  share; the block {the opener sub_5EE050 + [u16 target][pos us][pos target] (sub_5DA8C0; VERIFIED: the client's
//  ESAWildTalon 0x5072F0 reads them), the damage event, EXP 30003 kind 0}; the 30107 follow-up; the skill's TM per hit.
void GameClientSession::esa_wild_talon(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    mark_monster_owner(*t);
    const int ap2 = static_cast<int>(static_cast<double>(16 * st_hv_ + st_ap_) * static_cast<double>(rowf(act, "Ratio")) * 0.5);
    const int dp  = static_cast<int>(static_cast<double>(t->dp) *
                                     (static_cast<double>(static_cast<int16_t>(rowi(act, "DisDefenceRatio"))) / 100.0));
    const SkillDamage h = skill_phys_damage(*t, ap2, 1.0f, 0, dp);
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
    LOG_DEBUG("GameClientSession %u: Raging Nail -> mob %u AP' %d DP' %d dmg=%d result=%d%s", session_id(), tid, ap2, dp,
              h.dmg, h.result, killed ? " [KILLED]" : "");
}

// ── ESAction 1381 "Sharp Scream" (GameServer sub_5DCD10, ESASharpScream; no selection; StateType 2) ──────────────
//  The row ESAction_SharpScream (sub_51D100: Ratio +4, Radius +8 i16, Count +10 i16, ApplyRatio +12 — not at +0, Comment
//  +16). sub_53E360 around us, at most 4, Radius; then n = min(that, Count). The block: the opener sub_5EE050 + [u16
//  Radius] (VERIFIED: the client's ESASharpScream 0x514AA0 reads a u16); each of the n: it stops (sub_41C5D0), AP' =
//  (int)((16 * HV + AP) * Ratio * 0.75) against its DP (sub_5E6040, the ApplyRatio, element 0), the damage event, its EXP
//  share, EXP 30003 kind 0. Then the use TM.
void GameClientSession::esa_sharp_scream(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const int16_t radius = static_cast<int16_t>(rowi(act, "Radius"));
    const int16_t count  = static_cast<int16_t>(rowi(act, "Count"));
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    std::vector<MonsterEntity*> targets = area_monsters(cx, cy, radius, 4, now);
    if (static_cast<int>(targets.size()) > count) targets.resize(static_cast<size_t>(std::max<int>(0, count)));
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(static_cast<uint16_t>(radius));
    group.push_back(std::move(o));
    const int ap2 = static_cast<int>(static_cast<double>(16 * st_hv_ + st_ap_) * static_cast<double>(rowf(act, "Ratio")) * 0.75);
    const float apply = rowf(act, "ApplyRatio");
    std::vector<CHit> done;
    for (MonsterEntity* m : targets) {
        int32_t x, y;
        m->motion.pos(now, x, y);
        m->motion.rest(x, y);
        m->pos_x = x; m->pos_y = y;
        const SkillDamage h = skill_phys_damage(*m, ap2, apply, 0);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const CHit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Sharp Scream (radius %d, AP' %d): %zu hit", session_id(), radius, ap2, done.size());
}

// ── ESAction 1493 "Beast Instinct" (GameServer sub_5E00A0; no selection; StateType 2) ─────────────────────────────
//  The action {ESAction 1494, EState 0, ESClass 0, flags 2, skill 4305, the row} on us through sub_524690(.., 1): the
//  limiter, the ESABeastDo (es.cpp EsKind::Mutation — its own init gives the use TM too); then the use TM.
void GameClientSession::esa_beast(const SkillDef& d, uint8_t lv, const EsaRow* act)
{
    SkillDef dd = d;
    dd.esaction = 1494;
    dd.estate   = 0;
    dd.esclass  = 0;
    if (dd.type != 2 && dd.id != 7046) {
        if (es_limiter_admit(dd.esclass, dd.esaction, static_cast<uint16_t>(dd.id)))
            es_create(dd, lv, act, EsKind::Mutation, true, 2);
    } else {
        es_create(dd, lv, act, EsKind::Mutation, false, 2);
    }
    skill_use_tm(su_.skill);
}

// +704 + 4i: the base stat — gulim13 by the points, or what a mutation (or its end) left there while the points stay.
int32_t GameClientSession::base_stat(int i) const
{
    if (i < 0 || i >= 12) return 0;
    if (base_fixed_) {
        bool same = true;
        for (int k = 0; k < 12; ++k) if (base_fixed_pts_[k] != accpts_[k]) { same = false; break; }
        if (same) return base_fixed_val_[i];
    }
    return server_.stat_table().value(acc_points(i) / 4 + 1, i);   // (sub_717170: + the party's adder)
}

// S2C 784 (sub_602380) to us: [u8 5]{[u8 stat][u32 its base]} — ESAWildMutation / ESABeastDo: AP (0), DX (2), DP (10),
// HV (11), LK (8); ESAWildChange: AP, DX, LK, DP, HV. VERIFIED: the client's avatar handler (sub_6B4E00 case 0x310) puts
// each u32 at avatar +604 + 4 * stat and refreshes the stat window.
void GameClientSession::send_base_stats(bool change_order)
{
    static const int kMutation[5] = { 0, 2, 10, 11, 8 };
    static const int kChange[5]   = { 0, 2, 8, 10, 11 };
    PacketBuffer p(static_cast<Opcode>(0x0310), is_paula());
    p.write<uint8_t>(5);
    for (int s : change_order ? kChange : kMutation) {
        p.write<uint8_t>(static_cast<uint8_t>(s));
        p.write<uint32_t>(static_cast<uint32_t>(base_stat(s)));
    }
    send(p);
}

namespace {
// The three mutations: ESAWildMutation (1208, Evolution: StateType 4), ESABeastDo (1494, Beast Instinct: 64),
// ESAWildChange (1248, Metamorphosis: 8).
uint32_t mutation_bit(int esaction) { return esaction == 1494 ? 64u : esaction == 1248 ? 8u : 4u; }
const char* mutation_name(int esaction)
{
    return esaction == 1494 ? "Beast Instinct" : esaction == 1248 ? "Metamorphosis" : "Evolution";
}

// Their base stats from the record's (the +232.. ratios of InitMutation / InitBeast / InitChange and of their
// UpdatePlayerBaseStat, vtbl+108): ESAWildMutation AP += (int)(AP * ApPRatio), DX -= DxPValue, DP -= (int)(DP * DpMRatio),
// HV -= (int)(HV * HvMRatio), LK -= (int)(LK * LkMRatio); ESABeastDo AP += (int)(AP * AP_Ratio), DX -= (int)DX_Ratio,
// DP = (int)(DP * DP_Ratio), HV = (int)(HV * HV_Ratio), LK -= (int)(LK * LK_Ratio); ESAWildChange AP -= (int)(AP *
// ApMRatio), DX += DxMValue, LK -= (int)(LK * LkMRatio), DP += (int)(DP * DpPRatio), HV += (int)(HV * HvPRatio).
void mutation_formulas(int esaction, const EsaRow* act, int32_t b[12])
{
    auto mul = [](int32_t v, float r) { return static_cast<int32_t>(static_cast<double>(v) * static_cast<double>(r)); };
    if (esaction == 1494) {
        b[0] += mul(b[0], rowf(act, "AP_Ratio"));
        b[2] -= static_cast<int32_t>(rowf(act, "DX_Ratio"));
        b[10] = mul(b[10], rowf(act, "DP_Ratio"));
        b[11] = mul(b[11], rowf(act, "HV_Ratio"));
        b[8] -= mul(b[8], rowf(act, "LK_Ratio"));
    } else if (esaction == 1248) {
        b[0] -= mul(b[0], rowf(act, "ApMRatio"));
        b[2] += rowi(act, "DxMValue");
        b[8] -= mul(b[8], rowf(act, "LkMRatio"));
        b[10] += mul(b[10], rowf(act, "DpPRatio"));
        b[11] += mul(b[11], rowf(act, "HvPRatio"));
    } else {
        b[0] += mul(b[0], rowf(act, "ApPRatio"));
        b[2] -= rowi(act, "DxPValue");
        b[10] -= mul(b[10], rowf(act, "DpMRatio"));
        b[11] -= mul(b[11], rowf(act, "HvMRatio"));
        b[8] -= mul(b[8], rowf(act, "LkMRatio"));
    }
}
} // namespace

// InitMutation / InitBeast / InitChange (sub_574AD0 / sub_5BB1B0 / sub_575730): the protection 3000 ms (sub_73CF80); our
// StateType without the bit 1 -> "... Already Mutation state" (InitBeast logs ESAWildMutation's, retail), refused; the
// zone's record of us (sub_764850: the model, the twelve base stats, the equipment — one per player, sub_800510); then the
// base stats (mutation_formulas), the recompute (vtbl+160(1)), S2C 784, StateType 4 / 64 / 8.
bool GameClientSession::mutation_start(ActiveEs& e, const EsaRow* act)
{
    protect(3000);
    if (!(state_type_ & 1)) {
        if (e.esaction == 1248) LOG_INFO("ESAWildChange::InitMutation() -- Already Mutation state Name : %s", char_name_);
        else                    LOG_INFO("ESAWildMutation::InitMutation() -- Already Mutation state Name : %s", char_name_);
        return false;
    }
    if (mut_record_) return false;
    for (int k = 0; k < 12; ++k) mut_saved_[k] = base_stat(k);
    mut_record_ = true;
    int32_t b[12];
    for (int k = 0; k < 12; ++k) b[k] = mut_saved_[k];
    mutation_formulas(e.esaction, act, b);
    for (int k = 0; k < 12; ++k) { base_fixed_val_[k] = b[k]; base_fixed_pts_[k] = accpts_[k]; }
    base_fixed_ = true;
    recompute_stats(true);
    send_base_stats(e.esaction == 1248);
    state_type_ = mutation_bit(e.esaction);
    LOG_DEBUG("GameClientSession %u: %s — AP %d DX %d DP %d HV %d LK %d (base)", session_id(), mutation_name(e.esaction),
              b[0], b[2], b[10], b[11], b[8]);
    return true;
}

// EndMutation / EndBeast / EndChange (sub_574DB0 / sub_5BB3D0 / sub_575A10): the protection 3000 ms; our StateType without
// the bit 4 / 64 / 8 -> "... Already Normal state", refused; the record (sub_800440) — none: refused —, its base stats back,
// the recompute, S2C 784, the record dropped (sub_800570), StateType 1, HP = (int)(HP * HpRemainRatio) and S2C 0x0024.
// (Refused, the caller cuts the player: sub_8AA290.)
bool GameClientSession::mutation_end(const ActiveEs& e)
{
    protect(3000);
    if (!(state_type_ & mutation_bit(e.esaction))) {
        if (e.esaction == 1494)      LOG_INFO("ESABeastDo::EndBeast() -- Already Normal state Name : %s", char_name_);
        else if (e.esaction == 1248) LOG_INFO("ESAWildChange::EndMutation() -- Already Normal state Name : %s", char_name_);
        else                         LOG_INFO("ESAWildMutation::EndMutation() -- Already Normal state Name : %s", char_name_);
        return false;
    }
    if (!mut_record_) return false;
    for (int k = 0; k < 12; ++k) { base_fixed_val_[k] = mut_saved_[k]; base_fixed_pts_[k] = accpts_[k]; }
    base_fixed_ = true;
    recompute_stats(true);
    send_base_stats(e.esaction == 1248);
    mut_record_ = false;
    state_type_ = 1;
    hp_ = static_cast<int32_t>(static_cast<double>(hp_) * static_cast<double>(e.ratio));
    PacketBuffer p(static_cast<Opcode>(0x0024), is_paula());
    p.write<uint32_t>(static_cast<uint32_t>(hp_));
    send(p);
    LOG_DEBUG("GameClientSession %u: %s ends — HP %d", session_id(), mutation_name(e.esaction), hp_);
    return true;
}

// sub_7164C0 (the base stats from the table — the points changed) then the limiter's vtbl+76 (sub_5E1A10): each ES's
// vtbl+108; a mutation's (UpdatePlayerBaseStat sub_574980 / sub_5BB070 / sub_5755E0), its caster here: with its StateType
// bit, the record dropped (sub_800570 — none: "Shell::DelCSBackUpData - already data delete m_id") and taken again of the
// new base (sub_764850), its formulas on it; without, a log. No S2C 784 (the level-up's own packets follow).
void GameClientSession::mutation_rebase()
{
    for (const ActiveEs& e : es_) {
        if (e.kind != EsKind::Mutation || e.orphan) continue;
        if (!(state_type_ & mutation_bit(e.esaction))) {
            if (e.esaction == 1494)
                LOG_INFO("ESBeastDo::UpdatePlayerBaseStat() -- Already Beast state Name : %s", char_name_);
            else
                LOG_INFO("ESAWildMutation::UpdatePlayerBaseStat() -- Already Mutation state Name : %s", char_name_);
            continue;
        }
        if (!mut_record_) LOG_INFO("Shell::DelCSBackUpData - already data delete m_id : %u", object_id_);
        for (int k = 0; k < 12; ++k) mut_saved_[k] = server_.stat_table().value(accpts_[k] / 4 + 1, k);
        mut_record_ = true;
        const SkillDef* sd = server_.skill_db().get(e.skill);
        const EsaRow* act = sd ? server_.esa_db().row(sd->esa_table, e.level) : nullptr;
        int32_t b[12];
        for (int k = 0; k < 12; ++k) b[k] = mut_saved_[k];
        mutation_formulas(e.esaction, act, b);
        for (int k = 0; k < 12; ++k) { base_fixed_val_[k] = b[k]; base_fixed_pts_[k] = accpts_[k]; }
        base_fixed_ = true;
    }
}

// ESASharpClaw's bleed (sub_56A1E0, every 2000 ms): us and the target here, +212 > 0: the roll sub_510060(ApplyRatio, 0)
// — applied: DamageInfo {+212 on a Type-13 monster, else (int)(+212 * ConRatio), death type 2, result 0, element 0}, its
// EXP share, 30002 from us, EXP 30003 kind 0 when we may attack it (vtbl+164).
void GameClientSession::sharp_claw_bleed(ActiveEs& e)
{
    MonsterEntity* m = e.target ? server_.entity_mgr().get_monster(e.target) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive || m->hp <= 0 || e.value <= 0) return;
    if (apply_roll(static_cast<float>(e.hp_ratio), 0) != 1) return;
    const MobDef* md = server_.mob_db().get(static_cast<int>(m->mob_id));
    SkillDamage h;
    h.dmg        = (md && md->type == 13) ? e.value
                                          : static_cast<int32_t>(static_cast<double>(e.value) * static_cast<double>(e.base_ratio));
    h.death_type = 2;
    h.result     = 0;
    h.element    = 0;
    const int32_t hp_before = m->hp;
    std::vector<PacketBuffer> group;
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*m, h.dmg, hp_before, killed, 0, !may_attack_owned(*m), std::chrono::steady_clock::now(), 0);
}
