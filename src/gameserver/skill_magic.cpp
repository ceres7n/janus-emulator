#include "game_session.hpp"
#include "game_server.hpp"
#include "logger.hpp"
#include "combat_util.hpp"
#include "paula_crypt.hpp"
#include <algorithm>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
//  The magic-type skills (Sheep / Dragon line) with a handler of their own in the ESAction switch
//  sub_5BE9A0 — 1:1 with the retail GameServer (GameServerRelease), the client's classes verified in
//  Trickster.bin (the ESAction factory sub_55CD40, each class's vtbl+132 reads the opener's tail).
//
//  The area pieces they share:
//    the zone's monster list (zone +4224, walked from its head: the order they joined) — a monster is a
//      target when it is on our layer (sub_893F00: one layer here), not MonsterParamEx2 Type 8 / 12
//      (sub_718610 / sub_718710) and, a shadow monster, only with the Third Eye (sub_4268F0 /
//      sub_426950); players only in PvP (sub_5C3C30 / sub_5C4200 — no PvP here);
//    the distance sqrtf((float)(dx² + dy²)) (sub_42F1B0) against the float Radius; at most 7 targets.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
constexpr int kAreaMaxTargets = 7;      // sub_5C3E10 / sub_5C4410: v >= 7 -> return 7
constexpr int kStormPhaseMs   = 900;    // ESAMagicStormHit sub_54CE00: +8 >= 900 in each state
}

bool GameClientSession::area_target_ok(const MonsterEntity& m) const
{
    if (m.banished) return false;                                      // its layer -1 (sub_893F00)
    if (const MobDef* md = server_.mob_db().get(static_cast<int>(m.mob_id)))
        if (md->type == 8 || md->type == 12) return false;
    if (m.shadow() && !third_eye_) return false;
    return true;
}

// sub_53E360 for a player caster: the zone's monster list walked from its head (sub_5A07F0) while fewer than
// `max` are taken, each one by sub_5A08D0 — not Type 12 (sub_718710), not dead (sub_41C790), our layer
// (sub_893F00), within `radius` of (cx, cy) (the int radius < sqrtf(dx² + dy²) excludes, sub_42F1B0), not Type 8
// (sub_718610), a shadow one only with the Third Eye (sub_4268F0 / sub_426950), and no owner (+1212, sub_4269D0)
// or ours (or our party's, sub_716EF0: owned_by_other). (Players are added in PvP, sub_5A0AB0 — no PvP here.)
std::vector<MonsterEntity*> GameClientSession::area_monsters(int32_t cx, int32_t cy, int radius, int max,
                                                             std::chrono::steady_clock::time_point now)
{
    std::vector<MonsterEntity*> out;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        if (static_cast<int>(out.size()) >= max) break;
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0 || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (static_cast<double>(radius) < static_cast<double>(dist_f(mx, my, cx, cy))) continue;
        if (owned_by_other(*m)) continue;
        out.push_back(m);
    }
    return out;
}

// sub_891C80 on the object a skill selected: a monster by its level, a player by its own.
void GameClientSession::skill_hit_tm_obj(uint16_t id, uint16_t skill)
{
    if (const MonsterEntity* m = server_.entity_mgr().get_monster(id)) { skill_hit_tm(*m, skill); return; }
    if (id == static_cast<uint16_t>(object_id_)) { skill_hit_tm_level(level_, skill); return; }
    if (GameClientSession* s = server_.player_session(id)) skill_hit_tm_level(s->level_, skill);
}

