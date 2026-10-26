#include "game_session.hpp"
#include "game_server.hpp"
#include "logger.hpp"
#include "combat_util.hpp"
#include "paula_crypt.hpp"
#include <algorithm>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
//  The Treasure Hunter (Fox, 2nd job) and its 3rd job the Thief Master — 1:1 with the retail GameServer
//  (GameServerRelease), the client's classes verified in Trickster.bin (each class's read function reads the
//  opener's tail).
//
//  The throws spend the Throw item of the common slot 20 (Player +4608, sub_894600); Precise Pitch's ESAPowerThrow
//  (1146) makes a Type-16 item's AP count Ratio times (precise_pitch_ratio). Hide (ESAHiding 1286) is Character
//  +452 bit 0; the skills with SkillOption 128 (Sudden Attack, Poison Assault) need it (skill_use). The mines are ESs
//  of ours (es.cpp EsKind::Mine) set off here. The skill-resist ESs (Strong Mind / Gas Mask / Lightning Rod) go through
//  esa_prevent_slow (skill_sense.cpp).
// ─────────────────────────────────────────────────────────────────────────────

namespace {
float row_f(const EsaRow* r, const char* k) { return r ? static_cast<float>(r->get(k, 0.0)) : 0.0f; }
int   row_i(const EsaRow* r, const char* k) { return r ? static_cast<int>(r->get(k, 0.0)) : 0; }

void put_pos(PacketBuffer& p, int32_t x, int32_t y)
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

// vtbl+264 (sub_72BC10): the EXP share of a hit — min(dmg, HP) * EXP / MaxHP, an int32 product (as gain_attack_exp).
int32_t exp_share_of(const MonsterEntity& m, int32_t dmg, int32_t hp_before)
{
    if (m.hp_max <= 0) return 0;
    const int32_t part = std::max(0, std::min(dmg, hp_before));
    const int32_t prod = static_cast<int32_t>(static_cast<uint32_t>(part) * m.exp_reward);
    return prod / m.hp_max;
}

struct Hit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };

// sub_5205F0 for a monster target (no ESAShield / ESAGuard / 1463 / 1492 of a player to soak the blow): a throw that
// missed, was blocked or did nothing records {0, result, -1}; else it adds up (+dmg, a hit more) and records {dmg,
// result, -1}.
struct ThrowRec { int32_t dmg; int32_t result; uint16_t info; };
void add_throw(int32_t dmg, int32_t result, int32_t& total, uint16_t& hits, std::vector<ThrowRec>& recs)
{
    if (dmg <= 0 || result == 1 || result == 2) {
        recs.push_back({ 0, result, 0xFFFF });
        return;
    }
    total += dmg;
    ++hits;
    recs.push_back({ dmg, result, 0xFFFF });
}
} // namespace

// ── Helpers ───────────────────────────────────────────────────────────────────────────────────────────

// Character +452 bit 0 (sub_570EE0) and S2C 827 [u16 id][u32 +452] to the area around us (sub_6035B0 -> sub_60D230).
// VERIFIED: the client's case 827 (sub_6F0780) puts the u32 into the actor's +1200 and sub_6BCD40 shows its bit 0 —
// half-seen to ourselves (and our pet / mount), unseen to the others. (Its pet +1712 — its +1684 too — and its mount
// +7112 get theirs: neither exists here.)
void GameClientSession::hide_set(bool on)
{
    hiding_ = on;
    PacketBuffer p(static_cast<Opcode>(0x033B), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(object_id_));
    p.write<uint32_t>(on ? 1u : 0u);
    server_.broadcast_zone(zone_id_, p, 0);
    LOG_DEBUG("GameClientSession %u: hide %s", session_id(), on ? "on" : "off");
}

// A hidden player's limiter vtbl+60(1286): its ESAHiding ends (the hide off with it).
void GameClientSession::hide_break()
{
    if (hiding_) es_remove_esaction(1286);
}

// Precise Pitch (3101) with its ESAPowerThrow (1146) up: the learned level's row Ratio (+4) — sub_552DF0 / sub_5D9200 /
// sub_5DD580 / sub_58D4F0; else 1.0. Chain / Rain of Knives (sub_5DCC00 / sub_5DED50) test the learned level the other
// way round (<= 0) and so never read it: 1.0 for a learned skill (retail).
float GameClientSession::precise_pitch_ratio(bool retail_inverted) const
{
    if (!es_find(1146)) return 1.0f;
    const LearnedSkill* pp = find_skill(3101);
    if (!pp) return 1.0f;
    if (retail_inverted || pp->level <= 0) return 1.0f;
    const SkillDef* sd = server_.skill_db().get(3101);
    const EsaRow* row = sd ? server_.esa_db().row(sd->esa_table, pp->level) : nullptr;
    return row ? static_cast<float>(row->get("Ratio", 0.0)) : 1.0f;
}

int32_t GameClientSession::throw_item_count() const
{
    const BagItem* b = throw_item_ ? const_cast<GameClientSession*>(this)->bag_find_uid(throw_item_) : nullptr;
    return b ? static_cast<int32_t>(b->count) : 0;
}

// min(the bag's weight (+1716), MaxWT (+584)).
int32_t GameClientSession::bag_weight_capped() const
{
    return (bag_weight_ <= st_maxwt_) ? bag_weight_ : st_maxwt_;
}

