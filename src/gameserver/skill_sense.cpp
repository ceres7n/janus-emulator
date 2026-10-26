#include "game_session.hpp"
#include "game_server.hpp"
#include "logger.hpp"
#include "combat_util.hpp"
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
//  The sense-type skills (Fox / Lion line) with a handler of their own in the ESAction switch
//  sub_5BE9A0 — 1:1 with the retail GameServer (GameServerRelease). (Stone Strike 1090 and Stone Shower
//  1298 are physical strikes: skill.cpp esa_phys_strike.)
// ─────────────────────────────────────────────────────────────────────────────

// ── ESAction 1328 "Leg Training" (GameServer sub_5D8B50 -> sub_5D8B90; no selection) ──────────────────
//  The zone's players (zone +28) in our party (sub_716EF0: our share group, us included; none: ourselves), on our
//  layer, not dead (sub_41C790: +92 flag 4), within the row's Radius (+10, i16) of us (sub_42F1B0) each get the action
//  {ESAction 1328, EState 0, ESClass 0, skill 3208, flags 2, the row, us its caster} through sub_524690(.., 1)
//  (sub_5D8CE0): ESClass 0 -> the limiter's admission, the ES (ESAPreventSlowDo, es.cpp EsKind::SkillResist; its TM to
//  us, the caster; a member's on its strand, party_action_from).
//  The same walk: ESAction 1269 "Strong Mind" (sub_5D3C90 -> sub_5D3CD0 / sub_5D3E20: {1270 ESAInsulatorDo, skill
//  3303}), 1313 "Gas Mask" (sub_5D7A20 -> sub_5D7A60 / sub_5D7BB0: {1313, 3107}), 1314 "Lightning Rod" (sub_5D7C50 ->
//  sub_5D7C90 / sub_5D7DE0: {1314, 3108}) — `es_action` is the ES's.
void GameClientSession::esa_prevent_slow(const SkillDef& d, uint8_t lv, const EsaRow* act, int es_action)
{
    const int32_t radius = act ? static_cast<int16_t>(act->get("Radius", 0.0)) : 0;
    const auto now = std::chrono::steady_clock::now();
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    const uint16_t self = static_cast<uint16_t>(object_id_);
    const uint16_t sk   = static_cast<uint16_t>(d.id);
    for (uint32_t oid : server_.entity_mgr().players_in_zone(zone_id_)) {
        GameClientSession* s = server_.player_session(oid);
        if (!s || !s->object_id_ || !s->in_world()) continue;
        if (s != this && !server_.party().same_group(char_uid_, s->char_uid_)) continue;
        if (s->dead_) continue;
        int32_t x, y;
        s->cur_pos(now, x, y);
        if (!(static_cast<double>(dist_f(x, y, cx, cy)) <= static_cast<double>(radius))) continue;
        if (s == this) party_action_from(self, sk, lv, es_action, zone_id_);
        else s->party_action_pulse(self, sk, lv, es_action, zone_id_);
    }
}

// sub_894600: the Throw item (Player +4608) gives n — more than n left: the count goes down (sub_7256D0 +
// sub_6DB620, its DB update sub_765730); else the item is gone (sub_79DD20) and +4608 = 0.
void GameClientSession::consume_throw_item(int n)
{
    BagItem* b = throw_item_ ? bag_find_uid(throw_item_) : nullptr;
    const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
    if (!b || !d || n <= 0) return;
    const BagRecord rec = bag_decrease(*b, static_cast<uint16_t>(n));
    if (static_cast<int>(b->count) <= n) {
        server_.item_store().RemoveItem(rec.uid);
        bag_apply(rec);
        throw_item_ = 0;
        return;
    }
    server_.item_store().SetAmount(rec.uid, rec.count);
    bag_apply(rec);
}

