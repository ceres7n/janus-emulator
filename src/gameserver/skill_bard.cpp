#include "game_session.hpp"
#include "game_server.hpp"
#include "logger.hpp"
#include "combat_util.hpp"
#include "paula_crypt.hpp"
#include <algorithm>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
//  The Bard and the Spiritualist (Sheep, 2nd / 3rd job) — 1:1 with the retail GameServer (GameServerRelease),
//  the client's classes verified in Trickster.bin (each class's vtbl+132 reads the opener's tail).
//
//  The elemental attacks add the caster's element value of their kind (Character +836 + 2i, elem_slot: Water 1,
//  Elec 4, ...) to the row's AP. The seals (2102 / 2104 / 2106 / 2108 / 2110) are passives only the skill
//  learning reads (exclusive pairs, sub_760A20); Water Web (2112) and Electroshock (2113) are the passives Drip
//  Bomb and Electro Attack follow up with (passive_follow_es).
// ─────────────────────────────────────────────────────────────────────────────

namespace {
float row_f(const EsaRow* r, const char* k) { return r ? static_cast<float>(r->get(k, 0.0)) : 0.0f; }
int   row_i(const EsaRow* r, const char* k) { return r ? static_cast<int>(r->get(k, 0.0)) : 0; }
} // namespace

// sub_5C7500 (Water Web after Drip Bomb) / sub_55F480 (Electroshock after Electro Attack): the passive learned
// in our PS limiter (sub_8923C0) at a level above 0 — its row at that level (sub_50F540); a monster with a
// DebuffSet first rolls against the passive's DebuffBitCode (sub_891F80; all resisted -> S2C 1247); else the
// action {esaction, EState 0, ESClass 1, flags 2, the passive, that row} through sub_524690(.., 1) — a passive
// (Type 1): the monster's limiter admits it. (The resist +52: 0 for Water Web; the target's resist to 2113 for
// Electroshock — a monster has none.)
void GameClientSession::passive_follow_es(MonsterEntity& m, int passive, int esaction)
{
    const LearnedSkill* ls = find_skill(static_cast<uint16_t>(passive));
    const SkillDef* sd = server_.skill_db().get(passive);
    if (!ls || ls->level <= 0 || !sd) return;
    const EsaRow* row = server_.esa_db().row(sd->esa_table, ls->level);
    uint32_t bits = 0;
    if (monster_debuff_immune(m, sd->debuff_bits, bits)) { send_debuff_resisted(m, bits); return; }
    SkillDef dd = *sd;
    dd.esaction = esaction;
    dd.estate   = 0;
    dd.esclass  = 1;
    es_apply_on(m, dd, ls->level, row);
}

// ── ESAction 1156 "Drip Bomb" (GameServer sub_5C7180, ESAWaterBall) / 1158 "Whirlwind Blaze" (sub_5C76D0,
//    ESAWhirlFlame) / 1257 "Aqua Bomb" (Spiritualist, sub_5D2BA0, ESAWaterShower); ESAUI 1 ───────────────────
//  The selected target (zone vtbl+16 kind 3) — none: nothing; we mark it (vtbl+132). The rows (sub_51B150 /
//  sub_51B1A0 / sub_51C070: ApplyRatio +0, AP +4, Comment +8): the magic damage sub_5E6270(AP + 3 * Water / Fire
//  (element slot 1 / 0, +838 / +836), ApplyRatio, MA offset 49, element 2 = Water / 1 = Fire), its EXP share; the
//  block {the opener (to the area of us and the target) + [u16 target][pos us][pos target] (sub_5C74C0 / sub_5C7930
//  / sub_5D2DF0; VERIFIED: the clients' ESAWaterBall 0x4D19A0 / ESAWaterShower 0x4EE2C0 read them), the damage
//  event, EXP 30003 kind 1}; the skill's TM per hit. Drip Bomb then, on a hit (result not 1 / 2 / 6 / 8) on a
//  target with HP left: Water Web (2112) learned -> its ESAWaterNetDo (1189: DX += our MA * Ratio / 100, 1000 *
//  Seconds; es.cpp). No 30107.
void GameClientSession::esa_water_ball(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    mark_monster_owner(*m);
    const bool water = (d.esaction == 1156 || d.esaction == 1257);
    const int32_t base = row_i(act, "AP") + 3 * elem_slot(water ? 1 : 0);
    const SkillDamage h = skill_magic_damage(*m, base, row_f(act, "ApplyRatio"), 49, water ? 2 : 1);
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
    LOG_DEBUG("GameClientSession %u: %s -> mob %u (base %d) dmg=%d result=%d hp=%d/%d%s", session_id(),
              d.esaction == 1257 ? "Aqua Bomb" : water ? "Drip Bomb" : "Whirlwind Blaze", tid, base, h.dmg, h.result,
              m->hp, m->hp_max, killed ? " [KILLED]" : "");
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, false, now, 1);
    const int r = h.result;
    if (d.esaction == 1156 && r != 2 && r != 1 && r != 6 && r != 8 && m->alive && m->hp > 0)
        passive_follow_es(*m, 2112, 1189);
}

// ── ESAction 1159 "Cleaving Terra" (GameServer sub_5C7970; ESAUI 6: a point) ───────────────────────────
//  No packet of its own (the client has no class for 1159). The row ESAction_EarthDivide (sub_51B1F0: ApplyRatio
//  +0, AP +4, Radius +8 int, Comment +12) and the payload's point; sub_5C7A60 walks the zone's monsters in list
//  order — not dead, our layer, not Type 8 / 12, a shadow one only with the Third Eye — within (float)Radius of the
//  point; such a one once 7 are taken stops the walk; one without an owner or ours (our party's) gets an
//  ESAEarthDivideHit (sub_5C7D20: {ESAction 1160, EState 0, ESClass -1, skill 2108 — the Seal of Earth's id, with
//  SkillParam 2009's +92 / +42: retail —, flags 2, the row} through sub_524690(.., 1): made at once). (PvP
//  players sub_5C7DD0.)
//  ESAEarthDivideHit (init sub_55C760): the row's ApplyRatio, AP' = AP + 3 * our Earth (element slot 3, +842),
//  the target stops (sub_41C5D0) and takes the condition 1, state 0, the block {S2C 224 sub_5EDCD0 [us][target]
//  [ES id][1160][2][ESClass -1][2108][0], no tail}, the TM per hit of 2108 (sub_891C80). Not kept without its apply
//  roll (no +184): its first base tick ends it. Its state machine (sub_55C990): 500 ms, then the hit (sub_55CAF0:
//  the condition 1 off, our mark, the magic damage sub_5E6270(AP', ApplyRatio, MA offset 25, element 8 = Earth),
//  the EXP share, 30002 in a block, then 30003 kind 3 when we may attack it, vtbl+164), state 1; 500 ms later its
//  end (S2C 280, the condition 1 off if still on, our mark with 5000). Us or the target gone: the end.
void GameClientSession::esa_earth_divide(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    int16_t px = 0, py = 0;
    if (su_.payload.size() >= 4) {
        px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
        py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
    }
    const float   radius = static_cast<float>(row_i(act, "Radius"));
    const float   apply  = row_f(act, "ApplyRatio");
    const int32_t ap     = row_i(act, "AP") + 3 * elem_slot(3);
    int n = 0;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0 || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (!(static_cast<double>(radius) >= static_cast<double>(dist_f(mx, my, px, py)))) continue;
        if (n >= 7) break;
        if (owned_by_other(*m)) continue;
        ++n;
        CastEs e;
        e.id       = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());
        e.esaction = 1160;
        e.skill    = 2108;
        e.target   = static_cast<uint16_t>(oid);
        e.ap       = ap;
        e.apply    = apply;
        e.state    = 0;
        e.elapsed  = 0;
        e.released = false;
        e.result   = (apply != 0.0f && rnd(100) >= clampi(static_cast<int>(static_cast<double>(apply) * 100.0), 0, 100))
                         ? 2 : 1;                                        // InitESAction sub_510060 (no resist)
        m->motion.rest(mx, my);                                          // sub_41C5D0 on the target
        m->pos_x = mx; m->pos_y = my;
        m->cond_add(1);
        {
            std::vector<PacketBuffer> group;
            PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());     // sub_5EDCD0
            o.write<uint16_t>(static_cast<uint16_t>(object_id_));
            o.write<uint16_t>(e.target);
            o.write<uint16_t>(e.id);
            o.write<uint16_t>(1160);
            o.write<uint8_t>(2);
            o.write<uint8_t>(0xFF);
            o.write<uint16_t>(2108);
            o.write<uint16_t>(0);
            group.push_back(std::move(o));
            server_.broadcast_block(zone_id_, group, 0);
        }
        cast_es_.push_back(e);
        skill_hit_tm(*m, 2108);                                          // sub_891C80
        LOG_DEBUG("GameClientSession %u: Cleaving Terra hit %u on mob %u (AP' %d, roll %d)", session_id(), e.id, oid,
                  ap, e.result);
    }
    LOG_DEBUG("GameClientSession %u: Cleaving Terra at (%d,%d) radius %.0f: %d targets", session_id(), px, py, radius, n);
    (void)d;
}