// ── ESAction 1214 "Pouch of Pain" (GameServer sub_5CD6C0, ESAHeavyBaggage; ESAUI 1, ESAIC 2) ───────────
//  The selected target (zone vtbl+16 kind 3) — none: nothing. The row ESAction_HeavyBaggage (sub_51BBF0: ApplyRatio
//  +0, Ratio +4, Radius +8, Comment +10). The targets (sub_5CD880): the target first, then the zone's monsters in list
//  order — not dead, not the target, our layer, not Type 8 / 12, a shadow one only with the Third Eye — within Radius
//  of the TARGET (sqrtf); such a one once 5 are taken stops the walk; one with no owner or ours (our party's:
//  owned_by_other) is taken (players only in PvP, sub_5CDBB0). The block: the opener sub_5EE050 + [u16 target][u16 n][u16 id]*n
//  (VERIFIED: the client's ESAHeavyBaggage 0x4E2AC0 reads them); each one (sub_5CDD30): AP' = (int)(min(the bag's
//  weight, MaxWT) * Ratio) against its DP (sub_5E6040, ApplyRatio, element 0), its EXP share, the damage event, EXP
//  30003 kind 0. Then the skill's use TM. No 30107.
void GameClientSession::esa_heavy_baggage(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    const float apply  = row_f(act, "ApplyRatio");
    const float ratio  = row_f(act, "Ratio");
    const int16_t radius = static_cast<int16_t>(row_i(act, "Radius"));
    int32_t tx, ty;
    t->motion.pos(now, tx, ty);
    std::vector<uint16_t> ids{ tid };
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0 || oid == tid || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (!(static_cast<double>(dist_f(mx, my, tx, ty)) <= static_cast<double>(radius))) continue;
        if (ids.size() >= 5) break;
        if (owned_by_other(*m)) continue;
        ids.push_back(static_cast<uint16_t>(oid));
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    o.write<uint16_t>(static_cast<uint16_t>(ids.size()));
    for (uint16_t v : ids) o.write<uint16_t>(v);
    group.push_back(std::move(o));
    const int ap2 = static_cast<int>(static_cast<double>(bag_weight_capped()) * static_cast<double>(ratio));
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
    LOG_DEBUG("GameClientSession %u: Pouch of Pain at mob %u (weight %d, AP' %d): %zu hit", session_id(), tid,
              bag_weight_capped(), ap2, ids.size());
}

// ── ESAction 1242 "Sticky Icky" (GameServer sub_5D0DC0; no selection) ────────────────────────────────────
//  The row ESAction_Adhesive (sub_51C300: ApplyRatio +0, HvDRatio +4, LkDRatio +8, DpDRatio +12, Seconds +16, Radius
//  +18) kept with us as the caster. The zone's monsters in list order (sub_5D0EB0) — not dead, our layer, not Type 8 /
//  12, a shadow one only with the Third Eye — within Radius of us; such a one once 4 are taken stops the walk; one with
//  no owner or ours gets (sub_5D1170) — a monster with a DebuffSet first rolls against 3106's DebuffBitCode (sub_891F80;
//  all resisted -> S2C 1247) — the action {ESAction 1243, EState 0, ESClass 1, flags 2, skill 3106, the row} through
//  sub_524690(.., 1): the monster's limiter, its ESAAdhesiveDo (es.cpp). (Players in PvP, sub_5D12B0.) No packet, no
//  TM of its own (each ES gives its TM per hit).
void GameClientSession::esa_adhesive(const SkillDef& d, uint8_t lv, const EsaRow* act,
                                     std::chrono::steady_clock::time_point now)
{
    const float radius = static_cast<float>(static_cast<int16_t>(row_i(act, "Radius")));
    int32_t px, py;
    cur_pos(now, px, py);
    std::vector<MonsterEntity*> picks;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0 || !area_target_ok(*m)) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (!(static_cast<double>(radius) >= static_cast<double>(dist_f(mx, my, px, py)))) continue;
        if (picks.size() >= 4) break;
        if (owned_by_other(*m)) continue;
        picks.push_back(m);
    }
    SkillDef dd = d;
    dd.esaction = 1243;
    dd.estate   = 0;
    dd.esclass  = 1;
    for (MonsterEntity* m : picks) {
        uint32_t bits = 0;
        if (monster_debuff_immune(*m, d.debuff_bits, bits)) { send_debuff_resisted(*m, bits); continue; }
        es_resisted_ = bits;
        es_apply_on(*m, dd, lv, act);
        es_resisted_ = 0;
    }
    LOG_DEBUG("GameClientSession %u: Sticky Icky (radius %.0f): %zu monster(s)", session_id(), radius, picks.size());
}

