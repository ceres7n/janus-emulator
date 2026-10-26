#include "game_session.hpp"
#include "game_server.hpp"
#include "logger.hpp"
#include "combat_util.hpp"
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
//  The power-type skills (Bunny / Buffalo line: Boxer, Warrior and their 3rd jobs) with a handler of their
//  own in the ESAction switch sub_5BE9A0 — 1:1 with the retail GameServer (GameServerRelease), the client's
//  classes verified in Trickster.bin (each class's vtbl+132 reads the opener's tail).
// ─────────────────────────────────────────────────────────────────────────────

// ── ESAction 1167 "Uppercut" (GameServer sub_5C8D90, ESAUpperCut) ───────────────────────────────────
//  The selected target (zone vtbl+16 kind 3); our mark (vtbl+132). AP' = (int)(AP * Ratio) (sub_560500,
//  ApplyRatio, element 0). A hit that is not a miss / block (1 / 2 / 6 / 8) with the learned Counter Punch
//  (1106, in the PS limiter) adds a second roll (sub_5C9140): sub_560500 with its learned row's Ratio and
//  the apply (AC - 39) * 0.01 — its damage, when above 0, joins the first and the flag goes to 1. The EXP
//  share; the damage event 30002 BEFORE the block (its packets unblocked — retail order); the block {the
//  opener sub_5EE050 + [u16 target][pos us][pos target][u8 the flag] (sub_5C90F0; VERIFIED: the client's
//  ESAUpperCut sub_4D7240 reads them)}, EXP 30003 kind 0; the skill's TM per hit; the 30107 follow-up.
void GameClientSession::esa_uppercut(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    mark_monster_owner(*m);
    const float ratio       = act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f;
    const float apply_ratio = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    SkillDamage h = skill_phys_damage(*m, static_cast<int>(static_cast<double>(st_ap_) * ratio), apply_ratio, 0);
    uint8_t counter = 0;
    if (h.result != 1 && h.result != 2 && h.result != 6 && h.result != 8) {
        const LearnedSkill* cp = find_skill(1106);
        const SkillDef* cd = server_.skill_db().get(1106);
        const EsaRow* cr = (cp && cp->level > 0 && cd) ? server_.esa_db().row(cd->esa_table, cp->level) : nullptr;
        if (cr) {
            const float c_ratio = static_cast<float>(cr->get("Ratio", 0.0));
            const float c_apply = static_cast<float>(static_cast<double>(st_ac_ - 39) * 0.009999999776482582);
            const SkillDamage c = skill_phys_damage(*m, static_cast<int>(static_cast<double>(st_ap_) * c_ratio),
                                                    c_apply, 0);
            if (c.absorbed > 0) absorb_heal(*m, c.absorbed);
            if (c.dmg > 0) { h.dmg += c.dmg; counter = 1; }
        }
    }
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
    o.write<uint8_t>(counter);
    group.push_back(std::move(o));
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: Uppercut -> mob %u dmg=%d (counter %u) result=%d hp=%d/%d%s", session_id(), tid,
              h.dmg, counter, h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "");
    skill_follow_after(tid);
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, false, now);
}

// ── The multi-hit strikes: 1195 "Quad Punch" (sub_5CB400, ESAChainPunch) / 1327 "Tetra Punch" (sub_5D8630,
//    ESAComboHit) ──────────────────────────────────────────────────────────────────────────────────────
//  Both clear our +0x148 flag 2 first. AP' = (int)(AP * Ratio); four rolls of sub_5E6040 (ApplyRatio,
//  element 0) against the target's DP (a guard ES 1492 on it would replace it — monsters have none here);
//  Tetra Punch: a landing roll after another one deals twice that one's damage (sub_5D8910: a miss breaks the
//  chain). Each kept as a record (sub_5205F0: a miss / block or no damage {0, result}; else its damage
//  counted, the guard ESs 1333 / 1492 / 1309 / 1463 would soak it — none on monsters). The block: the opener
//  sub_5EE050 + [u16 target][pos us][pos target] + 4 x {u32 damage, u32 result} — the client's
//  ESAChainPunch sub_4E0970 / ESAComboHit sub_501830 read 8 bytes a record (the recovered server build also
//  writes a u16 -1 after each; we follow the client); then ONE damage event (sub_5CB7C0 / sub_5D8A10): the
//  EXP share and 30003 kind 0 FIRST, then 30002 with DamageInfo {sum, 2, 0, 0, 0x8000, 0, +24 = the landed
//  hits}. Then the 30107 follow-up and the skill's TM per hit.
void GameClientSession::esa_chain_punch(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now,
                                        bool doubling)
{
    su_.resume = false;                                       // sub_5101D0(+0x148, 2)
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    const float ratio       = act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f;
    const float apply_ratio = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) * ratio);
    struct Rec { int32_t dmg; int32_t result; } rec[4];
    int32_t total = 0;
    int hits = 0;
    int32_t prev = 0;
    for (int i = 0; i < 4; ++i) {
        SkillDamage s = skill_phys_damage(*m, ap2, apply_ratio, 0);
        if (s.absorbed > 0) absorb_heal(*m, s.absorbed);
        if (doubling) {
            if (s.dmg > 0 && prev) s.dmg = 2 * prev;
            prev = s.dmg;
        }
        if (s.dmg <= 0 || s.result == 1 || s.result == 2) {
            rec[i] = { 0, s.result };
        } else {
            total += s.dmg;
            ++hits;
            rec[i] = { s.dmg, s.result };
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
    gain_attack_exp(*m, total, hp_before);                    // 30003 kind 0 before 30002

    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    m->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    o.write<uint16_t>(static_cast<uint16_t>(ax)); o.write<uint16_t>(static_cast<uint16_t>(ay));
    o.write<uint16_t>(static_cast<uint16_t>(tx)); o.write<uint16_t>(static_cast<uint16_t>(ty));
    for (const Rec& r : rec) {
        o.write<int32_t>(r.dmg);
        o.write<int32_t>(r.result);
    }
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: %s -> mob %u ap'=%d hits %d/%d/%d/%d = %d hp=%d/%d%s", session_id(),
              doubling ? "Tetra Punch" : "Quad Punch", tid, ap2, rec[0].dmg, rec[1].dmg, rec[2].dmg, rec[3].dmg, total,
              m->hp, m->hp_max, killed ? " [KILLED]" : "");
    skill_follow_after(tid);
    skill_after_hit(*m, total, hp_before, killed, su_.skill, true, now);
}


