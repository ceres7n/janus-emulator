#include "game_session.hpp"
#include "game_server.hpp"
#include "logger.hpp"
#include "combat_util.hpp"
#include "paula_crypt.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
//  The Sorcerer (Dragon, 2nd job) and its 3rd jobs, the Priest and the Dark Lord — 1:1 with the retail
//  GameServer (GameServerRelease), the client's classes verified in Trickster.bin (each class's vtbl+132 reads
//  the opener's tail).
//
//  Their magic adds no element value of ours (the rows' AP alone); dark is element 64 (DarkR, slot 17), light 32
//  (LightR, 16). The pacts (2202 / 2204) are passives only the skill learning reads (an exclusive pair,
//  sub_760A20); Wicked Flame (2206) and Withering Disease (2217) are the passives Hellfire and Gravity Crush follow
//  up with (passive_follow_es). Their effect states live in es.cpp.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
float row_f(const EsaRow* r, const char* k) { return r ? static_cast<float>(r->get(k, 0.0)) : 0.0f; }
int   row_i(const EsaRow* r, const char* k) { return r ? static_cast<int>(r->get(k, 0.0)) : 0; }
} // namespace

// ── ESAction 1163 "Hellfire" (GameServer sub_5C85E0, ESAHellFire) / 1164 "Arrow of Light" (sub_5C8B00,
//    ESALightningBolt) / 1187 "Dark Lance" (sub_5CA660, ESADarkSpear); ESAUI 1 ───────────────────────────
//  The selected target (zone vtbl+16 kind 3) — none: nothing; we mark it (vtbl+132, not on ourselves). The rows
//  (sub_51B310 / sub_51B360 / sub_51B310: ApplyRatio +0, AP +4, Comment +8): the magic damage sub_5E6270(AP,
//  ApplyRatio, MA offset 49, element 64 = Dark / 32 = Light / 64), its EXP share; the block {the opener + [u16
//  target][pos us][pos target] (sub_5C88A0 / sub_5C8D50 / sub_5CA8B0; VERIFIED: the clients' ESAHellFire 0x4D3310 /
//  ESALightningBolt 0x4D3960 / ESADarkSpear 0x4DB680 read them), the damage event, EXP 30003 kind 1}; the skill's
//  TM per hit. Hellfire then, with HP left on the target after a hit (result not 1 / 2 / 8 / 6, damage >= 0;
//  sub_5C88E0): Wicked Flame (2206) learned -> its ESACruelFlameDo (1178, cruel_flame_on).
void GameClientSession::esa_dark_light_bolt(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    mark_monster_owner(*m);
    const int elem = (d.esaction == 1164) ? 32 : 64;
    const SkillDamage h = skill_magic_damage(*m, row_i(act, "AP"), row_f(act, "ApplyRatio"), 49, elem);
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
    LOG_DEBUG("GameClientSession %u: %s -> mob %u dmg=%d result=%d hp=%d/%d%s", session_id(),
              d.esaction == 1163 ? "Hellfire" : d.esaction == 1164 ? "Arrow of Light" : "Dark Lance", tid, h.dmg, h.result,
              m->hp, m->hp_max, killed ? " [KILLED]" : "");
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, false, now, 1);
    const int r = h.result;
    if (d.esaction == 1163 && m->alive && m->hp > 0 && r != 1 && r != 2 && r != 8 && r != 6 && h.dmg >= 0)
        passive_follow_es(*m, 2206, 1178);
}

// ESACruelFlameDo (1178, init sub_561090; the action carries Wicked Flame's row: ApplyRatio +0, Ratio +4, Seconds +8
// i16 — sub_51A360): the payload's target, us; not applied -> no object; our learned Hellfire (2203) row (none -> no
// object): the magic damage sub_5E6270(its AP, Wicked Flame's ApplyRatio, MA offset 49, element 64 = Dark) made once
// -> +260 (0 or less -> no object); +200 = ApplyRatio, +252 = 1000 * Seconds, +196 = Ratio; the block {S2C 224
// sub_5EDCD0 [us][target][ES id][1178][2][ESClass 1][2206][0], no tail — VERIFIED: the client's ESACruelFlameDo
// 0x4D9410 reads none}. No mark, no TM. Its tick (es.cpp, sub_561360): past +252 (>), us or the target gone, or its
// stop flag -> the end; past 2000 ms (>) a burn (cruel_burn) — then a monster of Type 13 stops it —; the base tick.
bool GameClientSession::cruel_flame_on(MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act)
{
    if (e.result != 1) return false;
    const LearnedSkill* hf = find_skill(2203);
    const SkillDef* sd = server_.skill_db().get(2203);
    if (!hf || hf->level <= 0 || !sd) return false;
    const EsaRow* hrow = server_.esa_db().row(sd->esa_table, hf->level);
    const SkillDamage h = skill_magic_damage(m, row_i(hrow, "AP"), row_f(act, "ApplyRatio"), 49, 64);
    if (h.dmg <= 0) return false;
    e.value      = h.dmg;                                   // +260
    e.base_apply = row_f(act, "ApplyRatio");                // +200
    e.ratio      = row_f(act, "Ratio");                     // +196
    e.end_ms     = 1000 * static_cast<int16_t>(row_i(act, "Seconds"));   // +252
    e.pulse_ms   = 0;                                       // +256
    e.phase      = 0;                                       // +264: the stop flag
    e.flags      = 2;
    {
        std::vector<PacketBuffer> group;
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(1178);
        o.write<uint8_t>(2);
        o.write<uint8_t>(static_cast<uint8_t>(e.esclass));
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(static_cast<uint16_t>(e.estate));
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    m.es.push_back(e);
    LOG_DEBUG("GameClientSession %u: Wicked Flame %u on mob %u: %d (x%.2f) for %d ms", session_id(), e.id,
              static_cast<unsigned>(m.object_id), e.value, e.ratio, e.end_ms);
    return true;
}

// ESACruelFlameDo's burn (sub_5614B0): us and the target here, +260 > 0: the roll sub_510060(+200, 0) — applied: the
// block {DamageInfo {+260 (a monster of Type 13) else (int)(+260 * +196), death type 3, result 0, element 64}, its EXP
// share, 30002, then 30003 kind 1 when we may attack it (vtbl+164)}.
void GameClientSession::cruel_burn(uint16_t mob, uint16_t es_id)
{
    if (leaving_ || state_ != GameClientSessionState::InWorld) return;
    MonsterEntity* m = server_.entity_mgr().get_monster(mob);
    if (!m || m->zone != zone_id_ || !m->alive || m->hp <= 0) return;
    int32_t base = 0;
    float ratio = 0.0f, apply = 0.0f;
    {
        std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
        const MonsterEntity::Es* e = nullptr;
        for (const MonsterEntity::Es& x : m->es) if (x.id == es_id) { e = &x; break; }
        if (!e) return;
        base = e->value; ratio = e->ratio; apply = e->base_apply;
    }
    if (base <= 0 || apply_roll(apply, 0) != 1) return;
    const auto now = std::chrono::steady_clock::now();
    SkillDamage h;
    const MobDef* md = server_.mob_db().get(static_cast<int>(m->mob_id));
    h.dmg        = (md && md->type == 13) ? base : static_cast<int32_t>(static_cast<double>(base) * static_cast<double>(ratio));
    h.death_type = 3;
    h.result     = 0;
    h.element    = 64;
    const int32_t hp_before = m->hp;
    std::vector<PacketBuffer> group;
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*m, h.dmg, hp_before, killed, 0, !may_attack_owned(*m), now, 1);
}

