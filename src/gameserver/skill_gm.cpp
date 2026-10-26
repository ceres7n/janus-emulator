#include "game_session.hpp"
#include "game_server.hpp"
#include "logger.hpp"
#include "combat_util.hpp"
#include "paula_crypt.hpp"
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
//  Skills of every class — 1:1 with the retail GameServer (GameServerRelease), the client's classes verified in
//  Trickster.bin: Identification Card (8002) and the GMs' cheers (20000..20003). (The couple skills 5020..5041 need
//  the couple system: their ESs — ESAHeartM .. ESASpoonsWithYouW, 1400..1421 — only take on the caster's partner,
//  Player +7216 vs the target's +1648 (sub_590D60); Guardian Summon 8003 — ESASummonSoulShade 1394 — drives the
//  mount / soul shade object of Player +7112: neither system exists here.)
// ─────────────────────────────────────────────────────────────────────────────

namespace {
int rowi(const EsaRow* r, const char* k) { return r ? static_cast<int>(r->get(k, 0.0)) : 0; }
} // namespace

// ── ESAction 1393 "Identification Card" (GameServer sub_5DD600, ESACardIdentify; no selection) ───────────────
//  The block {S2C 224 sub_5EDE00 [us][us][0][1393]... with no tail (the static empty stream unk_EE3478) — VERIFIED:
//  the client's ESACardIdentify 0x516F00 reads none and, the object being its own player, opens its window
//  (sub_BD8EC0)}; then we stop (sub_41C5D0).
void GameClientSession::esa_card_identify(const SkillDef& d, std::chrono::steady_clock::time_point now)
{
    std::vector<PacketBuffer> group;
    group.push_back(skill_opener(d, static_cast<uint16_t>(object_id_), 0));
    server_.broadcast_block(zone_id_, group, 0);
    skill_stop_motion(now);
}

// ── ESAction 1452 / 1455 "GM's Support (Attack / Defense)" (GameServer sub_5DD6C0 with ESFGM_Cheer_Attack / _Defence,
//    sub_5C1D50 / sub_5C1E40; ESAUI 1) ─────────────────────────────────────────────────────────────────────────
//  The selected object (zone vtbl+16, kind 3: a monster or a player of the zone — none: retail goes on with a null
//  and faults in sub_524690; nothing here) gets {ESAction 1453 ESAGMCheerAttackDo / 1456 ESAGMCheerDefenceDo, EState
//  0, ESClass 0, flags 2, skill 20000 / 20001, its LEVEL-1 row} through sub_524690(.., 1): its limiter (list 0)
//  admits it (a player: es.cpp EsKind::GMCheer on its own strand; a monster: gm_cheer_on). No packet of its own.
void GameClientSession::esa_gm_cheer(const SkillDef& d)
{
    const uint16_t tid = su_.target;
    if (!tid) return;
    if (MonsterEntity* m = server_.entity_mgr().get_monster(tid)) {
        if (m->zone != zone_id_) return;
        SkillDef dd = d;
        dd.esaction = (d.esaction == 1452) ? 1453 : 1456;
        dd.estate   = 0;
        dd.esclass  = 0;
        es_apply_on(*m, dd, 1, server_.esa_db().row(d.esa_table, 1));
        return;
    }
    if (tid == static_cast<uint16_t>(object_id_)) { gm_cheer_from(static_cast<uint16_t>(object_id_), d.id, zone_id_); return; }
    if (GameClientSession* s = server_.player_session(tid)) s->gm_cheer_pulse(static_cast<uint16_t>(object_id_), d.id, zone_id_);
}

// ── ESAction 1454 / 1457 "GM's Dignity (Attack / Defense)" (GameServer sub_5DD720 with ESFGM_Area_Cheer_Attack /
//    _Defence, sub_5C1F30 / sub_5C2020; ESAUI 6: a point) ──────────────────────────────────────────────────────
//  The block {S2C 224 sub_5EE050 [us][us][0][1458 ESAGMCheerDo][2][ESClass -1][skill][EState] + [pos the payload's
//  point] (VERIFIED: the client's ESAGMCheerDo 0x51BA20 reads it — its eft_skl_GM_Dignity effect)}; then (sub_5DD810) the
//  zone's players — not dead, not us, our layer — within the row's Radius (+32) of the point each get {1453 / 1456,
//  EState 0, ESClass 0, flags 2, skill 20002 / 20003, its LEVEL-1 row} (on their strands). (Monsters are not walked.)
void GameClientSession::esa_gm_dignity(const SkillDef& d, const EsaRow* act, std::chrono::steady_clock::time_point now)
{
    (void)now;
    int16_t px = 0, py = 0;
    if (su_.payload.size() >= 4) {
        px = static_cast<int16_t>(su_.payload[0] | (su_.payload[1] << 8));
        py = static_cast<int16_t>(su_.payload[2] | (su_.payload[3] << 8));
    }
    SkillDef dd = d;
    dd.esaction = 1458;
    dd.esclass  = -1;
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(dd, static_cast<uint16_t>(object_id_), 0);
    o.write<uint16_t>(static_cast<uint16_t>(px));
    o.write<uint16_t>(static_cast<uint16_t>(py));
    group.push_back(std::move(o));
    server_.broadcast_block(zone_id_, group, 0);
    const int16_t radius = static_cast<int16_t>(rowi(act, "Radius"));
    size_t n = 0;
    for (uint32_t oid : server_.entity_mgr().players_in_zone(zone_id_)) {
        if (oid == object_id_) continue;
        GameClientSession* s = server_.player_session(static_cast<uint16_t>(oid));
        if (!s) continue;
        int32_t x, y;
        if (!s->ai_targetable(x, y)) continue;               // (not dead, in the world: sub_41C790)
        if (!(static_cast<double>(dist_f(x, y, px, py)) <= static_cast<double>(radius))) continue;
        s->gm_cheer_pulse(static_cast<uint16_t>(object_id_), d.id, zone_id_);
        ++n;
    }
    LOG_INFO("GameClientSession %u: GM's Dignity (skill %d) at (%d,%d): %zu player(s)", session_id(), d.id, px, py, n);
}