// ── ESAction 1111 "Shuriken Master" (ESAWeaponThrow, init sub_552810; ESAUI 7: an object + the item) ──────
//  A generic ES (sub_524950) on the caster — ESClass -1, made at once (sub_524690) — whose init does the
//  throw: the selected object (the payload's [u16 target], zone vtbl+16 kind 3) we may attack (sub_5E6D70;
//  the owner test vtbl+164 is not modelled), the Throw item of the common slot 20 (Player +4608) — else no
//  object, no packet —, one of it used up (sub_894600); the payload's item ([u16 target][u32 item id][u64
//  uid]): Type 16 -> AP' = (int)((10 * DA + its AP * r) * Ratio), r = the learned Precise Pitch's (3101) row
//  Ratio (+4) while its ES 1146 is up (sub_552DF0), else 1.0; any other item -> its Weight / 2; at least 1
//  (sub_552D40). The physical skill damage sub_5E6040 (ApplyRatio, element 0), +240 = min(HP, damage); the
//  block {the opener sub_5EE050 with the object's id + [u16 target][pos us][pos target][u16 item id][u64
//  uid] (sub_552CE0; VERIFIED: the client's ESAWeaponThrow sub_4C11B0 reads them and draws that item), the
//  damage event}; EXP 30003 [0][+240][0] — min(HP, damage) itself, not the share (retail); the skill's TM
//  per hit (sub_891C80); our mark (vtbl+132(target, 5000)). The object lives 1000 ms (sub_552C90).
//  DEVIATION (anti-cheat): the retail trusts the payload's item id for the damage; we require it to be the
//  equipped Throw item's (else no object).
void GameClientSession::esa_weapon_throw(const SkillDef& d, uint8_t lv, const EsaRow* act)
{
    const auto now = std::chrono::steady_clock::now();
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive || m->hp <= 0 || !area_target_ok(*m)) return;
    const BagItem* tb = throw_item_ ? bag_find_uid(throw_item_) : nullptr;
    if (!tb) return;
    const std::vector<uint8_t>& pl = su_.payload;
    uint32_t item_id = 0, uid_lo = 0, uid_hi = 0;
    if (pl.size() >= 14) {
        for (int k = 0; k < 4; ++k) item_id |= static_cast<uint32_t>(pl[2 + k]) << (8 * k);
        for (int k = 0; k < 4; ++k) uid_lo  |= static_cast<uint32_t>(pl[6 + k]) << (8 * k);
        for (int k = 0; k < 4; ++k) uid_hi  |= static_cast<uint32_t>(pl[10 + k]) << (8 * k);
    }
    if (item_id != tb->item_id) {
        LOG_INFO("GameClientSession %u: Shuriken Master with item %u, the Throw item is %u — refused", session_id(),
                 item_id, tb->item_id);
        return;
    }
    const uint16_t id = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());
    consume_throw_item(1);
    float r = 1.0f;
    if (es_find(1146)) {
        if (const LearnedSkill* pp = find_skill(3101)) {
            const SkillDef* pd = server_.skill_db().get(3101);
            const EsaRow* row = (pd && pp->level > 0) ? server_.esa_db().row(pd->esa_table, pp->level) : nullptr;
            if (row) r = static_cast<float>(row->get("Ratio", 0.0));
        }
    }
    const float ratio = act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f;
    int ap2 = 0;
    if (const ItemDef* it = server_.item_db().get(item_id)) {
        if (it->type == 16)
            ap2 = static_cast<int>((static_cast<double>(10 * st_da_) + static_cast<double>(it->ap) * r) * ratio);
        else
            ap2 = it->weight / 2;
    }
    if (ap2 <= 0) ap2 = 1;
    const float apply_ratio = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    SkillDamage h = skill_phys_damage(*m, ap2, apply_ratio, 0);
    const int32_t hp_before = m->hp;
    const int32_t raw = std::min(m->hp, h.dmg);

    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    m->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_), id);   // with the object's id
    o.write<uint16_t>(tid);
    o.write<uint16_t>(static_cast<uint16_t>(ax)); o.write<uint16_t>(static_cast<uint16_t>(ay));
    o.write<uint16_t>(static_cast<uint16_t>(tx)); o.write<uint16_t>(static_cast<uint16_t>(ty));
    o.write<uint16_t>(static_cast<uint16_t>(item_id));
    o.write<uint32_t>(uid_lo);
    o.write<uint32_t>(uid_hi);
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    gain_exp_share(*m, raw, 0);
    LOG_DEBUG("GameClientSession %u: Shuriken Master -> mob %u item %u ap'=%d dmg=%d result=%d hp=%d/%d%s", session_id(),
              tid, item_id, ap2, h.dmg, h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "");
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, true, now);
    if (!killed && m->alive) mark_monster_owner(*m);           // vtbl+132(target, 5000)
    es_push_timer(d, lv, id, 1000);
}