// ── ESAction 1184 "Summon Boulder" (GameServer sub_5C9FC0, ESARockSummons; ESAUI 1) ────────────────────
//  The selected target (zone vtbl+16 kind 3) — none: nothing; its position is the center. The row
//  ESAction_RockSummons (sub_51B7C0: ApplyRatio +0, AP +4, Radius +8 i16, Comment +10). The block: the opener (to the
//  area of us and the target) + [u16 target] (VERIFIED: the client's ESARockSummons 0x4D9EA0 reads it), then
//  sub_5CA370 walks the zone's monsters in list order — our layer, not Type 8 / 12, a shadow one only with the
//  Third Eye (no dead test, no owner test: retail) — within (float)Radius of the center; such a one once 7 are hit
//  stops the walk; each: the magic damage sub_5E6270(AP + 3 * Earth (element slot 3, +842), ApplyRatio, MA offset
//  25, element 8 = Earth) scaled (int)(dmg * (1 - d / Radius)), its EXP share, 30002, then 30003 kind 1. No mark,
//  no TM. (A corpse still in the zone takes its roll; its damage event does nothing — sub_729100 in state 2 —
//  and its share 0 still gives 1 EXP.)
void GameClientSession::esa_rock_summons(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_) return;
    int32_t cx, cy;
    t->motion.pos(now, cx, cy);
    const float   radius = static_cast<float>(static_cast<int16_t>(row_i(act, "Radius")));
    const float   apply  = row_f(act, "ApplyRatio");
    const int32_t base   = row_i(act, "AP") + 3 * elem_slot(3);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    group.push_back(std::move(o));
    struct Hit { MonsterEntity* m; int32_t dmg, hp_before; bool killed, corpse; };
    std::vector<Hit> done;
    int n = 0;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        const float dd = dist_f(mx, my, cx, cy);
        if (!(static_cast<double>(radius) >= static_cast<double>(dd))) continue;
        if (n >= 7) break;
        const float f = static_cast<float>(1.0 - static_cast<double>(dd / radius));
        SkillDamage h = skill_magic_damage(*m, base, apply, 25, 8);
        h.dmg = static_cast<int32_t>(static_cast<double>(h.dmg) * f);
        const bool corpse = !m->alive || m->hp <= 0;
        const int32_t hp_before = corpse ? 0 : m->hp;
        const bool killed = corpse ? false : skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed, corpse });
        ++n;
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : done) {
        if (h.corpse) { gain_attack_exp(*h.m, h.dmg, 0, 1); continue; }
        skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now, 1);
    }
    LOG_DEBUG("GameClientSession %u: Summon Boulder at mob %u (radius %.0f, base %d): %d hit", session_id(), tid, radius,
              base, n);
}

// ── ESAction 1185 "Incinerate" (GameServer sub_5CCCD0, ESAFireExplosion; ESAUI 1) ───────────────────────
//  The selected target (kind 3) — none: nothing. The row ESAction_FireExplosion (sub_51B7C0). sub_5CCED0 takes the
//  FIRST monster of the zone's list — not dead, our layer, not Type 8 / 12, a shadow one only with the Third Eye —
//  within Radius of the target, not the target, without an owner or ours (our party's) (PvP players sub_5CD120).
//  The block: the opener (to the area of us and the target) + [u8 found][u16 target][u16 that one, when found]
//  (VERIFIED: the client's ESAFireExplosion 0x4DA770 reads the second only with the flag), then each of the 1 / 2
//  (sub_5CD290): the magic damage sub_5E6270(AP + 3 * Fire (element slot 0, +836), ApplyRatio, MA offset 49,
//  element 1 = Fire), its EXP share, 30002, then 30003 kind 1. No mark, no TM.
void GameClientSession::esa_fire_explosion(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    const int16_t radius = static_cast<int16_t>(row_i(act, "Radius"));
    int32_t tx, ty;
    t->motion.pos(now, tx, ty);
    uint16_t second = 0;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0 || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (!(static_cast<double>(dist_f(mx, my, tx, ty)) <= static_cast<double>(radius)) || oid == tid) continue;
        if (owned_by_other(*m)) continue;
        second = static_cast<uint16_t>(oid);
        break;
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint8_t>(second ? 1 : 0);
    o.write<uint16_t>(tid);
    if (second) o.write<uint16_t>(second);
    group.push_back(std::move(o));
    const int32_t base  = row_i(act, "AP") + 3 * elem_slot(0);
    const float   apply = row_f(act, "ApplyRatio");
    struct Hit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };
    std::vector<Hit> done;
    for (uint16_t v : { tid, second }) {
        if (!v) continue;
        MonsterEntity* m = server_.entity_mgr().get_monster(v);
        if (!m) continue;
        const SkillDamage h = skill_magic_damage(*m, base, apply, 49, 1);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now, 1);
    LOG_DEBUG("GameClientSession %u: Incinerate at mob %u (+%u)", session_id(), tid, second);
}