// ── ESAction 1145 "Lacerator" (ESASharpBrandish, init sub_55FA10; ESAUI 1) ───────────────────────────
//  A generic ES on the caster (ESClass -1, made at once) whose init does the cut: +196 = Ratio, +200 =
//  ApplyRatio, +216 = Radius (the row: Ratio +4, Radius +8 i16); the selected target (zone vtbl+16 kind 3) we
//  may attack (sub_5E6D70) — else no object; the owner test (vtbl+164) is not modelled. The tail: [u8 the
//  apply result][u16 target][pos us][pos target] (sub_5605D0) [u16 n][u16 id]*n — applied: the zone's monsters
//  in list order (not dead, our layer, not Type 8 / 12, a shadow one only with the Third Eye) within Radius of
//  the target (sqrt), at most 7: the target itself, and any other whose direction from the target is within
//  +-10 of ours to the target (sub_893D30) (sub_55FDD0; players only in PvP). The block: the opener sub_5EE050
//  with the object's id + the tail (VERIFIED: the client's ESASharpBrandish sub_4D5D30 reads them), then each
//  of the n (sub_560310): AP' = (int)(AP * Ratio) against it (sub_560500, ApplyRatio, element 0), its EXP
//  share, the damage event, EXP 30003 kind 1. Then the 30107 follow-up and the skill's TM per hit on the
//  target. The object lives 3000 ms (sub_560550), less without its caster or target.
void GameClientSession::esa_sharp_brandish(const SkillDef& d, uint8_t lv, const EsaRow* act)
{
    const auto now = std::chrono::steady_clock::now();
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive || t->hp <= 0 || !area_target_ok(*t)) return;
    const float ratio  = act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f;
    const float apply  = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    const int16_t radius = static_cast<int16_t>(act ? static_cast<int>(act->get("Radius", 0.0)) : 0);
    // InitESAction's apply roll (sub_510060) on the row's ApplyRatio (+0)
    int result = 1;
    if (apply != 0.0f) result = (rnd(100) < clampi(static_cast<int>(static_cast<double>(apply) * 100.0), 0, 100)) ? 1 : 2;
    const uint16_t id = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());

    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    t->motion.pos(now, tx, ty);
    std::vector<uint16_t> ids;
    if (result == 1) {
        for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
            MonsterEntity* m = server_.entity_mgr().get_monster(oid);
            if (!m || !m->alive || m->hp <= 0 || !area_target_ok(*m)) continue;
            int32_t mx, my;
            m->motion.pos(now, mx, my);
            if (!(static_cast<double>(dist_f(mx, my, tx, ty)) <= static_cast<double>(radius))) continue;
            if (ids.size() >= 7) break;
            if (oid == tid ||
                server_in_line(static_cast<int16_t>(ax), static_cast<int16_t>(ay), static_cast<int16_t>(tx),
                               static_cast<int16_t>(ty), static_cast<int16_t>(mx), static_cast<int16_t>(my), 10.0f))
                ids.push_back(static_cast<uint16_t>(oid));
        }
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_), id);
    o.write<uint8_t>(static_cast<uint8_t>(result));
    o.write<uint16_t>(tid);
    o.write<uint16_t>(static_cast<uint16_t>(ax)); o.write<uint16_t>(static_cast<uint16_t>(ay));
    o.write<uint16_t>(static_cast<uint16_t>(tx)); o.write<uint16_t>(static_cast<uint16_t>(ty));
    o.write<uint16_t>(static_cast<uint16_t>(ids.size()));
    for (uint16_t v : ids) o.write<uint16_t>(v);
    group.push_back(std::move(o));
    struct Hit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };
    std::vector<Hit> hits;
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) * ratio);
    for (uint16_t v : ids) {
        MonsterEntity* m = server_.entity_mgr().get_monster(v);
        if (!m || !m->alive) continue;
        const SkillDamage h = skill_phys_damage(*m, ap2, apply, 0);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        hits.push_back({ m, h.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : hits) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now, 1);
    skill_follow_after(tid);
    if (MonsterEntity* tt = server_.entity_mgr().get_monster(tid)) skill_hit_tm(*tt, su_.skill);
    LOG_DEBUG("GameClientSession %u: Lacerator at mob %u (result %d, radius %d): %zu hit", session_id(), tid, result,
              radius, ids.size());
    es_push_timer(d, lv, id, 3000);
}