// ── ESAction 1326 "Fan of Knives" (GameServer sub_5D8D80, ESAWideThrow; ESAUI 7: an object + the item) ─────
//  The selected target (kind 3) we may attack (vtbl+164) — else nothing. The row ESAction_WideThrow (sub_51C210:
//  ApplyRatio +0, Ratio +4, Radius +8 i16, Angle +12 float, MonNum +16, Comment +20). The targets: the target first,
//  then (sub_5D48B0 -> sub_5E09D0) the zone's monsters in list order while fewer than min(MonNum, 7) - 1 are taken —
//  each one (sub_5E0AB0) not dead, on our layer, not Type 8 (Type 12 ones are taken: retail), a shadow one only with
//  the Third Eye, within Radius of US (the int Radius < sqrtf excludes), its direction from the target within ±Angle
//  of ours to it (sub_893D30 — the target itself, at its own spot, gives no angle and is not taken again) and no owner
//  or ours. No Throw item (+4608): nothing more. Else n of it is used up (sub_894600) and the block: the opener
//  sub_5EE050 + [u16 n][u16 id]*n[u16 the payload's item id][u64 its uid] (VERIFIED: the client's ESAWideThrow 0x5027E0
//  reads them); each one (sub_5D9010): AP' (sub_5D9180) of the payload's item — Type 16: (int)((its AP * r + 10 * DA)
//  * Ratio), r = precise_pitch_ratio; else its Weight / 2; at least 1 — against its DP (sub_5E6040, ApplyRatio, element
//  0), its EXP share, the damage event, EXP 30003 kind 0. Then the skill's TM per hit on the target (sub_891C80).
//  DEVIATION (anti-cheat, as Shuriken Master): the retail trusts the payload's item for the damage; we require it to
//  be the equipped Throw item's id (else nothing).
void GameClientSession::esa_wide_throw(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const ThrowPick pk = throw_pick(su_.payload);
    const uint16_t tid = pk.target ? pk.target : su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    if (!may_attack_owned(*t)) return;
    int mon_num = row_i(act, "MonNum");
    if (mon_num > 7) mon_num = 7;
    const int16_t radius = static_cast<int16_t>(row_i(act, "Radius"));
    const float   angle  = row_f(act, "Angle");
    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    t->motion.pos(now, tx, ty);
    std::vector<uint16_t> ids{ tid };
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        if (static_cast<int>(ids.size()) - 1 >= mon_num - 1) break;
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0 || m->banished) continue;
        if (const MobDef* md = server_.mob_db().get(static_cast<int>(m->mob_id)))
            if (md->type == 8) continue;
        if (m->shadow() && !third_eye_) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (static_cast<double>(radius) < static_cast<double>(dist_f(ax, ay, mx, my))) continue;
        if (!server_in_line(static_cast<int16_t>(ax), static_cast<int16_t>(ay), static_cast<int16_t>(tx),
                            static_cast<int16_t>(ty), static_cast<int16_t>(mx), static_cast<int16_t>(my), angle))
            continue;
        if (owned_by_other(*m)) continue;
        ids.push_back(static_cast<uint16_t>(oid));
    }
    const BagItem* tb = throw_item_ ? bag_find_uid(throw_item_) : nullptr;
    if (!tb) return;
    if (pk.item != tb->item_id) {
        LOG_INFO("GameClientSession %u: Fan of Knives with item %u, the Throw item is %u — refused", session_id(), pk.item,
                 tb->item_id);
        return;
    }
    consume_throw_item(static_cast<int>(ids.size()));
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(static_cast<uint16_t>(ids.size()));
    for (uint16_t v : ids) o.write<uint16_t>(v);
    o.write<uint16_t>(static_cast<uint16_t>(pk.item));
    o.write<uint32_t>(pk.uid_lo);
    o.write<uint32_t>(pk.uid_hi);
    group.push_back(std::move(o));
    int ap2 = 0;
    if (const ItemDef* it = server_.item_db().get(pk.item)) {
        if (it->type == 16)
            ap2 = static_cast<int>((static_cast<double>(it->ap) * static_cast<double>(precise_pitch_ratio(false)) +
                                    static_cast<double>(10 * st_da_)) * static_cast<double>(row_f(act, "Ratio")));
        else
            ap2 = it->weight / 2;
    }
    if (ap2 <= 0) ap2 = 1;
    const float apply = row_f(act, "ApplyRatio");
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
    skill_hit_tm(*t, su_.skill);
    LOG_DEBUG("GameClientSession %u: Fan of Knives at mob %u (AP' %d): %zu hit", session_id(), tid, ap2, ids.size());
}