// The inits of three debuffs on the selected monster that do more than the generic ES (es_apply_on hands them over
// after the limiter's admission and the apply roll; true: the object stays).
//  1315 ESAIceCrystal "Shard of the Glacier" (sub_57E4B0; row sub_51C980: ApplyRatio +0, AP +4, Ratio +8, Comment
//   +12, Seconds +14) / 1355 ESAChillingPrison "Icey Shackles" (sub_584C70; row sub_51CBC0: ApplyRT +0, M_AP +4,
//   Seconds +8): we must be able to attack it (sub_5E6D70); the magic damage sub_5E6270(AP + 3 * Water (element slot
//   1), the row's +0, MA offset 49 - our LK (IceCrystal) / 49, element 2 = Water) and its EXP share; a miss / block /
//   result 8 or no damage -> not applied (+180 = 2); the lifetime 1000 * Seconds; the block {S2C 224 sub_5EDE00 ...
//   + [u8 +180] — the recovered server also writes [u16 the payload's target], the clients' ESAIceCrystal 0x4FEE60 /
//   ESAChillingPrison 0x50A280 read only the u8: the client is followed}; not applied: no lifetime (+188 = 0), its
//   first tick ends it. Applied: our mark; IceCrystal: its ElecR (+924 slot 15) -= (int)min(95, our MA * Ratio / 2)
//   unless its DebuffSet resisted 0x800000 (+192); the block {the damage event, EXP 30003 kind 1}; IceCrystal: it
//   stops and takes the condition 1 unless resisted 2; ChillingPrison: it stops, the condition 15 and its +0x148 flag
//   32 (melee on it misses, sub_5E5700); the skill's TM per hit.
//  1316 ESAElectricField "Tesla Field" (sub_5B6160; row sub_51C130: ApplyRatio +0, AP +4, Radius +8, Seconds +10): a
//   field on the target's spot (sub_7149C0), +236 = 1000 * Seconds, the lifetime, the block {the opener + [u8 +180]
//   [u16 Radius][pos] — and [u32 its time]: the client's ESAElectricField 0x4FBB30 reads one more u32 than the
//   recovered server writes (its +560, the client's own end), so the field's 1000 * Seconds is sent}; the use TM
//   whatever the roll; no mark. Its tick (sub_5B6360): without us or past +236 the end; every 2000 ms its hit
//   (tesla_hit); not applied: the base tick ends it.
bool GameClientSession::bard_debuff_on(MonsterEntity& m, MonsterEntity::Es& e, const SkillDef& d, const EsaRow* act)
{
    const auto now = std::chrono::steady_clock::now();
    const MobDef* md = server_.mob_db().get(static_cast<int>(m.mob_id));
    if (md && (md->type == 8 || md->type == 12)) return false;           // sub_5E6D70
    auto opener = [&](std::vector<uint8_t> tail) {
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
    if (e.esaction == 1316) {
        int32_t tx, ty;
        m.motion.pos(now, tx, ty);
        e.fx       = static_cast<int16_t>(tx);
        e.fy       = static_cast<int16_t>(ty);
        e.value    = static_cast<int16_t>(row_i(act, "Radius"));
        e.end_ms   = 1000 * static_cast<int16_t>(row_i(act, "Seconds"));
        e.life_ms  = e.end_ms;
        e.pulse_ms = 0;
        std::vector<uint8_t> tail;
        tail.push_back(static_cast<uint8_t>(e.result));
        for (uint16_t v : { static_cast<uint16_t>(e.value), static_cast<uint16_t>(e.fx), static_cast<uint16_t>(e.fy) }) {
            tail.push_back(static_cast<uint8_t>(v & 0xFF));
            tail.push_back(static_cast<uint8_t>(v >> 8));
        }
        for (int k = 0; k < 4; ++k) tail.push_back(static_cast<uint8_t>(static_cast<uint32_t>(e.end_ms) >> (8 * k)));
        std::vector<PacketBuffer> group;
        group.push_back(opener(tail));
        server_.broadcast_block(zone_id_, group, 0);
        m.es.push_back(e);
        skill_use_tm(e.skill);
        LOG_DEBUG("GameClientSession %u: Tesla Field %u at (%d,%d) radius %d, %d ms, result %d", session_id(), e.id, e.fx,
                  e.fy, e.value, e.end_ms, e.result);
        return true;
    }
    const bool ice = (e.esaction == 1315);
    if (!ice) {                                              // ChillingPrison's +0 is the row's ApplyRT
        const float art = row_f(act, "ApplyRT");
        e.result = (art != 0.0f && rnd(100) >= clampi(static_cast<int>(static_cast<double>(art) * 100.0), 0, 100)) ? 2 : 1;
    }
    const int32_t base = row_i(act, ice ? "AP" : "M_AP") + 3 * elem_slot(1);
    const SkillDamage h = skill_magic_damage(m, base, row_f(act, ice ? "ApplyRatio" : "ApplyRT"), ice ? 49 - st_lk_ : 49, 2);
    if (h.result == 2 || h.result == 1 || h.result == 8 || h.dmg == 0) e.result = 2;
    e.end_ms  = 1000 * static_cast<int16_t>(row_i(act, "Seconds"));
    e.life_ms = (e.result == 1) ? e.end_ms : 0;
    e.ratio   = row_f(act, "Ratio");
    {
        std::vector<PacketBuffer> group;
        group.push_back(opener({ static_cast<uint8_t>(e.result) }));
        server_.broadcast_block(zone_id_, group, 0);
    }
    if (e.result != 1) {
        m.es.push_back(e);                                   // its first tick ends it
        return true;
    }
    mark_monster_owner(m);
    if (ice) {
        double cut = static_cast<double>(stat_value(4)) * static_cast<double>(e.ratio) / 2.0;
        if (cut > 95.0) cut = 95.0;
        e.value = static_cast<int32_t>(cut);
        if (!(e.resisted & 0x800000u)) m.resist_buff[4] = static_cast<int16_t>(m.resist_buff[4] - e.value);   // ElecR
    }
    const int32_t hp_before = m.hp;
    std::vector<PacketBuffer> group;
    const bool killed = skill_damage_event(m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    m.es.push_back(e);
    if (!killed && (!ice || !(e.resisted & 2u))) {
        int32_t x, y;
        m.motion.pos(now, x, y);
        m.motion.rest(x, y);
        m.pos_x = x; m.pos_y = y;
        m.cond_add(ice ? 1 : 15);
        if (!ice) m.skill_flags = static_cast<uint16_t>(m.skill_flags | 32u);
    }
    skill_after_hit(m, h.dmg, hp_before, killed, e.skill, false, now, 1);
    LOG_DEBUG("GameClientSession %u: %s on mob %u dmg=%d (ElecR -%d) %d ms", session_id(), ice ? "Shard of the Glacier" :
              "Icey Shackles", static_cast<unsigned>(m.object_id), h.dmg, ice ? e.value : 0, e.end_ms);
    (void)d;
    return true;
}

// ESAElectricField's pulse (sub_5B6420): sub_53E360 around its spot, at most 5, Radius; any -> the action {ESAction
// 1317, EState 0, ESClass -1, flags 2, skill 2123, its row, [u16 n][u16 id]*n} through the switch (sub_5C0250 ->
// sub_5D70F0, ESAElectricFieldHit): the block {S2C 224 sub_5EE050 [us][us][0][1317]... + [u16 n][u16 id]*n
// (VERIFIED: the client's ESAElectricFieldHit 0x4FC1C0 reads them), each (sub_5D7240, as an object; Type 8 / 12 left
// out): the magic damage sub_5E6270(AP + 3 * Elec (element slot 4), ApplyRatio, MA offset -(our MaxMP / 120),
// element 16 = Elec), its EXP share, 30002, then 30003 kind 0}; then the skill's use TM.
void GameClientSession::tesla_hit(const MonsterEntity::Es& f)
{
    if (dead_ || leaving_ || state_ != GameClientSessionState::InWorld) return;
    const auto now = std::chrono::steady_clock::now();
    const std::vector<MonsterEntity*> arr = area_monsters(f.fx, f.fy, f.value, 5, now);
    if (arr.empty()) return;
    const SkillDef* sd = server_.skill_db().get(f.skill);
    const EsaRow* row = sd ? server_.esa_db().row(sd->esa_table, f.level) : nullptr;
    const int32_t base  = row_i(row, "AP") + 3 * elem_slot(4);
    const float   apply = row_f(row, "ApplyRatio");
    std::vector<PacketBuffer> group;
    PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
    o.write<uint16_t>(static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(0);
    o.write<uint16_t>(1317);
    o.write<uint8_t>(2);
    o.write<uint8_t>(0xFF);
    o.write<uint16_t>(2123);
    o.write<uint16_t>(0);
    o.write<uint16_t>(static_cast<uint16_t>(arr.size()));
    for (MonsterEntity* m : arr) o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
    group.push_back(std::move(o));
    struct Hit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };
    std::vector<Hit> done;
    for (MonsterEntity* m : arr) {
        const SkillDamage h = skill_magic_damage(*m, base, apply, -(mp_max_ / 120), 16);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now, 0);
    skill_use_tm(2123);
}

// ESAWhirlWind's pulse (sub_5662E0, every 2000 ms of its tick while we stand): the zone's monsters in list order —
// not dead, our layer, not Type 8 / 12, a shadow one only with the Third Eye — within Radius of where we stand now;
// such a one once min(KillNum, 7) are hit stops the walk; one without an owner or ours (our party's) takes an
// ESAWhirlWindDo (sub_566710: {ESAction 1198, EState 0, ESClass -1, skill 2116, flags 2, the row}, made at once).
// ESAWhirlWindDo (init sub_566A90): AP' = AP + 3 * our Wind (element slot 2, +840), the row's ApplyRatio; in a block
// the hit first (sub_566BA0: our mark, the magic damage sub_5E6270(AP', ApplyRatio, MA offset 49, element 4 = Wind),
// the EXP share, 30002, then 30003 kind 3 when we may attack it, vtbl+164), then S2C 224 sub_5EDCD0 [us][target]
// [ES id][1198][2][ESClass -1][2116][0] (VERIFIED: the client's ESAWhirlWindDo 0x4DB080 reads none). It lives 300 ms
// (sub_566DF0; not applied: its first base tick ends it), then S2C 280.
void GameClientSession::whirlwind_pulse(const ActiveEs& w)
{
    const SkillDef* sd = server_.skill_db().get(w.skill);
    const EsaRow* row = sd ? server_.esa_db().row(sd->esa_table, w.level) : nullptr;
    const auto now = std::chrono::steady_clock::now();
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    const int32_t ap    = row_i(row, "AP") + 3 * elem_slot(2);
    const float   apply = row_f(row, "ApplyRatio");
    int n = 0;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0 || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (!(static_cast<double>(dist_f(mx, my, cx, cy)) <= static_cast<double>(w.radius))) continue;
        if (n >= w.count || n >= 7) break;
        if (owned_by_other(*m)) continue;
        ++n;
        CastEs e;
        e.id       = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());
        e.esaction = 1198;
        e.skill    = 2116;
        e.target   = static_cast<uint16_t>(oid);
        e.ap       = ap;
        e.apply    = apply;
        e.result   = (apply != 0.0f && rnd(100) >= clampi(static_cast<int>(static_cast<double>(apply) * 100.0), 0, 100))
                         ? 2 : 1;                                        // InitESAction sub_510060 (no resist)
        mark_monster_owner(*m);
        const SkillDamage h = skill_magic_damage(*m, ap, apply, 49, 4);
        const int32_t hp_before = m->hp;
        std::vector<PacketBuffer> group;
        const bool killed = skill_damage_event(*m, h, group);
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());       // sub_5EDCD0
        o.write<uint16_t>(static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(e.target);
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(1198);
        o.write<uint8_t>(2);
        o.write<uint8_t>(0xFF);
        o.write<uint16_t>(2116);
        o.write<uint16_t>(0);
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
        cast_es_.push_back(e);
        skill_after_hit(*m, h.dmg, hp_before, killed, 0, !may_attack_owned(*m), now, 3);
    }
}