// ── ESAction 1469 "Clinch Release" (GameServer sub_5DDCD0, ESAStunCancel) ───────────────────────────
//  rand_between(1, 100) <= ApplyRatio * 100 (sub_420AB0): each of the row's 16 ESActions (ESAction0..15,
//  1000 = none) that our ES limiter holds (vtbl+80) ends (vtbl+60); then the block {the opener sub_5EE050,
//  no tail — the client's ESAStunCancel sub_51D060 reads none} and the use TM. A failed roll: nothing.
void GameClientSession::esa_stun_cancel(const SkillDef& d, const EsaRow* act)
{
    const float apply = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    const int roll = 1 + rnd(99);                            // sub_420AB0(1, 100) = 1 + rand() % (100 - 1)
    if (!(static_cast<double>(apply) * 100.0 >= static_cast<double>(roll))) return;
    for (int i = 0; i < 16; ++i) {
        char col[16];
        std::snprintf(col, sizeof(col), "ESAction%d", i);
        const int esa = act ? static_cast<int>(act->get(col, 1000.0)) : 1000;
        if (esa != 1000 && es_find(esa)) es_remove_esaction(esa);
    }
    std::vector<PacketBuffer> group;
    group.push_back(skill_opener(d, static_cast<uint16_t>(object_id_)));
    server_.broadcast_block(zone_id_, group, 0);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Clinch Release (roll %d / %.0f)", session_id(), roll, apply * 100.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  The Warrior's sword strikes (Buffalo, 2nd job). Each takes its targets through sub_53E360 (area_monsters)
//  or a walk of its own, sends the opener sub_5EE050 ([me][me][0]...) with their ids, rolls the physical
//  skill damage sub_5E6040 (element 0) against each one's DP and gives the skill's use TM (sub_891DC0). The
//  element strikes add the caster's element value of their kind (Character +836 + 2i, elem_slot) to the Ratio.
//  The rows (loaders sub_51BAA0 / sub_51C210 / sub_51DA40): ApplyRatio +0, Ratio +4, then the table's own.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
float row_f(const EsaRow* r, const char* k) { return r ? static_cast<float>(r->get(k, 0.0)) : 0.0f; }
int   row_i(const EsaRow* r, const char* k) { return r ? static_cast<int>(r->get(k, 0.0)) : 0; }
} // namespace

// ── ESAction 1218 "Blazing Strike" (GameServer sub_5CEB50, ESAFastSword; ESAUI 1) ───────────────────
//  The hits on each target (the handler's +500) = clamp((10 - DX) / 6 + 1, 0, 10). The selected target (zone
//  vtbl+16 kind 3) — none: nothing. The row ESAction_FastSword: Radius +8 (i16), KillNum +12 (i16). sub_53E360
//  around the TARGET, at most KillNum (2..5 — the array holds 5), Radius; the target not among them takes the
//  first entry (the count stays: none found, none hit). Each (sub_5CEF30): AP' = (int)(AP * Ratio) against its
//  DP (a guard ES 1492 on it would replace it — none on monsters), the hits rolled by sub_5E6040 (ApplyRatio)
//  and kept as records (sub_5205F0: a miss / block or no damage {0, result, -1}; else {damage, result, -1}
//  counted — the guard ESs 1333 / 1492 / 1309 / 1463 would soak it, none on monsters). The block: the opener
//  + [u8 n], per target [u16 id][u8 hits]{u32 damage, u32 result, u16}*hits[u8 the landed hits] (sub_5CEE10;
//  VERIFIED: the client's ESAFastSword sub_4E4CB0 reads all of it, 10 bytes a record), then per target
//  (sub_5CF080) its EXP share and 30003 kind 0 FIRST, then 30002 with DamageInfo {sum, 2, 0, 0, 0x8000, 0,
//  +24 = the landed hits}. Then the skill's use TM. No 30107.
void GameClientSession::esa_fast_sword(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const int hits = clampi((10 - st_dx_) / 6 + 1, 0, 10);
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    const float   ratio    = row_f(act, "Ratio");
    const float   apply    = row_f(act, "ApplyRatio");
    const int16_t radius   = static_cast<int16_t>(row_i(act, "Radius"));
    const int16_t kill_num = static_cast<int16_t>(row_i(act, "KillNum"));
    int32_t tx, ty;
    t->motion.pos(now, tx, ty);
    std::vector<MonsterEntity*> arr = area_monsters(tx, ty, radius, std::min<int>(kill_num, 5), now);
    bool found = false;
    for (MonsterEntity* m : arr) if (m->object_id == tid) found = true;
    if (!found && !arr.empty()) arr[0] = t;

    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) * ratio);
    struct Rec { int32_t dmg, result; };
    struct Tgt { MonsterEntity* m; std::vector<Rec> rec; int32_t total; int landed; };
    std::vector<Tgt> tg;
    for (MonsterEntity* m : arr) {
        Tgt x{ m, {}, 0, 0 };
        for (int j = 0; j < hits; ++j) {
            const SkillDamage s = skill_phys_damage(*m, ap2, apply, 0);
            if (s.absorbed > 0) absorb_heal(*m, s.absorbed);
            if (s.dmg <= 0 || s.result == 1 || s.result == 2) {
                x.rec.push_back({ 0, s.result });
            } else {
                x.total += s.dmg;
                ++x.landed;
                x.rec.push_back({ s.dmg, s.result });
            }
        }
        tg.push_back(std::move(x));
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint8_t>(static_cast<uint8_t>(tg.size()));
    for (const Tgt& x : tg) {
        o.write<uint16_t>(static_cast<uint16_t>(x.m->object_id));
        o.write<uint8_t>(static_cast<uint8_t>(hits));
        for (const Rec& r : x.rec) {
            o.write<int32_t>(r.dmg);
            o.write<int32_t>(r.result);
            o.write<int16_t>(-1);
        }
        o.write<uint8_t>(static_cast<uint8_t>(x.landed));
    }
    group.push_back(std::move(o));
    struct Hit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };
    std::vector<Hit> done;
    for (const Tgt& x : tg) {
        SkillDamage h;
        h.dmg        = x.total;
        h.death_type = 2;
        h.result     = 0;
        h.element    = 0;
        h.flags      = 0x8000;
        h.extra      = x.landed;
        const int32_t hp_before = x.m->hp;
        gain_attack_exp(*x.m, x.total, hp_before);            // 30003 kind 0 before 30002
        const bool killed = skill_damage_event(*x.m, h, group);
        done.push_back({ x.m, x.total, hp_before, killed });
        LOG_DEBUG("GameClientSession %u: Blazing Strike -> mob %u %d hits, %d landed = %d hp=%d/%d%s", session_id(),
                  static_cast<unsigned>(x.m->object_id), hits, x.landed, x.total, x.m->hp, x.m->hp_max,
                  killed ? " [KILLED]" : "");
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, true, now);
    skill_use_tm(su_.skill);
}