// ─────────────────────────────────────────────────────────────────────────────
//  The Doctor (Lion, 3rd job). Gun Overdrive (1345) is a generic ES (es.cpp CustomizingGun); Sharp Sense (1385
//  ESASharpSense_EX) and Revealing Eye (1289 ESAOpticalEyes) too (es.cpp).
// ─────────────────────────────────────────────────────────────────────────────

// ── ESAction 1277 "Berserk" (Doctor; GameServer sub_5D5410, ESAReckless; no selection) ─────────────────────
//  An item in the equipment slot 1 (+2120, sub_41C7D0) and a loaded magazine (+4604) — else the log
//  "Player::RECKLESS_SKILL_CANCEL" and nothing. The row ESAction_Reckless (sub_51B700: ApplyRatio +0, Ratio +4, Radius
//  +8 i16, MonNum +12). The roll sub_893840(ApplyRatio); it passes -> sub_53E360 around us, at most min(MonNum, the
//  magazine's amount), Radius; any found -> the gun's AP (its option 10012, else ItemParam AP) and that many bullets
//  used up (sub_894560). The block: the opener sub_5EE050 + [u8 the roll passed and any found][u16 n][u16 id]*n
//  (VERIFIED: the client's ESAReckless 0x4F72F0 reads them); then each one (sub_5D56E0): the gun skill damage
//  sub_5E53C0(the gun's AP, Ratio, ApplyRatio, element 256), its EXP share, the damage event, EXP 30003 kind 0. The use
//  TM only when it went off.
void GameClientSession::esa_reckless(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    const BagItem* gun = equip_[1] ? bag_find_uid(equip_[1]) : nullptr;
    BagItem* mag = mag_.item ? bag_find_uid(mag_.item) : nullptr;
    if (!gun || !mag) {
        LOG_INFO("Player::RECKLESS_SKILL_CANCEL %u,%s", static_cast<unsigned>(object_id_), char_name_);
        return;
    }
    const float apply = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    const float ratio = act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f;
    bool ok = apply_chance(apply);
    std::vector<MonsterEntity*> targets;
    int32_t gun_ap = 0;
    if (ok) {
        const int mon_num = act ? static_cast<int>(act->get("MonNum", 0.0)) : 0;
        const int n = std::min(mon_num, static_cast<int>(mag->count));
        const int16_t radius = static_cast<int16_t>(act ? static_cast<int>(act->get("Radius", 0.0)) : 0);
        int32_t cx, cy;
        cur_pos(now, cx, cy);
        targets = area_monsters(cx, cy, radius, n, now);
        ok = !targets.empty();
        gun_ap = equipped_gun_ap();
        consume_bullets(static_cast<int>(targets.size()));
    }
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_));
    o.write<uint8_t>(ok ? 1 : 0);
    o.write<uint16_t>(static_cast<uint16_t>(targets.size()));
    for (MonsterEntity* m : targets) o.write<uint16_t>(static_cast<uint16_t>(m->object_id));
    group.push_back(std::move(o));
    struct Hit { MonsterEntity* m; int32_t dmg, hp_before; bool killed; };
    std::vector<Hit> done;
    if (ok) {
        for (MonsterEntity* m : targets) {
            const SkillDamage h = skill_gun_damage(*m, gun_ap, ratio, apply);
            const int32_t hp_before = m->hp;
            const bool killed = skill_damage_event(*m, h, group);
            done.push_back({ m, h.dmg, hp_before, killed });
        }
    }
    server_.broadcast_block(zone_id_, group, 0);
    for (const Hit& h : done) skill_after_hit(*h.m, h.dmg, h.hp_before, h.killed, 0, false, now);
    if (ok) skill_use_tm(su_.skill);
    LOG_DEBUG("GameClientSession %u: Berserk (Doctor) %s: %zu target(s), gun AP %d", session_id(), ok ? "fired" : "failed",
              targets.size(), gun_ap);
}

// ── ESAction 1491 "Bunker" (GameServer sub_5DFF40; no selection) ──────────────────────────────────────────
//  The action {ESAction 1492, EState 0, ESClass 0, flags 2, skill 3505, the row} on us through sub_524690(.., 1): the
//  limiter's admission, the ESABunkerDo (es.cpp EsKind::Bunker); then the use TM whatever came of it.
void GameClientSession::esa_bunker(const SkillDef& d, uint8_t lv, const EsaRow* act)
{
    SkillDef dd = d;
    dd.esaction = 1492;
    dd.estate   = 0;
    dd.esclass  = 0;
    if (dd.type != 2 && dd.id != 7046) {
        if (es_limiter_admit(dd.esclass, dd.esaction, static_cast<uint16_t>(dd.id)))
            es_create(dd, lv, act, EsKind::Bunker, true, 2);
    } else {
        es_create(dd, lv, act, EsKind::Bunker, false, 2);
    }
    skill_use_tm(su_.skill);
}

