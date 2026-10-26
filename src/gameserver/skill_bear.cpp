#include "game_session.hpp"
#include "game_server.hpp"
#include "combat_util.hpp"
#include "logger.hpp"
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
//  The Bear line (Paula, the Polar Bear: Animal Lover / Trainer / Zoologist — skills 4701..4910, ESActions
//  1523..1551). A RECONSTRUCTION, not a port: the GameServer Eternal Paula ran (GameServerRelease, 2012-09-25)
//  ends its ESAction switch at 1497, so on that server these skills spent their MP and did nothing. What is
//  rebuilt here, and from what:
//    - values: the 2014 KR client data (SkillParam2 + ESAction_* / ESAII_* tables, db/skill_db.yml +
//      db/esa_db.yml);
//    - packets: the 2014 client's own ESA classes (factory sub_55CD40) — every tail below is VERIFIED there;
//    - rules: the KR server's handling of the same column types on older skills (the physical damage
//      sub_5E6040, the Tornado Bomb records, the stat adders, the stun / slow / DP-down / poison classes);
//    - the shapes the KR server cannot give: doc/paula_bear_skills_report.md (the tPoseidon report: client
//      tooltips, a Korean player guide, a private-server wiki), each marked INFERRED where only a guess fits.
//
//  Phase 1 (this file + the hooks in es.cpp): Energetic Hammer 4709 (1551), Double Hammer 4702 (1523), Over Swing
//  4803 (1535), Flying Hammer 4807 (1539), Earthquake Hammer 4708 (1530 + its QuakeHammerDo 1531), Rain of Hammers
//  4906 (1546); the buffs 4703 / 4704 / 4706 / 4802 / 4901 / 4902 / 4903 (es.cpp, EsKind::BearStat); the debuffs
//  Mental Breakdown 4705 (1526), Mole Hammer 4805 (1537), Bear Attack 4904 (1543). Equip Hammer 4701 (ESAction
//  1000, PSAbility 13) needs nothing here.
//  Phase 2: Hammer Spin 4907 (1547, EsKind::HammerWheel) + its booster Rampage Wheel 4909, Party Guard 4707 (1528),
//  Best Shield 4905 (1544), the Crazy Hammer chain 4801 / 4804 / 4806 / 4908 / 4910 (the gauge 1532 + the mode 1533,
//  hooked into the normal attack, the skill fire, the move speed and the world tick).
// ─────────────────────────────────────────────────────────────────────────────

namespace {
float row_f(const EsaRow* r, const char* k) { return r ? static_cast<float>(r->get(k, 0.0)) : 0.0f; }
int   row_i(const EsaRow* r, const char* k) { return r ? static_cast<int>(r->get(k, 0.0)) : 0; }
constexpr uint8_t kEsKind = 2;                    // ESAction +152 (the action struct +20 = 2)

// A hit that lands: not a miss / block / 6 / 8 (the KR passives' test after a hit, e.g. sub_5CE540).
bool landed(int result) { return result != 1 && result != 2 && result != 6 && result != 8; }
}  // namespace

// The slot-1 item's bonus of one stat (sub_718750 for that item alone): its options 10000 + stat summed when it has
// any, else ItemParam Plus.
int32_t GameClientSession::weapon_stat(int stat) const
{
    if (stat < 0 || stat >= 12 || !equip_[1]) return 0;
    for (const BagItem& b : bag_) {
        if (b.uid != equip_[1]) continue;
        const ItemDef* d = server_.item_db().get(b.item_id);
        if (!d) return 0;
        int32_t sum = 0;
        bool have = false;
        for (const ItemOption& o : b.opts)
            if (o.id == 10000 + stat) { sum += o.value; have = true; }
        return have ? static_cast<int16_t>(sum) : d->plus[stat];
    }
    return 0;
}

// ── ESAction 1551 "Energetic Hammer" (4709; client ESAHammerAp sub_528DC0) ─────────────────────────────
//  The Bear's starter strike. Its row (ESAction_HammerAp: Ratio 2, addAP 140, ApplyRatio 0.8, Comment 150) and its
//  "attack 150%" tooltip are the starter Punch's (ESAction 1296: Ratio1 2, Ratio2 140, ApplyRatio 0.8), so its shape:
//  the mark, AP' = AP + min(level, 30) * Ratio + addAP, the physical damage (ApplyRatio, element 0), EXP kind 1,
//  30107, the TM per hit. The opener [us][us] + [u16 target] (VERIFIED: the client's ESAHammerAp reads the u16 and
//  links that actor).
void GameClientSession::esa_hammer_ap(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    mark_monster_owner(*m);
    const int lv  = std::min<int>(level_, 30);
    const int ap2 = st_ap_ + static_cast<int>(static_cast<double>(lv) * static_cast<double>(row_f(act, "Ratio"))) +
                    row_i(act, "addAP");
    const SkillDamage h = skill_phys_damage(*m, ap2, row_f(act, "ApplyRatio"), 0);
    const int32_t hp_before = m->hp;
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: Energetic Hammer -> mob %u ap'=%d dmg=%d result=%d hp=%d/%d%s", session_id(), tid,
              ap2, h.dmg, h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "");
    skill_follow_after(tid);
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, false, now, 1);
}