// ── ESAction 1244 "Tidal Slash" (GameServer sub_5D0180, ESAPowerBrandish; ESAUI 1) ───────────────────
//  The selected target (kind 3) — none: nothing. The row ESAction_PowerBrandish: Radius +8 (i16), Angle +12
//  (float), MonNum +16 (unused). The targets (sub_5D0360): the target first, then the zone's monsters in list
//  order — not dead, not the target, our layer, not Type 8 / 12, a shadow one only with the Third Eye —
//  within Radius of the TARGET (sqrtf); such a one once 5 are taken stops the walk; one whose direction from
//  the target is within +-Angle of ours to it (sub_893D30) and that has no owner or is ours (our party's — no
//  party here) is taken (players only in PvP, sub_5D0670). The block: the opener + [u16 target][u16 n][u16
//  id]*n, the target first (VERIFIED: the client's ESAPowerBrandish sub_4E6980 reads them); each one
//  (sub_5D0850, as a monster, else a player): AP' = (int)(AP * (Water * 0.025 + Ratio)) (element slot 1,
//  +838) against its DP (sub_5E6040, ApplyRatio), its EXP share, the damage event, then EXP 30003 kind 0.
//  Then the skill's use TM. No 30107.
void GameClientSession::esa_power_brandish(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    const float   ratio  = row_f(act, "Ratio");
    const float   apply  = row_f(act, "ApplyRatio");
    const int16_t radius = static_cast<int16_t>(row_i(act, "Radius"));
    const float   angle  = row_f(act, "Angle");
    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    t->motion.pos(now, tx, ty);
    std::vector<uint16_t> ids{ tid };
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0 || oid == tid || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (!(static_cast<double>(dist_f(mx, my, tx, ty)) <= static_cast<double>(radius))) continue;
        if (ids.size() >= 5) break;
        if (!server_in_line(static_cast<int16_t>(ax), static_cast<int16_t>(ay), static_cast<int16_t>(tx),
                            static_cast<int16_t>(ty), static_cast<int16_t>(mx), static_cast<int16_t>(my), angle))
            continue;
        if (owned_by_other(*m)) continue;
        ids.push_back(static_cast<uint16_t>(oid));
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    o.write<uint16_t>(static_cast<uint16_t>(ids.size()));
    for (uint16_t v : ids) o.write<uint16_t>(v);
    group.push_back(std::move(o));
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) *
                                     (static_cast<double>(elem_slot(1)) * 0.02500000037252903 + ratio));
    struct Hit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };
    std::vector<Hit> done;
    for (uint16_t v : ids) {
        MonsterEntity* m = server_.entity_mgr().get_monster(v);
        if (!m) continue;
        const SkillDamage h = skill_phys_damage(*m, ap2, apply, 0);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Tidal Slash at mob %u (radius %d, angle %.0f, AP' %d): %zu hit", session_id(), tid,
              radius, angle, ap2, ids.size());
}

// The strikes around the caster or a list of targets that share one shape (sub_5D6DB0 / sub_5D8350 /
// sub_5DC4E0 / sub_5DDFE0): each target the damage sub_5E6040 (AP', ApplyRatio, element 0) against its DP,
// its EXP share, the damage event 30002 into the block, then EXP 30003 kind 0 (after the block here).
namespace {
struct AreaHit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };
} // namespace