// ── ESAction 1362 "Chain of Knives" (GameServer sub_5DC6A0, ESAChainThrow; ESAUI 7) ────────────────────────
//  The selected target (kind 3) — none: nothing. The row ESAction_ChainThrow (sub_51C690: ApplyRatio +0, Ratio +4,
//  Comment +8, Count +10 i16). The throws = min(Count, the Throw item's amount (+4608 vtbl+48)) (+40; no Throw item: a
//  null read in the retail — here nothing). AP' (sub_5DCC00) = (int)((the Throw item's AP * r + 16 * DA) * Ratio *
//  (throws / Count)), at least 1 — r is Precise Pitch's ratio by the inverted test (1.0: retail). Each throw (sub_5DC9B0)
//  rolls the physical damage (sub_5E6040: AP', its DP, ApplyRatio, element 0); a hit neither missed nor blocked is
//  divided by the throws; sub_5205F0 records it (sub_5204B0: no shield of a player on a monster). The block: the opener
//  sub_5EE050 + [pos us][u32 Throw item id][u64 its uid][u8 throws][u16 target][pos target][u8 throws]{[u32 damage]
//  [u32 result][u16 -1]}*throws (sub_5DC8D0; VERIFIED: the client's ESAChainThrow 0x50FC80 reads them, at most 4),
//  then one damage event (sub_5DCAC0): DamageInfo {the sum, death type 2, result 0, element 0, flags 0x8000, [6] = the
//  hits}, its EXP share, 30003 kind 0, 30002. Then the throws are used up, the skill's use TM, our mark (vtbl+132(target,
//  5000)). No 30107.
void GameClientSession::esa_chain_throw(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    const BagItem* tb = throw_item_ ? bag_find_uid(throw_item_) : nullptr;
    if (!tb) return;
    const int count  = static_cast<int16_t>(row_i(act, "Count"));
    const int throws = std::min(count, static_cast<int>(tb->count));
    const uint32_t item_id = tb->item_id;
    const uint64_t uid = tb->uid;
    int ap2 = 1;
    if (const ItemDef* it = server_.item_db().get(item_id)) {
        ap2 = static_cast<int>((static_cast<double>(it->ap) * static_cast<double>(precise_pitch_ratio(true)) +
                                static_cast<double>(16 * st_da_)) * static_cast<double>(row_f(act, "Ratio")) *
                               (static_cast<double>(throws) / static_cast<double>(count)));
        if (ap2 <= 0) ap2 = 1;
    }
    const float apply = row_f(act, "ApplyRatio");
    int32_t total = 0;
    uint16_t hits = 0;
    std::vector<ThrowRec> recs;
    for (int i = 0; i < throws; ++i) {
        SkillDamage h = skill_phys_damage(*t, ap2, apply, 0, t->dp);
        if (h.absorbed > 0) absorb_heal(*t, h.absorbed);   // 30182 in the roll (sub_5E6760)
        if (h.dmg > 0 && h.result != 1 && h.result != 2) h.dmg /= throws;
        add_throw(h.dmg, h.result, total, hits, recs);
    }
    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    t->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    put_pos(o, ax, ay);
    o.write<uint32_t>(item_id);
    o.write<uint32_t>(static_cast<uint32_t>(uid & 0xFFFFFFFFu));
    o.write<uint32_t>(static_cast<uint32_t>(uid >> 32));
    o.write<uint8_t>(static_cast<uint8_t>(throws));
    o.write<uint16_t>(tid);
    put_pos(o, tx, ty);
    o.write<uint8_t>(static_cast<uint8_t>(throws));
    for (const ThrowRec& r : recs) {
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
    consume_throw_item(throws);
    skill_use_tm(su_.skill);
    mark_monster_owner(*t);                                   // vtbl+132(target, 5000): no owner yet -> ours
    LOG_DEBUG("GameClientSession %u: Chain of Knives at mob %u: %d throw(s), %u hit, %d damage%s", session_id(), tid,
              throws, hits, total, killed ? " [KILLED]" : "");
}

// ── ESAction 1475 "Rain of Knives" (GameServer sub_5DE6E0, ESAUpThrow; ESAUI 6 / ESAIC 3: a point) ─────────
//  The row ESAction_UpThrow (sub_51DBB0: ApplyRatio +0, KillNum +4, Count +8, KnifeCount +12, Ratio +16, Comment +20,
//  Radius +24). sub_53E360 around the point (the payload's [u16 x][u16 y]), at most KillNum, Radius. AP' (sub_5DED50) =
//  (int)((the Throw item's AP * r + 10 * DA) * Ratio), at least 1 (no Throw item: 1; r by the inverted test: 1.0).
//  Each target takes Count rolls (sub_5DEA60: sub_5E6040 against its DP, ApplyRatio, element 0 — not divided), recorded
//  as Chain of Knives'; then one damage event each (sub_5DEBB0): {its sum, 2, 0, 0, flags 0x8000, [6] = its hits}, the
//  EXP share, 30003 kind 0, 30002. Then KnifeCount of the Throw item is used up and the skill's use TM.
//  The opener's tail: the client's ESAUpThrow reader is replaced (the private server's .myown patch: 0x51DCF0 jumps to
//  0x1272E38) by one that reads [u16 n (at most 32)][u16 id]*n — a knife on each — so that is what goes out (the
//  client wins); the retail tail sub_5DE930 — [pos][u32 Throw item id][u64 uid][u8 KnifeCount][u8 n]{[u16 id][u8
//  Count]{[u32][u32][u16]}*Count}*n — is what the original reader (still after the jmp) read.
void GameClientSession::esa_up_throw(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const BagItem* tb = throw_item_ ? bag_find_uid(throw_item_) : nullptr;
    if (!tb) return;                                          // (the retail reads the Throw item unchecked)
    int16_t px = 0, py = 0;
    if (su_.payload.size() >= 4) {
        px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
        py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
    }
    const int kill_num = row_i(act, "KillNum");
    const int count    = row_i(act, "Count");
    const int knives   = row_i(act, "KnifeCount");
    const int radius   = row_i(act, "Radius");
    const float apply  = row_f(act, "ApplyRatio");
    std::vector<MonsterEntity*> targets = area_monsters(px, py, radius, kill_num, now);
    int ap2 = 1;
    if (const ItemDef* it = server_.item_db().get(tb->item_id)) {
        ap2 = static_cast<int>((static_cast<double>(it->ap) * static_cast<double>(precise_pitch_ratio(true)) +
                                static_cast<double>(10 * st_da_)) * static_cast<double>(row_f(act, "Ratio")));
        if (ap2 <= 0) ap2 = 1;
    }
    struct Tot { int32_t total; uint16_t hits; };
    std::vector<Tot> tots;
    for (MonsterEntity* m : targets) {
        Tot tt{ 0, 0 };
        std::vector<ThrowRec> recs;
        for (int j = 0; j < count; ++j) {
            const SkillDamage h = skill_phys_damage(*m, ap2, apply, 0, m->dp);
            if (h.absorbed > 0) absorb_heal(*m, h.absorbed);
            add_throw(h.dmg, h.result, tt.total, tt.hits, recs);
        }
        tots.push_back(tt);
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(static_cast<uint16_t>(targets.size()));
    for (MonsterEntity* m : targets) o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
    group.push_back(std::move(o));
    std::vector<Hit> done;
    for (size_t i = 0; i < targets.size(); ++i) {
        MonsterEntity* m = targets[i];
        SkillDamage c;
        c.dmg        = tots[i].total;
        c.death_type = 2;
        c.result     = 0;
        c.flags      = 0x8000;
        c.extra      = tots[i].hits;
        const int32_t hp_before = m->hp;
        const bool killed = skill_damage_event(*m, c, group);
        done.push_back({ m, c.dmg, hp_before, killed });
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now);
    consume_throw_item(knives);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Rain of Knives at (%d,%d) radius %d: %zu target(s), %d roll(s) each", session_id(),
              px, py, radius, targets.size(), count);
}

// ── ESAction 1290 "Sudden Attack" (GameServer sub_5D5CE0, ESASuddenAttack; ESAUI 1; SkillOption 128: hidden) ─
//  The selected target (kind 3) — none: nothing. The row ESAction_SuddenAttack (sub_51BA50: ApplyRatio +0, Ratio +4).
//  The block {the opener sub_5EDE00 [us][target][0]... with no tail (VERIFIED: the client's ESASuddenAttack 0x4F7EE0
//  reads none), the damage (sub_5D5DB0): AP' = (int)((AP + 16 * (LK + DA)) * Ratio) against its DP — sub_5E6040 gets
//  the row's Ratio (+4) as its ApplyRatio, not +0 (retail) —, element 0; its EXP share, 30002, EXP 30003 kind 0}. Then
//  the skill's use TM.
void GameClientSession::esa_sudden_attack(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    const float ratio = row_f(act, "Ratio");
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_ + 16 * (st_lk_ + st_da_)) * static_cast<double>(ratio));
    const SkillDamage h = skill_phys_damage(*t, ap2, ratio, 0);
    const int32_t hp_before = t->hp;
    std::vector<PacketBuffer> group;
    group.push_back(skill_opener(d, tid));
    const bool killed = skill_damage_event(*t, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*t, h.dmg, hp_before, killed, 0, false, now);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Sudden Attack -> mob %u AP' %d dmg=%d result=%d%s", session_id(), tid, ap2, h.dmg,
              h.result, killed ? " [KILLED]" : "");
}