// ── ESAction 1523 "Double Hammer" (4702) / 1535 "Over Swing" (4803) ───────────────────────────────────────
//  Rows: DoubleHammer {Ratio, ApplyRatio, Comment}, OverSwing {Ratio, AdRatio, ApplyRatio, Comment}; tooltips "attack
//  <Comment>%, 2 / 3 hits". Hit n (0-based): AP' = (int)(AP * Ratio + DA * AdRatio * n) — Double Hammer has no AdRatio
//  (the report: WEB-KO + WEB-OTO, whose Lv1/Lv5 numbers match the tables). Each hit the physical damage (ApplyRatio,
//  element 0), kept as Tornado Bomb keeps its three (sub_5205F0): {u32 dmg, u32 result, u16 0xFFFF}, a miss {0, result}.
//  The block: the opener [us][us] + [u16 target] + the records (VERIFIED: the clients' ESADoubleHammer sub_5226E0 /
//  ESAOverSwing sub_525920 read the u16 and exactly 2 / 3 records of u32, u32, u16), then ONE damage event {the sum,
//  death type 2, result 0, flags 0x8000, the hits}, its EXP first (kind 3) — Tornado Bomb's shape (sub_5C4E10).
void GameClientSession::esa_multi_hammer(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now,
                                         int hits)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    const float ratio    = row_f(act, "Ratio");
    const float ad_ratio = row_f(act, "AdRatio");
    const float apply    = row_f(act, "ApplyRatio");
    struct Rec { int32_t dmg; int32_t result; uint16_t v; };
    std::vector<Rec> rec;
    int32_t total = 0;
    int landed_hits = 0;
    for (int n = 0; n < hits; ++n) {
        const int ap2 = static_cast<int>(static_cast<double>(st_ap_) * static_cast<double>(ratio) +
                                         static_cast<double>(st_da_) * static_cast<double>(ad_ratio) * n);
        SkillDamage s = skill_phys_damage(*m, ap2, apply, 0);
        if (s.absorbed > 0) absorb_heal(*m, s.absorbed);
        if (s.dmg <= 0 || s.result == 1 || s.result == 2) {
            rec.push_back({ 0, s.result, 0xFFFF });
        } else {
            total += s.dmg;
            ++landed_hits;
            rec.push_back({ s.dmg, s.result, 0xFFFF });
        }
    }
    SkillDamage h;
    h.dmg        = total;
    h.death_type = 2;
    h.result     = 0;
    h.element    = 0;
    h.flags      = 0x8000;
    h.extra      = landed_hits;
    const int32_t hp_before = m->hp;
    gain_attack_exp(*m, total, hp_before, 3);                 // its EXP first (as Tornado Bomb)
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    for (const Rec& r : rec) {
        o.write<int32_t>(r.dmg);
        o.write<int32_t>(r.result);
        o.write<uint16_t>(r.v);
    }
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: %s -> mob %u %d/%d hits = %d hp=%d/%d%s", session_id(),
              hits == 2 ? "Double Hammer" : "Over Swing", tid, landed_hits, hits, total, m->hp, m->hp_max,
              killed ? " [KILLED]" : "");
    skill_follow_after(tid);
    skill_use_tm(su_.skill);
    skill_after_hit(*m, total, hp_before, killed, 0, true, now);
}

// ── ESAction 1539 "Flying Hammer" (4807; client ESAFlyHammer sub_526560) ──────────────────────────────────
//  Row {Ratio, HPRatio, ApplyRatio}. AP' = (int)(AP + the weapon's DA * Ratio + HP * HPRatio) — the report: both web
//  sources agree on Ratio x a DA and HPRatio x the current HP; which DA is open (WEB-KO "the weapon's DA", WEB-OTO
//  "bonus DA"): the weapon's (slot 1) is taken. The physical damage (ApplyRatio, element 0); the opener [us][us] +
//  [u16 target] (VERIFIED: the client's ESAFlyHammer reads the u16 and links that actor); the damage event; 30107; the
//  TM per hit.
void GameClientSession::esa_fly_hammer(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive) return;
    const int32_t wda = weapon_stat(7);
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) +
                                     static_cast<double>(wda) * static_cast<double>(row_f(act, "Ratio")) +
                                     static_cast<double>(hp_) * static_cast<double>(row_f(act, "HPRatio")));
    const SkillDamage h = skill_phys_damage(*m, ap2, row_f(act, "ApplyRatio"), 0);
    const int32_t hp_before = m->hp;
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    LOG_DEBUG("GameClientSession %u: Flying Hammer -> mob %u ap'=%d (weapon DA %d, HP %d) dmg=%d result=%d hp=%d/%d%s",
              session_id(), tid, ap2, wda, hp_, h.dmg, h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "");
    skill_follow_after(tid);
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, false, now);
}