// ── ESAction 1310 "Inferno Blade" (GameServer sub_5D6C40, ESABlastBrandish; no selection) ────────────
//  The row ESAction_BlastBrandish: Radius +8 (i16), KillNum +12 (0 — unused). sub_53E360 around the caster,
//  at most 5, Radius. The block: the opener + [u16 n][u16 id]*n (VERIFIED: the client's ESABlastBrandish
//  0x4FABE0 reads them); each (sub_5D6DB0): AP' = (int)(AP * (Fire * 0.01 + Ratio)) (element slot 0, +836).
//  Then the skill's use TM.
void GameClientSession::esa_blast_brandish(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const float   ratio  = row_f(act, "Ratio");
    const float   apply  = row_f(act, "ApplyRatio");
    const int16_t radius = static_cast<int16_t>(row_i(act, "Radius"));
    int32_t ax, ay;
    cur_pos(now, ax, ay);
    const std::vector<MonsterEntity*> arr = area_monsters(ax, ay, radius, 5, now);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(static_cast<uint16_t>(arr.size()));
    for (MonsterEntity* m : arr) o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
    group.push_back(std::move(o));
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) *
                                     (static_cast<double>(elem_slot(0)) * 0.009999999776482582 + ratio));
    std::vector<AreaHit> done;
    for (MonsterEntity* m : arr) {
        const SkillDamage h = skill_phys_damage(*m, ap2, apply, 0);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const AreaHit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Inferno Blade (radius %d, AP' %d): %zu hit", session_id(), radius, ap2, arr.size());
}

// ── ESAction 1325 "Tempest Strike" (GameServer sub_5D8100, ESAWindBrandish; ESAUI 1) / 1336 "Gale Strike"
//    (sub_5D9BF0, ESAWindCircleBrandish, the Gladiator's) ──────────────────────────────────────────────
//  The selected target (kind 3) — none: nothing. The rows (ESAction_WindBrandish / _WindCircleBrandish,
//  sub_51BAA0): Radius +8 (i16), KillNum +12 (i16). sub_53E360 around the CASTER, at most min(KillNum, 5 / 7),
//  Radius; the target not among them takes the first entry (none found: it is the one). The block: the opener
//  + [u16 n][u16 id]*n (VERIFIED: the client's ESAWindBrandish 0x500BB0 / ESAWindCircleBrandish 0x504DA0 read
//  them); then sub_5D8350 / sub_5D9E40: v = (float) the caster's Wind (element slot 2, +840) — above 0 it
//  becomes (rand() % Wind) / 100.0; AP' = (int)(AP * (float)(v * 5.0 / 7.0 + Ratio)); each target as above;
//  then the 30107 follow-up on the FIRST entry (the flag 2 clear, not dead; no gun test). Then the skill's use TM.
void GameClientSession::esa_wind_brandish(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now,
                                          int cap, double wind_mult)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    const float   ratio    = row_f(act, "Ratio");
    const float   apply    = row_f(act, "ApplyRatio");
    const int16_t radius   = static_cast<int16_t>(row_i(act, "Radius"));
    const int16_t kill_num = static_cast<int16_t>(row_i(act, "KillNum"));
    int32_t ax, ay;
    cur_pos(now, ax, ay);
    std::vector<MonsterEntity*> arr = area_monsters(ax, ay, radius, std::min<int>(kill_num, cap), now);
    bool found = false;
    for (MonsterEntity* m : arr) if (m->object_id == tid) { found = true; break; }
    if (!found) {
        if (arr.empty()) arr.push_back(t);
        else arr[0] = t;
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(static_cast<uint16_t>(arr.size()));
    for (MonsterEntity* m : arr) o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
    group.push_back(std::move(o));
    const int16_t wind = elem_slot(2);
    float v = static_cast<float>(wind);
    if (v > 0.0f) v = static_cast<float>(static_cast<double>(rnd(wind)) / 100.0);
    const float k = static_cast<float>(static_cast<double>(v) * wind_mult + ratio);
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) * k);
    std::vector<AreaHit> done;
    for (MonsterEntity* m : arr) {
        const SkillDamage h = skill_phys_damage(*m, ap2, apply, 0);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
    }
    skill_follow_after(static_cast<uint16_t>(arr[0]->object_id));   // 30107 (sub_5D8350) on the first entry
    server_.broadcast_block(zone_id_, group, 0);
    for (const AreaHit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: %s at mob %u (radius %d, AP' %d): %zu hit", session_id(),
              cap == 5 ? "Tempest Strike" : "Gale Strike", tid, radius, ap2, arr.size());
}

// ── ESAction 1352 "Earthquake Blade" (GameServer sub_5DC340; no selection) ───────────────────────────
//  The row ESAction_EarthquakeSword: Radius +8 (i16), KillNum +12 (i16). sub_53E360 around the caster, at
//  most 7, Radius; then at most KillNum of them. The block: the opener + [u16 n][u16 id]*n — the client's
//  factory sub_55CD40 has no case of its own for 1352: its patch section (.myown 0x127275B) sends it to
//  ESAEarthquakeSword (the class of 1495) with dword_12723A8 = 0, whose read (0x510150 -> 0x510155) takes
//  [u16 n]{u16} (VERIFIED). Each (sub_5DC4E0): AP' = (int)(AP * (Ratio + Earth * 0.01 + Earth * 0.01))
//  (element slot 3, +842). Then the skill's use TM.
void GameClientSession::esa_earthquake_sword(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const float   ratio    = row_f(act, "Ratio");
    const float   apply    = row_f(act, "ApplyRatio");
    const int16_t radius   = static_cast<int16_t>(row_i(act, "Radius"));
    const int16_t kill_num = static_cast<int16_t>(row_i(act, "KillNum"));
    int32_t ax, ay;
    cur_pos(now, ax, ay);
    std::vector<MonsterEntity*> arr = area_monsters(ax, ay, radius, 7, now);
    if (static_cast<int>(arr.size()) > kill_num) arr.resize(kill_num > 0 ? static_cast<size_t>(kill_num) : 0);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(static_cast<uint16_t>(arr.size()));
    for (MonsterEntity* m : arr) o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
    group.push_back(std::move(o));
    const double earth = static_cast<double>(elem_slot(3));
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) * (ratio + earth * 0.01 + earth * 0.01));
    std::vector<AreaHit> done;
    for (MonsterEntity* m : arr) {
        const SkillDamage h = skill_phys_damage(*m, ap2, apply, 0);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const AreaHit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Earthquake Blade (radius %d, AP' %d): %zu hit", session_id(), radius, ap2,
              arr.size());
}