// ── ESAction 1086 "Mana Ring" (GameServer sub_5C39C0, ESAMagicRing; ESAUI 1: an object) ──────────────
//  The selected object (zone vtbl+16, kind 3) is the ring's center: its position (+144). The row
//  (ESAction_Magic_Ring): ApplyRatio +0, AP +4, Radius +8 — Radius as a float, AP an int. A player adds the
//  learned Mana Ring Booster's (2012, a "Surpport" passive) row AP (sub_50F090) and takes its ApplyRatio
//  (sub_8938A0; 0 / not learned -> the Mana Ring row's). The block: the opener sub_5EE050 ([me][me][0]...)
//  + [u16 the selected id] (VERIFIED: the client's ESAMagicRing sub_4BC930 reads one u16), then sub_5C3E10:
//  each monster of the list within Radius of the center, at most 7 — the magic damage sub_5E6270(AP,
//  ApplyRatio, MA offset 0, element 128 = NoPropR), its damage scaled (int)(dmg * (1 - d / Radius)) (the
//  result flags stay those of the whole hit — retail), the EXP share of the scaled damage, the damage event
//  30002 and EXP 30003 kind 1. Then the skill's TM per hit on the selected object (sub_891C80). No mark, no
//  30107 follow-up.
void GameClientSession::esa_magic_ring(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t sel = su_.target;
    int32_t cx, cy;
    if (!sel || !skill_target_pos(sel, now, cx, cy)) return;
    const float radius = static_cast<float>(act ? static_cast<int32_t>(act->get("Radius", 0.0)) : 0);
    int32_t ap  = act ? static_cast<int32_t>(act->get("AP", 0.0)) : 0;
    float apply = 0.0f;
    if (const LearnedSkill* b = find_skill(2012)) {
        const SkillDef* bd = server_.skill_db().get(2012);
        const EsaRow* r = (bd && b->level > 0) ? server_.esa_db().row(bd->esa_table, b->level) : nullptr;
        if (r) {
            if (bd->support) ap += static_cast<int32_t>(r->get("AP", 0.0));    // sub_50F090: +4
            apply = static_cast<float>(r->get("ApplyRatio", 0.0));             // sub_8938A0: +0
        }
    }
    if (apply <= 0.0f) apply = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;

    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(sel);
    group.push_back(std::move(o));
    struct Hit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };
    std::vector<Hit> hits;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0 || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        const float dd = dist_f(mx, my, cx, cy);
        if (!(static_cast<double>(radius) >= static_cast<double>(dd))) continue;
        if (static_cast<int>(hits.size()) >= kAreaMaxTargets) break;
        const float f = static_cast<float>(1.0 - static_cast<double>(dd / radius));
        SkillDamage h = skill_magic_damage(*m, ap, apply, 0, 128);
        h.dmg = static_cast<int32_t>(static_cast<double>(h.dmg) * f);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        hits.push_back({ m, h.dmg, hp_before, killed });
        LOG_DEBUG("GameClientSession %u: Mana Ring -> mob %u d=%.1f f=%.2f dmg=%d result=%d hp=%d/%d%s", session_id(),
                  oid, dd, f, h.dmg, h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "");
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : hits) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now, 1);
    skill_hit_tm_obj(sel, su_.skill);
    LOG_DEBUG("GameClientSession %u: Mana Ring at %u (%d,%d) AP %d radius %.0f apply %.2f: %zu hit", session_id(), sel,
              cx, cy, ap, radius, apply, hits.size());
}