// The GM cheer's Do made on us by `caster` (ourselves or another player of `zone`): {1453 / 1456, EState 0, ESClass 0,
// flags 2, the skill, its LEVEL-1 row} through sub_524690(.., 1) — the skill is no Type 2: our limiter's admission.
void GameClientSession::gm_cheer_from(uint16_t caster, int skill, int16_t zone)
{
    if (leaving_ || state_ != GameClientSessionState::InWorld || zone_id_ != zone) return;
    const SkillDef* sd = server_.skill_db().get(skill);
    if (!sd) return;
    SkillDef dd = *sd;
    dd.esaction = (skill == 20000 || skill == 20002) ? 1453 : 1456;
    dd.estate   = 0;
    dd.esclass  = 0;
    const EsaRow* row = server_.esa_db().row(sd->esa_table, 1);
    const uint16_t by = (caster == static_cast<uint16_t>(object_id_)) ? 0 : caster;
    if (dd.type != 2 && dd.id != 7046) {
        if (es_limiter_admit(dd.esclass, dd.esaction, static_cast<uint16_t>(dd.id)))
            es_create(dd, 1, row, EsKind::GMCheer, true, 2, 0, by);
    } else {
        es_create(dd, 1, row, EsKind::GMCheer, false, 2, 0, by);
    }
}

// sub_597210: the owner's vtbl+160 (recompute) and, a player, S2C 283 [u8 8]{[u8 stat][u32 its buff]} for AP (0), AC
// (1), MA (4), MD (5), DA (7), LK (8), DP (10), HV (11).
void GameClientSession::gm_cheer_update()
{
    recompute_stats(true);
    PacketBuffer p(static_cast<Opcode>(0x011B), is_paula());
    p.write<uint8_t>(8);
    for (int s : { 0, 1, 4, 5, 7, 8, 10, 11 }) {
        p.write<uint8_t>(static_cast<uint8_t>(s));
        p.write<uint32_t>(static_cast<uint32_t>(buff_[s]));
    }
    send(p);
}

// ESAGMCheerAttackDo / ESAGMCheerDefenceDo on a monster (GM's Support on one, sub_596D10): the eight values, +212 =
// Seconds, the lifetime; the block {S2C 224 sub_5EDE00 [us][target][ES id][1453 / 1456][2][ESClass 0][skill][0], no
// tail}; applied -> the adder (flat: skills 20000 / 20001) and the monster's recompute (no packet: it is no player). Its
// tick: the generic one (past 1000 * Seconds the end); its end: the recompute.
bool GameClientSession::gm_cheer_on(MonsterEntity& m, MonsterEntity::Es& e, const EsaRow* act)
{
    e.end_ms    = 1000 * rowi(act, "Seconds");
    e.life_ms   = e.end_ms;
    e.elapsed_ms = 0;
    e.bonus[0]  = static_cast<int16_t>(rowi(act, "ApUp"));
    e.bonus[1]  = static_cast<int16_t>(rowi(act, "AcUp"));
    e.bonus[4]  = static_cast<int16_t>(rowi(act, "MaUp"));
    e.bonus[5]  = static_cast<int16_t>(rowi(act, "MdUp"));
    e.bonus[7]  = static_cast<int16_t>(rowi(act, "DaUp"));
    e.bonus[8]  = static_cast<int16_t>(rowi(act, "LkUp"));
    e.bonus[10] = static_cast<int16_t>(rowi(act, "DpUp"));
    e.bonus[11] = static_cast<int16_t>(rowi(act, "HvUp"));
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
        o.write<uint8_t>(2);
        o.write<uint8_t>(static_cast<uint8_t>(e.esclass));
        o.write<uint16_t>(e.skill);
        o.write<uint16_t>(static_cast<uint16_t>(e.estate));
        group.push_back(std::move(o));
        server_.broadcast_block(zone_id_, group, 0);
    }
    m.es.push_back(e);
    if (e.result == 1) server_.monster_recompute(m);
    LOG_INFO("GameClientSession %u: GM cheer %d on mob %u (%d ms)", session_id(), e.esaction,
             static_cast<unsigned>(m.object_id), e.end_ms);
    return true;
}