// ── ESAction 1530 "Earthquake Hammer" (4708) / 1546 "Rain of Hammers" (4906) ──────────────────────────────
//  No selection (AffectionRel 1, no ESAUI): around us, like the KR area skills (sub_53E360's walk, at most min(7,
//  KillNum), Radius). Rows: QuakeHammer {Ratio, DaRatio, KillNum, Radius, ApplyRatio, PRatio, PSeconds}, HighSpeedDrop
//  {Ratio, KillNum, Radius, ApplyRatio}; AP' = (int)(AP * (Ratio + DA * DaRatio)) / (int)(AP * Ratio) (WEB-KO's
//  "AP x {Ratio + DA/100 x 2.7}" is the master DaRatio 0.027). The opener [us][us] with no tail (VERIFIED: the
//  clients' ESAQuakeHammer sub_524330 / ESAHighSpeedDrop sub_528260 read none), then each target: the physical damage
//  (ApplyRatio, element 0), its event and EXP. Earthquake Hammer's DebuffBitCode 2 (paralysis) and PRatio / PSeconds:
//  on each target it hit that still stands, a PRatio roll -> its QuakeHammerDo (1531, quake_paralysis). Then the use TM.
void GameClientSession::esa_area_hammer(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now,
                                        bool quake)
{
    int32_t ax, ay;
    cur_pos(now, ax, ay);
    const int kill = row_i(act, "KillNum");
    const std::vector<MonsterEntity*> arr =
        area_monsters(ax, ay, static_cast<int16_t>(row_i(act, "Radius")), std::max(0, std::min(7, kill)), now);
    const double k = quake ? static_cast<double>(row_f(act, "Ratio")) +
                                 static_cast<double>(st_da_) * static_cast<double>(row_f(act, "DaRatio"))
                           : static_cast<double>(row_f(act, "Ratio"));
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) * k);
    const float apply = row_f(act, "ApplyRatio");
    std::vector<PacketBuffer> group;
    group.push_back(skill_opener(d, static_cast<uint16_t>(object_id_)));
    struct Hit { MonsterEntity* m; int32_t dmg, hp_before, result; bool killed; };
    std::vector<Hit> done;
    for (MonsterEntity* m : arr) {
        SkillDamage h = skill_phys_damage(*m, ap2, apply, 0);
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, h, group);   // (it applies the absorbed part, 30182)
        done.push_back({ m, h.dmg, hp_before, h.result, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : done) {
        skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now);
        if (quake && landed(h.result) && h.m->alive && h.m->hp > 0 && apply_roll(row_f(act, "PRatio"), 0) == 1)
            quake_paralysis(*h.m, d, act);
    }
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: %s: ap'=%d, %zu hit", session_id(), quake ? "Earthquake Hammer" : "Rain of Hammers",
              ap2, arr.size());
}

// ESAQuakeHammerDo (1531): Earthquake Hammer's paralysis on one target, made like the KR strikes' stun (Lightning
// Strike's ESAElectricSwordDo, es_electric_stun) with the KR paralysis (Electroshock's ESAElectricShockDo 1183: the
// target stops, condition 1 — no MoveTo). A DebuffSet first (sub_891F80 on DebuffBitCode 2; all resisted -> S2C 1247);
// ESClass 1, EState 0, flags 2, the skill 4708, its time 1000 * PSeconds; the block {S2C 224 [us][target][ES id][1531]
// [2][1][4708][0] + [u32 ms]} (VERIFIED: the client's ESAQuakeHammerDo sub_524560 reads a u32 — its time); we mark it.
// Its tick (es.cpp): the time; its end: the condition 1 off.
void GameClientSession::quake_paralysis(MonsterEntity& m, const SkillDef& d, const EsaRow* act)
{
    uint32_t bits = 0;
    if (monster_debuff_immune(m, d.debuff_bits, bits)) { send_debuff_resisted(m, bits); return; }
    std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
    for (size_t i = 0; i < m.es.size(); ++i)                 // a second one replaces the first (the same ESAction)
        if (m.es[i].esaction == 1531) { server_.monster_es_end(m, i); break; }
    MonsterEntity::Es e;
    e.id       = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());
    e.esaction = 1531;
    e.estate   = 0;
    e.esclass  = 1;
    e.skill    = static_cast<uint16_t>(d.id);
    e.flags    = 2;
    e.caster   = object_id_;
    e.added    = tick_ms();
    e.result   = 1;
    e.end_ms   = 1000 * static_cast<int16_t>(row_i(act, "PSeconds"));
    e.life_ms  = e.end_ms;
    {
        std::vector<PacketBuffer> group;
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(1531);
        o.write<uint8_t>(kEsKind);
        o.write<uint8_t>(1);
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(0);
        o.write<uint32_t>(static_cast<uint32_t>(e.end_ms));
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    m.es.push_back(e);
    mark_monster_owner(m);
    const auto now = std::chrono::steady_clock::now();
    int32_t x, y;
    m.motion.pos(now, x, y);
    m.motion.rest(x, y);
    m.pos_x = x; m.pos_y = y;
    if (!m.cond_test(1)) m.cond_add(1);
    LOG_DEBUG("GameClientSession %u: Earthquake Hammer paralyses mob %u (ES %u, %d ms)", session_id(),
              static_cast<unsigned>(m.object_id), e.id, e.end_ms);
}