// ── ESAction 1087 "Mana Storm" (GameServer sub_5C4110; ESAUI 6: a point) ────────────────────────────
//  The row (ESAction_Magic_Storm): ApplyRatio +0, AP +4, Radius +8; the chosen point (the payload's
//  [u16 x][u16 y]). No packet of its own: sub_5C4410 walks the monster list — within Radius of the point,
//  at most 7, a monster owned by another player (+1212, sub_4269D0; but our party's, sub_716EF0:
//  owned_by_other) left alone — and puts on each an ESAMagicStormHit (sub_5C4360: the action {ESAction 1088, EState
//  0, ESClass -1, skill 2009, flags 2, the storm's row} through sub_524690(.., 1): ESClass -1 -> made at
//  once, no limiter).
//  ESAMagicStormHit (1088): its init sub_54CBD0 — AP and ApplyRatio of the row, the target's motion stops
//  (sub_41C5D0), the target takes the condition 1 (sub_50D650: it may not move), kept whatever the apply
//  roll (+184 = 1), state 0, the block {S2C 224 sub_5EDCD0 [me][target][ES id][1088][2][ESClass -1][2009]
//  [0], no tail} (VERIFIED: the client's ESAMagicStormHit sub_4BCC20 reads none), the skill's TM per hit
//  (sub_891C80). Its state machine (sub_54CE00): 900 ms in state 0, then the hit sub_54CF60 — the condition
//  1 off (once, +252), our mark on it (vtbl+132), the magic damage sub_5E6270(AP, ApplyRatio, MA offset 0,
//  element 128), the EXP share, the damage event in a block, EXP 30003 kind 3 — and state 1; 900 ms later
//  its end (vtbl+28): S2C 280 [target][ES id] to the area, then sub_54D230 (the condition 1 off if still
//  on, our mark with 5000). We or the target gone -> it ends at once (sub_54CDA0).
void GameClientSession::esa_magic_storm(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    int16_t px = 0, py = 0;
    if (su_.payload.size() >= 4) {
        px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
        py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
    }
    const int32_t ap     = act ? static_cast<int32_t>(act->get("AP", 0.0)) : 0;
    const float   radius = static_cast<float>(act ? static_cast<int32_t>(act->get("Radius", 0.0)) : 0);
    const float   apply  = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    int n = 0;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0 || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        const float dd = dist_f(mx, my, px, py);
        if (!(static_cast<double>(radius) >= static_cast<double>(dd))) continue;
        if (n >= kAreaMaxTargets) break;
        if (owned_by_other(*m)) continue;      // another player's, not of our share group
        ++n;
        // sub_524690 -> InitESAction + ESAMagicStormHit::Init (sub_54CBD0)
        CastEs e;
        e.id       = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());
        e.esaction = 1088;
        e.skill    = 2009;
        e.target   = static_cast<uint16_t>(oid);
        e.ap       = ap;
        e.apply    = apply;
        e.state    = 0;
        e.elapsed  = 0;
        e.released = false;
        m->motion.rest(mx, my);                                         // sub_41C5D0 on the target
        m->pos_x = mx; m->pos_y = my;
        m->cond_add(1);                                                  // sub_50D650(1)
        {
            std::vector<PacketBuffer> group;
            PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());     // sub_5EDCD0
            o.write<uint16_t>(static_cast<uint16_t>(object_id_));
            o.write<uint16_t>(e.target);
            o.write<uint16_t>(e.id);
            o.write<uint16_t>(1088);
            o.write<uint8_t>(2);
            o.write<uint8_t>(0xFF);                                      // ESClass -1
            o.write<uint16_t>(2009);
            o.write<uint16_t>(0);
            group.push_back(std::move(o));
            server_.broadcast_block(zone_id_, group, 0);
        }
        cast_es_.push_back(e);
        skill_hit_tm(*m, 2009);                                          // sub_891C80
        LOG_DEBUG("GameClientSession %u: Mana Storm hit %u on mob %u (AP %d)", session_id(), e.id, oid, ap);
    }
    LOG_DEBUG("GameClientSession %u: Mana Storm at (%d,%d) radius %.0f: %d targets", session_id(), px, py, radius, n);
    (void)d;
}