// ── ESAction 1168 "Catastrophe Healing" (GameServer sub_5C92A0, ESAFirstAid; ESAUI 0) ─────────────────────
//  The row ESAction_FirstAid (sub_51A4C0: ApplyRatio +0, Ratio +4, Comment +8): the heal = (int)((our MA + LK) * 0.5
//  * Ratio); our HP + it (at most MaxHP) — healed when it rose from below MaxHP: S2C 0x24 [u32 HP] (sub_5F4670) —;
//  the block {the opener + [u8 healed][u32 the heal]} (VERIFIED: the client's ESAFirstAid 0x4D7540 reads them);
//  healed: the skill's use TM.
void GameClientSession::esa_first_aid(const SkillDef& d, const EsaRow* act)
{
    const int32_t heal = static_cast<int32_t>(static_cast<double>(stat_value(8) + stat_value(4)) * 0.5 *
                                              static_cast<double>(row_f(act, "Ratio")));
    int32_t v = heal + hp_;
    const bool healed = (v > hp_) && hp_ < hp_max_;
    if (v > hp_max_) v = hp_max_;
    hp_ = v;
    if (healed) {
        PacketBuffer p(static_cast<Opcode>(0x0024), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(hp_));
        send(p);
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint8_t>(healed ? 1 : 0);
    o.write<uint32_t>(static_cast<uint32_t>(heal));
    group.push_back(std::move(o));
    server_.broadcast_block(zone_id_, group, 0);
    if (healed) skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Catastrophe Healing +%d -> HP %d/%d", session_id(), heal, hp_, hp_max_);
}

// ── ESAction 1190 "Light Wave" (GameServer sub_5CA8F0, ESALightWave; ESAUI 1) ─────────────────────────────
//  The selected target (kind 3) — none: nothing. The row ESAction_LightWave (sub_51B9E0: ApplyRatio +0, AP +4, Radius
//  +8 i16, Comment +10, Angle +12 float). sub_5CAAA0 walks the zone's monsters in list order — not dead, our layer,
//  not Type 8 / 12, a shadow one only with the Third Eye — within Radius of the TARGET; such a one once 7 are taken
//  stops the walk; the target itself is taken, any other only in line from us through the target (sub_893D30 with
//  the Angle) and without an owner or ours (our party's). (PvP players sub_5CADB0.) The block: the opener + [u16
//  target][u16 n][u16 id]*n (VERIFIED: the client's ESALightWave 0x4DC190 reads them), then each (sub_5CAFC0): the
//  magic damage sub_5E6270(AP, ApplyRatio, MA offset 49, element 32 = Light), its EXP share, 30002, then 30003 kind
//  0. No mark, no TM.
void GameClientSession::esa_light_wave(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_) return;
    const int16_t radius = static_cast<int16_t>(row_i(act, "Radius"));
    const float   angle  = row_f(act, "Angle");
    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    t->motion.pos(now, tx, ty);
    std::vector<MonsterEntity*> arr;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0 || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (!(static_cast<double>(dist_f(mx, my, tx, ty)) <= static_cast<double>(radius))) continue;
        if (arr.size() >= 7) break;
        if (m == t) { arr.push_back(m); continue; }
        if (!server_in_line(static_cast<int16_t>(ax), static_cast<int16_t>(ay), static_cast<int16_t>(tx),
                            static_cast<int16_t>(ty), static_cast<int16_t>(mx), static_cast<int16_t>(my), angle))
            continue;
        if (owned_by_other(*m)) continue;
        arr.push_back(m);
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    o.write<uint16_t>(static_cast<uint16_t>(arr.size()));
    for (MonsterEntity* m : arr) o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
    group.push_back(std::move(o));
    const int32_t ap    = row_i(act, "AP");
    const float   apply = row_f(act, "ApplyRatio");
    struct Hit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };
    std::vector<Hit> done;
    for (MonsterEntity* m : arr) {
        const SkillDamage h = skill_magic_damage(*m, ap, apply, 49, 32);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now, 0);
    LOG_DEBUG("GameClientSession %u: Light Wave through mob %u: %zu hit", session_id(), tid, arr.size());
}

// ── ESAction 1215 "Gravity Crush" (GameServer sub_5CDF40, ESAGravity; ESAUI 0) ───────────────────────────
//  The row ESAction_Gravity (sub_51BC50: ApplyRatio +0, AP +4, Radius +8 i16, KillNum +10 i16, Comment +12).
//  sub_5CE0C0: sub_53E360's walk around us, at most min(7, KillNum), Radius. The block: the opener + [u16 n][u16 id]*n
//  (VERIFIED: the client's ESAGravity 0x4E32C0 reads them, at most 7), then each (sub_5CE540): the magic damage
//  sub_5E6270(AP, ApplyRatio, MA offset 49, element 64 = Dark), its EXP share, 30002, 30003 kind 1 — then, a hit
//  (result not 2 / 1 / 6 / 8) on a target with HP left and Withering Disease (2217) learned: its ESAUnableRecoveryDo
//  (1332, sub_5CE750; unable_recovery_on). Then the skill's use TM.
void GameClientSession::esa_gravity(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    int32_t ax, ay;
    cur_pos(now, ax, ay);
    const int kill = static_cast<int16_t>(row_i(act, "KillNum"));
    const std::vector<MonsterEntity*> arr =
        area_monsters(ax, ay, static_cast<int16_t>(row_i(act, "Radius")), std::max(0, std::min(7, kill)), now);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(static_cast<uint16_t>(arr.size()));
    for (MonsterEntity* m : arr) o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
    group.push_back(std::move(o));
    const int32_t ap    = row_i(act, "AP");
    const float   apply = row_f(act, "ApplyRatio");
    struct Hit { MonsterEntity* m; int32_t dmg, hp_before, result; bool killed; };
    std::vector<Hit> done;
    for (MonsterEntity* m : arr) {
        const SkillDamage h = skill_magic_damage(*m, ap, apply, 49, 64);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, h.result, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : done) {
        skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now, 1);
        const int r = h.result;
        if (r != 2 && r != 1 && r != 6 && r != 8 && h.m->alive && h.m->hp > 0) passive_follow_es(*h.m, 2217, 1332);
    }
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Gravity Crush: %zu hit", session_id(), arr.size());
}