// The Bear's debuffs on the selected monster (es_apply_on hands them over with the generic fields made: the id, the
// caster, the apply roll on ApplyRatio, Ratio, 1000 * Seconds). Each block {S2C 224 [us][target][ES id][ESAction][2]
// [ESClass 1][skill][EState] + its tail}; we mark the target; applied -> the skill's TM per hit.
//  1526 ESAMentalBreak (Mental Breakdown 4705, DebuffBitCode 1 stun): its row has no ApplyRatio, its chance is Ratio
//    (0.55..0.85 — INFERRED, the only probability column); its time MSeconds (ms). Tail [u8 failed][u32 ms]
//    (VERIFIED: the client's ESAMentalBreak sub_523740 reads both — a 1 shows the target's "resisted" balloon,
//    sub_71F760, and plays nothing). Applied: the target stops and takes the condition 15 — the KR stun (ESAPowerStun
//    1144); its end: the condition off.
//  1543 ESABearAttack (Bear Attack 4904, DebuffBitCode DP + SPD): the KR DP debuff rule (ESADpDown 1081: DP -= the
//    caster's AP * Ratio, read from its live stats) and the KR slow (ESABreakdownGroundDo 1359: walk / run / chase
//    -= p * 100 / 160 / 130) with p = RunSpeed (0.45 — INFERRED, the column's scale fits). Tail [u32 ms] (VERIFIED:
//    the client's ESABearAttack sub_527BA0 reads a u32).
//  1537 ESABbongHammer (Mole Hammer 4805, DebuffBitCode poison + DP): DP -= its own DP * DPRatio while it lasts, and
//    every TicTime ms a physical hit of AP * Ratio (ApplyRatio 1.0) against that lowered DP — WEB-OTO's "(AP x Ratio -
//    0.75 x DP x (1 - DPRatio)) x (1 - physical resist)", the KR poison pulse (ESAPoisonBaggageDo, poison_hit) as its
//    model. Tail [u32 ms] (VERIFIED: the client's ESABbongHammer sub_526140 reads a u32).
bool GameClientSession::bear_debuff_on(MonsterEntity& m, MonsterEntity::Es& e, const SkillDef& d, const EsaRow* act)
{
    e.flags      = 2;
    e.elapsed_ms = 0;
    e.pulse_ms   = 0;
    bool failed_tail = false;
    if (e.esaction == 1526) {
        e.result  = apply_roll(row_f(act, "Ratio"), 0);
        e.end_ms  = row_i(act, "MSeconds");
        e.life_ms = e.end_ms;
        failed_tail = true;
    } else if (e.esaction == 1543) {
        e.ratio2 = row_f(act, "RunSpeed");
    } else if (e.esaction == 1537) {
        e.ratio3   = row_f(act, "DPRatio");
        e.phase_ms = row_i(act, "TicTime");
    }
    if (e.result != 1 && !failed_tail) {
        // Bear Attack / Mole Hammer not applied: no block — their client classes read no result and would play the
        // effect for its whole time.
        LOG_DEBUG("GameClientSession %u: skill %u ESAction %d on mob %u not applied (%d)", session_id(), e.skill,
                  e.esaction, static_cast<unsigned>(m.object_id), e.result);
        return false;
    }
    if (!m.base_set) {                                       // the base stats (+704)
        m.base_set = true;
        m.base_ap = m.ap; m.base_ac = m.ac; m.base_dx = m.dx; m.base_da = m.da;
        m.base_lk = m.lk; m.base_dp = m.dp; m.base_hv = m.hv;
        m.base_ma = m.ma; m.base_md = m.md;
    }
    {
        std::vector<PacketBuffer> group;
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(static_cast<uint16_t>(object_id_));
        o.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        o.write<uint16_t>(e.id);
        o.write<uint16_t>(static_cast<uint16_t>(e.esaction));
        o.write<uint8_t>(kEsKind);
        o.write<uint8_t>(static_cast<uint8_t>(e.esclass));
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(static_cast<uint16_t>(e.estate));
        if (failed_tail) o.write<uint8_t>(e.result == 1 ? 0 : 1);
        o.write<uint32_t>(static_cast<uint32_t>(e.end_ms));
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    if (e.result != 1) {                                     // Mental Break resisted: nothing stays (the client showed it)
        LOG_DEBUG("GameClientSession %u: skill %u ESAction %d on mob %u resisted (%d)", session_id(), e.skill,
                  e.esaction, static_cast<unsigned>(m.object_id), e.result);
        return false;
    }
    m.es.push_back(e);
    mark_monster_owner(m);
    skill_hit_tm(m, e.skill);
    if (e.esaction == 1526) {
        const auto now = std::chrono::steady_clock::now();
        int32_t x, y;
        m.motion.pos(now, x, y);
        m.motion.rest(x, y);
        m.pos_x = x; m.pos_y = y;
        m.cond_add(15);
    } else if (e.esaction == 1543) {
        const double p = static_cast<double>(e.ratio2);
        m.walk_speed  = static_cast<float>(m.walk_speed  - p * 100.0);
        m.run_speed   = static_cast<float>(m.run_speed   - p * 160.0);
        m.chase_speed = static_cast<float>(m.chase_speed - p * 130.0);
        server_.monster_recompute(m);
    } else if (e.esaction == 1537) {
        server_.monster_recompute(m);
    }
    LOG_DEBUG("GameClientSession %u: ES %u ESAction %d (skill %u) on mob %u: ratio %.2f, %d ms, DP %d", session_id(),
              e.id, e.esaction, e.skill, static_cast<unsigned>(m.object_id), e.ratio, e.end_ms, m.dp);
    return true;
}

// ESABbongHammer's pulse (Mole Hammer, on our strand): the physical damage of AP * Ratio, ApplyRatio 1.0, against the
// target's DP as it stands (the debuff's cut in it), element 0; the event; EXP kind 0 when we may attack it.
void GameClientSession::mole_hammer_hit(uint16_t mob, uint16_t es_id)
{
    if (leaving_ || state_ != GameClientSessionState::InWorld) return;
    MonsterEntity* m = server_.entity_mgr().get_monster(mob);
    if (!m || m->zone != zone_id_ || !m->alive || m->hp <= 0) return;
    float ratio = 0.0f;
    {
        std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
        const MonsterEntity::Es* e = nullptr;
        for (const MonsterEntity::Es& x : m->es) if (x.id == es_id) { e = &x; break; }
        if (!e) return;
        ratio = e->ratio;
    }
    const auto now = std::chrono::steady_clock::now();
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) * static_cast<double>(ratio));
    SkillDamage h = skill_phys_damage(*m, ap2, 1.0f, 0, m->dp);
    const int32_t hp_before = m->hp;
    std::vector<PacketBuffer> group;
    const bool killed = skill_damage_event(*m, h, group);       // (it applies the absorbed part, 30182)
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*m, h.dmg, hp_before, killed, 0, !may_attack_owned(*m), now, 0);
}

