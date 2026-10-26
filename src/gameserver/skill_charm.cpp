#include "game_session.hpp"
#include "game_server.hpp"
#include "logger.hpp"
#include "combat_util.hpp"
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
//  The charm-type skills (Cat / Raccoon line) — 1:1 with the retail GameServer (GameServerRelease).
//  (Power Blow 1094 is a physical strike: skill.cpp esa_phys_strike.)
// ─────────────────────────────────────────────────────────────────────────────

// ── ESAction 1116 "Galder Thrower" (ESAGellderHit, init sub_553D00; ESAUI 8: an object + an amount) ──────
//  A generic ES (sub_524950) on the caster — ESClass -1, made at once — whose init does the throw: the
//  selected object (the payload's [u16 target], zone vtbl+16 kind 3) we may attack (sub_5E6D70), the
//  payload's [u32 money] only logged when not above 0; a player pays 100 gelder (sub_554130(100): not
//  enough -> no object; money -= 100 and S2C 0x0029 [u32 money], sub_5F4E80); the owner test (vtbl+164) is
//  not modelled. AP' = (int)(100 * Ratio) (sub_554110); the physical skill damage sub_5E6040 with DP 0 —
//  the target's defence left out — and element 512 (no resist); +240 = min(HP, damage); the block {the
//  opener sub_5EE050 with the object's id + [u16 target][pos us][pos target] (sub_5540D0; VERIFIED: the
//  client's ESAGellderHit sub_4C2280 reads them), the damage event}; EXP 30003 [0][+240][0] — min(HP,
//  damage) itself (retail); the skill's TM per hit. No mark. The object lives 2000 ms (sub_554080).
void GameClientSession::esa_gellder_hit(const SkillDef& d, uint8_t lv, const EsaRow* act)
{
    const auto now = std::chrono::steady_clock::now();
    const uint16_t tid = su_.target;
    MonsterEntity* m = tid ? server_.entity_mgr().get_monster(tid) : nullptr;
    if (!m || m->zone != zone_id_ || !m->alive || m->hp <= 0 || !area_target_ok(*m)) return;
    const std::vector<uint8_t>& pl = su_.payload;
    int32_t money_arg = 0;
    if (pl.size() >= 6)
        for (int k = 0; k < 4; ++k) money_arg |= static_cast<int32_t>(pl[2 + k]) << (8 * k);
    if (money_arg <= 0)
        LOG_INFO("!!!!!!!!!!!!!!! ESAGellderHit::Money - invalid :%s", char_name_);
    if (money_ < 100) return;                                  // sub_554130(100)
    money_ -= 100;
    send_money();
    const uint16_t id = static_cast<uint16_t>(server_.entity_mgr().alloc_oid());
    const float ratio = act ? static_cast<float>(act->get("Ratio", 0.0)) : 0.0f;
    const int ap2 = static_cast<int>(static_cast<double>(100) * ratio);
    const float apply_ratio = act ? static_cast<float>(act->get("ApplyRatio", 0.0)) : 0.0f;
    SkillDamage h = skill_phys_damage(*m, ap2, apply_ratio, 512, 0);
    const int32_t hp_before = m->hp;
    const int32_t raw = std::min(m->hp, h.dmg);

    int32_t ax, ay, tx, ty;
    cur_pos(now, ax, ay);
    m->motion.pos(now, tx, ty);
    std::vector<PacketBuffer> group;
    PacketBuffer o = skill_opener(d, static_cast<uint16_t>(object_id_), id);
    o.write<uint16_t>(tid);
    o.write<uint16_t>(static_cast<uint16_t>(ax)); o.write<uint16_t>(static_cast<uint16_t>(ay));
    o.write<uint16_t>(static_cast<uint16_t>(tx)); o.write<uint16_t>(static_cast<uint16_t>(ty));
    group.push_back(std::move(o));
    const bool killed = skill_damage_event(*m, h, group);
    server_.broadcast_block(zone_id_, group, 0);
    gain_exp_share(*m, raw, 0);
    LOG_DEBUG("GameClientSession %u: Galder Thrower -> mob %u ap'=%d dmg=%d result=%d hp=%d/%d%s (money %d)",
              session_id(), tid, ap2, h.dmg, h.result, m->hp, m->hp_max, killed ? " [KILLED]" : "", money_);
    skill_after_hit(*m, h.dmg, hp_before, killed, su_.skill, true, now);
    es_push_timer(d, lv, id, 2000);
}