// ── ESAction 1470 "Lightning Strike" (GameServer sub_5DDE00, ESAElectricSword; no selection) ─────────
//  The row ESAction_ElectricSword (sub_51DA40): KillNum +8 (int), StunTime +12 (int), StunRatio +16 (float),
//  Radius +24 (int). sub_53E360 around the caster, at most KillNum (2..6 — the array holds 6), Radius. The
//  block: the opener + [u8 n][u16 id]*n (sub_5DDF70; VERIFIED: the client's ESAElectricSword 0x51D1A0 reads
//  them); each (sub_5DDFE0): AP' = (int)(AP * (Elec * 0.01 + Elec * 0.01 + Ratio)) (element slot 4, +844).
//  After the block (sub_5DE1A0) each target without a Prevent Stun ES (1398) is stunned on rand_between(1,
//  100) <= (int)(StunRatio * 100) (es_electric_stun); then the skill's use TM.
void GameClientSession::esa_electric_sword(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const float ratio      = row_f(act, "Ratio");
    const float apply      = row_f(act, "ApplyRatio");
    const int   radius     = row_i(act, "Radius");
    const int   kill_num   = row_i(act, "KillNum");
    const float stun_ratio = row_f(act, "StunRatio");
    int32_t ax, ay;
    cur_pos(now, ax, ay);
    const std::vector<MonsterEntity*> arr = area_monsters(ax, ay, radius, std::min(kill_num, 6), now);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint8_t>(static_cast<uint8_t>(arr.size()));
    for (MonsterEntity* m : arr) o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
    group.push_back(std::move(o));
    const double elec = static_cast<double>(elem_slot(4));
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) *
                                     (elec * 0.009999999776482582 + elec * 0.009999999776482582 + ratio));
    std::vector<AreaHit> done;
    for (MonsterEntity* m : arr) {
        const SkillDamage h = skill_phys_damage(*m, ap2, apply, 0);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const AreaHit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now);
    for (MonsterEntity* m : arr) {                                       // sub_5DE1A0
        bool prevent = false;
        for (const MonsterEntity::Es& e : m->es) if (e.esaction == 1398) prevent = true;
        if (prevent) continue;
        const int roll = 1 + rnd(99);                                    // sub_420AB0(1, 100)
        if (roll <= static_cast<int>(static_cast<double>(stun_ratio) * 100.0)) es_electric_stun(*m, d, act);
    }
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Lightning Strike (radius %d, AP' %d): %zu hit", session_id(), radius, ap2,
              arr.size());
}

// ─────────────────────────────────────────────────────────────────────────────
//  The Champion (Bunny, 3rd job). Adrenaline (ESAAdrenaline 1274, AP up) and the dash of Flash Cut / Mega
//  Flash Cut (ESAFlashAttacking / ESAPowerFlashAttacking 1275 / 1367) are effect states (es.cpp).
// ─────────────────────────────────────────────────────────────────────────────

// ── ESAction 1276 (GameServer sub_5D5AA0, ESAFlashAttackingDo): the hit at the end of a Flash Cut dash ──
//  Our condition 15 -> nothing. The target (zone vtbl+16 kind 3) there: the block {S2C 224 sub_5EDE00 [us]
//  [target][0][1276][2][ESClass -1][skill 1302 / 1304][0] + [u8 the roll] (VERIFIED: the client's
//  ESAFlashAttackingDo 0x4F6EA0 reads it); a landed roll: sub_5D5BD0 — AP' = (int)((AP + (Mega ? 16 : 8) * AC)
//  * Ratio), the damage sub_5E6040 with the row's Ratio as its ApplyRatio (retail), element 0, against its DP,
//  its EXP share, 30002, then 30003 kind 0}. (A corpse ignores 30002 — sub_729100 in state 2 — but the EXP of
//  its share 0 still comes: 1.) No TM (the dash's init gave it), no 30107.
void GameClientSession::esa_flash_hit(const ActiveEs& e, bool applied)
{
    if (pcond_test(15)) return;
    MonsterEntity* m = server_.entity_mgr().get_monster(e.target);
    if (!m || m->zone != zone_id_) return;
    const auto now = std::chrono::steady_clock::now();
    const SkillDef* sd = server_.skill_db().get(e.skill);
    const EsaRow* row = sd ? server_.esa_db().row(sd->esa_table, e.level) : nullptr;
    const float ratio = row_f(row, "Ratio");
    std::vector<PacketBuffer> group;
    PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
    o.write<uint16_t>(static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(e.target);
    o.write<uint16_t>(0);
    o.write<uint16_t>(1276);
    o.write<uint8_t>(2);
    o.write<uint8_t>(0xFF);
    o.write<uint16_t>(e.skill);
    o.write<uint16_t>(0);
    o.write<uint8_t>(applied ? 1 : 0);
    group.push_back(std::move(o));
    if (!applied) {
        server_.broadcast_block(zone_id_, group, 0);
        return;
    }
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_ + (e.power ? 16 : 8) * st_ac_) * ratio);
    const SkillDamage h = skill_phys_damage(*m, ap2, ratio, 0);
    if (!m->alive || m->hp <= 0) {
        server_.broadcast_block(zone_id_, group, 0);
        gain_attack_exp(*m, h.dmg, 0);
        return;
    }
    const int32_t hp_before = m->hp;
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*m, h.dmg, hp_before, killed, 0, false, now);
    LOG_DEBUG("GameClientSession %u: %s hits mob %u (AP' %d) dmg=%d result=%d hp=%d/%d%s", session_id(),
              e.power ? "Mega Flash Cut" : "Flash Cut", e.target, ap2, h.dmg, h.result, m->hp, m->hp_max,
              killed ? " [KILLED]" : "");
}