// ── Phase 2 ───────────────────────────────────────────────────────────────────────────────────────
// Hammer Spin's row: Rampage Wheel (4909, "Hammer Spin Booster" — a Type 1 passive whose description says it powers
// up Hammer Spin when used; its ESAction_FireWheel row has every column Hammer Spin's has, bigger) replaces it at the
// passive's level when learned — INFERRED (WEB-KO's "AP x {4.5 + DA/100 x 3}" is that row's master Ratio / DaRatio, not
// a sum); else the skill's own row.
const EsaRow* GameClientSession::hammer_wheel_row(const SkillDef& d, uint8_t level, const EsaRow* act) const
{
    (void)d; (void)level;
    for (const LearnedSkill& s : skills_) {
        if (s.id != 4909 || s.level == 0) continue;
        const SkillDef* bd = server_.skill_db().get(4909);
        if (const EsaRow* r = bd ? server_.esa_db().row(bd->esa_table, s.level) : nullptr) return r;
    }
    return act;
}

// Hammer Spin's hit (every TicTime ms): up to min(7, KillNum) monsters around us within Radius (sub_53E360's walk), each
// the physical damage of AP * (Ratio + DA * DaRatio) with ApplyRatio, element 0 — its own damage block, EXP kind 0.
void GameClientSession::hammer_wheel_pulse(const ActiveEs& w)
{
    if (dead_ || leaving_ || state_ != GameClientSessionState::InWorld) return;
    const auto now = std::chrono::steady_clock::now();
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    const std::vector<MonsterEntity*> arr =
        area_monsters(cx, cy, w.radius, std::max(0, std::min<int>(7, w.count)), now);
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_) *
                                     (static_cast<double>(w.ratio) + static_cast<double>(st_da_) * static_cast<double>(w.base_ratio)));
    for (MonsterEntity* m : arr) {
        SkillDamage h = skill_phys_damage(*m, ap2, static_cast<float>(w.hp_ratio), 0);
        const int32_t hp_before = m->hp;
        std::vector<PacketBuffer> group;
        const bool killed = skill_damage_event(*m, h, group);   // (it applies the absorbed part, 30182)
        server_.broadcast_block(zone_id_, group, 0);
        skill_after_hit(*m, h.dmg, hp_before, killed, 0, false, now, 0);
    }
}