// ── ESAction 1321 "Razor Gale" (GameServer sub_5D77E0, ESASharpWind; ESAUI 1) ─────────────────────────
//  The selected target (kind 3) — none: nothing. The row ESAction_SharpWind (sub_51C070: ApplyRatio +0, AP +4,
//  Comment +8). The block: the opener + [u16 target] (VERIFIED: the client's ESASharpWind 0x4FD030 reads it), then
//  the magic damage sub_5E6270(AP + 3 * Wind (element slot 2, +840), ApplyRatio, MA offset 49 - our AC, element 4 =
//  Wind), its EXP share, 30002, then 30003 kind 0. Then the skill's use TM. No mark.
void GameClientSession::esa_sharp_wind(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    group.push_back(std::move(o));
    const int32_t base = row_i(act, "AP") + 3 * elem_slot(2);
    const SkillDamage h = skill_magic_damage(*m, base, row_f(act, "ApplyRatio"), 49 - st_ac_, 4);
    const int32_t hp_before = m->hp;
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*m, h.dmg, hp_before, killed, 0, false, now, 0);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Razor Gale -> mob %u dmg=%d result=%d", session_id(), tid, h.dmg, h.result);
}

// ── ESAction 1356 "Thunderbolt" (GameServer sub_5DB4D0, ESACallLightning; ESAUI 1) ─────────────────────
//  The selected target (kind 3) — none: nothing; we mark it. The row ESAction_CallLightning (sub_51CC20: ApplyRT +0,
//  M_AP +4, MaxCount +8, Comment +10): the magic damage sub_5E6270((rand() % MaxCount + 1) * (M_AP + 3 * Elec
//  (element slot 4, +844)), ApplyRT, MA offset 25, element 16 = Elec), its EXP share; the block {the opener (to the
//  area of us and the target) + [u16 target][pos us][pos target] (sub_5DB740; VERIFIED: the client's
//  ESACallLightning 0x50A900 reads them), the damage event, EXP 30003 kind 1}; the skill's TM per hit.
void GameClientSession::esa_call_lightning(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    mark_monster_owner(*m);
    const int32_t one   = 3 * elem_slot(4) + row_i(act, "M_AP");
    const int     maxc  = static_cast<int16_t>(row_i(act, "MaxCount"));
    const int32_t base  = (rnd(maxc) + 1) * one;
    const SkillDamage h = skill_magic_damage(*m, base, row_f(act, "ApplyRT"), 25, 16);
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
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, false, now, 1);
    LOG_DEBUG("GameClientSession %u: Thunderbolt -> mob %u (base %d) dmg=%d result=%d", session_id(), tid, base, h.dmg,
              h.result);
}

// ── ESAction 1318 "Phoenix Rising" (GameServer sub_5D7420, ESAFireBurn; ESAUI 1) ───────────────────────
//  The selected target (kind 3) — none: nothing. The row ESAction_FireBurn (sub_51C130: ApplyRatio +0, AP +4, Radius
//  +8, Seconds +10). sub_53E360 around the TARGET, at most 3, Radius; any -> the magic hit roll sub_5E5EF0 (rand(100)
//  < clamp((int)((LK - target's LK + 90) * 1.2 * ApplyRatio), 20, 100)) against the target. The block: the opener +
//  [u8 hit][u16 Radius][pos target] (VERIFIED: the client's ESAFireBurn 0x4FC500 reads them); missed: each taken one
//  gets a 30002 with DamageInfo {0, 0, 1 = miss}. Hit: after the block (sub_5D7670) each gets an ESAFireBurnDo — a
//  monster with a DebuffSet first rolls against Phoenix Rising's DebuffBitCode (S2C 1247 when all resisted) —
//  {ESAction 1319, EState 0, ESClass 1, flags 2, skill 2124, this row} through sub_524690(.., 1) (an active skill:
//  no limiter; es.cpp), then the skill's use TM.
void GameClientSession::esa_fire_burn(const SkillDef& d, uint8_t lv, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    const int16_t radius = static_cast<int16_t>(row_i(act, "Radius"));
    int32_t tx, ty;
    t->motion.pos(now, tx, ty);
    const std::vector<MonsterEntity*> arr = area_monsters(tx, ty, radius, 3, now);
    bool hit = false;
    if (!arr.empty()) {
        const int v = static_cast<int>(static_cast<double>(st_lk_ - t->lk + 90) * 1.200000047683716 *
                                       static_cast<double>(row_f(act, "ApplyRatio")));
        hit = rnd(100) < clampi(v, 20, 100);                             // sub_5E5EF0
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint8_t>(hit ? 1 : 0);
    o.write<uint16_t>(static_cast<uint16_t>(radius));
    o.write<uint16_t>(static_cast<uint16_t>(tx)); o.write<uint16_t>(static_cast<uint16_t>(ty));
    group.push_back(std::move(o));
    if (!hit) {
        for (MonsterEntity* m : arr) {
            SkillDamage miss;
            miss.death_type = 0;
            miss.result     = 1;
            skill_damage_event(*m, miss, group);
        }
    }
    server_.broadcast_block(zone_id_, group, 0);
    if (!hit) {
        for (MonsterEntity* m : arr) engage_monster(*m);
        return;
    }
    for (MonsterEntity* m : arr) {
        uint32_t bits = 0;
        if (monster_debuff_immune(*m, d.debuff_bits, bits)) { send_debuff_resisted(*m, bits); continue; }
        SkillDef dd = d;
        dd.esaction = 1319;
        dd.estate   = 0;
        dd.esclass  = 1;
        es_apply_on(*m, dd, lv, act);
    }
    skill_use_tm(su_.skill);
}

// ESAFireBurnDo's burn (sub_5B6A50): DamageInfo {the init's damage, death type 3, result 0, element 1}, its EXP
// share, 30002, then 30003 kind 1 when we may attack it (vtbl+164).
void GameClientSession::burn_hit(uint16_t mob, int32_t dmg, uint16_t skill)
{
    if (leaving_ || state_ != GameClientSessionState::InWorld) return;
    MonsterEntity* m = server_.entity_mgr().get_monster(mob);
    if (!m || m->zone != zone_id_ || !m->alive || m->hp <= 0) return;
    const auto now = std::chrono::steady_clock::now();
    SkillDamage h;
    h.dmg        = dmg;
    h.death_type = 3;
    h.result     = 0;
    h.element    = 1;
    const int32_t hp_before = m->hp;
    std::vector<PacketBuffer> group;
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*m, h.dmg, hp_before, killed, 0, !may_attack_owned(*m), now, 1);
    (void)skill;
}

// ESAFireField / ESABreakdownGround's pulse (sub_53DF80 / sub_53F1E0, every 1000 ms): first each one of its set — gone
// from the zone: out of the set; farther than Radius from the spot: out of the set and its Do (1354 / 1359) ends (its
// limiter's vtbl+60) —; then, the set not full (min(KillNum, 7)), sub_53E360 around the spot, at most that many, Radius:
// each one not in the set gets the Do {ESAction 1354 / 1359, EState 0, ESClass 1, flags 2, skill 2129 / 2130, the
// field's row, its spot} through sub_524690(.., 1) and joins the set, until it is full (ESABreakdownGround: a monster
// with a DebuffSet first rolls against 2130's DebuffBitCode — all resisted: S2C 1247, not taken).
void GameClientSession::field_pulse_es(size_t i)
{
    if (i >= es_.size()) return;
    ActiveEs& f = es_[i];
    const int child = (f.esaction == 1353) ? 1354 : 1359;
    const auto now = std::chrono::steady_clock::now();
    for (size_t k = 0; k < f.ids.size();) {
        MonsterEntity* m = server_.entity_mgr().get_monster(f.ids[k]);
        if (!m || m->zone != zone_id_) { f.ids.erase(f.ids.begin() + static_cast<std::ptrdiff_t>(k)); continue; }
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (static_cast<double>(f.radius) >= static_cast<double>(dist_f(mx, my, f.fx, f.fy))) { ++k; continue; }
        f.ids.erase(f.ids.begin() + static_cast<std::ptrdiff_t>(k));
        std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
        for (size_t j = 0; j < m->es.size(); ++j)
            if (m->es[j].esaction == child) { server_.monster_es_end(*m, j); break; }
    }
    if (static_cast<int>(f.ids.size()) >= f.count) return;
    const SkillDef* sd = server_.skill_db().get(f.skill);
    if (!sd) return;
    const EsaRow* row = server_.esa_db().row(sd->esa_table, f.level);
    const std::vector<MonsterEntity*> arr = area_monsters(f.fx, f.fy, f.radius, f.count, now);
    for (MonsterEntity* m : arr) {
        const uint16_t id = static_cast<uint16_t>(m->object_id);
        if (std::find(f.ids.begin(), f.ids.end(), id) != f.ids.end()) continue;
        if (child == 1359) {
            uint32_t bits = 0;
            if (monster_debuff_immune(*m, sd->debuff_bits, bits)) { send_debuff_resisted(*m, bits); continue; }
        }
        SkillDef dd = *sd;
        dd.esaction = child;
        dd.estate   = 0;
        dd.esclass  = 1;
        es_apply_on(*m, dd, f.level, row);
        f.ids.push_back(id);
        if (static_cast<int>(f.ids.size()) >= f.count) break;
    }
}