// ── ESAction 1335 "Flaming Fist" (GameServer sub_5DA160, ESAFightingFire; ESAUI 1) ──────────────────
//  The selected target (zone vtbl+16 kind 3) — none: nothing; we mark it (vtbl+132). The row (sub_51A3C0:
//  ApplyRatio, Ratio): AP' = (int)(AP * (float(Fire / 100) + Ratio)) (our element slot 0, +836); with Fire above
//  0 the magic damage sub_5E6270(AP', ApplyRatio, MA offset 0, element 1 = Fire), else the physical sub_5E6040
//  (ApplyRatio, element 0) against its DP. Its EXP share; the block {the opener (to the area of us and the
//  target) + [u16 target][pos us][pos target] (sub_5DA500; VERIFIED: the client's ESAFightingFire 0x506E00 reads
//  them), the damage event, EXP 30003 kind 0}; the 30107 follow-up (the flag 2 clear, the target not dead; no
//  gun test); the skill's TM per hit.
void GameClientSession::esa_fighting_fire(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    mark_monster_owner(*m);
    const float ratio = row_f(act, "Ratio");
    const float apply = row_f(act, "ApplyRatio");
    const int16_t fire = elem_slot(0);
    const float f = static_cast<float>(static_cast<double>(fire) / 100.0);
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) * (static_cast<double>(f) + ratio));
    const SkillDamage h = (fire <= 0) ? skill_phys_damage(*m, ap2, apply, 0) : skill_magic_damage(*m, ap2, apply, 0, 1);
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
    LOG_DEBUG("GameClientSession %u: Flaming Fist -> mob %u (fire %d, AP' %d) dmg=%d result=%d hp=%d/%d%s", session_id(),
              tid, fire, ap2, h.dmg, h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "");
    skill_follow_after(tid);
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, false, now);
}

// ── ESAction 1481 "Chi Release" (GameServer sub_5DF0B0, ESAPhysicalMatch; no selection) ───────────────
//  The row ESAction_PhysicalMatch (sub_51BAA0): Radius +8 (i16), KillNum +12 (i16). sub_53E360 around the
//  caster, at most 6, Radius; then at most KillNum of them. The block: the opener + [u16 n][u16 id]*n (VERIFIED:
//  the client's ESAPhysicalMatch 0x51E940 reads them); each (sub_5DF250): AP' = (int)(AP * Ratio), the damage
//  sub_5E6040 (ApplyRatio, element 0) against its DP, its EXP share, 30002, then 30003 kind 0. Then the skill's
//  use TM.
void GameClientSession::esa_physical_match(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const float   ratio    = row_f(act, "Ratio");
    const float   apply    = row_f(act, "ApplyRatio");
    const int16_t radius   = static_cast<int16_t>(row_i(act, "Radius"));
    const int16_t kill_num = static_cast<int16_t>(row_i(act, "KillNum"));
    int32_t ax, ay;
    cur_pos(now, ax, ay);
    std::vector<MonsterEntity*> arr = area_monsters(ax, ay, radius, 6, now);
    if (static_cast<int>(arr.size()) > kill_num) arr.resize(kill_num > 0 ? static_cast<size_t>(kill_num) : 0);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(static_cast<uint16_t>(arr.size()));
    for (MonsterEntity* m : arr) o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
    group.push_back(std::move(o));
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) * ratio);
    std::vector<AreaHit> done;
    for (MonsterEntity* m : arr) {
        const SkillDamage h = skill_phys_damage(*m, ap2, apply, 0);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const AreaHit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Chi Release (radius %d, AP' %d): %zu hit", session_id(), radius, ap2, arr.size());
}

// ─────────────────────────────────────────────────────────────────────────────
//  The Gladiator (Buffalo, 3rd job). Gale Strike is the Tempest Strike handler with its own cap and Wind
//  factor (esa_wind_brandish); Chi Sword (ESAEnergySword 1368) and Chakra Balance's heal (ESASelfCureDo 1483)
//  are effect states (es.cpp).
// ─────────────────────────────────────────────────────────────────────────────

// ── ESAction 1261 "Sonic Slash" (GameServer sub_5D46D0, ESAPowerSeaBrandish; ESAUI 1) ─────────────────
//  The selected target (kind 3) — none: nothing. The row ESAction_PowerSeaBrandish (sub_51C210): Radius +8
//  (i16), Angle +12 (float), MonNum +16 (unused). The targets: the target first, then (sub_5D48B0 ->
//  sub_5E09D0) the zone's monsters in list order while fewer than 4 are taken, each by sub_5E0AB0 — not dead,
//  our layer, not Type 8 (no Type 12 test here), a shadow one only with the Third Eye, within Radius of the
//  TARGET, its direction from the target within +-Angle of ours to it (sub_893D30 — the target itself, at
//  distance 0, fails it: a NaN angle), no owner or ours (players only in PvP, sub_5E0C70). The block: the
//  opener (to the area of us and the target) + [u16 n][u16 id]*n (VERIFIED: the client's ESAPowerSeaBrandish
//  0x4F4000 reads them); each (sub_5D4980): AP' = (int)(AP * (Water * 0.035 + Ratio)) (element slot 1, +838),
//  the damage sub_5E6040 with the row's Ratio as its ApplyRatio (retail), element 0, against its DP, its EXP
//  share, 30002, then 30003 kind 0. Then the skill's use TM.
void GameClientSession::esa_power_sea_brandish(const SkillDef& d, const EsaRow* act,
                                               std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    const float   ratio  = row_f(act, "Ratio");
    const int16_t radius = static_cast<int16_t>(row_i(act, "Radius"));
    const float   angle  = row_f(act, "Angle");
    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    t->motion.pos(now, tx, ty);
    std::vector<MonsterEntity*> arr{ t };
    int others = 0;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        if (others >= 4) break;
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0) continue;
        if (const MobDef* md = server_.mob_db().get(static_cast<int>(m->mob_id)))
            if (md->type == 8) continue;
        if (m->shadow() && !third_eye_) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (static_cast<double>(radius) < static_cast<double>(dist_f(mx, my, tx, ty))) continue;
        if (!server_in_line(static_cast<int16_t>(ax), static_cast<int16_t>(ay), static_cast<int16_t>(tx),
                            static_cast<int16_t>(ty), static_cast<int16_t>(mx), static_cast<int16_t>(my), angle))
            continue;
        if (owned_by_other(*m)) continue;
        arr.push_back(m);
        ++others;
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(static_cast<uint16_t>(arr.size()));
    for (MonsterEntity* m : arr) o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
    group.push_back(std::move(o));
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) *
                                     (static_cast<double>(elem_slot(1)) * 0.03500000014901161 + ratio));
    std::vector<AreaHit> done;
    for (MonsterEntity* m : arr) {
        const SkillDamage h = skill_phys_damage(*m, ap2, ratio, 0);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const AreaHit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Sonic Slash at mob %u (radius %d, angle %.0f, AP' %d): %zu hit", session_id(), tid,
              radius, angle, ap2, arr.size());
}