// Party Guard (4707) / Best Shield (4905) on the party: the same skill's ES on each member of our share group (the
// same-zone, living, online members — PartyServer::group_sessions), made on its strand with us as the caster; Best
// Shield's value there = our equipment DP * PDpRatio / 100. (The report: "caster and whole party"; its range is not
// known — the share group's same-zone rule stands in for it.)
void GameClientSession::bear_party_spread(const SkillDef& d, uint8_t level, int32_t value)
{
    const uint16_t skill = static_cast<uint16_t>(d.id);
    const uint16_t me    = static_cast<uint16_t>(object_id_);
    for (uint32_t sid : server_.party().group_sessions(char_uid_))
        server_.post_to_session(sid, [skill, level, me, value](GameClientSession& s) { s.bear_party_es(skill, level, me, value); });
}

void GameClientSession::bear_party_es(uint16_t skill, uint8_t level, uint16_t caster, int32_t value)
{
    if (dead_ || leaving_ || state_ != GameClientSessionState::InWorld) return;
    const SkillDef* d = server_.skill_db().get(skill);
    if (!d) return;
    const EsaRow* act = server_.esa_db().row(d->esa_table, level);
    const EsKind kind = (d->esaction == 1528) ? EsKind::Guard : EsKind::BearStat;
    if (!es_limiter_admit(d->esclass, d->esaction, skill)) return;
    if (!es_create(*d, level, act, kind, true, 2, 0, caster)) return;
    if (d->esaction == 1544) {
        for (ActiveEs& e : es_)
            if (e.esaction == 1544 && e.value == 0 && e.result == 1) { e.value = value; break; }
        es_stat_update(10);
    }
}

// ── The Crazy Hammer chain ────────────────────────────────────────────────────────────────────────────
//  4801 Crazy Hammer (Type 1 — a passive: ESAction 1532, ESAction_CrazyHammer {Ratio, GaugeP, Seconds, DX, AtPoint,
//  SkillPoint}) and its four Type 4 passives, which have no client class and only shape the mode: Double Crazy 4804
//  (1536 {RunSpeed}), Triple Crazy 4806 (1538 {Ratio}), Quadruple Crazy 4908 (1548 {CriticalRatio}), Quintuple Crazy
//  4910 (1550 {KillNum, Radius}). The client side (VERIFIED):
//    - ESACrazyHammer (1532, reader sub_524700): the opener's tail [u32 fill][u32 max] opens the gauge window
//      (sub_8C7780, fill / max) — whoever's avatar the ES is on, so it goes to us alone. S2C 1424 [u16 actor][u32
//      fill] moves it (sub_8C7810): the dispatcher queues 1424 to the actor, whose ES container hands each message to
//      its ESs (sub_49D360 -> sub_55F800 -> ES vtbl+12) and the class's handler (sub_524680) takes 1424. Its end
//      closes the window (sub_8C77E0).
//    - ESACrazyHammerDo (1533, reader sub_524C00): the tail [u32 ms]; the sound "snd_ph_101_ap" and its effect; on our
//      own avatar the window turns to the mode (sub_8C7740(1)) at 0. It plays out its time by itself (sub_524A90);
//      its end (S2C 280) turns the window back (sub_8C7740(0)).
//  The rules (INFERRED — doc/paula_bear_skills_report.md: tooltips, a KR guide, a private-server wiki): a landed
//  normal blow (a hit / crit / strong crit — not a miss or a block) adds AtPoint and a skill fired SkillPoint; at
//  GaugeP the mode starts, paid with the ESAII_CrazyHammer MpCon (the row's one real cost; the passives' rows hold
//  1s) through the MP discount chain — not affordable: the gauge waits full. For Seconds: the normal attack's AP +
//  (base + items AP) * (Ratio + 4806 Ratio) ("base AP x Ratio", tooltip "AP +<Comment>%"); its cooldown at DX -DX
//  (WEB-KO "DX fixed at -30"); the crit chance + 4908 CriticalRatio (inside the 90 cap); run / walk * (1 + 4804
//  RunSpeed); with 4910, after each landed blow a shockwave on up to KillNum other monsters within Radius of the
//  target: the blow's AP against their DP alone (no resist, WEB-OTO), death type 0. No fill while it runs; death
//  ends it (the gauge stays). Not done: WEB-OTO's "the start breaks off what the bear was doing".
const EsaRow* GameClientSession::learned_row(int skill, uint8_t* level) const
{
    const LearnedSkill* s = find_skill(skill);
    if (!s || s->level == 0) return nullptr;
    const SkillDef* d = server_.skill_db().get(skill);
    if (!d) return nullptr;
    if (level) *level = static_cast<uint8_t>(s->level);
    return server_.esa_db().row(d->esa_table, static_cast<uint8_t>(s->level));
}