// sub_53E6A0: a field's end — the Do on each one of its set ends (its limiter's vtbl+60), the set cleared.
void GameClientSession::field_end_children(const ActiveEs& f)
{
    const int child = (f.esaction == 1353) ? 1354 : 1359;
    std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
    for (uint16_t id : f.ids) {
        MonsterEntity* m = server_.entity_mgr().get_monster(id);
        if (!m) continue;
        for (size_t j = 0; j < m->es.size(); ++j)
            if (m->es[j].esaction == child) { server_.monster_es_end(*m, j); break; }
    }
}

// ESAFireFieldDo's burn (sub_5847B0): the magic damage sub_5E6270(M_AP + 3 * our Fire (element slot 0), 1.0, MA offset
// 49, element 1 = Fire) rolled anew each time with its result set to 0, its EXP share, 30002, then 30003 kind 1 when we
// may attack it (vtbl+164).
void GameClientSession::field_burn(uint16_t mob, uint8_t level)
{
    if (leaving_ || state_ != GameClientSessionState::InWorld) return;
    MonsterEntity* m = server_.entity_mgr().get_monster(mob);
    if (!m || m->zone != zone_id_ || !m->alive || m->hp <= 0) return;
    const SkillDef* sd = server_.skill_db().get(2129);
    const EsaRow* row = sd ? server_.esa_db().row(sd->esa_table, level) : nullptr;
    const auto now = std::chrono::steady_clock::now();
    SkillDamage h = skill_magic_damage(*m, row_i(row, "M_AP") + 3 * elem_slot(0), 1.0f, 49, 1);
    h.result = 0;
    const int32_t hp_before = m->hp;
    std::vector<PacketBuffer> group;
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*m, h.dmg, hp_before, killed, 0, !may_attack_owned(*m), now, 1);
}

// ESAGaleBreak's hit (sub_586D50, once its delay is over): sub_53E360 around its point, at most 7, Radius; then at most
// KillNum of them; each stops (sub_41C5D0); then each: the magic damage sub_5E6270(+208, +212, MA offset 49, element 4 =
// Wind), its EXP share, 30002 (no block here), then 30003 kind 1. (It also puts [u16 its id][u8 n]{[u16 id][u32 damage]}
// into a message 924 to us — sub_410BD0, a message to ourselves that no handler of the Player takes: no packet goes out.
// The client has a handler for an S2C 924 to the ESAGaleBreak; the recovered server never sends it.)
void GameClientSession::gale_break_hit(ActiveEs& e)
{
    const auto now = std::chrono::steady_clock::now();
    std::vector<MonsterEntity*> arr = area_monsters(e.fx, e.fy, e.radius, 7, now);
    if (static_cast<int>(arr.size()) > e.count) arr.resize(e.count > 0 ? static_cast<size_t>(e.count) : 0);
    for (MonsterEntity* m : arr) {
        int32_t x, y;
        m->motion.pos(now, x, y);
        m->motion.rest(x, y);
        m->pos_x = x; m->pos_y = y;
    }
    for (MonsterEntity* m : arr) {
        const SkillDamage h = skill_magic_damage(*m, e.value, e.ratio, 49, 4);
        const int32_t hp_before = m->hp;
        std::vector<PacketBuffer> pk;
        const bool killed = skill_damage_event(*m, h, pk);
        for (PacketBuffer& p : pk) server_.broadcast_zone(zone_id_, p, 0);
        skill_after_hit(*m, h.dmg, hp_before, killed, 0, false, now, 1);
    }
    LOG_DEBUG("GameClientSession %u: Tornado Blast at (%d,%d): %zu hit", session_id(), e.fx, e.fy, arr.size());
}

// ── ESAction 1472 "Elemental Boost" (GameServer sub_5DE390; no selection, no packet of its own) ─────────
//  The row ESAction_ElementUp (sub_51DAE0: ApplyRatio +0, Seconds +4, Ratio +8, Radius +12, Comment +16). sub_5DE420:
//  the zone's players (zone +28) in our party (sub_716EF0: our share group, us included; none: ourselves), on our
//  layer, not dead, within (float)Radius, at most 6; each gets an ESAElementUpDo (sub_5DE570: {ESAction 1473, EState 0,
//  ESClass 0, flags 2, skill 2132, this row, us its caster} through sub_524690(.., 1) — the limiter admits it; es.cpp:
//  its values from ITS elements; a member's on its strand, element_up_from). No TM.
void GameClientSession::esa_element_up(const SkillDef& d, uint8_t lv, const EsaRow* act)
{
    const float radius = static_cast<float>(row_i(act, "Radius"));
    const auto now = std::chrono::steady_clock::now();
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    const uint16_t self = static_cast<uint16_t>(object_id_);
    std::vector<GameClientSession*> found;
    for (uint32_t oid : server_.entity_mgr().players_in_zone(zone_id_)) {
        GameClientSession* s = server_.player_session(oid);
        if (!s || !s->object_id_ || !s->in_world()) continue;
        if (s != this && !server_.party().same_group(char_uid_, s->char_uid_)) continue;
        if (s->dead_) continue;
        int32_t x, y;
        s->cur_pos(now, x, y);
        if (static_cast<double>(radius) >= static_cast<double>(dist_f(x, y, cx, cy)) && found.size() < 6)
            found.push_back(s);
    }
    const uint16_t skill = static_cast<uint16_t>(d.id);
    for (GameClientSession* s : found) {
        if (s == this) element_up_from(self, skill, lv, zone_id_);
        else s->element_up_pulse(self, skill, lv, zone_id_);
    }
}

// Elemental Boost's ESAElementUpDo on us (from ourselves or a party member in `zone`).
void GameClientSession::element_up_from(uint16_t caster, uint16_t skill, uint8_t lv, int16_t zone)
{
    if (leaving_ || state_ != GameClientSessionState::InWorld || zone_id_ != zone || !object_id_) return;
    const SkillDef* sd = server_.skill_db().get(skill);
    if (!sd) return;
    SkillDef dd = *sd;
    dd.esaction = 1473;
    dd.estate   = 0;
    dd.esclass  = 0;
    const EsaRow* act = server_.esa_db().row(sd->esa_table, lv);
    const bool limited = dd.type != 2 && dd.id != 7046;
    if (limited && !es_limiter_admit(0, 1473, skill)) return;
    es_create(dd, lv, act, EsKind::ElementUp, limited, 2, 0,
              (caster == static_cast<uint16_t>(object_id_)) ? 0 : caster);
}

// ═════════════════════════════════════════════════════════════════════════════
//  The Spiritualist (Sheep, 3rd job). Aqua Bomb (1257) is Drip Bomb's twin (esa_water_ball); the blessings (1337..
//  1341) and the gardens (1369..1377) are ESs of es.cpp, with their pieces here.
// ═════════════════════════════════════════════════════════════════════════════

