#include "game_session.hpp"
#include "game_server.hpp"
#include "combat_util.hpp"
#include "logger.hpp"
#include <algorithm>
#include <chrono>

// ─────────────────────────────────────────────────────────────────────────────
//  Guns — retail GameServer (Player cases 477 / 511, GunAttackStrategy), verified against the KR
//  client.
//
//  A gun is an item of SubType 54 in slot 1. Equipping one needs the PSAbility 1 of the passive
//  skill Gun Carrier (3008) in the player's ESLimiter (VerifyEquipItem sub_762A00 -> sub_5255F0);
//  its equip effect (sub_717E70) gives the player the gun's attack ranges (Item_GunParam2 Min / Max,
//  approach 3*(Min+Max)>>2), the GunAttackStrategy (5) and the NormalBattleFollowingStrategy (1),
//  and drops the running action (ChangeState 0); taking it off (sub_760600) restores the
//  CharacterInfo ranges, the Normal attack (1) and the Lazy follow (2). A melee request (C2S 0x001D)
//  is ignored while a gun is in slot 1. Bullets (Type 29) are loaded into the magazine through the
//  common slot 20 (C2S 0x0205, equip.cpp).
//
//  The shot. The client fills its reload gauge (GaugeSpeed), then the power gauge, and on a click
//  sends C2S 0x01DD [u16 target][u16 crc][u16 bullets per shot][u8 move type] (sub_5976B0):
//    crc = ((PowStartPoint<<5 ^ PowRange<<8 ^ GaugeSpeed<<12) ^ seed) | power — the seed is the
//    last S2C 0x0200 (C2S 0x01FF asks for one when a gun is equipped, client sub_6AF7F0), power =
//    the power gauge stopped inside (PowStartPoint, PowRange).
//  Player case 477:
//    dead -> S2C 0x0136 [u32 0]; no item in slot 1, or not SubType 54 -> 0x0136 (logged);
//    CRC: (crc ^ (key & 0xFFFE)) & 0xFFFE must be the seed & 0xFFFE; FastShot: at least
//    (int)((int)(20000 / (GaugeSpeed * 1/6)) * 0.7) ms since the last shot (the Speedloader ES 1476
//    scales it by its Ratio / 100); a failure answers nothing, logs "Gun Hack Detected!" and the third one in a
//    row cuts the player (cut reason 1);
//    a new seed (tick & 0xFFFE) -> S2C 0x0200 [u16 seed];
//    the magazine must fire exactly `bullets per shot` and its stack hold that many, the target
//    must exist and be a monster or a player — else 0x0136;
//    BeginAttackGun (sub_75FC30): move type 2 with more than 89% of the max WT carried -> 1;
//    Player +1032 = 5 for a power shot (else 0); already attacking -> the new target (vtbl+140),
//    else the attack state (vtbl+124 + ChangeState 1: S2C 0x001E to the player).
//  GunAttackStrategy::DoAttack (sub_41C210), when the follow has the target in range:
//    S2C 0x001B (area), stop; the line of sight over the cells' heights (sub_5E69C0), blocked ->
//    S2C 0x020D (the client: "CannotShot_Block"), the power flag drops, nothing else;
//    the gun's AP = its option 10012 (GunAp) else ItemParam AP (sub_41C150); the damage
//    (sub_5E4FD0); the monster's EXP share; the bullets (sub_894560 — none loaded: nothing more);
//    the command block {S2C 0x01E5 [u16 att][u16 2][u16 tgt][pos][pos][i16 dmg][u8 result] (the
//    client's gun action 54, which also takes the bullets off its own stack), the damage event's
//    0x002A / 0x011E / 0x002B}; the last-shot tick (sub_765480); EXP (30003); the strategy stops on
//    its next tick -> S2C 0x0020. One request = one shot.
//  The damage (sub_5E4FD0): miss when rand()%100 > LK - LK(target) + 80 (sub_5E7680);
//    normal = (int)(((AC-49)*20 + AP + 20)*1.0 - HV(target)*(12 player / 30 monster)), at least 0,
//    result 0 (1 when 0), |0x80 when it kills; a power shot never misses: (int)(((AC-49)*20 + AP +
//    20)*1.2*(+1064, 1.0 or Keen Sense's Ratio) - HV*k), at least 1, result 3 / 131; then the target's GunR (slot 19,
//    sub_5E6760). The damage record (S2C 0x002A) carries [1] = 4 and [3] = 256 (the gun element).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr double kFastShotBase  = 20000.0;              // dbl_9C9268
constexpr double kFastShotGauge = 0.1666666716337204;   // dbl_99DD90
constexpr double kFastShotRatio = 0.699999988079071;    // dbl_99DD88
constexpr int    kGunHackCut    = 3;                    // Player +0x1BCC reaches 3 -> cut
constexpr int    kGunElement    = 256;                  // sub_5E6760(.., 256): GunR, slot 19

} // namespace