void GameClientSession::crazy_init()
{
    if (crazy_.gauge_id || !object_id_) return;
    const SkillDef* d = server_.skill_db().get(4801);
    const EsaRow* r = learned_row(4801);
    if (!d || !r) return;
    crazy_.gauge_id = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());
    crazy_.gauge    = 0;
    crazy_.max      = std::max(1, row_i(r, "GaugeP"));
    const uint16_t self = static_cast<uint16_t>(object_id_);
    std::vector<PacketBuffer> mine, none;
    PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
    o.write<uint16_t>(self);
    o.write<uint16_t>(self);
    o.write<uint16_t>(crazy_.gauge_id);
    o.write<uint16_t>(1532);
    o.write<uint8_t>(kEsKind);
    o.write<uint8_t>(static_cast<uint8_t>(d->esclass));
    o.write<uint16_t>(4801);
    o.write<uint16_t>(0);
    o.write<uint32_t>(static_cast<uint32_t>(crazy_.gauge));
    o.write<uint32_t>(static_cast<uint32_t>(crazy_.max));
    mine.push_back(std::move(o));
    server_.broadcast_block_split(zone_id_, session_id(), mine, none);
    LOG_DEBUG("GameClientSession %u: Crazy Hammer gauge (ES %u) 0/%d", session_id(), crazy_.gauge_id, crazy_.max);
}

void GameClientSession::crazy_add(bool skill)
{
    if (!crazy_.gauge_id || crazy_.do_id || dead_) return;
    const EsaRow* r = learned_row(4801);
    if (!r) return;
    const int32_t before = crazy_.gauge;
    crazy_.gauge = std::min(crazy_.max, crazy_.gauge + std::max(0, row_i(r, skill ? "SkillPoint" : "AtPoint")));
    if (crazy_.gauge >= crazy_.max) {
        crazy_start();
        if (crazy_.do_id) return;                            // (its S2C 1424 [0] went out)
    }
    if (crazy_.gauge == before) return;
    PacketBuffer p(static_cast<Opcode>(0x0590), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(object_id_));
    p.write<uint32_t>(static_cast<uint32_t>(crazy_.gauge));
    send(p);
}