// ESAMagicStormHit's hit (sub_54CF60).
void GameClientSession::storm_hit_damage(CastEs& e)
{
    MonsterEntity* m = server_.entity_mgr().get_monster(e.target);
    if (!m) return;
    if (!e.released) { e.released = true; m->cond_remove(1); }        // sub_50D7C0(1)
    if (!m->alive || m->hp <= 0) return;                               // (a corpse takes no damage event)
    mark_monster_owner(*m);                                            // vtbl+132(target, 0)
    const auto now = std::chrono::steady_clock::now();
    const bool earth = (e.esaction == 1160 || e.esaction == 1266);    // ESAEarthDivideHit sub_55CAF0 /
    SkillDamage h = skill_magic_damage(*m, e.ap, e.apply, earth ? 25 : 0, earth ? 8 : 128);   //   ESAEarthquakeDo
                                                                       //   sub_571B30: MA offset 25, Earth
    const int32_t hp_before = m->hp;
    std::vector<PacketBuffer> group;
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);                       // sub_54D1F0: the init's block again
    LOG_DEBUG("GameClientSession %u: Mana Storm hit %u -> mob %u dmg=%d result=%d hp=%d/%d%s", session_id(), e.id,
              e.target, h.dmg, h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "");
    // (ESAEarthDivideHit / ESAEarthquakeDo post their 30003 only when we may attack the target, vtbl+164 —
    // ESAEarthquakeDo before its 30002: our EXP goes after the block either way)
    if (earth && !may_attack_owned(*m)) {
        skill_after_hit(*m, h.dmg, hp_before, killed, 0, true, now, 3);   // (exp_done: none comes)
        return;
    }
    skill_after_hit(*m, h.dmg, hp_before, killed, 0, false, now, 3);
}

// Our CastEs' state machines and ticks (ESAMagicStormHit sub_54CE00 / sub_54CDA0).
void GameClientSession::cast_es_tick(int elapsed_ms)
{
    for (size_t i = 0; i < cast_es_.size();) {
        CastEs& e = cast_es_[i];
        MonsterEntity* m = server_.entity_mgr().get_monster(e.target);
        if (!m || m->zone != zone_id_) { cast_es_end(i); continue; }   // the target is gone
        if ((e.esaction == 1160 || e.esaction == 1266) && e.result != 1) { cast_es_end(i); continue; }   // the base
                                                                        // tick: not applied, not kept
        if (e.esaction == 1198) {                                       // ESAWhirlWindDo sub_566DF0: 300 ms
            if (e.result != 1) { cast_es_end(i); continue; }
            e.elapsed += elapsed_ms;
            if (e.elapsed > 300) { cast_es_end(i); continue; }
            ++i;
            continue;
        }
        e.elapsed += elapsed_ms;
        const int phase = (e.esaction == 1160 || e.esaction == 1266) ? 500 : kStormPhaseMs;   // sub_55C990 /
        if ((e.esaction == 1088 || e.esaction == 1160 || e.esaction == 1266) && e.elapsed >= phase) {   // sub_5719D0
            e.elapsed = 0;
            if (e.state == 0) {
                storm_hit_damage(e);
                cast_es_[i].state = 1;
            } else {
                cast_es_end(i);
                continue;
            }
        }
        ++i;
    }
}

// vtbl+28 (sub_50F770): S2C 280 [target][ES id] to the area (flags 2), then the class's vtbl+100
// (ESAMagicStormHit sub_54D230: the condition 1 off if still on, our mark with 5000).
void GameClientSession::cast_es_end(size_t i)
{
    if (i >= cast_es_.size()) return;
    const CastEs e = cast_es_[i];
    cast_es_.erase(cast_es_.begin() + static_cast<std::ptrdiff_t>(i));
    server_.entity_mgr().free_oid(e.id);
    MonsterEntity* m = server_.entity_mgr().get_monster(e.target);
    if (m) {
        PacketBuffer p(static_cast<Opcode>(0x0118), is_paula());
        p.write<uint16_t>(e.target);
        p.write<uint16_t>(e.id);
        server_.broadcast_zone(m->zone, p, 0);
        if (e.esaction == 1088 || e.esaction == 1160 || e.esaction == 1266) {   // sub_54D230 / sub_55CD80 / sub_571D90
            if (!e.released) m->cond_remove(1);
            if (m->alive && m->zone == zone_id_ && !dead_) mark_monster_owner(*m);
        }
    }
    LOG_DEBUG("GameClientSession %u: cast ES %u (ESAction %d) on mob %u ended", session_id(), e.id, e.esaction, e.target);
}

void GameClientSession::cast_es_end_all()
{
    while (!cast_es_.empty()) cast_es_end(cast_es_.size() - 1);
}