// ── ESAction 1344 "Poison Pouch" (GameServer sub_5DB030, ESAPoisonBaggage; ESAUI 1) ─────────────────────────
//  The selected target (kind 3) — none: nothing. The row ESAction_PoisonBaggage (sub_51A360: ApplyRatio +0, Ratio +4,
//  Seconds +8). AP' = (int)(min(the bag's weight, MaxWT) * r), r = the Ratio of Pouch of Pain's (3105) LEVEL 11 row
//  when it is learned (sub_50F540(.., 11) — whatever its level: retail), else 0. The physical damage (sub_5E6040,
//  ApplyRatio, element 0), its EXP share; the block {the opener sub_5EE050 + [u16 target][pos us][pos target]
//  (sub_5DB370; VERIFIED: the client's ESAPoisonBaggage 0x508BD0 reads the target), the damage event}; EXP 30003 kind 0;
//  the skill's use TM. Then a miss / block / guard / shield (result 1 / 2 / 6 / 8) or no EXP share: one Throw item is
//  used up when there is one (retail); else (sub_5DB3B0) the poison — a monster with a DebuffSet first rolls against
//  3305's DebuffBitCode (all resisted -> S2C 1247) — {ESAction 1350, EState 0, ESClass 0, flags 2, skill 3305, the
//  row} through sub_524690(.., 1): the monster's ESAPoisonBaggageDo (poison_do_on).
void GameClientSession::esa_poison_baggage(const SkillDef& d, uint8_t lv, const EsaRow* act,
                                           std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    float r = 0.0f;
    if (find_skill(3105)) {
        const SkillDef* pd = server_.skill_db().get(3105);
        const EsaRow* row = pd ? server_.esa_db().row(pd->esa_table, 11) : nullptr;
        r = row_f(row, "Ratio");
    }
    const int ap2 = static_cast<int>(static_cast<double>(bag_weight_capped()) * static_cast<double>(r));
    const SkillDamage h = skill_phys_damage(*t, ap2, row_f(act, "ApplyRatio"), 0);
    const int32_t hp_before = t->hp;
    const int32_t share = exp_share_of(*t, h.dmg, hp_before);
    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    t->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    put_pos(o, ax, ay);
    put_pos(o, tx, ty);
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*t, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*t, h.dmg, hp_before, killed, 0, false, now);
    skill_use_tm(su_.skill);
    const int res = h.result;
    if (res == 1 || res == 2 || res == 6 || res == 8 || share == 0) {
        if (throw_item_) consume_throw_item(1);
    } else {
        uint32_t bits = 0;
        if (monster_debuff_immune(*t, d.debuff_bits, bits)) {
            send_debuff_resisted(*t, bits);
        } else {
            SkillDef dd = d;
            dd.esaction = 1350;
            dd.estate   = 0;
            dd.esclass  = 0;
            es_resisted_ = bits;
            es_apply_on(*t, dd, lv, act);
            es_resisted_ = 0;
        }
    }
    LOG_DEBUG("GameClientSession %u: Poison Pouch -> mob %u AP' %d dmg=%d result=%d", session_id(), tid, ap2, h.dmg,
              h.result);
}