// ── ESAction 1262 "Shadow" (GameServer sub_5D3A20, ESAOnesOtherSelf; ESAUI 1) ───────────────────────
//  The selected target (kind 3) — none: nothing; we mark it (vtbl+132). The row ESAction_OnesOtherSelf
//  (sub_51C690: ApplyRatio +0, Ratio +4, Comment +8, Count +10 i16): AP' = (int)((24 * (11 - DX) + AP) * Ratio *
//  Count), the damage sub_5E6040 (ApplyRatio, element 0) against its DP, its EXP share; the block {S2C 224
//  sub_5EDE00 [us][target][0]... + [u16 Count] (VERIFIED: the client's ESAOnesOtherSelf 0x4EFBA0 reads it — the
//  shadows it shows), the damage event, EXP 30003 kind 0}; the skill's TM per hit. No 30107.
void GameClientSession::esa_ones_other_self(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    mark_monster_owner(*m);
    const float   ratio = row_f(act, "Ratio");
    const float   apply = row_f(act, "ApplyRatio");
    const int16_t count = static_cast<int16_t>(row_i(act, "Count"));
    const int ap2 = static_cast<int>(static_cast<double>(24 * (11 - st_dx_) + st_ap_) * ratio *
                                     static_cast<double>(count));
    const SkillDamage h = skill_phys_damage(*m, ap2, apply, 0);
    const int32_t hp_before = m->hp;
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, tid);
    o.write<uint16_t>(static_cast<uint16_t>(count));
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: Shadow -> mob %u (count %d, AP' %d) dmg=%d result=%d hp=%d/%d%s", session_id(), tid,
              count, ap2, h.dmg, h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "");
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, false, now);
}

// ── ESAction 1287 "Suffocate" (GameServer sub_5D44C0, ESAFluorescene; no selection) ────────────────────
//  The row ESAction_Fluorescene (sub_51BF50: ApplyRatio +0, Ratio +4, Second +8 i16, Radius +10 i16). sub_53E360
//  around the caster, at most 10, Radius; each takes an ESAFluoresceneDo (sub_5D4610: {ESAction 1288, EState 0,
//  ESClass 1, flags 2, skill 1503, this row} through sub_524690(.., 1) — es_fluorescene_do); then the block: the
//  opener + [u8 any taken] (VERIFIED: the client's ESAFluorescene 0x4F3940 reads it). No TM of its own.
void GameClientSession::esa_fluorescene(const SkillDef& d, uint8_t lv, const EsaRow* act,
                                        std::chrono::steady_clock::time_point now)
{
    const int16_t radius = static_cast<int16_t>(row_i(act, "Radius"));
    int32_t ax, ay;
    cur_pos(now, ax, ay);
    const std::vector<MonsterEntity*> arr = area_monsters(ax, ay, radius, 10, now);
    for (MonsterEntity* m : arr) es_fluorescene_do(*m, d, act);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint8_t>(arr.empty() ? 0 : 1);
    group.push_back(std::move(o));
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: Suffocate lv %u (radius %d): %zu taken", session_id(), lv, radius, arr.size());
}

// ── ESAction 1482 "Chakra Balance" (GameServer sub_5DF3F0; no selection) ──────────────────────────────
//  In a block, the action {ESAction 1483, EState 0, ESClass 0, flags 2, skill 1506, this row} on ourselves
//  through sub_524690(.., 1) — 1506 is an active skill (Type 2): no limiter (two of them heal side by side) —
//  ESASelfCureDo (es.cpp); then the skill's use TM. (The client makes its ESAHeal for the 1482 opener — none
//  is sent; it reads nothing.)
void GameClientSession::esa_self_cure(const SkillDef& d, uint8_t lv, const EsaRow* act)
{
    SkillDef dd = d;
    dd.esaction = 1483;
    dd.estate   = 0;
    dd.esclass  = 0;
    es_create(dd, lv, act, EsKind::SelfCure, false, 2);
    skill_use_tm(su_.skill);
}