// ── ESAction 1263 "Staff of Thunder" (GameServer sub_5D4AC0, ESAElectricSpear; ESAUI 1) ─────────────────
//  The selected target (zone vtbl+16 kind 3) — none: nothing. The row ESAction_ElectricSpear (sub_51BFC0: ApplyRatio
//  +0, AP +4, Radius +8 i16, Comment +10). sub_53E360 around the TARGET, at most 5, Radius; the target not among them
//  takes the first one's place (none found: nothing is hit). The block: the opener + [u16 n][u16 id]*n (VERIFIED: the
//  client's ESAElectricSpear 0x4F44D0 reads them, at most 5), then each (sub_5D4CC0): the magic damage sub_5E6270(AP +
//  3 * Elec (element slot 4, +844), ApplyRatio, MA offset 25, element 16 = Elec), its EXP share, 30002, then 30003 kind
//  1. Then the skill's use TM. No mark. (The target as a corpse takes its roll; its damage event does nothing —
//  sub_729100 in state 2 — and its share 0 still gives 1 EXP.)
void GameClientSession::esa_electric_spear(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_) return;
    int32_t tx, ty;
    t->motion.pos(now, tx, ty);
    std::vector<MonsterEntity*> arr = area_monsters(tx, ty, static_cast<int16_t>(row_i(act, "Radius")), 5, now);
    if (!arr.empty() && std::find(arr.begin(), arr.end(), t) == arr.end()) arr[0] = t;
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(static_cast<uint16_t>(arr.size()));
    for (MonsterEntity* m : arr) o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
    group.push_back(std::move(o));
    const int32_t base  = row_i(act, "AP") + 3 * elem_slot(4);
    const float   apply = row_f(act, "ApplyRatio");
    struct Hit { MonsterEntity* m; int32_t dmg, hp_before; bool killed, corpse; };
    std::vector<Hit> done;
    for (MonsterEntity* m : arr) {
        const SkillDamage h = skill_magic_damage(*m, base, apply, 25, 16);
        const bool corpse = !m->alive || m->hp <= 0;
        const int32_t hp_before = corpse ? 0 : m->hp;
        const bool killed = corpse ? false : skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed, corpse });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : done) {
        if (h.corpse) { gain_attack_exp(*h.m, h.dmg, 0, 1); continue; }
        skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now, 1);
    }
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Staff of Thunder at mob %u (base %d): %zu hit", session_id(), tid, base, arr.size());
}

// ── ESAction 1264 "Dragon Storm" (GameServer sub_5D4DE0, ESAFireDragon; ESAUI 1) ─────────────────────────
//  The selected target (kind 3) — none: nothing. The row ESAction_FireDragon (sub_51C070: ApplyRatio +0, AP +4,
//  Comment +8). The block: the opener sub_5EDE00 [us][target][0]... without a tail (VERIFIED: the client's
//  ESAFireDragon 0x4F46A0 reads none — it takes the target from the opener), then (sub_5D4EB0) the magic damage
//  sub_5E6270(AP + 3 * Fire (element slot 0, +836), ApplyRatio, MA offset 49, element 1 = Fire), its EXP share, 30002,
//  then 30003 kind 1. Then the skill's use TM. No mark.
void GameClientSession::esa_fire_dragon(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_) return;
    std::vector<PacketBuffer> group;
    group.push_back(skill_opener(d, tid));
    const SkillDamage h = skill_magic_damage(*m, row_i(act, "AP") + 3 * elem_slot(0), row_f(act, "ApplyRatio"), 49, 1);
    const bool corpse = !m->alive || m->hp <= 0;
    const int32_t hp_before = corpse ? 0 : m->hp;
    const bool killed = corpse ? false : skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    if (corpse) gain_attack_exp(*m, h.dmg, 0, 1);
    else skill_after_hit(*m, h.dmg, hp_before, killed, 0, false, now, 1);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Dragon Storm -> mob %u dmg=%d result=%d%s", session_id(), tid, h.dmg, h.result,
              killed ? " [KILLED]" : "");
}

// ── ESAction 1265 "Earthquake" (GameServer sub_5D4F90, ESAEarthquake; ESAUI 6: a point) ───────────────────
//  The row ESAction_Earthquake (sub_51BFC0) and the payload's point: sub_53E360 around it, at most 5, Radius (i16 +8);
//  none: nothing at all (no opener). Else the block: the opener + [pos the point] (sub_5E0EB0; VERIFIED: the client's
//  ESAEarthquake 0x4F47E0 reads it); then each found one (sub_5D50A0) gets an ESAEarthquakeDo {ESAction 1266, EState 0,
//  ESClass -1, flags 2, skill 2304, this row, SkillParam 2304's +92 / +42} through sub_524690(.., 1): made at once.
//  ESAEarthquakeDo (init sub_5717C0) is ESAEarthDivideHit's twin (skill_magic.cpp, a CastEs): the row's ApplyRatio, AP' =
//  AP + 3 * our Earth (element slot 3, +842), the target stops and takes the condition 1, state 0, the block {S2C 224
//  sub_5EDCD0 [us][target][ES id][1266][2][ESClass -1][2304][0], no tail — VERIFIED: the client's ESAEarthquakeDo
//  0x4F4B20 reads none}, the TM per hit of 2304 (sub_891C80); 500 ms later the hit (sub_571B30: as sub_55CAF0, but its
//  30003 kind 3 goes before the 30002), 500 ms after it the end (S2C 280; the condition 1 off if still on; our mark with
//  5000). Not applied: its first base tick ends it. No use TM.
void GameClientSession::esa_earthquake(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    int16_t px = 0, py = 0;
    if (su_.payload.size() >= 4) {
        px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
        py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
    }
    const std::vector<MonsterEntity*> arr = area_monsters(px, py, static_cast<int16_t>(row_i(act, "Radius")), 5, now);
    if (arr.empty()) return;
    {
        std::vector<PacketBuffer> group;
        PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(static_cast<uint16_t>(px));
        o.write<uint16_t>(static_cast<uint16_t>(py));
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    const float   apply = row_f(act, "ApplyRatio");
    const int32_t ap    = row_i(act, "AP") + 3 * elem_slot(3);
    for (MonsterEntity* m : arr) {
        CastEs e;
        e.id       = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());
        e.esaction = 1266;
        e.skill    = 2304;
        e.target   = static_cast<uint16_t>(m->object_id);
        e.ap       = ap;
        e.apply    = apply;
        e.state    = 0;
        e.elapsed  = 0;
        e.released = false;
        e.result   = (apply != 0.0f && rnd(100) >= clampi(static_cast<int>(static_cast<double>(apply) * 100.0), 0, 100))
                         ? 2 : 1;                                        // InitESAction sub_510060 (no resist)
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        m->motion.rest(mx, my);                                          // sub_41C5D0 on the target
        m->pos_x = mx; m->pos_y = my;
        m->cond_add(1);
        {
            std::vector<PacketBuffer> group;
            PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());     // sub_5EDCD0
            o.write<uint16_t>(static_cast<uint16_t>(object_id_));
            o.write<uint16_t>(e.target);
            o.write<uint16_t>(e.id);
            o.write<uint16_t>(1266);
            o.write<uint8_t>(2);
            o.write<uint8_t>(0xFF);
            o.write<uint16_t>(2304);
            o.write<uint16_t>(0);
            group.push_back(std::move(o));
            server_.broadcast_block(zone_id_, group, 0);
        }
        cast_es_.push_back(e);
        skill_hit_tm(*m, 2304);                                          // sub_891C80
    }
    LOG_DEBUG("GameClientSession %u: Earthquake at (%d,%d): %zu targets", session_id(), px, py, arr.size());
    (void)d;
}

// ── ESAction 1284 "Raging Storm" (GameServer sub_5D5800, ESAMustangWind; ESAUI 0) ──────────────────────────
//  The row ESAction_MustangWind (sub_51BFC0). sub_53E360 around us, at most 3, Radius (i16 +8). The block: the opener +
//  [u16 n][u16 id]*n (VERIFIED: the client's ESAMustangWind 0x4F59E0 reads them), then each (sub_5D5970): the magic
//  damage sub_5E6270(AP + 3 * Wind (element slot 2, +840), ApplyRatio, MA offset 49 - our AC, element 4 = Wind), its
//  EXP share, 30002, then 30003 kind 1. Then the skill's use TM (also with none found).
void GameClientSession::esa_mustang_wind(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    int32_t ax, ay;
    cur_pos(now, ax, ay);
    const std::vector<MonsterEntity*> arr = area_monsters(ax, ay, static_cast<int16_t>(row_i(act, "Radius")), 3, now);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(static_cast<uint16_t>(arr.size()));
    for (MonsterEntity* m : arr) o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
    group.push_back(std::move(o));
    const int32_t base  = row_i(act, "AP") + 3 * elem_slot(2);
    const float   apply = row_f(act, "ApplyRatio");
    struct Hit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };
    std::vector<Hit> done;
    for (MonsterEntity* m : arr) {
        const SkillDamage h = skill_magic_damage(*m, base, apply, 49 - st_ac_, 4);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now, 1);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Raging Storm (base %d): %zu hit", session_id(), base, arr.size());
}