// ── ESAction 1383 "Poison Assault" (GameServer sub_5DD020, ESAPoisonAssault; ESAUI 7; SkillOption 128: hidden) ─
//  The selected target — not ourselves — (kind 3) — none: nothing; a Throw item with at least 1 left — else nothing.
//  The row ESAction_PoisonAssault (sub_51D1E0: ApplyRatio +0, Ratio +4, Seconds +8, DamagePerSec +12, PosionRatio +16).
//  base (sub_5DD510) of the Throw item: Type 16 -> (int)(its AP * r) (r = precise_pitch_ratio), else its Weight / 2; at
//  least 1. AP' = (int)(base + DA * 16 * Ratio). The physical damage (ApplyRatio, element 0), its EXP share; the block
//  {the opener sub_5EE050 + [u16 target][pos us][pos target][u16 Throw item id][u64 its uid] (sub_5DD380; VERIFIED: the
//  client's ESAPoisonAssault 0x515AC0 reads them), the damage event}; EXP 30003 kind 0; the use TM. Then a miss / block
//  / guard / shield or no EXP share: one Throw item used up; else (sub_5DD3F0) the poison — DebuffSet roll on 3306's
//  code, all resisted -> S2C 1247 — {ESAction 1384, EState 0, ESClass 0, flags 2, skill 3306, the row}: the monster's
//  ESAPoisonAssaultDo (poison_do_on), which uses up the Throw item.
void GameClientSession::esa_poison_assault(const SkillDef& d, uint8_t lv, const EsaRow* act,
                                           std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    if (tid == static_cast<uint16_t>(object_id_)) return;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    const BagItem* tb = throw_item_ ? bag_find_uid(throw_item_) : nullptr;
    if (!tb || tb->count < 1) return;
    const uint32_t item_id = tb->item_id;
    const uint64_t uid = tb->uid;
    int base = 0;
    if (const ItemDef* it = server_.item_db().get(item_id)) {
        if (it->type == 16) base = static_cast<int>(static_cast<double>(it->ap) * static_cast<double>(precise_pitch_ratio(false)));
        else                base = it->weight / 2;
    }
    if (base <= 0) base = 1;
    const int ap2 = static_cast<int>(static_cast<double>(base) +
                                     static_cast<double>(st_da_) * 16.0 * static_cast<double>(row_f(act, "Ratio")));
    const SkillDamage h = skill_phys_damage(*t, ap2, row_f(act, "ApplyRatio"), 0);
    const int32_t hp_before = t->hp;
    const int32_t share = exp_share_of(*t, h.dmg, hp_before);
    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    t->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(tid);
    put_pos(o, ax, ay);
    put_pos(o, tx, ty);
    o.write<uint16_t>(static_cast<uint16_t>(item_id));
    o.write<uint32_t>(static_cast<uint32_t>(uid & 0xFFFFFFFFu));
    o.write<uint32_t>(static_cast<uint32_t>(uid >> 32));
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*t, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*t, h.dmg, hp_before, killed, 0, false, now);
    skill_use_tm(su_.skill);
    const int res = h.result;
    if (res == 1 || res == 2 || res == 6 || res == 8 || share == 0) {
        if (throw_item_) consume_throw_item(1);
    } else {
        uint32_t bits = 0;
        if (monster_debuff_immune(*t, d.debuff_bits, bits)) {
            send_debuff_resisted(*t, bits);
        } else {
            SkillDef dd = d;
            dd.esaction = 1384;
            dd.estate   = 0;
            dd.esclass  = 0;
            es_resisted_ = bits;
            es_apply_on(*t, dd, lv, act);
            es_resisted_ = 0;
        }
    }
    LOG_DEBUG("GameClientSession %u: Poison Assault -> mob %u AP' %d dmg=%d result=%d", session_id(), tid, ap2, h.dmg,
              h.result);
}