// ESAUnableRecoveryDo (1332, init sub_526BB0; the action carries Withering Disease's row: ApplyRatio +0, Seconds +4
// i16 — sub_51AB60): +196 = 1000 * Seconds, the lifetime; the block {S2C 224 sub_5EDE00 [us][target][ES id][1332]
// [2][ESClass 1][2217][0] + [u8 +180]} (VERIFIED: the client's ESAUnableRecoveryDo 0x5044A0 reads the u8); applied
// and not yet on: the target's condition 0xC0000 (no recovery). No mark, no TM. Its tick (sub_526DB0): past +196 the
// end; its end (sub_526E00): the condition off.
bool GameClientSession::unable_recovery_on(MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act)
{
    e.end_ms  = 1000 * static_cast<int16_t>(row_i(act, "Seconds"));
    e.life_ms = e.end_ms;
    e.flags   = 2;
    {
        std::vector<PacketBuffer> group;
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(1332);
        o.write<uint8_t>(2);
        o.write<uint8_t>(static_cast<uint8_t>(e.esclass));
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(static_cast<uint16_t>(e.estate));
        o.write<uint8_t>(static_cast<uint8_t>(e.result));
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    if (e.result == 1 && !m.cond_test(0xC0000)) m.cond_add(0xC0000);
    m.es.push_back(e);
    return true;
}

// ── ESAction 1219 "Basic Healing" (GameServer sub_5CF880, ESAPrimaryCure; ESAUI 1) ───────────────────────
//  The selected object (zone vtbl+16 kind 3: a player or a monster) — none: nothing. The row ESAction_PrimaryCure
//  (sub_51BD20: ESAction0..ESAction10, ints): each one above 1000 leaves the target's limiter (vtbl+60, by ESAction —
//  removed: cured); the block {S2C 224 sub_5EDE00 [us][target][0]... + [u8 cured]} (VERIFIED: the client's
//  ESAPrimaryCure 0x4E5A80 reads the u8); the skill's use TM. (Another player is cured on its strand.)
void GameClientSession::esa_primary_cure(const SkillDef& d, const EsaRow* act)
{
    const uint16_t tid = su_.target;
    std::vector<int> list;
    for (int i = 0; i < 11; ++i) {
        char col[16];
        std::snprintf(col, sizeof(col), "ESAction%d", i);
        const int a = row_i(act, col);
        if (a > 1000) list.push_back(a);
    }
    bool cured = false;
    if (MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr) {
        if (m->zone != zone_id_) return;
        std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
        for (int a : list) {
            for (size_t j = 0; j < m->es.size(); ++j)
                if (m->es[j].esaction == a) { server_.monster_es_end(*m, j); cured = true; break; }
        }
    } else if (tid == static_cast<uint16_t>(object_id_)) {
        for (int a : list) if (es_remove_esaction(a)) cured = true;
    } else if (GameClientSession* p = tid ? server_.player_session(tid) : nullptr) {
        if (p->zone_id() != zone_id_) return;
        // another player: its limiter on its strand, the opener sent from there (no ES of these on players yet)
        const uint16_t self = static_cast<uint16_t>(object_id_);
        const uint16_t sk = su_.skill;
        const SkillDef dd = d;
        p->run_on_strand([p, list, self, sk, dd]() { p->cure_from(self, list, dd, sk); });
        skill_use_tm(su_.skill);
        return;
    } else {
        return;
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, tid);
    o.write<uint8_t>(cured ? 1 : 0);
    group.push_back(std::move(o));
    server_.broadcast_block(zone_id_, group, 0);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Basic Healing on %u: cured %d", session_id(), tid, cured ? 1 : 0);
}

// Basic Healing / Med. Cure from another player (or Med. Cure from ourselves), on our strand: the listed ESActions leave
// our limiter; the opener [caster][us][0]... + [u8 cured] to the area. True: cured.
bool GameClientSession::cure_from(uint16_t caster, const std::vector<int>& list, const SkillDef& d, uint16_t skill)
{
    if (leaving_ || state_ != GameClientSessionState::InWorld) return false;
    bool cured = false;
    for (int a : list) if (es_remove_esaction(a)) cured = true;
    std::vector<PacketBuffer> group;
    PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
    o.write<uint16_t>(caster);
    o.write<uint16_t>(static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(0);
    o.write<uint16_t>(static_cast<uint16_t>(d.esaction));
    o.write<uint8_t>(2);
    o.write<uint8_t>(static_cast<uint8_t>(d.esclass));
    o.write<uint16_t>(skill);
    o.write<uint16_t>(static_cast<uint16_t>(d.estate));
    o.write<uint8_t>(cured ? 1 : 0);
    group.push_back(std::move(o));
    server_.broadcast_block(zone_id_, group, 0);
    return cured;
}

// ── ESAction 1256 "Radiant Strike" (GameServer sub_5CF9F0, ESAShiningRay; ESAUI 0) ───────────────────────
//  The row ESAction_ShiningRay (sub_51BFC0: ApplyRatio +0, AP +4, Radius +8 i16). sub_5CFB60: sub_53E360's walk around
//  us, at most 3, Radius. The block: the opener + [u16 n][u16 id]*n (VERIFIED: the client's ESAShiningRay 0x4E8BE0
//  reads them), then each (sub_5CFFC0): the magic damage sub_5E6270(AP, ApplyRatio, MA offset 49, element 32 =
//  Light), its EXP share, 30002, then 30003 kind 1. Then the skill's use TM.
void GameClientSession::esa_shining_ray(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    int32_t ax, ay;
    cur_pos(now, ax, ay);
    const std::vector<MonsterEntity*> arr = area_monsters(ax, ay, static_cast<int16_t>(row_i(act, "Radius")), 3, now);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(static_cast<uint16_t>(arr.size()));
    for (MonsterEntity* m : arr) o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
    group.push_back(std::move(o));
    const int32_t ap    = row_i(act, "AP");
    const float   apply = row_f(act, "ApplyRatio");
    struct Hit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };
    std::vector<Hit> done;
    for (MonsterEntity* m : arr) {
        const SkillDamage h = skill_magic_damage(*m, ap, apply, 49, 32);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now, 1);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Radiant Strike: %zu hit", session_id(), arr.size());
}

// ── ESAction 1323 "Heart's Grace" (GameServer sub_5D7E80, ESAWideFirstaid; ESAUI 0) ──────────────────────
//  The row ESAction_WideFirstaid (sub_51BF50: ApplyRatio +0, Ratio +4, Second +8, Radius +10 i16, Comment +12). The
//  zone's players (zone +28) in our party (sub_716EF0: our share group, us included; none: ourselves), not dead
//  (sub_41C790), within Radius of us (no layer test): the heal = (int)(its MaxHP * Ratio) + (int)(our MA * Ratio *
//  40.0); its HP + it (at most MaxHP) — healed when it rose from below MaxHP —; its block {S2C 224 sub_5EDE00 [us][it]
//  [0]... + [u8 healed][u32 the heal]} (VERIFIED: the client's ESAWideFirstaid 0x5000D0 reads them) with, healed, S2C
//  0x24 [u32 HP] to it inside; healed: the skill's use TM for us (for each one). (A member's part runs on its strand:
//  grace_heal.)
void GameClientSession::esa_wide_first_aid(const SkillDef& d, const EsaRow* act)
{
    const float ratio = row_f(act, "Ratio");
    const int32_t radius = static_cast<int16_t>(row_i(act, "Radius"));
    const int32_t ma_part =
        static_cast<int32_t>(static_cast<double>(stat_value(4)) * static_cast<double>(ratio) * 40.0);
    const auto now = std::chrono::steady_clock::now();
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    const uint16_t self = static_cast<uint16_t>(object_id_);
    size_t n = 0;
    for (uint32_t oid : server_.entity_mgr().players_in_zone(zone_id_)) {
        GameClientSession* s = server_.player_session(oid);
        if (!s || !s->object_id_ || !s->in_world()) continue;
        if (s != this && !server_.party().same_group(char_uid_, s->char_uid_)) continue;
        if (s->dead_) continue;
        int32_t x, y;
        s->cur_pos(now, x, y);
        if (!(static_cast<double>(dist_f(x, y, cx, cy)) <= static_cast<double>(radius))) continue;
        PacketBuffer o = skill_opener(d, static_cast<uint16_t>(s->object_id_));
        if (s == this) grace_heal(o, self, su_.skill, ratio, ma_part, zone_id_);
        else s->grace_heal_pulse(o, self, su_.skill, ratio, ma_part, zone_id_);
        ++n;
    }
    LOG_DEBUG("GameClientSession %u: Heart's Grace (ratio %.3f, radius %d): %zu player(s)", session_id(), ratio, radius, n);
}

// Heart's Grace on us (from ourselves or a party member in `zone`): the heal, its block {the opener + [u8 healed][u32
// heal], healed: S2C 0x24 to us}, healed: the caster's use TM.
void GameClientSession::grace_heal(PacketBuffer o, uint16_t caster, uint16_t skill, float ratio, int32_t ma_part,
                                   int16_t zone)
{
    if (leaving_ || state_ != GameClientSessionState::InWorld || zone_id_ != zone || !object_id_ || dead_) return;
    const int32_t heal = static_cast<int32_t>(static_cast<double>(hp_max_) * static_cast<double>(ratio)) + ma_part;
    int32_t v = heal + hp_;
    const bool healed = (v > hp_) && hp_ < hp_max_;
    if (v > hp_max_) v = hp_max_;
    hp_ = v;
    o.write<uint8_t>(healed ? 1 : 0);
    o.write<uint32_t>(static_cast<uint32_t>(heal));
    std::vector<PacketBuffer> mine, theirs;
    theirs.push_back(o);
    mine.push_back(std::move(o));
    if (healed) {                                            // (sub_5F4670 inside the block: to it alone)
        PacketBuffer p(static_cast<Opcode>(0x0024), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(hp_));
        mine.push_back(std::move(p));
    }
    server_.broadcast_block_split(zone_id_, session_id(), mine, theirs);
    if (healed) {
        if (caster == static_cast<uint16_t>(object_id_)) skill_use_tm(skill);
        else if (GameClientSession* c = server_.player_session(caster)) c->skill_use_tm_later(skill);
    }
    LOG_DEBUG("GameClientSession %u: Heart's Grace from %u +%d -> HP %d/%d", session_id(), caster, heal, hp_, hp_max_);
}

// ── ESAction 1474 "Revival" (GameServer sub_5DE620; ESAUI 1) ────────────────────────────────────────────
//  The selected player (zone vtbl+16 kind 1) — none: nothing. The row ESAction_Revival (sub_51DB50: fHPRestoreRatio
//  +4, fMpRestoreRatio +8, fExpRestoreRatio +12, fTmxpRestoreRatio +16): its revive (sub_764260 with them; only a dead
//  one not already reviving) -> S2C 760 [u16 it][u16 0] to its area (sub_601C90) and our use TM. (It runs on that
//  player's strand: revive_by_skill.)
void GameClientSession::esa_revival(const SkillDef& d, const EsaRow* act)
{
    const uint16_t tid = su_.target;
    GameClientSession* p = tid ? server_.player_session(tid) : nullptr;
    if (!p || p->zone_id() != zone_id_) return;
    p->revive_by_skill(object_id_, row_f(act, "fHPRestoreRatio"), row_f(act, "fMpRestoreRatio"),
                       row_f(act, "fExpRestoreRatio"), row_f(act, "fTmxpRestoreRatio"), su_.skill);
    (void)d;
}

void GameClientSession::revive_by_skill(uint32_t caster, float hp, float mp, float exp, float tmxp, uint16_t skill)
{
    run_on_strand([this, caster, hp, mp, exp, tmxp, skill]() {
        if (leaving_ || state_ != GameClientSessionState::InWorld) return;
        if (!revive(hp, mp, exp, tmxp)) return;
        PacketBuffer p(static_cast<Opcode>(0x02F8), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        p.write<uint16_t>(0);
        server_.broadcast_zone(zone_id_, p, 0);
        LOG_DEBUG("GameClientSession %u: revived by player %u's Revival", session_id(), caster);
        if (GameClientSession* c = server_.player_session(caster)) c->skill_use_tm_later(skill);
    });
}

// ESABind's ESABindDo on those around (sub_534410, applied): the zone's monsters in list order — our layer, not
// Type 8 / 12, a shadow one only with the Third Eye (no dead test: retail) — within (float)Radius of us; such a one
// once 7 are bound stops the walk; one without an owner or ours (our party's) gets (sub_5346A0) — a monster with a
// DebuffSet first rolls against Hinder Snare's (2201) DebuffBitCode (all resisted: S2C 1247, none) — the action
// {ESAction 1155, EState 0, ESClass 1, flags 2, skill 2201, the Bind's row} through sub_524690(.., 1): each one made
// joins the Bind's list.
void GameClientSession::bind_children(ActiveEs& b)
{
    const auto now = std::chrono::steady_clock::now();
    const SkillDef* sd = server_.skill_db().get(2201);
    if (!sd) return;
    const EsaRow* row = server_.esa_db().row(sd->esa_table, b.level);
    const uint16_t bid = b.id;
    int32_t ax, ay;
    cur_pos(now, ax, ay);
    const float radius = static_cast<float>(b.radius);
    int n = 0;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (!(static_cast<double>(radius) >= static_cast<double>(dist_f(mx, my, ax, ay)))) continue;
        if (n >= 7) break;
        if (owned_by_other(*m)) continue;
        uint32_t bits = 0;
        if (monster_debuff_immune(*m, sd->debuff_bits, bits)) { send_debuff_resisted(*m, bits); continue; }
        SkillDef dd = *sd;
        dd.esaction = 1155;
        dd.estate   = 0;
        dd.esclass  = 1;
        const size_t before = m->es.size();
        es_apply_on(*m, dd, b.level, row);
        if (m->es.size() > before && m->es.back().esaction == 1155) {
            size_t k = 0;
            while (k < es_.size() && es_[k].id != bid) ++k;
            if (k < es_.size()) es_[k].ids.push_back(static_cast<uint16_t>(oid));
            ++n;
        }
    }
    LOG_DEBUG("GameClientSession %u: Hinder Snare binds %d", session_id(), n);
}

// ESABindDo (1155, init sub_55AF80; the action carries the Bind's row: Seconds +8): +220 = 1000 * Seconds; us and the
// target here; its Prevent Stun (1398) -> no object; the block {S2C 224 sub_5EDCD0 [us][target][ES id][1155][2]
// [ESClass 1][2201][0], no tail — VERIFIED: the client's ESABindDo 0x4D0300 reads none}; our mark with 5000
// (vtbl+132); the stun whatever the roll (sub_55B100: it stops, the condition 15); kept unapplied too (+184 = 1). Its
// tick (sub_55B0A0): past +220 the end; its end (sub_55B160): the condition 15 off, then sub_717DB0 on us — every
// monster we own lets us go (retail).
bool GameClientSession::bind_do_on(MonsterEntity& m, MonsterEntity::Es& e)
{
    for (const MonsterEntity::Es& o : m.es) if (o.esaction == 1398) return false;
    const auto now = std::chrono::steady_clock::now();
    e.flags = 2;
    e.stay  = true;
    e.life_ms = 0;
    {
        std::vector<PacketBuffer> group;
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(1155);
        o.write<uint8_t>(2);
        o.write<uint8_t>(static_cast<uint8_t>(e.esclass));
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(static_cast<uint16_t>(e.estate));
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    mark_monster_owner(m);
    int32_t x, y;
    m.motion.pos(now, x, y);
    m.motion.rest(x, y);
    m.pos_x = x; m.pos_y = y;
    m.cond_add(15);
    m.es.push_back(e);
    return true;
}

// ESADarkSlow (1322, Dark Distractor, init sub_57F7F0; row sub_51C920: ApplyRatio +0, DownValue +8 i16, Seconds +10
// i16): +196 = DownValue, +200 = Seconds; us and the target here; we may attack it (sub_5E6D70: not Type 8 / 12) —
// else no object; the lifetime 1000 * Seconds; the block {S2C 224 sub_5EDE00 [us][target][ES id]... + [u8 +180]}
// (VERIFIED: the client's ESADarkSlow 0x4FFA10 reads the u8); applied: the cut (sub_57FBC0(0): v = DownValue / 100;
// its walk / run / chase speeds -= v * 100 / 160 / 130, kept on the target, +1376..+1384) and our TM per hit. Its
// tick (sub_57FB10): past 1000 * Seconds, or us / the target gone -> the end; its end (sub_57FA20): applied -> the
// speeds back.
bool GameClientSession::dark_slow_on(MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act)
{
    if (const MobDef* md = server_.mob_db().get(static_cast<int>(m.mob_id)))
        if (md->type == 8 || md->type == 12) return false;
    e.value   = static_cast<int16_t>(row_i(act, "DownValue"));
    e.end_ms  = 1000 * static_cast<int16_t>(row_i(act, "Seconds"));
    e.life_ms = e.end_ms;
    {
        std::vector<PacketBuffer> group;
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(1322);
        o.write<uint8_t>(2);
        o.write<uint8_t>(static_cast<uint8_t>(e.esclass));
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(static_cast<uint16_t>(e.estate));
        o.write<uint8_t>(static_cast<uint8_t>(e.result));
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    if (e.result == 1) {
        const double v = static_cast<double>(e.value) / 100.0;
        m.walk_speed  = m.walk_speed  - static_cast<float>(v * 100.0);
        m.run_speed   = m.run_speed   - static_cast<float>(v * 160.0);
        m.chase_speed = m.chase_speed - static_cast<float>(v * 130.0);
        skill_hit_tm(m, e.skill);
    }
    m.es.push_back(e);
    LOG_DEBUG("GameClientSession %u: Dark Distractor on mob %u: -%d%% speed for %d ms (result %d)", session_id(),
              static_cast<unsigned>(m.object_id), e.value, e.end_ms, e.result);
    return true;
}

// ESADarkBarrier's 30002 back to the attacker (its handler sub_56D7E0): the full damage event on it from us (the
// owner mark, S2C 0x2A, its HP, a kill), no EXP — into `group` (a melee blow: the swing's block) or, sent later
// (the 200 ms of a death type 0 blow), at once.
void GameClientSession::barrier_send_back(MonsterEntity& m, const int32_t dinfo[7], std::vector<PacketBuffer>* group,
                                          std::vector<CounterHit>* hits)
{
    SkillDamage h;
    h.dmg        = dinfo[0];
    h.death_type = dinfo[1];
    h.result     = dinfo[2];
    h.element    = dinfo[3];
    h.flags      = dinfo[4];
    h.extra      = dinfo[6];
    if (!m.alive || m.hp <= 0) return;
    const int32_t hp_before = m.hp;
    if (group) {
        const bool killed = skill_damage_event(m, h, *group);
        if (hits) hits->push_back({ &m, h.dmg, hp_before, killed });
        return;
    }
    std::vector<PacketBuffer> pk;
    const bool killed = skill_damage_event(m, h, pk);
    for (PacketBuffer& p : pk) server_.broadcast_zone(zone_id_, p, 0);
    skill_after_hit(m, h.dmg, hp_before, killed, 0, true, std::chrono::steady_clock::now(), 1);
    LOG_DEBUG("GameClientSession %u: Dark Barrier sends %d back to mob %u%s", session_id(), h.dmg,
              static_cast<unsigned>(m.object_id), killed ? " [KILLED]" : "");
}

// ESABloodyOath's handler of the message 30097 (sub_5865C0: a damage event we caused, before it is taken): a dark one
// (DamageInfo [3] & 0x40): the cost = (int)(+1618 * HPRatio), +1618 = 0; the cost would leave no HP -> S2C 924 [u16
// its id][u32 -1] to us (sub_6067D0 — VERIFIED: the client's ESABloodyOath 0x50EC20 shows "Alert_BloodyOath_LowHP"),
// +1616 off and it ends; else HP -= the cost, S2C 0x24 [u32 HP] (sub_5F4670) and S2C 924 [u16 its id][u32 the cost]
// to the area (sub_606740; the client shows it when not 0).
void GameClientSession::bloody_on_dealt(const SkillDamage& h)
{
    if (!(h.element & 0x40)) return;
    for (size_t i = 0; i < es_.size(); ++i) {
        if (es_[i].kind != EsKind::BloodyOath) continue;
        const uint16_t id = es_[i].id;
        const int16_t cost_base = bloody_cost_;
        bloody_cost_ = 0;
        const int32_t cost = static_cast<int32_t>(static_cast<double>(cost_base) * static_cast<double>(es_[i].ratio));
        if (cost >= hp_) {
            PacketBuffer p(static_cast<Opcode>(0x039C), is_paula());
            p.write<uint16_t>(id);
            p.write<int32_t>(-1);
            send(p);
            bloody_on_ = false;
            es_[i].value = 0;
            es_end(i);
            return;
        }
        hp_ -= cost;
        {
            PacketBuffer p(static_cast<Opcode>(0x0024), is_paula());
            p.write<uint32_t>(static_cast<uint32_t>(hp_));
            send(p);
        }
        PacketBuffer p(static_cast<Opcode>(0x039C), is_paula());
        p.write<uint16_t>(id);
        p.write<int32_t>(cost);
        server_.broadcast_zone(zone_id_, p, 0);
        return;
    }
}

// ESADarkGhost's handler of the message 30097 (sub_5839C0: a damage event we caused, before it is taken): a dark one
// (DamageInfo [3] & 0x40) not its own (no [4] 0x100): the magic damage sub_5E6270(M_AP (+196), us, it, its APRatio (+200)
// as the ApplyRatio, MA offset 49, element 64), [4] |= 0x100, the 30002 to it (the whole damage event, no EXP) and S2C
// 924 [u16 its id][the 28-byte DamageInfo][u16 it] to the area (sub_605F80; VERIFIED: the client's ESADarkGhost
// sub_5086B0 reads them). True when that hit killed it (the damage it is about to take then does nothing).
bool GameClientSession::dark_ghost_on_dealt(MonsterEntity& m, const SkillDamage& h, std::vector<PacketBuffer>& group)
{
    if (!(h.element & 0x40) || (h.flags & 0x100)) return false;
    for (size_t i = 0; i < es_.size(); ++i) {
        if (es_[i].kind != EsKind::DarkGhost) continue;
        const uint16_t id = es_[i].id;
        SkillDamage g = skill_magic_damage(m, static_cast<int16_t>(es_[i].value), es_[i].ratio, 49, 64);
        g.flags |= 0x100;
        const bool killed = skill_damage_event(m, g, group);
        PacketBuffer p(static_cast<Opcode>(0x039C), is_paula());
        p.write<uint16_t>(id);
        p.write<int32_t>(g.dmg);
        p.write<int32_t>(g.death_type);
        p.write<int32_t>(g.result);
        p.write<int32_t>(g.element);
        p.write<int32_t>(g.flags);
        p.write<int32_t>(0);                                 // [5] (our copy: OnMessage sets its own)
        p.write<int32_t>(g.extra);
        p.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        group.push_back(std::move(p));
        LOG_DEBUG("GameClientSession %u: Dark Commando hits mob %u again for %d%s", session_id(),
                  static_cast<unsigned>(m.object_id), g.dmg, killed ? " [KILLED]" : "");
        if (killed || !m.alive) return true;
    }
    return false;
}

// ── ESAction 1259 "Med. Cure" (GameServer sub_5D33B0; ESAUI 0) ───────────────────────────────────────
//  The row ESAction_IntermediateCure (sub_51C5E0: Radius +4 i16, ESAction0..ESAction20 +8..). The zone's players (zone
//  +28) until 3 are done (sub_5D3420 — cured or not): in our party (sub_716EF0: our share group, us included; none:
//  ourselves), on our layer, not dead, within (float)Radius of us — each gets the action {ESAction 1260, EState 0,
//  ESClass -1, flags 2, skill 2501, the row, its id, us its caster} through the switch (sub_5C0250 -> sub_5D3640,
//  ESAIntermediateCureDo): each of the 21 ESActions leaves its limiter (vtbl+60 — removed: cured); the block {S2C 224
//  sub_5EDE00 [us][it][0][1260][2][ESClass -1][2501][0] + [u8 cured]} (VERIFIED: the client's ESAIntermediateCureDo
//  0x4F11C0 reads the u8); cured: our use TM. (Med. Cure sends no opener of its own; a member's part runs on its
//  strand: cure_from.)
void GameClientSession::esa_med_cure(const SkillDef& d, const EsaRow* act)
{
    std::vector<int> list;
    for (int i = 0; i < 21; ++i) {
        char col[16];
        std::snprintf(col, sizeof(col), "ESAction%d", i);
        list.push_back(row_i(act, col));
    }
    SkillDef dd = d;
    dd.esaction = 1260;
    dd.esclass  = -1;
    dd.estate   = 0;
    const float radius = static_cast<float>(static_cast<int16_t>(row_i(act, "Radius")));
    const auto now = std::chrono::steady_clock::now();
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    const uint16_t self = static_cast<uint16_t>(object_id_);
    int n = 0;
    for (uint32_t oid : server_.entity_mgr().players_in_zone(zone_id_)) {
        if (n >= 3) break;
        GameClientSession* s = server_.player_session(oid);
        if (!s || !s->object_id_ || !s->in_world()) continue;
        if (s != this && !server_.party().same_group(char_uid_, s->char_uid_)) continue;
        if (s->dead_) continue;
        int32_t x, y;
        s->cur_pos(now, x, y);
        if (!(static_cast<double>(radius) >= static_cast<double>(dist_f(x, y, cx, cy)))) continue;
        ++n;
        if (s == this) {
            if (cure_from(self, list, dd, 2501)) skill_use_tm(2501);
            continue;
        }
        s->run_on_strand([s, list, self, dd]() {
            if (s->cure_from(self, list, dd, 2501))
                if (GameClientSession* c = s->server_.player_session(self)) c->skill_use_tm_later(2501);
        });
    }
    LOG_DEBUG("GameClientSession %u: Med. Cure: %d player(s)", session_id(), n);
}

// ── ESAction 1343 "Celestial Strike" (GameServer sub_5DAC70, ESALightWeight; ESAUI 6: a point) ──────────
//  The row ESAction_LightWeight (sub_51BFC0: ApplyRatio +0, AP +4, Radius +8 i16) and the payload's point: sub_53E360
//  around it, at most 7, Radius; none: nothing at all. Else: n = our learned skills of eSkillSymbolType 7 (light,
//  sub_892520 on the skill list +2264); each found one stops (sub_41C5D0); the block: the opener + [u8 n][u16 id]*n
//  (VERIFIED: the client's ESALightWeight 0x508200 reads them), then each: the magic damage sub_5E6270(AP + (int)(AP *
//  (n + 10.0) / 20.0), ApplyRatio, MA offset 25, element 32 = Light), its EXP share, 30002, 30003 kind 1. Then the
//  skill's use TM.
void GameClientSession::esa_light_weight(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    int16_t px = 0, py = 0;
    if (su_.payload.size() >= 4) {
        px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
        py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
    }
    const std::vector<MonsterEntity*> arr = area_monsters(px, py, static_cast<int16_t>(row_i(act, "Radius")), 7, now);
    if (arr.empty()) return;
    int light = 0;
    for (const LearnedSkill& ls : skills_)
        if (const SkillDef* sd = server_.skill_db().get(ls.id)) if (sd->symbol_type == 7) ++light;
    for (MonsterEntity* m : arr) {
        int32_t x, y;
        m->motion.pos(now, x, y);
        m->motion.rest(x, y);
        m->pos_x = x; m->pos_y = y;
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint8_t>(static_cast<uint8_t>(arr.size()));
    for (MonsterEntity* m : arr) o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
    group.push_back(std::move(o));
    const int32_t ap    = row_i(act, "AP");
    const int32_t bonus = static_cast<int32_t>(static_cast<double>(ap) * (static_cast<double>(light) + 10.0) / 20.0);
    const float   apply = row_f(act, "ApplyRatio");
    struct Hit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };
    std::vector<Hit> done;
    for (MonsterEntity* m : arr) {
        const SkillDamage h = skill_magic_damage(*m, bonus + ap, apply, 25, 32);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);
        done.push_back({ m, h.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now, 1);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Celestial Strike at (%d,%d) (+%d, %d light skills): %zu hit", session_id(), px, py,
              bonus, light, arr.size());
}

// ── ESAction 1486 "Refreshing Dew" (GameServer sub_5DFA50; ESAUI 0) ─────────────────────────────────────
//  The row ESAction_SettleDown (sub_51DF90: ApplyRatio +0, Ratio +4 — kept as 1 / Ratio —, Seconds +8, Comment +12,
//  Radius +16, ints): our share group's players on our layer within Radius, at most 5 (sub_58B9F0: us among them; no
//  group: none); the block: the opener without a tail; our use TM; then ESASettleDownDo {ESAction 1487, EState 0,
//  ESClass 0, flags 2, skill 2505, this row, us its caster} through sub_524690(.., 1) on us, then on each of them — us
//  again (ours replaced, retail); a member's on its strand (party_action_from; es.cpp).
void GameClientSession::esa_settle_down(const SkillDef& d, uint8_t lv, const EsaRow* act)
{
    const std::vector<GameClientSession*> found = party_in_radius(row_i(act, "Radius"), 5);
    std::vector<PacketBuffer> group;
    group.push_back(skill_opener(d, static_cast<uint16_t>(object_id_)));
    server_.broadcast_block(zone_id_, group, 0);
    skill_use_tm(su_.skill);
    const uint16_t self = static_cast<uint16_t>(object_id_);
    const uint16_t sk   = static_cast<uint16_t>(d.id);
    party_action_from(self, sk, lv, 1487, zone_id_);
    for (GameClientSession* s : found) {
        if (s == this) party_action_from(self, sk, lv, 1487, zone_id_);
        else s->party_action_pulse(self, sk, lv, 1487, zone_id_);
    }
}

// ── ESAction 1282 "Scythe" (GameServer sub_5D3EC0, ESADeathScythe) / 1283 "Staff of Agony" (sub_5D42B0,
//    ESADespairSpear); ESAUI 1 ───────────────────────────────────────────────────────────────────────
//  The selected target (kind 3) — none: nothing; we mark it (vtbl+132). The row (sub_51C6F0: ApplyRatio +0, AP +4, Ratio
//  +8, Comment +12): the magic damage sub_5E6270(AP + 30 / 15 * our Dark (element slot 6, +848), ApplyRatio, MA offset
//  49, element 64 = Dark), its EXP share; the block {S2C 224 sub_5EDE00 [us][target][0]... without a tail (VERIFIED: the
//  clients' ESADeathScythe 0x4F1BF0 / ESADespairSpear 0x4F1D20 read none), 30002, 30003 kind 1}; the skill's TM per
//  hit. Scythe then, with HP left on the target and not a miss / block / 6 / 8 (sub_5D4120): Fear (2702) learned -> its
//  ESAFearDo (1279, fear_on).
void GameClientSession::esa_death_scythe(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    mark_monster_owner(*m);
    const bool scythe = (d.esaction == 1282);
    const int32_t base = row_i(act, "AP") + (scythe ? 30 : 15) * elem_slot(6);
    const SkillDamage h = skill_magic_damage(*m, base, row_f(act, "ApplyRatio"), 49, 64);
    const int32_t hp_before = m->hp;
    std::vector<PacketBuffer> group;
    group.push_back(skill_opener(d, tid));
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: %s -> mob %u (base %d) dmg=%d result=%d%s", session_id(),
              scythe ? "Scythe" : "Staff of Agony", tid, base, h.dmg, h.result, killed ? " [KILLED]" : "");
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, false, now, 1);
    const int r = h.result;
    if (scythe && m->alive && m->hp > 0 && r != 1 && r != 2 && r != 6 && r != 8) passive_follow_es(*m, 2702, 1279);
}

// ESAFearDo (1279, init sub_56F1C0; the action carries Fear's row: ApplyRatio +0, Ratio +4, Second +8 i16 — sub_51BEF0):
// +196 = 1000 * Second, the lifetime; the block {S2C 224 sub_5EDE00 [us][target][ES id][1279][2][ESClass 1][2702][0], no
// tail — VERIFIED: the client's ESAFearDo 0x4F1A10 reads none}; our TM per hit whatever the roll; applied and its
// DebuffSet resisted neither 64 nor 128 (+192): it stops, the condition 4098. Its tick (sub_56F2F0): past +196 the end;
// its end (sub_56F440): the condition off.
bool GameClientSession::fear_on(MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act)
{
    e.end_ms  = 1000 * static_cast<int16_t>(row_i(act, "Second"));
    e.life_ms = e.end_ms;
    e.flags   = 2;
    {
        std::vector<PacketBuffer> group;
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(1279);
        o.write<uint8_t>(2);
        o.write<uint8_t>(static_cast<uint8_t>(e.esclass));
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(static_cast<uint16_t>(e.estate));
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    skill_hit_tm(m, e.skill);
    if (e.result == 1 && !(e.resisted & 64u) && !(e.resisted & 128u)) {
        const auto now = std::chrono::steady_clock::now();
        int32_t x, y;
        m.motion.pos(now, x, y);
        m.motion.rest(x, y);
        m.pos_x = x; m.pos_y = y;
        m.cond_add(4098);
    }
    m.es.push_back(e);
    LOG_DEBUG("GameClientSession %u: Fear %u on mob %u for %d ms (result %d)", session_id(), e.id,
              static_cast<unsigned>(m.object_id), e.end_ms, e.result);
    return true;
}

// ── ESAction 1488 "Dark Assimilation" (GameServer sub_5DFC10; no packet of its own) ─────────────────────
//  The action {ESAction 1489, EState 0, ESClass 0, flags 2, skill 2706, its row (sub_51E020: ApplyRatio +0, Ratio +4,
//  Seconds +8, Comment +12)} through sub_524690(.., 1) on us (ESADarkUpDo, es.cpp), then our use TM.
void GameClientSession::esa_dark_up(const SkillDef& d, uint8_t lv, const EsaRow* act)
{
    SkillDef dd = d;
    dd.esaction = 1489;
    dd.estate   = 0;
    dd.esclass  = 0;
    if (es_limiter_admit(0, 1489, static_cast<uint16_t>(dd.id))) es_create(dd, lv, act, EsKind::DarkUp, true, 2);
    skill_use_tm(su_.skill);
}

// ESADarkWhisper's pulse (sub_58BF90, +198 at 0, then every 2000 ms): sub_53E360 around its spot, at most min(KillNum,
// 5), Radius; any -> S2C 924 [u16 its id][u8 n][u16 id]*n to the area (sub_606850; VERIFIED: the client's ESADarkWhisper
// sub_5147A0 reads them), then each: the magic damage sub_5E6270(M_AP, ApplyRatio, MA offset 49, element 64 = Dark),
// its EXP share, 30002, 30003 kind 1 (no block).
void GameClientSession::dark_whisper_pulse(size_t i)
{
    if (i >= es_.size()) return;
    const ActiveEs w = es_[i];
    const auto now = std::chrono::steady_clock::now();
    const int n = std::min<int>(static_cast<uint8_t>(w.count), 5);
    const std::vector<MonsterEntity*> arr = area_monsters(w.fx, w.fy, w.radius, n, now);
    if (arr.empty()) return;
    {
        PacketBuffer p(static_cast<Opcode>(0x039C), is_paula());
        p.write<uint16_t>(w.id);
        p.write<uint8_t>(static_cast<uint8_t>(arr.size()));
        for (MonsterEntity* m : arr) p.write<uint16_t>(static_cast<uint16_t>(m->object_id));
        server_.broadcast_zone(zone_id_, p, 0);
    }
    for (MonsterEntity* m : arr) {
        const SkillDamage h = skill_magic_damage(*m, static_cast<int16_t>(w.dinfo[0]), w.ratio, 49, 64);
        const int32_t hp_before = m->hp;
        std::vector<PacketBuffer> pk;
        const bool killed = skill_damage_event(*m, h, pk);
        for (PacketBuffer& p : pk) server_.broadcast_zone(zone_id_, p, 0);
        skill_after_hit(*m, h.dmg, hp_before, killed, 0, false, now, 1);
    }
}

// The message 87 (C2S 0x57, an item use: the Player's global case 87 hands it to the limiter, vtbl+72): ESADarkWhisper
// (sub_58C2F0) ends.
void GameClientSession::es_on_item_use()
{
    for (size_t i = 0; i < es_.size();) {
        if (es_[i].kind == EsKind::DarkWhisper) { es_end(i); continue; }
        ++i;
    }
}