// ── ESAction 1484 "차원의틈" (Dimension Crack; GameServer sub_5DF510, ESADimensionCrack; ESAUI 6: a point) ─────────
//  The row ESAction_DimensionCrack (sub_51DF00: ApplyRatio +0, Ratio +4 float, Seconds +8, Radius +12, Comment +16,
//  KillNum +20 — ints) and the payload's point: sub_53E360 around it, at most KillNum (5..7 in the data: retail's buffer
//  holds 7), Radius. sub_5DF5C0: first each one (sub_5DF8C0, before any block): the magic damage sub_5E6270((int)Ratio,
//  ApplyRatio, MA offset 25, element 128 = NoProp), its EXP share, the message 30330 (Character::OnMessage 0x767A: the
//  30002 path, its damage shown by S2C 1307 — skill_damage_event), then 30003 kind 1; then the block: the opener +
//  [u16 n][pos]*n (VERIFIED: the client's ESADimensionCrack 0x51F2A5 reads them); then each one not dead whose damage
//  was not 0 (sub_5DF780): a monster with a DebuffSet first rolls against 2316's DebuffBitCode (S2C 1247 when all
//  resisted), else the action {ESAction 1485, EState 0, ESClass 1, flags 2, skill 2316, this row} through sub_524690(..,
//  1) — an active skill: no limiter (dimension_do_on). Then the skill's use TM.
void GameClientSession::esa_dimension_crack(const SkillDef& d, uint8_t lv, const EsaRow* act,
                                            std::chrono::steady_clock::time_point now)
{
    int16_t px = 0, py = 0;
    if (su_.payload.size() >= 4) {
        px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
        py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
    }
    const std::vector<MonsterEntity*> arr = area_monsters(px, py, row_i(act, "Radius"), row_i(act, "KillNum"), now);
    const int32_t base  = static_cast<int32_t>(row_f(act, "Ratio"));
    const float   apply = row_f(act, "ApplyRatio");
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(static_cast<uint16_t>(arr.size()));
    for (MonsterEntity* m : arr) {
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        o.write<uint16_t>(static_cast<uint16_t>(mx));
        o.write<uint16_t>(static_cast<uint16_t>(my));
    }
    std::vector<int32_t> dmg;
    for (MonsterEntity* m : arr) {
        const SkillDamage h = skill_magic_damage(*m, base, apply, 25, 128);
        const int32_t hp_before = m->hp;
        std::vector<PacketBuffer> pk;
        const bool killed = skill_damage_event(*m, h, pk, 0x051B);
        for (PacketBuffer& p : pk) server_.broadcast_zone(zone_id_, p, 0);
        skill_after_hit(*m, h.dmg, hp_before, killed, 0, false, now, 1);
        dmg.push_back(h.dmg);
    }
    {
        std::vector<PacketBuffer> group;
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    for (size_t k = 0; k < arr.size(); ++k) {
        MonsterEntity* m = arr[k];
        if (!m->alive || m->hp <= 0 || dmg[k] == 0) continue;
        uint32_t bits = 0;
        if (monster_debuff_immune(*m, d.debuff_bits, bits)) { send_debuff_resisted(*m, bits); continue; }
        SkillDef dd = d;
        dd.esaction = 1485;
        dd.estate   = 0;
        dd.esclass  = 1;
        es_apply_on(*m, dd, lv, act);
    }
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Dimension Crack at (%d,%d) (base %d): %zu hit", session_id(), px, py, base,
              arr.size());
}