// ESAPoisonBaggageDo (1350, sub_583DB0) / ESAPoisonAssaultDo (1384, sub_58CE10) — the action carries the skill's row.
//  1350: +244.. = the row's first 12 bytes (ApplyRatio, Ratio, Seconds), +264 = 0, +8 = 0, +260 = 1000 * Seconds, its
//  caster (+196) and target (+220) — either gone: no object —; +256 = (int)(min(the caster's bag weight, MaxWT) * the
//  Ratio of Pouch of Pain's level-11 row, 0 without it); the lifetime +260; the block {S2C 224 sub_5EDE00 [us][target]
//  [ES id][1350][2][ESClass 0][3305][0], no tail — VERIFIED: the client's ESAPoisonBaggageDo 0x509320 reads none}. No
//  mark, no TM.
//  1384: its caster (+196) and target (+200) — none, or the same: no object —; +240 = 0 (the stop flag), +228 = 0, +8 =
//  0, +224 = 1000 * Seconds, +232 = 1000 * DamagePerSec (the pulse gap); the lifetime +224; the block {S2C 224 [us]
//  [target][ES id][1384][2][0][3306][0], no tail — VERIFIED: the client's ESAPoisonAssaultDo 0x515F90 reads none}; then
//  +236 = (int)(PosionRatio * ((base + DA * 16) * Ratio)) — base (sub_58D470) of the Throw item: Type 16 -> (int)(AP *
//  r), else Weight / 2, at least 1 —; one Throw item used up; a dead target: no object (after all that, retail).
//  Their ticks: es.cpp monster_es_tick; their hits: poison_hit.
bool GameClientSession::poison_do_on(MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act)
{
    e.flags    = 2;
    e.elapsed_ms = 0;
    e.pulse_ms = 0;
    e.phase    = 0;
    e.end_ms   = 1000 * static_cast<int16_t>(row_i(act, "Seconds"));
    e.life_ms  = e.end_ms;
    e.ratio    = row_f(act, "Ratio");
    if (e.esaction == 1350) {
        float r = 0.0f;
        if (find_skill(3105)) {
            const SkillDef* pd = server_.skill_db().get(3105);
            const EsaRow* row = pd ? server_.esa_db().row(pd->esa_table, 11) : nullptr;
            r = row_f(row, "Ratio");
        }
        e.value = static_cast<int32_t>(static_cast<double>(bag_weight_capped()) * static_cast<double>(r));
    } else {
        e.phase_ms = 1000 * static_cast<int16_t>(row_i(act, "DamagePerSec"));
    }
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
        o.write<uint16_t>(static_cast<uint16_t>(e.estate));
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    if (e.esaction == 1384) {
        int base = 0;
        if (const BagItem* tb = throw_item_ ? bag_find_uid(throw_item_) : nullptr)
            if (const ItemDef* it = server_.item_db().get(tb->item_id)) {
                if (it->type == 16) base = static_cast<int>(static_cast<double>(it->ap) *
                                                            static_cast<double>(precise_pitch_ratio(false)));
                else                base = it->weight / 2;
            }
        if (base <= 0) base = 1;
        e.value = static_cast<int32_t>(static_cast<double>(row_f(act, "PosionRatio")) *
                                       ((static_cast<double>(base) + static_cast<double>(st_da_) * 16.0) *
                                        static_cast<double>(e.ratio)));
        if (throw_item_) consume_throw_item(1);
        if (!m.alive) return false;
    }
    m.es.push_back(e);
    LOG_DEBUG("GameClientSession %u: poison %d (ES %u) on mob %u: %d per hit, %d ms", session_id(), e.esaction, e.id,
              static_cast<unsigned>(m.object_id), e.value, e.end_ms);
    return true;
}

// The poison hits. ESAPoisonBaggageDo (sub_584080): the physical damage sub_5E6040(+256, its DP, ApplyRatio 1.0,
// element 0), a damage > 0 times Ratio (+248); its EXP share; 30002; EXP 30003 kind 0 when we may attack it (vtbl+164).
// ESAPoisonAssaultDo (sub_58D170): DamageInfo {+236 — 1 on a Type-13 monster —, death type 2, result 0, element 0,
// flags 2048}; the same after. (Not in a command block retail; the burns here go as their own block.)
void GameClientSession::poison_hit(uint16_t mob, uint16_t es_id)
{
    if (leaving_ || state_ != GameClientSessionState::InWorld) return;
    MonsterEntity* m = server_.entity_mgr().get_monster(mob);
    if (!m || m->zone != zone_id_ || !m->alive || m->hp <= 0) return;
    int esaction = 0;
    int32_t value = 0;
    float ratio = 0.0f;
    {
        std::lock_guard<std::recursive_mutex> lk(server_.mob_es_mutex());
        const MonsterEntity::Es* e = nullptr;
        for (const MonsterEntity::Es& x : m->es) if (x.id == es_id) { e = &x; break; }
        if (!e) return;
        esaction = e->esaction; value = e->value; ratio = e->ratio;
    }
    const auto now = std::chrono::steady_clock::now();
    SkillDamage h;
    if (esaction == 1350) {
        h = skill_phys_damage(*m, value, 1.0f, 0, m->dp);
        if (h.dmg > 0) h.dmg = static_cast<int32_t>(static_cast<double>(h.dmg) * static_cast<double>(ratio));
    } else {
        const MobDef* md = server_.mob_db().get(static_cast<int>(m->mob_id));
        h.dmg        = (md && md->type == 13) ? 1 : value;
        h.death_type = 2;
        h.result     = 0;
        h.element    = 0;
        h.flags      = 2048;
    }
    const int32_t hp_before = m->hp;
    std::vector<PacketBuffer> group;
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*m, h.dmg, hp_before, killed, 0, !may_attack_owned(*m), now, 0);
}

// ── ESAction 1490 "Exploit Weakness" (GameServer sub_5DFCF0, ESACrackAttack; ESAUI 1) ───────────────────────
//  The selected target (kind 3) — none: nothing. The row ESAction_CrackAttack (sub_51E080: ApplyRatio +0, Ratio +4).
//  The block {the opener sub_5EDE00 [us][target][0]... with no tail (VERIFIED: the client's ESACrackAttack 0x51FC00
//  reads none), the damage (sub_5DFDC0): AP' = (int)((AP + 16 * DA) * Ratio) against its DP less its StatusValueAdder
//  chain's DP (sub_716820(10): the base DP, debuffs or buffs off), ApplyRatio, element 0; DamageInfo flags |= 0x2000;
//  its EXP share, 30002, EXP 30003 kind 0}. Then the skill's use TM.
void GameClientSession::esa_crack_attack(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const uint16_t tid = su_.target;
    MonsterEntity* t = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!t || t->zone != zone_id_ || !t->alive) return;
    const int ap2 = static_cast<int>(static_cast<double>(st_ap_ + 16 * st_da_) * static_cast<double>(row_f(act, "Ratio")));
    const int dp  = t->base_set ? t->base_dp : t->dp;
    SkillDamage h = skill_phys_damage(*t, ap2, row_f(act, "ApplyRatio"), 0, dp);
    h.flags |= 0x2000;
    const int32_t hp_before = t->hp;
    std::vector<PacketBuffer> group;
    group.push_back(skill_opener(d, tid));
    const bool killed = skill_damage_event(*t, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(*t, h.dmg, hp_before, killed, 0, false, now);
    skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Exploit Weakness -> mob %u AP' %d (DP %d) dmg=%d result=%d%s", session_id(), tid, ap2,
              dp, h.dmg, h.result, killed ? " [KILLED]" : "");
}