const ItemDef* GameClientSession::slot_item(int slot) const
{
    if (slot < 0 || slot >= 23 || !equip_[slot]) return nullptr;
    for (const BagItem& b : bag_)
        if (b.uid == equip_[slot]) return server_.item_db().get(b.item_id);
    return nullptr;
}

bool GameClientSession::gun_in_hand() const
{
    const ItemDef* d = slot_item(1);
    return d && d->sub_type == 54;
}

bool GameClientSession::gun_in_any_slot() const
{
    for (int s : { 4, 9, 1, 22 })
        if (const ItemDef* d = slot_item(s)) if (d->sub_type == 54) return true;
    return false;
}

const GunDef* GameClientSession::equipped_gun() const
{
    const ItemDef* d = slot_item(1);
    return (d && d->sub_type == 54) ? server_.gun_db().get(d->id) : nullptr;
}

// m_ESLimiter (Player +4504): every learned passive (SkillParam Type 1) is registered there with its
// PSAbility (sub_525220, at login and on each learn); sub_5255F0 asks whether an ability is there.
// (A passive with SkillOption 512 stays out in the zone types 1/5/6 — no PSAbility skill has it.)
bool GameClientSession::has_ps_ability(int ability) const
{
    for (const LearnedSkill& s : skills_) {
        const SkillDef* d = server_.skill_db().get(s.id);
        if (d && d->type == 1 && d->ps_ability == ability) return true;
    }
    return false;
}

// The player's attack ranges (actor +1040 Min, +1044 approach, +1048 Max, +1052 Chase): the
// CharacterInfo row of its type, or the gun's while one is in slot 1 (sub_717E70); a Chi Sword on
// (ESAEnergySword sub_588A40) adds its Range to the approach, Max and Chase. (Retail keeps them as fields
// that an equip change sets anew, so a gun equipped under a Chi Sword drops its part and its end takes it
// off the gun's values; here the part comes and goes with the ES alone.)
CharRange GameClientSession::attack_range() const
{
    CharRange r = server_.char_range().get(type_);
    if (const GunDef* g = equipped_gun()) {
        r.min_atk      = g->min_attack_range;
        r.max_atk      = g->max_attack_range;
        r.approach_atk = (3 * (g->min_attack_range + g->max_attack_range)) >> 2;
    }
    if (const int cs = chi_sword_range()) {
        r.approach_atk = r.approach() + cs;
        r.max_atk     += cs;
        r.chase       += cs;
    }
    return r;
}

// C2S 0x01FF (Player case 511): a new seed from the tick, S2C 0x0200 [u16 seed] (sub_5F5840).
// The client stores it (case 512) and turns its gun mode on if it is off (sub_6AEA90).
void GameClientSession::handle_gun_seed()
{
    gun_seed_ = static_cast<uint16_t>(tick_ms() & 0xFFFE);
    PacketBuffer p(static_cast<Opcode>(0x0200), is_paula());
    p.write<uint16_t>(gun_seed_);
    send(p);
}

// S2C 0x0136 [u32 why] (sub_5F59F0): the attack request failed. The client cancels its attack, shows
// "AttackCancelType<why>" when why != 0, and puts a fired gun back to "ready" (sub_6ABFB0).
void GameClientSession::send_attack_cancel(uint32_t why)
{
    PacketBuffer p(static_cast<Opcode>(0x0136), is_paula());
    p.write<uint32_t>(why);
    send(p);
}