void GameClientSession::crazy_start()
{
    uint8_t lv = 0;
    const SkillDef* d = server_.skill_db().get(4801);
    const EsaRow* r = learned_row(4801, &lv);
    if (!d || !r) return;
    int32_t cost = 0;
    if (const EsaRow* ii = server_.esa_db().row(d->esaii_table, lv))
        cost = mp_discount(static_cast<int32_t>(ii->get("MpCon", 0.0)));
    if (cost > 0 && mp_ < cost) return;                      // the gauge waits full
    if (cost > 0) {
        mp_ -= cost;
        PacketBuffer p(static_cast<Opcode>(0x0025), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(mp_));
        send(p);
    }
    crazy_.gauge = 0;
    {
        PacketBuffer p(static_cast<Opcode>(0x0590), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        p.write<uint32_t>(0);
        send(p);
    }
    crazy_.ap_ratio = row_f(r, "Ratio");
    crazy_.dx       = row_i(r, "DX");
    crazy_.left_ms  = 1000 * row_i(r, "Seconds");
    if (const EsaRow* t = learned_row(4806)) crazy_.ap_ratio += row_f(t, "Ratio");
    const EsaRow* q = learned_row(4908);
    crazy_.crit    = q ? static_cast<int32_t>(row_f(q, "CriticalRatio") * 100.0f + 0.5f) : 0;
    const EsaRow* s = learned_row(4804);
    crazy_.speed   = s ? row_f(s, "RunSpeed") : 0.0f;
    const EsaRow* w = learned_row(4910);
    crazy_.shock_n = w ? row_i(w, "KillNum") : 0;
    crazy_.shock_r = w ? row_i(w, "Radius") : 0;
    crazy_.do_id   = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());
    {
        const uint16_t self = static_cast<uint16_t>(object_id_);
        std::vector<PacketBuffer> group;
        PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
        o.write<uint16_t>(self);
        o.write<uint16_t>(self);
        o.write<uint16_t>(crazy_.do_id);
        o.write<uint16_t>(1533);
        o.write<uint8_t>(kEsKind);
        o.write<uint8_t>(static_cast<uint8_t>(d->esclass));
        o.write<uint16_t>(4801);
        o.write<uint16_t>(0);
        o.write<uint32_t>(static_cast<uint32_t>(crazy_.left_ms));
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    if (crazy_.speed > 0.0f) apply_move_type_speed(motion_.moving(std::chrono::steady_clock::now()) ? motion_.type : 2);
    LOG_DEBUG("GameClientSession %u: Crazy mode (ES %u, %d ms): AP + base * %.2f, DX -%d, crit +%d, speed x%.2f, "
              "shockwave %d / %d, MP -%d", session_id(), crazy_.do_id, crazy_.left_ms, crazy_.ap_ratio, crazy_.dx,
              crazy_.crit, 1.0f + crazy_.speed, crazy_.shock_n, crazy_.shock_r, cost);
}

void GameClientSession::crazy_end(bool send_end)
{
    if (!crazy_.do_id) return;
    const uint16_t id = crazy_.do_id;
    crazy_.do_id = 0;
    crazy_.left_ms = 0;
    server_.entity_mgr().free_oid(id);
    if (send_end && object_id_) {
        PacketBuffer p(static_cast<Opcode>(0x0118), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(object_id_));
        p.write<uint16_t>(id);
        server_.broadcast_zone(zone_id_, p, 0);
    }
    const bool fast = crazy_.speed > 0.0f;
    crazy_.ap_ratio = 0.0f; crazy_.dx = 0; crazy_.crit = 0; crazy_.speed = 0.0f; crazy_.shock_n = 0; crazy_.shock_r = 0;
    if (fast) apply_move_type_speed(motion_.moving(std::chrono::steady_clock::now()) ? motion_.type : 2);
    LOG_DEBUG("GameClientSession %u: Crazy mode over (ES %u)", session_id(), id);
}

void GameClientSession::crazy_tick(int elapsed_ms)
{
    if (!crazy_.do_id) return;
    crazy_.left_ms -= elapsed_ms;
    if (crazy_.left_ms <= 0) crazy_end(true);
}

void GameClientSession::crazy_carry_out()
{
    if (!crazy_.gauge_id && !crazy_.do_id) return;
    cGameServer::CrazyCarry c;
    c.gauge_id = crazy_.gauge_id;  c.do_id   = crazy_.do_id;
    c.gauge    = crazy_.gauge;     c.max     = crazy_.max;     c.left_ms = crazy_.left_ms;
    c.dx       = crazy_.dx;        c.crit    = crazy_.crit;    c.shock_n = crazy_.shock_n;  c.shock_r = crazy_.shock_r;
    c.ap_ratio = crazy_.ap_ratio;  c.speed   = crazy_.speed;
    server_.crazy_put(char_uid_, c);
    crazy_ = CrazyHammer{};                                  // (the ids now belong to the carry)
}

void GameClientSession::crazy_restore_carry()
{
    cGameServer::CrazyCarry c;
    if (server_.crazy_take(char_uid_, c)) {
        crazy_.gauge_id = c.gauge_id;  crazy_.do_id   = c.do_id;
        crazy_.gauge    = c.gauge;     crazy_.max     = c.max;     crazy_.left_ms = c.left_ms;
        crazy_.dx       = c.dx;        crazy_.crit    = c.crit;    crazy_.shock_n = c.shock_n;  crazy_.shock_r = c.shock_r;
        crazy_.ap_ratio = c.ap_ratio;  crazy_.speed   = c.speed;
        if (crazy_.speed > 0.0f) apply_move_type_speed(2);
        return;
    }
    crazy_init();
}

int GameClientSession::crazy_swing_ap() const
{
    if (!crazy_.do_id || crazy_.ap_ratio <= 0.0f) return st_ap_;
    const int32_t base_item = stat_[ST_AP] - buff_[ST_AP];
    return st_ap_ + static_cast<int>(static_cast<double>(base_item) * static_cast<double>(crazy_.ap_ratio));
}

void GameClientSession::crazy_shockwave(const MonsterEntity& main, int32_t mx, int32_t my, int ap,
                                        std::chrono::steady_clock::time_point now)
{
    if (!crazy_.do_id || crazy_.shock_n <= 0) return;
    const std::vector<MonsterEntity*> arr =
        area_monsters(mx, my, crazy_.shock_r, std::min(7, crazy_.shock_n + 1), now);
    int n = 0;
    for (MonsterEntity* m : arr) {
        if (m->object_id == main.object_id) continue;
        if (n++ >= crazy_.shock_n) break;
        SkillDamage h;
        h.dmg        = melee_damage(ap, m->dp);
        h.death_type = 0;
        h.result     = (h.dmg >= m->hp) ? 0x80 : 0;
        const int32_t hp_before = m->hp;
        std::vector<PacketBuffer> group;
        const bool killed = skill_damage_event(*m, h, group);
        server_.broadcast_block(zone_id_, group, 0);
        skill_after_hit(*m, h.dmg, hp_before, killed, 0, false, now, 0);
    }
}

void cGameServer::crazy_put(uint32_t char_uid, const CrazyCarry& c)
{
    std::lock_guard<std::mutex> lock(crazy_carry_mutex_);
    CrazyCarry e = c;
    e.at = std::chrono::steady_clock::now();
    crazy_carry_[char_uid] = e;
}

bool cGameServer::crazy_take(uint32_t char_uid, CrazyCarry& out)
{
    std::lock_guard<std::mutex> lock(crazy_carry_mutex_);
    auto it = crazy_carry_.find(char_uid);
    if (it == crazy_carry_.end()) return false;
    const CrazyCarry e = it->second;
    crazy_carry_.erase(it);
    if (std::chrono::steady_clock::now() - e.at > std::chrono::seconds(60)) return false;
    out = e;
    return true;
}