// ── The mines' tick (ESAMine sub_565600 / ESAMineMaster sub_56E060) ─────────────────────────────────────────
//  Its caster gone: the end (it is ours). +240 += dt; past +244 (>) the end. Past 3200 ms (armed): the zone's monsters
//  in list order (sub_565750 / sub_56E1B0) — the first one not dead, on our layer, not Type 8 / 12, a shadow one only
//  with the Third Eye, within Radius of the point (Radius >= sqrtf) and with no owner or ours — sets it off: the action
//  {ESAction 1193 / 1268, EState 0, ESClass -1, flags 2, skill 3104 — both, retail —, the mine's row, the payload [pos
//  the point][u16 that monster]} through the ESAction switch (sub_5659C0 / sub_56E420 -> sub_5C0250): mine_detonate,
//  then the mine ends. (Players in PvP, sub_565AA0 / sub_56E500.) Else the base tick (not applied: the end).
bool GameClientSession::mine_tick(size_t i, int elapsed_ms)
{
    ActiveEs& e = es_[i];
    e.elapsed_ms += elapsed_ms;
    if (e.elapsed_ms > e.end_ms) { es_end(i); return false; }
    if (e.elapsed_ms > 3200) {
        const auto now = std::chrono::steady_clock::now();
        MonsterEntity* hit = nullptr;
        for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
            MonsterEntity* m = server_.entity_mgr().get_monster(oid);
            if (!m || !m->alive || m->hp <= 0 || !area_target_ok(*m)) continue;
            int32_t mx, my;
            m->motion.pos(now, mx, my);
            if (!(static_cast<double>(e.radius) >= static_cast<double>(dist_f(mx, my, e.fx, e.fy)))) continue;
            if (owned_by_other(*m)) continue;
            hit = m;
            break;
        }
        if (hit) {
            const ActiveEs mine = e;
            mine_detonate(mine, *hit);
            for (size_t k = 0; k < es_.size(); ++k)
                if (es_[k].id == mine.id) { es_end(k); break; }
            return false;
        }
    }
    if (es_[i].result != 1 && !es_[i].stay) { es_end(i); return false; }   // the base tick
    return true;
}

// ── ESAction 1193 / 1268 (GameServer sub_5CD450 / sub_5D37A0): a mine goes off on a monster ─────────────────
//  The payload's monster (kind 3) — none: nothing. The block: the opener sub_5EE050 [us][us][0][1193 / 1268][2][ESClass
//  -1][3104][0] + [pos the point][u16 the monster] (VERIFIED: the clients' ESAMineDo 0x4DDC30 / ESAMineMasterDo 0x4F0B00
//  read them), the damage: AP' = (int)(8 * (LK + DA) * TrapAp) — ESAMineMaster's (int)((16 * DA + 8 * LK) * TrapAp) —
//  against its DP (sub_5E6040, the mine's ApplyRatio, element 0), its EXP share, 30002, then EXP 30003 kind 3 when we
//  may attack it (vtbl+164). Then the TM per hit of the action's skill — 3104 for both (Explosives Expert's mines give
//  Landmine's TM: retail).
void GameClientSession::mine_detonate(const ActiveEs& mine, MonsterEntity& m)
{
    const auto now = std::chrono::steady_clock::now();
    const bool master = (mine.esaction == 1267);
    const SkillDef* sd = server_.skill_db().get(mine.skill);
    const EsaRow* row = sd ? server_.esa_db().row(sd->esa_table, mine.level) : nullptr;
    const int base = master ? 16 * st_da_ + 8 * st_lk_ : 8 * (st_lk_ + st_da_);
    const int ap2 = static_cast<int>(static_cast<double>(base) * static_cast<double>(row_f(row, "TrapAp")));
    const SkillDamage h = skill_phys_damage(m, ap2, row_f(row, "ApplyRatio"), 0);
    const int32_t hp_before = m.hp;
    std::vector<PacketBuffer> group;
    PacketBuffer o(static_cast<Opcode>(0x00E0), is_paula());
    o.write<uint16_t>(static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(static_cast<uint16_t>(object_id_));
    o.write<uint16_t>(0);
    o.write<uint16_t>(static_cast<uint16_t>(master ? 1268 : 1193));
    o.write<uint8_t>(2);
    o.write<uint8_t>(0xFF);                                   // ESClass -1
    o.write<uint16_t>(3104);
    o.write<uint16_t>(0);
    put_pos(o, mine.fx, mine.fy);
    o.write<uint16_t>(static_cast<uint16_t>(m.object_id));
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    skill_after_hit(m, h.dmg, hp_before, killed, 3104, !may_attack_owned(m), now, 3);
    LOG_DEBUG("GameClientSession %u: %s at (%d,%d) goes off on mob %u: AP' %d dmg=%d result=%d%s", session_id(),
              master ? "Explosives Expert mine" : "Landmine", mine.fx, mine.fy, static_cast<unsigned>(m.object_id), ap2, h.dmg,
              h.result, killed ? " [KILLED]" : "");
}