void GameClientSession::handle_gun_attack(PacketBuffer& pkt)
{
    try {
        if (dead_) { send_attack_cancel(0); return; }                   // actor +92 flag 4 (sub_41C840)
        head_notice_end();                     // sub_73C9C0 / sub_73CAF0 / sub_73CCD0: the conditions 2 / 3 / 8 end
        const uint16_t target = pkt.read<uint16_t>();
        const uint16_t crc    = pkt.read<uint16_t>();
        const uint16_t per    = pkt.read<uint16_t>();
        const uint8_t  mt     = pkt.read<uint8_t>();

        const ItemDef* gd = slot_item(1);                                // sub_41C7D0(+2120, 1)
        if (!gd) {
            LOG_INFO("%s:CMD_BEGIN_ATTACK_GUN:Gun in the Equipslot is NULL", char_name_);
            send_attack_cancel(0);
            return;
        }
        if (gd->sub_type != 54) {
            send_attack_cancel(0);
            LOG_INFO("%s:CMD_BEGIN_ATTACK_GUN:Gun SubType is invaild, SubType = [%d]", char_name_, gd->sub_type);
            return;
        }
        const GunDef* g = server_.gun_db().get(gd->id);                 // ItemParam +88 (sub_719AE0)
        if (!g) {
            LOG_ERROR("%s:CMD_BEGIN_ATTACK_GUN: gun %u has no Item_GunParam2 row", char_name_, gd->id);
            send_attack_cancel(0);
            return;
        }

        // The CRC and the FastShot check.
        const bool power = (crc & 1) != 0;
        const uint32_t key = (static_cast<uint32_t>(static_cast<uint16_t>(g->pow_start_point)) << 5)
                           ^ (static_cast<uint32_t>(static_cast<uint16_t>(g->pow_range)) << 8)
                           ^ (static_cast<uint32_t>(static_cast<uint16_t>(g->gauge_speed)) << 12);
        const uint16_t client_crc = static_cast<uint16_t>((crc ^ (key & 0xFFFE)) & 0xFFFE);
        const uint16_t server_crc = static_cast<uint16_t>(gun_seed_ & 0xFFFE);
        int hack = (client_crc != server_crc) ? 1 : 0;
        int predict = static_cast<int>(kFastShotBase / (static_cast<double>(g->gauge_speed) * kFastShotGauge));
        predict = static_cast<int>(static_cast<double>(predict) * kFastShotRatio);
        // The Speedloader ES (1476, ESAFastLoad) in the ESLimiter (+0x4B0 vtbl+0x54): predict =
        // (int)(predict x its float Ratio (+200, sub_79D040) / 100.0) — whatever its apply roll.
        if (const ActiveEs* fl = es_find(1476))
            predict = static_cast<int>(static_cast<double>(predict) * static_cast<double>(fl->ratio) / 100.0);
        const uint32_t elapsed = tick_ms() - last_shot_tick_;
        if (elapsed < static_cast<uint32_t>(predict)) hack += 2;
        if (hack > 0) {
            const char* what = (hack == 1) ? "CRC" : (hack == 2) ? "FastShot" : "CRC + FastShot";
            LOG_INFO("Gun Hack Detected! =>  name : %s, ChrUid: %u, Hack Type : %s , PredictTime : %d, MessageTime : %d "
                     "ServerCRC : %d, ClientCRC : %d", char_name_, char_uid_, what, predict, static_cast<int>(elapsed),
                     server_crc, client_crc);
            if (++gun_hacks_ >= kGunHackCut) {
                gun_hacks_ = 0;
                LOG_ERROR("%s: gun hack x%d — the player is cut (GSendCCmd_UpdateCutReason 1)", char_name_, kGunHackCut);
                close();                                                 // sub_6042C0(this, 1) + sub_8AA290
            }
            return;
        }
        gun_hacks_ = 0;
        handle_gun_seed();                                               // the next shot's seed

        // The magazine: exactly `per` bullets per shot, and the stack holds them.
        const BagItem* mb = mag_.item ? bag_find_uid(mag_.item) : nullptr;
        if (!mag_.item || mag_.per_shot != static_cast<int>(per) || !mb || static_cast<int>(per) > mb->count) {
            send_attack_cancel(0);
            return;
        }
        // The target (vtbl+20): a monster or a player of the zone (no player-vs-player here: a player
        // fails BeginAttackGun's sub_5E6D70 -> 0x0136 + ChangeState 0).
        MonsterEntity* m = server_.entity_mgr().get_monster(target);
        if (!m || m->zone != zone_id_) {
            send_attack_cancel(0);
            const PlayerEntity* pl = server_.entity_mgr().get_player(target);
            if (pl && pl->zone == zone_id_) end_attack(true);           // sub_414B50(this, 0)
            return;
        }

        // BeginAttackGun (sub_75FC30).
        uint8_t type = mt;
        const bool can_run = st_maxwt_ > 0 && 100 * bag_weight_ / st_maxwt_ <= 89;   // sub_421E50
        if (type == 2 && !can_run) type = 1;
        atk_move_type_ = type;                                           // sub_79E170: the stored move type
        atk_power_ = power ? 5 : 0;                                      // Player +1032
        if (attacking_) {
            // vtbl+140 (sub_719210): the new target, and the attack strategy's vtbl+44 (sub_41B8C0):
            // +60 = 1 (no re-entry: it is in its attack state) and the follow back to state 1.
            atk_target_ = target;
            gun_stop_   = false;
            pf_.state   = 1;
        } else {
            // vtbl+124 + ChangeState(1): the attack state's enter sends S2C 0x001E and starts the attack
            // strategy (vtbl+136 sub_7191C0 -> sub_41B890: +60 = 1), which enters its own attack state on
            // the next tick.
            pick_ = Pickup{};                                            // the pick-up state (4) is left
            drill_ = Drill{};                                            // (and the drill state 3)
            atk_target_ = target;
            attacking_  = true;
            gun_start_  = true;
            gun_stop_   = false;
            pf_ = BattleFollow{};
            PacketBuffer ack(static_cast<Opcode>(0x001E), is_paula());   // sub_5F59D0
            send(ack);
        }
        ensure_world_tick();
        LOG_DEBUG("GameClientSession %u: gun attack eid=%u power=%d per=%u moveType=%u", session_id(), target,
                  power ? 1 : 0, per, type);
    } catch (...) {}
}