// ── ESAction 1497 "Bunker Release" (GameServer sub_5E0020; no selection) ──────────────────────────────────
//  With an ESABunkerDo (1492) in the limiter (vtbl+80): it ends (vtbl+60) and the use TM; else nothing.
void GameClientSession::esa_bunker_release(const SkillDef& d)
{
    (void)d;
    if (!es_find(1492)) return;
    es_remove_esaction(1492);
    skill_use_tm(su_.skill);
}

// ESAOpticalEyes' look-around (sub_573D90). The characters around us (sub_574040: the zone's monsters, then its players,
// then its third list, at most 128 in all — each not dead, on our layer, within Radius of us (Radius >= sqrtf)); the
// hidden ones (+452 bit 0, sub_5A1830 — monsters never are): those seen before but not now -> S2C 828 [u8 0][u16 n][u16
// id]*n to us (sub_574140 counts only the ones still in the zone; sub_603640) and out of the seen list; those now but
// not seen before -> S2C 828 [u8 1][u16 n][u16 id]*n and into it. VERIFIED: the client's case 828 (sub_6F0800) reads
// [u8][u16 n][u16 id]*n — 1: the actor shown (sub_6BC130(1)), 0: its own hidden state again (sub_6BCD40).
void GameClientSession::optical_scan(ActiveEs& e)
{
    const auto now = std::chrono::steady_clock::now();
    int32_t cx, cy;
    cur_pos(now, cx, cy);
    int room = 128;
    for (uint32_t oid : server_.entity_mgr().monsters_in_zone_ordered(zone_id_)) {
        if (room <= 0) break;
        MonsterEntity* m = server_.entity_mgr().get_monster(oid);
        if (!m || !m->alive || m->hp <= 0 || m->banished) continue;
        int32_t mx, my;
        m->motion.pos(now, mx, my);
        if (static_cast<double>(e.radius) >= static_cast<double>(dist_f(cx, cy, mx, my))) --room;
    }
    std::vector<uint16_t> hidden;
    std::vector<uint16_t> present;
    for (uint32_t oid : server_.entity_mgr().players_in_zone(zone_id_)) {
        GameClientSession* s = server_.player_session(oid);
        if (!s || s->object_id() == 0) continue;
        present.push_back(static_cast<uint16_t>(s->object_id()));
        if (room <= 0 || s->dead_) continue;
        int32_t sx, sy;
        s->cur_pos(now, sx, sy);
        if (static_cast<double>(e.radius) < static_cast<double>(dist_f(cx, cy, sx, sy))) continue;
        --room;
        if (s->hiding_) hidden.push_back(static_cast<uint16_t>(s->object_id()));
    }
    auto has = [](const std::vector<uint16_t>& v, uint16_t id) { return std::find(v.begin(), v.end(), id) != v.end(); };
    std::vector<uint16_t> keep, lost, found;
    for (uint16_t id : e.ids) (has(hidden, id) ? keep : lost).push_back(id);
    std::vector<uint16_t> lost_here;
    for (uint16_t id : lost) if (has(present, id)) lost_here.push_back(id);
    if (!lost_here.empty()) {
        PacketBuffer p(static_cast<Opcode>(0x033C), is_paula());
        p.write<uint8_t>(0);
        p.write<uint16_t>(static_cast<uint16_t>(lost_here.size()));
        for (uint16_t id : lost_here) p.write<uint16_t>(id);
        send(p);
    }
    for (uint16_t id : hidden) if (!has(keep, id)) found.push_back(id);
    if (!found.empty()) {
        PacketBuffer p(static_cast<Opcode>(0x033C), is_paula());
        p.write<uint8_t>(1);
        p.write<uint16_t>(static_cast<uint16_t>(found.size()));
        for (uint16_t id : found) { p.write<uint16_t>(id); keep.push_back(id); }
        send(p);
    }
    e.ids = keep;
}