// ESADimensionDo (1485, init sub_5B99A0; the action carries Dimension Crack's row: Seconds +8): applied whatever the
// roll (sub_5275E0(this, 1): +180 = 1); +196 = 1000 * Seconds, the lifetime; the block {S2C 224 sub_5EDE00 [us][target]
// [ES id][1485][2][ESClass 1][2316][0], no tail — VERIFIED: the client's ESADimensionDo 0x51F470 reads none}; then
// sub_717DB0 on us: every monster we own lets us go (release_owned); S2C 366 [target] to its area (sub_5EE620);
// +200 = its layer, the layer -1 (vtbl+72 sub_7148F0 -> the sectors' sub_7EF120: each player around, who cannot see
// it, gets S2C 16 [u16 it] — VERIFIED: the client's case 16 posts 0x7543 to the object); it stops (sub_41C5D0), its
// state 0 (its battle given up), the condition 15; then those whose +1212 is it let it go (none: only monsters own)
// and its own owner goes (vtbl+120). Its tick (sub_5B9B80): the target gone or past +196 -> the end, else the base
// tick. Its end (vtbl+28: S2C 280 — to the layer -1, so to no one —, then sub_5B9BF0): its layer back (the sectors:
// S2C 8, its appear, to the players around), the condition 15 off (es.cpp).
bool GameClientSession::dimension_do_on(MonsterEntity& m, MonsterEntity::Es& e)
{
    const auto now = std::chrono::steady_clock::now();
    const uint16_t tid = static_cast<uint16_t>(m.object_id);
    e.result  = 1;
    e.flags   = 2;
    e.life_ms = e.end_ms;
    {
        std::vector<PacketBuffer> group;
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(tid);
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(1485);
        o.write<uint8_t>(2);
        o.write<uint8_t>(static_cast<uint8_t>(e.esclass));
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(0);
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    server_.release_owned(object_id_);
    {
        PacketBuffer p(static_cast<Opcode>(0x016E), is_paula());       // sub_5EE620
        p.write<uint16_t>(tid);
        server_.broadcast_zone(zone_id_, p, 0);
    }
    m.banished = true;
    {
        PacketBuffer p(static_cast<Opcode>(0x0010), is_paula());       // sub_7EF120 -> sub_7EFE30
        p.write<uint16_t>(tid);
        server_.broadcast_zone(zone_id_, p, 0);
    }
    int32_t x, y;
    m.motion.pos(now, x, y);
    m.motion.rest(x, y);
    m.pos_x = x; m.pos_y = y;
    if (m.target_oid) {                                                  // sub_414B50(target, 0)
        if (m.target_oid == object_id_) end_mob_attack(tid, false);
        else if (GameClientSession* c = server_.player_session(m.target_oid)) c->drop_mob_attack(tid);
    }
    m.cond_add(15);
    server_.release_owner(m);                                            // vtbl+120 (sub_72B400)
    m.es.push_back(e);
    LOG_DEBUG("GameClientSession %u: Dimension Crack sends mob %u away for %d ms (ES %u)", session_id(), tid, e.end_ms,
              e.id);
    return true;
}

// ESAWaterAreaDo / ESAThunderAreaDo / ESAFireAreaDo / ESAEarthAreaDo / ESAWindAreaDo (1370 / 1372 / 1374 / 1376 / 1378;
// init sub_58A890, then its class's: the slot +252 = ElecR 15 / FireR 11 / EarthR 14 / WindR 13 / WaterR 12): the
// action carries the garden's row copy, whose +0 is the row constructor's 0.0 (always applied — a failed roll would
// leave no object); +244 = 1000 * Seconds, the lifetime; +248 = RegiDown; the block {S2C 224 sub_5EDE00 [us][target]
// [ES id][Do][2][ESClass 1][skill][0], no tail — VERIFIED: the client's ESAWaterAreaDo 0x512260 reads none}; our TM per
// hit (sub_891C80). Then the cut (sub_58A560): v = min(0.5, (our MA / 400 + 0.3) * RegiDown), +260 = the target's resist
// of that slot from its items (+880) and +968 — a monster's base resist —, +256 = (int)(+260 * v), at least 1; the
// slot's +924 -= +256 (sub_58A650(0); S2C 826 for a player). Its tick (sub_58A460, es.cpp): us or the target gone -> the
// end; every 2000 ms (its +8 counted back — so never its time: the garden ends it) a changed +260 -> the cut redone; its
// release (vtbl+36 sub_58A7D0): the cut back.
bool GameClientSession::area_do_on(MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act)
{
    static const int kSlot[5] = { 15, 11, 14, 13, 12 };
    const int c = (e.esaction - 1370) / 2;
    if (c < 0 || c > 4 || e.esaction % 2 != 0) return false;
    e.result  = 1;
    e.flags   = 2;
    e.end_ms  = 1000 * static_cast<int16_t>(row_i(act, "Seconds"));
    e.life_ms = e.end_ms;
    e.ratio   = row_f(act, "RegiDown");
    e.phase   = kSlot[c];                                                // +252
    {
        std::vector<PacketBuffer> group;
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(static_cast<uint16_t>(e.esaction));
        o.write<uint8_t>(2);
        o.write<uint8_t>(static_cast<uint8_t>(e.esclass));
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(0);
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    skill_hit_tm(m, e.skill);
    const int k = kSlot[c] - 11;
    float v = static_cast<float>((static_cast<double>(stat_value(4)) / 400.0 + 0.300000011920929) *
                                 static_cast<double>(e.ratio));
    if (v > 0.5f) v = 0.5f;
    e.phase_ms = m.resist[k];                                            // +260
    e.value    = static_cast<int32_t>(static_cast<double>(e.phase_ms) * static_cast<double>(v));
    if (e.value <= 0) e.value = 1;
    m.resist_buff[k] = static_cast<int16_t>(m.resist_buff[k] - e.value);
    m.es.push_back(e);
    LOG_DEBUG("GameClientSession %u: garden Do %u (ESAction %d) on mob %u: resist slot %d -%d", session_id(), e.id,
              e.esaction, static_cast<unsigned>(m.object_id), kSlot[c], e.value);
    return true;
}

// ESAWaterArea .. ESAWindArea's pulse (sub_589380, every 1000 ms): first each one of its set — gone: out of the set;
// within Radius of the spot: kept, but a shadow one while we lack the Third Eye leaves the set (its Do stays); farther:
// out of the set and its Do ends (its limiter's vtbl+60) —; then, the set not full (min(Count, 5)) and we here,
// sub_53E360 (ours) around the spot, at most Count, Radius: each one not in the set — a monster with a DebuffSet first
// rolls against the skill's DebuffBitCode (all resisted: S2C 1247, not taken) — gets the Do {ESAction 1370 / 1372 /
// 1374 / 1376 / 1378, EState 0, ESClass 1, flags 2, the garden's skill, its row, its spot} through sub_524690(.., 1) and
// joins the set (not a shadow one while we lack the Third Eye), until it is full.
void GameClientSession::elem_area_pulse(size_t i)
{
    if (i >= es_.size()) return;
    ActiveEs& f = es_[i];
    const int child = f.esaction + 1;
    const auto now = std::chrono::steady_clock::now();
    for (size_t k = 0; k < f.ids.size();) {
        MonsterEntity* m = server_.entity_mgr().get_monster(f.ids[k]);
        if (!m || m->zone != zone_id_) { f.ids.erase(f.ids.begin() + static_cast<std::ptrdiff_t>(k)); continue; }
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (static_cast<double>(f.radius) >= static_cast<double>(dist_f(mx, my, f.fx, f.fy))) {
            if (m->shadow() && !third_eye_) { f.ids.erase(f.ids.begin() + static_cast<std::ptrdiff_t>(k)); continue; }
            ++k;
            continue;
        }
        f.ids.erase(f.ids.begin() + static_cast<std::ptrdiff_t>(k));
        std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
        for (size_t j = 0; j < m->es.size(); ++j)
            if (m->es[j].esaction == child) { server_.monster_es_end(*m, j); break; }
    }
    if (static_cast<int>(f.ids.size()) >= f.count) return;
    const SkillDef* sd = server_.skill_db().get(f.skill);
    if (!sd) return;
    const uint16_t fid   = f.id;
    const uint8_t  level = f.level;
    const EsaRow* row = server_.esa_db().row(sd->esa_table, level);
    const std::vector<MonsterEntity*> arr = area_monsters(f.fx, f.fy, f.radius, f.count, now);
    for (MonsterEntity* m : arr) {
        if (i >= es_.size() || es_[i].id != fid) return;
        const uint16_t id = static_cast<uint16_t>(m->object_id);
        if (std::find(es_[i].ids.begin(), es_[i].ids.end(), id) != es_[i].ids.end()) continue;
        uint32_t bits = 0;
        if (monster_debuff_immune(*m, sd->debuff_bits, bits)) { send_debuff_resisted(*m, bits); continue; }
        SkillDef dd = *sd;
        dd.esaction = child;
        dd.estate   = 0;
        dd.esclass  = 1;
        es_apply_on(*m, dd, level, row);
        if (i >= es_.size() || es_[i].id != fid) return;
        if (!(m->shadow() && !third_eye_)) es_[i].ids.push_back(id);
        if (static_cast<int>(es_[i].ids.size()) >= es_[i].count) break;
    }
}

// sub_5899F0: a garden's end — the Do on each one of its set ends (its limiter's vtbl+60), the set cleared.
void GameClientSession::elem_area_end(const ActiveEs& f)
{
    const int child = f.esaction + 1;
    std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
    for (uint16_t id : f.ids) {
        MonsterEntity* m = server_.entity_mgr().get_monster(id);
        if (!m) continue;
        for (size_t j = 0; j < m->es.size(); ++j)
            if (m->es[j].esaction == child) { server_.monster_es_end(*m, j); break; }
    }
}

// The blessings (ESAWaterCounter / ESAThunderCounter / ESAFireCounter / ESAEarthCounter / ESAWindCounter, 1337..1341,
// their handler of the message 30002 — sub_581070 / sub_581690 / sub_581CB0 / sub_5822D0 / sub_5828F0 — called by the
// limiter's vtbl+72 at the head of our Character::OnMessage 30002, before the damage is taken): from an attacker, an
// incoming DamageInfo without the blessing's bit in [4] (8 / 0x10 / 0x20 / 0x40 / 0x80 — a counter's own mark): the
// magic damage sub_5E6270((int)(+196 * 0.5), +200, MA offset our MA - (our LK + our AC), element 2 / 16 / 1 / 8 / 4) on
// the attacker, its [1] and [3] the incoming's, [2] = 7 when it does damage (else the incoming's), [4] |= the bit — the
// message 30248 to it (counter_damage_event).
void GameClientSession::counter_on_hit(MonsterEntity& m, const int32_t in[7], size_t k, std::vector<PacketBuffer>& group,
                                       std::vector<CounterHit>& hits)
{
    static const uint32_t kBit[5]  = { 0x08, 0x10, 0x20, 0x40, 0x80 };
    static const int      kElem[5] = { 2, 16, 1, 8, 4 };
    if (k >= es_.size()) return;
    const ActiveEs& e = es_[k];
    const int c = e.esaction - 1337;
    if (c < 0 || c > 4) return;
    if (static_cast<uint32_t>(in[4]) & kBit[c]) return;
    SkillDamage h = skill_magic_damage(m, static_cast<int32_t>(static_cast<double>(e.value) * 0.5), e.ratio,
                                       stat_value(4) - (st_lk_ + st_ac_), kElem[c]);
    h.death_type = in[1];
    h.element    = in[3];
    h.flags      = static_cast<int32_t>(static_cast<uint32_t>(h.flags) | kBit[c]);
    h.result     = (h.dmg > 0) ? 7 : in[2];
    const int32_t hp_before = m.hp;
    const bool killed = counter_damage_event(m, h, group);
    hits.push_back({ &m, h.dmg, hp_before, killed });
    LOG_DEBUG("GameClientSession %u: blessing %d hits back mob %u for %d%s", session_id(), e.esaction,
              static_cast<unsigned>(m.object_id), h.dmg, killed ? " [KILLED]" : "");
}

// The message 30248 on the monster (Character::OnMessage 0x7628): the 30002 to its +332 (the owner mark on a damaging
// hit, sub_423B70, and the hate), then vtbl+244 (sub_72BB80) — nothing on a dead one —: its HP, S2C 923 [u16 it][u16
// us][the 28-byte DamageInfo] (sub_605EE0; VERIFIED: the client's object handler sub_6BEC50 reads [u16][28 bytes] for
// 923) and its death at 0 (S2C 0x2B) — no S2C 0x11E, no EXP ([5] stays 0: this path does not set it).
bool GameClientSession::counter_damage_event(MonsterEntity& m, const SkillDamage& h, std::vector<PacketBuffer>& group)
{
    const uint16_t tid = static_cast<uint16_t>(m.object_id);
    if (h.absorbed > 0) absorb_heal(m, h.absorbed);          // 30182 (sub_5E6760, during the damage roll)
    if (!m.alive || m.hp <= 0) return false;
    if (h.dmg > 0) mark_monster_owner(m);
    int32_t hp = m.hp, mx = m.hp_max;
    bool killed = false;
    if (h.dmg > 0) killed = server_.entity_mgr().damage_monster(tid, h.dmg, hp, mx);
    {
        PacketBuffer p(static_cast<Opcode>(0x039B), is_paula());
        p.write<uint16_t>(tid);
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        p.write<int32_t>(h.dmg);
        p.write<int32_t>(h.death_type);
        p.write<int32_t>(h.result);
        p.write<int32_t>(h.element);
        p.write<int32_t>(h.flags);
        p.write<int32_t>(0);
        p.write<int32_t>(h.extra);
        group.push_back(std::move(p));
    }
    if (killed) {
        PacketBuffer kp(static_cast<Opcode>(0x002B), is_paula());
        kp.write<uint16_t>(tid);
        kp.write<uint8_t>(static_cast<uint8_t>(h.death_type));
        group.push_back(std::move(kp));
    }
    return killed;
}