// sub_894560: a shot takes `n` bullets from the loaded stack (a common item, sub_6DE040): the last
// ones take the stack away (sub_79DD20: the DB deletes it) and empty the magazine (sub_76CCA0); else
// the stack keeps the rest (sub_6DB620 -> sub_6DB4C0; an OptionsEx 64 item keeps all). No packet:
// the client takes its own bullets off when it plays the shot (sub_470DB0 -> sub_6AEA00).
void GameClientSession::consume_bullets(int n)
{
    BagItem* b = mag_.item ? bag_find_uid(mag_.item) : nullptr;
    const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
    if (!b || !d || !d->stackable() || n <= 0) return;
    const BagRecord rec = bag_decrease(*b, static_cast<uint16_t>(n));
    if (static_cast<int>(b->count) - n <= 0) {
        server_.item_store().RemoveItem(rec.uid);
        bag_apply(rec);
        mag_ = Magazine{};
        return;
    }
    if (d->options_ex & 64) return;
    server_.item_store().SetAmount(rec.uid, rec.count);
    bag_apply(rec);
}

// GunAttackStrategy::DoAttack (sub_41C210). True when the shot was fired (the strategy then stops on
// its next tick); false when it ended early — nothing more happens until a new request.
bool GameClientSession::gun_shot(MonsterEntity& m, std::chrono::steady_clock::time_point now)
{
    const uint16_t self = static_cast<uint16_t>(object_id_);
    const uint16_t tid  = atk_target_;
    int32_t cx, cy, mx, my;                                              // CURRENT positions (sub_4100C0)
    cur_pos(now, cx, cy);
    m.motion.pos(now, mx, my);

    // S2C 0x001B [u16 id][u16 x][u16 y][u8 type] to the area (sub_5F54E0; type = the move bits, 0 -> 2).
    {
        const uint32_t moving = motion_.state_flags(now);
        PacketBuffer a(static_cast<Opcode>(0x001B), is_paula());
        a.write<uint16_t>(self);
        a.write<uint16_t>(static_cast<uint16_t>(cx));
        a.write<uint16_t>(static_cast<uint16_t>(cy));
        a.write<uint8_t>(moving ? static_cast<uint8_t>(moving) : 2);
        server_.broadcast_zone(zone_id_, a, 0);
    }
    // stop (sub_41C5D0)
    motion_.rest(cx, cy);
    pos_x_ = cx; pos_y_ = cy;
    if (auto* p = server_.entity_mgr().get_player(object_id_)) { p->pos_x = cx; p->pos_y = cy; }

    // (an ES 1479 on the player would end it here — none exists)
    if (const MapGrid* grid = server_.zone_grid(zone_id_)) {
        if (!grid->line_of_sight(static_cast<int16_t>(cx), static_cast<int16_t>(cy), static_cast<int16_t>(mx),
                                 static_cast<int16_t>(my))) {
            if (atk_power_ == 5) atk_power_ = 0;
            PacketBuffer p(static_cast<Opcode>(0x020D), is_paula());     // _fputchar_54 -> sub_60BE50(this, 525)
            send(p);
            LOG_DEBUG("GameClientSession %u: gun shot at %u blocked (line of sight)", session_id(), tid);
            return false;
        }
    }
    // The gun's AP (sub_41C150): the item's options 10012 (GunAp, sub_6E8140) else ItemParam AP.
    const BagItem* gb = equip_[1] ? bag_find_uid(equip_[1]) : nullptr;
    const ItemDef* gd = gb ? server_.item_db().get(gb->item_id) : nullptr;
    if (!gb || !gd) return false;
    int32_t gun_ap = gd->ap;
    {
        bool have = false;
        int32_t sum = 0;
        for (const ItemOption& o : gb->opts)
            if (o.id == 10012) { sum += o.value; have = true; }
        if (have) gun_ap = static_cast<int16_t>(sum);
    }

    // The damage (sub_5E4FD0), player -> monster (defense factor 30, PvP factor 1.0). A player with
    // the Gun Overdrive ES (1345) up and the skill 3503 learned shoots with AP = (int)(AP x the
    // Ratio of that skill's LEARNED level row).
    if (es_find(1345)) {
        if (const LearnedSkill* ls = find_skill(3503)) {
            const SkillDef* sd = server_.skill_db().get(3503);
            const EsaRow* r = sd ? server_.esa_db().row(sd->esa_table, ls->level) : nullptr;
            if (r) gun_ap = static_cast<int32_t>(static_cast<double>(gun_ap) * static_cast<float>(r->get("Ratio", 0.0)));
        }
    }
    int32_t dmg = 0;
    uint8_t result = 1;
    const int hv = std::max(m.hv, 0);
    const bool miss = rnd(100) > st_lk_ - m.lk + 80;                     // sub_5E7680
    if (atk_power_ == 5) {
        const float v = static_cast<float>(((st_ac_ - 49) * 20.0 + gun_ap + 20.0) * 1.200000047683716 *
                                           static_cast<double>(power_shot_mult_));   // +1064 (ESASharpSense)
        const float k = static_cast<float>(hv * 30.0);
        int32_t d = static_cast<int32_t>(v - k);                         // sub_5E75E0
        if (d <= 0) d = 1;
        dmg    = static_cast<int32_t>(d * 1.0);
        result = (dmg < m.hp) ? 3 : 131;
    } else if (!miss) {
        const float v = static_cast<float>((st_ac_ - 49) * 20.0 + gun_ap + 20.0) * 1.0f;
        const float k = static_cast<float>(hv * 30.0);
        int32_t d = static_cast<int32_t>(v - k);                         // sub_5E7540
        if (d < 0) d = 0;
        dmg    = static_cast<int32_t>(d * 1.0);
        result = dmg ? 0 : 1;
        if (dmg >= m.hp) result |= 0x80;
    }
    int32_t absorbed = 0;
    dmg = skill_resist(dmg, m.resist_slot(19), absorbed);               // sub_5E6760(.., 256): GunR
    if (absorbed) absorb_heal(m, absorbed);                              // event 30182 (GunR > 100)

    // The monster's EXP share is taken before the damage (vtbl+264); then the bullets — a player with
    // no loaded stack fires nothing more.
    const int32_t hp_before = m.hp;
    hide_break();                                                        // a hidden player's shot ends its hide
    if (!mag_.item) {
        LOG_DEBUG("GameClientSession %u: gun shot without a loaded magazine", session_id());
        return false;
    }
    consume_bullets(mag_.per_shot);

    const int32_t shot_dmg = dmg;                                        // the shot (0x01E5) shows the blow's
    int32_t dinfo_flags = 0;
    if (m.es_on(1320)) {                                                 // ESAHarden's hook (sub_57F420)
        dmg = static_cast<int32_t>(static_cast<double>(dmg) * 0.5);
        dinfo_flags = 1;
    }
    if (dmg > 0) mark_monster_owner(m);
    int32_t hp = m.hp, hp_max = m.hp_max;
    bool killed = false;
    if (dmg > 0) { killed = server_.entity_mgr().damage_monster(tid, dmg, hp, hp_max); if (hp < 0) hp = 0; }


    LOG_DEBUG("GameClientSession %u: gun shot eid=%u d=%d ap=%d power=%d dmg=%d result=%u -> hp=%d/%d%s", session_id(),
              tid, dist_i(cx, cy, mx, my), gun_ap, atk_power_ == 5 ? 1 : 0, dmg, result, hp, hp_max,
              killed ? " [KILLED]" : "");

    // The command block: 0x01E5 (sub_5F5750, the opener: the client's gun action) + the damage event's
    // 0x002A / 0x011E [+ 0x002B on a kill].
    std::vector<PacketBuffer> group;
    {
        PacketBuffer s(static_cast<Opcode>(0x01E5), is_paula());
        s.write<uint16_t>(self);
        s.write<uint16_t>(2);                                            // kind 2 = gun (client action 54)
        s.write<uint16_t>(tid);
        s.write<uint16_t>(static_cast<uint16_t>(cx)); s.write<uint16_t>(static_cast<uint16_t>(cy));
        s.write<uint16_t>(static_cast<uint16_t>(mx)); s.write<uint16_t>(static_cast<uint16_t>(my));
        s.write<int16_t>(static_cast<int16_t>(shot_dmg));
        s.write<uint8_t>(result);
        group.push_back(std::move(s));
    }
    {
        PacketBuffer d(static_cast<Opcode>(0x002A), is_paula());
        d.write<uint16_t>(tid);
        d.write<int32_t>(dmg);                                           // [0] damage
        d.write<int32_t>(4);                                             // [1] (sub_5E4FD0: 4)
        d.write<int32_t>(result);                                        // [2] result (|0x80 fatal)
        d.write<int32_t>(kGunElement);                                   // [3] element (256 = gun)
        d.write<int32_t>(dinfo_flags);                                   // [4] flags (1: halved by ESAHarden)
        d.write<int32_t>(1);                                             // [5] the attacker's kind (sub_715A90)
        d.write<int32_t>(0);                                             // [6]
        group.push_back(std::move(d));
    }
    {
        PacketBuffer h(static_cast<Opcode>(0x011E), is_paula());
        h.write<uint16_t>(tid);
        h.write<int32_t>(hp);
        h.write<uint16_t>(0);
        h.write<uint8_t>(0);
        group.push_back(std::move(h));
    }
    if (killed) {
        PacketBuffer k(static_cast<Opcode>(0x002B), is_paula());
        k.write<uint16_t>(tid);
        k.write<uint8_t>(0);
        group.push_back(std::move(k));
    }
    server_.broadcast_block(zone_id_, group, 0);
    last_shot_tick_ = tick_ms();                                         // sub_765480

    if (killed) { owner_monster_quest(m); kill_tm_reward(m); server_.drop_monster_loot(m, mx, my); }
    gain_attack_exp(m, dmg, hp_before);                                  // 30003 (sub_5E71D0: PvE)
    if (killed) {
        end_attack(true);
        kill_monster_aftermath(m, tid, mx, my);
        return true;
    }
    engage_monster(m);                                                   // 30002: the monster takes us
    return true;
}
