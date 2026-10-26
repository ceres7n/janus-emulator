#include "game_server.hpp"
#include "game_session.hpp"
#include "combat_util.hpp"
#include "logger.hpp"
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
//  Compounds — 1:1 with the retail GameServer (GameServerRelease) and the KR client.
//
//  Compound_Start (NpcScript 20, sub_7E3190): the NPC's CompounderTable (NpcTalk +116, the first
//    Compounder_Info list of that name; none -> -1) -> S2C 117, the state 13, S2C 188 [u8 shown][u16
//    Grade]{u16 CompoundID of each entry not Hidden} (sub_5FC2F0; client case 188 -> the compound window
//    18). 0. C2S 187 (any state) enters the state 13 as well, without any NPC; C2S 192 (any state) ->
//    state 0 and S2C 193 (the window closes).
//  State 13 (Player jumptable 0074CB81; entering it resets the CompndMgr, Player +2752, sub_827B40):
//    C2S 0x18 / 0x1D / 0x21 / 0x2C and 0 are swallowed, a drop (67) -> S2C 0x45;
//    C2S 221 [u16 id]: its CompoundInfo (sub_62B370) -> S2C 222 [u16 type][u16 id][u32 Fee][u8 n]{u32
//      ResID}[u8 m]{u32 ReqID, u8 ReqNum}[u8 Probability] (sub_5FC5C0), else S2C 223;
//    C2S 227 [u16 id][u8 type][u8 spins][u8 n]{u64 uid, u32 item, u16 cnt} (CMD_REQUEST_COMPOUND_
//      PROBABILITY): fewer than n x 14 bytes left -> nothing; CheckValidItemCompound (sub_62B560: a rare
//      compound (type 1) only for one spin; no more ReqIDs than items; every ReqID among the items with
//      ReqNum x spins of it at most what the item says; 0 = as many items as ReqIDs, 1 = more, -1 =
//      not) must answer 0 and the bag must hold each item (sub_6DB270: that uid, of that id, at least
//      cnt) — else S2C 191 [i32 -1]; then the roll (CompndMgr::sub_827B90): the rate = min(100,
//      the DA bonus (sub_8280A0, random) + Probability); the client's type 10 (exactly the items asked)
//      -> each spin succeeds when rand % 100 < rate (sub_827EB0: over 20 spins, 20 rolls of spins/20 + 1
//      each, the rest rolled one by one), mode 3; type 6 the same at half the rate; 5 / 9 (their
//      helpers answer 0 in this build) -> rate 0, no success, mode 0. S2C 228 [u8 rate] and the request
//      becomes the session. (The KR client only sends type 10: its flag +892 is never set.)
//    C2S 186 [u8 n]{u64 uid, u32 item, u16 cnt} (CMD_COMPOUND): the session's count becomes n; fewer than
//      n x 14 bytes -> nothing; each item must be the session's (uid, id != 0, cnt) — else S2C 191 [-1];
//      the session's CompoundInfo; the fee (sub_8281E0: Fee x spins, x 0.9 from 10 spins; no spin ->
//      -1) within our money; the items in the bag and CheckValidItemCompound 0 — else 191 [-1]. Mode
//      1..3: each ResID x the successes, the WasteItem x the failures; mode 0: nothing. hItemCompound
//      (sub_77F970): every ResID known (else -1), the bag records (sub_6DC0E0: what is made first, then
//      the items given), the new entries must fit the bag's free places (else 0) -> DB -> OnOk
//      (sub_77FED0): the records, the fee paid (S2C 41), S2C 190 [u8 n]{u32 ResID}[u32 WasteItem][u8
//      successes][u8 failures][u8 records]{record} (sub_5FC410). Anything but 1 -> S2C 191 [result].
//  All VERIFIED in the client (sub_6F0960 cases 188 / 190 / 191 / 193 / 222 / 223 / 228; c2s_186 /
//  c2s_192 / c2s_221 / c2s_227).
//  Safety: a C2S 227 with more than 5 items is refused (the retail reads them over its 5-entry session
//  on the stack); a session is used by one C2S 186 only (the retail keeps its roll for the next C2S 186
//  until the state ends, so the same lucky roll could be spent again).
//  Not done: the achievement counter of a compound (sub_7B3B60(34, 1)).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr int kBagCapacity = 300;    // sub_6E4290: the bag's free places = 300 - its items

// sub_8280A0: the DA bonus to the compound rate (rand % 100 rolls against DA + 5).
int da_bonus(int da)
{
    const int v = da + 5;
    if (v > 0 && v < 11) return rnd(100) < v ? 5 : 0;
    if (v > 10 && v < 21) return rnd(100) >= v ? 5 : 10;
    if (v > 20 && v < 31) return rnd(100) >= v ? 10 : 20;
    if (v > 30 && v < 41) {
        for (int j = 3; j > 1; --j) if (rnd(100) < v) return 10 * j;
        return 10;
    }
    if (v > 40) {
        for (int i = 4; i > 1; --i) if (rnd(100) < v) return 10 * i;
        return 10;
    }
    return 0;
}

// sub_8281E0: the fee for `spin` spins; -1 without a spin or with a negative fee.
int compound_fee(int spin, int fee)
{
    if (!spin || fee < 0) return -1;
    if (spin >= 10) return static_cast<int>(static_cast<double>(fee * spin) * 0.9);
    return fee * spin;
}

} // namespace

// CheckValidItemCompound (sub_62B560): -1 not valid, 0 exactly the items asked, 1 more items.
template <class Item>
static int compound_check(const CompoundInfo* c, const Item* items, int n, int spin)
{
    if (!c) return -1;
    if (c->type == 1 && spin != 1) {
        LOG_INFO("Compounding Rare Item Failed - CheckValidItemCompound() : Item Type : eCompoundType_Rare;%s, SpinCnt : %d",
                 c->name.c_str(), spin);
        return -1;
    }
    const int reqs = static_cast<int>(c->req.size());
    if (reqs > n) return -1;
    int matched = 0;
    for (int r = 0; r < n; ++r) {
        if (r >= reqs) continue;                         // (the record's blank ReqIDs match nothing)
        int i = 0;
        for (; i < n; ++i) if (c->req[static_cast<size_t>(r)].item == items[i].item) break;
        if (i >= n) continue;
        if (spin * c->req[static_cast<size_t>(r)].num <= items[i].cnt) ++matched;      // sub_62B7C0
        else return -1;
    }
    if (matched != reqs) return -1;
    return reqs >= n ? 0 : 1;
}

// Hardening (not retail): each ReqID's item given in its total spin x ReqNum — sub_62B560 lets one entry answer
// several ReqIDs of its id; `given` is one entry per uid (given_merge: a uid given twice was taken once).
static bool compound_given_ok(const CompoundInfo* c, const std::vector<QuestItem>& given, int spin)
{
    if (!c) return false;
    for (const CompoundInfo::Req& r : c->req) {
        int64_t need = 0, got = 0;
        for (const CompoundInfo::Req& x : c->req) if (x.item == r.item) need += static_cast<int64_t>(spin) * x.num;
        for (const QuestItem& g : given) if (g.item == r.item) got += g.cnt;
        if (got < need) return false;
    }
    return true;
}

void GameClientSession::compound_enter()
{
    rest_state_ = 0;                                     // (the state 13 replaces the others)
    shop_       = 0;
    exchange_   = false;
    drill_      = Drill{};
    wh_leave_state();
    skill_shop_leave();
    compound_   = true;
    cmp_        = CompoundSession{};                     // sub_827B40
}

void GameClientSession::compound_leave()
{
    if (!compound_) return;
    compound_ = false;
    cmp_      = CompoundSession{};
}

// Compound_Start (sub_7E3190).
int GameClientSession::npc_compound_start(NpcEntity& n)
{
    const std::string* t = n.row ? n.row->get("CompounderTable") : nullptr;
    const CompounderList* l = (t && !t->empty()) ? server_.compound_db().list(*t) : nullptr;
    if (!l) return -1;
    exit_npc_dialog();                                   // S2C 117 (sub_5F3BA0)
    compound_enter();                                    // state 13
    int shown = 0;
    for (const CompounderList::Entry& e : l->entries) if (!e.hidden) ++shown;
    PacketBuffer p(static_cast<Opcode>(0x00BC), is_paula());          // S2C 188 (sub_5FC2F0)
    p.write<uint8_t>(static_cast<uint8_t>(shown));
    p.write<uint16_t>(static_cast<uint16_t>(l->grade));
    for (const CompounderList::Entry& e : l->entries)
        if (!e.hidden) p.write<uint16_t>(static_cast<uint16_t>(e.id));
    send(p);
    LOG_DEBUG("GameClientSession %u: compounder %s (%d compounds, grade %d)", session_id(), l->name.c_str(), shown,
              l->grade);
    return 0;
}

// C2S 221 (state 13).
void GameClientSession::handle_compound_info(PacketBuffer& pkt)
{
    try {
        const uint16_t id = pkt.read<uint16_t>();
        if (!compound_) return;
        const CompoundInfo* c = server_.compound_db().get(id);
        if (!c) {
            PacketBuffer f(static_cast<Opcode>(0x00DF), is_paula());  // S2C 223 (sub_5F4350)
            send(f);
            return;
        }
        PacketBuffer p(static_cast<Opcode>(0x00DE), is_paula());      // S2C 222 (sub_5FC5C0)
        p.write<uint16_t>(static_cast<uint16_t>(c->type));
        p.write<uint16_t>(static_cast<uint16_t>(c->id));
        p.write<uint32_t>(static_cast<uint32_t>(c->fee));
        p.write<uint8_t>(static_cast<uint8_t>(c->res.size()));
        for (uint32_t r : c->res) p.write<uint32_t>(r);
        p.write<uint8_t>(static_cast<uint8_t>(c->req.size()));
        for (const CompoundInfo::Req& r : c->req) {
            p.write<uint32_t>(r.item);
            p.write<uint8_t>(static_cast<uint8_t>(r.num));
        }
        p.write<uint8_t>(static_cast<uint8_t>(c->probability));
        send(p);
    } catch (...) {}
}

// C2S 227 (state 13): CMD_REQUEST_COMPOUND_PROBABILITY.
void GameClientSession::handle_compound_prob(PacketBuffer& pkt)
{
    try {
        if (!compound_) return;
        auto fail = [&](int32_t code) {                  // S2C 191 (sub_5FC550)
            PacketBuffer c(static_cast<Opcode>(0x00BF), is_paula());
            c.write<int32_t>(code);
            send(c);
        };
        CompoundSession v;
        v.id   = pkt.read<uint16_t>();
        v.type = pkt.read<uint8_t>();
        v.spin = pkt.read<uint8_t>();
        v.n    = pkt.read<uint8_t>();
        const size_t left = pkt.size() > pkt.rpos() ? pkt.size() - pkt.rpos() : 0;
        if (left < static_cast<size_t>(v.n) * 14) {
            LOG_INFO("CMD_REQUEST_COMPOUND_PROBABILITY : Invalid msg size : (%zu < %d * 14)", left, v.n);
            return;
        }
        if (v.n > 5) { fail(-1); return; }               // (safety: the session holds 5)
        for (int i = 0; i < v.n; ++i) {
            v.items[i].uid  = pkt.read<uint64_t>();
            v.items[i].item = pkt.read<uint32_t>();
            v.items[i].cnt  = pkt.read<uint16_t>();
        }
        const CompoundInfo* c = server_.compound_db().get(v.id);
        if (compound_check(c, v.items, v.n, v.spin) != 0) {
            LOG_INFO("%s : CMD_REQUEST_COMPOUND_PROBABILITY : Item Compound Failed (%d)", char_name_, v.id);
            fail(-1);
            return;
        }
        // sub_6DB270: each item (id != 0) is that bag item, of that id, with at least its count (> 0).
        for (int i = 0; i < v.n; ++i) {
            const CompoundSession::Item& it = v.items[i];
            if (!it.item) continue;
            const BagItem* b = bag_find_uid(it.uid);
            const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
            const int have = b ? (d ? d->item_count(b->count) : b->count) : 0;
            if (!b || b->item_id != it.item || have < it.cnt || it.cnt <= 0) { fail(-1); return; }
        }
        {                                                // Hardening (not retail): see compound_given_ok
            std::vector<QuestItem> given;
            for (int i = 0; i < v.n; ++i) given.push_back({ v.items[i].item, v.items[i].cnt, v.items[i].uid });
            if (!given_merge(given) || !compound_given_ok(c, given, v.spin)) {
                LOG_INFO("%s : CMD_REQUEST_COMPOUND_PROBABILITY : items given twice / short (%d)", char_name_, v.id);
                fail(-1);
                return;
            }
        }
        // CompndMgr::sub_827B90: the rate (sub_828040) and the roll (sub_827EB0).
        int rate = 0;
        if (c) rate = std::min(100, da_bonus(st_da_) + c->probability);
        int success = 0, failed = 0, mode = -1;
        auto roll = [&](int spins) {
            success = failed = 0;
            if (!rate) return;
            int rolls = spins, each = 1;
            if (spins > 20) { rolls = 20; each = spins / 20 + 1; }
            for (int i = 0; i < rolls; ++i) {
                if (rolls >= 20 && each * (i + 1) >= spins) {
                    for (int j = 0; j < spins - i * each; ++j) {
                        if (rnd(100) >= rate) ++failed;
                        else ++success;
                    }
                    break;
                }
                if (rnd(100) >= rate) failed += each;
                else success += each;
            }
            if (success + failed != spins)
                LOG_ERROR("CompndMgr::CalcResult: m_iItemCntFail(%d) + m_iItemCntSuccess(%d) != iSumOfItems(%d)", failed,
                          success, spins);
        };
        switch (v.type) {
        case 5:                                          // (sub_828250 / sub_828260 answer 0)
        case 9:
            rate = 0;
            success = 0;
            failed = v.spin;
            mode = 0;
            break;
        case 6:
        case 10: {
            const int r = compound_check(c, v.items, v.n, v.spin);
            if (v.type == 6) rate /= 2;
            if (r < 0) break;
            if (r > 0) rate /= 2;
            roll(v.spin);
            mode = 3;
            break;
        }
        default:
            break;
        }
        if (mode == -1) { fail(-1); return; }
        v.mode    = mode;
        v.rate    = rate;
        v.success = success;
        v.fail    = failed;
        cmp_ = v;
        PacketBuffer p(static_cast<Opcode>(0x00E4), is_paula());      // S2C 228 (sub_5FC710)
        p.write<uint8_t>(static_cast<uint8_t>(rate));
        send(p);
        LOG_DEBUG("GameClientSession %u: compound %d x%d: rate %d -> %d made, %d failed", session_id(), v.id, v.spin, rate,
                  success, failed);
    } catch (...) {}
}

// C2S 186 (state 13): CMD_COMPOUND.
void GameClientSession::handle_compound(PacketBuffer& pkt)
{
    try {
        if (!compound_) return;
        auto fail = [&](int32_t code) {                  // S2C 191 (sub_5FC550)
            PacketBuffer c(static_cast<Opcode>(0x00BF), is_paula());
            c.write<int32_t>(code);
            send(c);
        };
        const int n = pkt.read<uint8_t>();
        cmp_.n = static_cast<int16_t>(n);
        const size_t left = pkt.size() > pkt.rpos() ? pkt.size() - pkt.rpos() : 0;
        if (left < static_cast<size_t>(n) * 14) {
            LOG_INFO("CMD_COMPOUND : Invalid msg size : (SrcCnt:%d, %d, %zu)", n, n * 2, left);
            return;
        }
        for (int i = 0; i < n; ++i) {
            const uint64_t uid  = pkt.read<uint64_t>();
            const uint32_t item = pkt.read<uint32_t>();
            const int      cnt  = pkt.read<uint16_t>();
            const CompoundSession::Item none{};
            const CompoundSession::Item& s = i < 5 ? cmp_.items[i] : none;
            if (uid != s.uid) {
                LOG_INFO("CMD_COMPOUND : [%s] uid (%llu) is not same", char_name_, static_cast<unsigned long long>(uid));
                fail(-1);
                return;
            }
            if (item != s.item) {
                LOG_INFO("CMD_COMPOUND : [%s] id (%u) is not same", char_name_, item);
                fail(-1);
                return;
            }
            if (!s.item) {
                LOG_INFO("CMD_COMPOUND : [%s] invalid itemID (eItemID_None)", char_name_);
                fail(-1);
                return;
            }
            if (cnt != s.cnt) {
                LOG_INFO("CMD_COMPOUND : [%s] cnt (%d) is not same", char_name_, cnt);
                fail(-1);
                return;
            }
        }
        const CompoundInfo* c = server_.compound_db().get(cmp_.id);
        if (!c) { fail(-1); return; }
        const int fee = compound_fee(cmp_.spin, c->fee);
        if (fee < 0 || money_ < fee) { fail(-1); return; }
        const int cnt = std::min(n, 5);
        bool has = true;
        for (int i = 0; i < cnt && has; ++i) {
            const CompoundSession::Item& it = cmp_.items[i];
            if (!it.item) continue;
            const BagItem* b = bag_find_uid(it.uid);
            const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
            const int have = b ? (d ? d->item_count(b->count) : b->count) : 0;
            if (!b || b->item_id != it.item || have < it.cnt || it.cnt <= 0) has = false;
        }
        if (!has || compound_check(c, cmp_.items, cnt, cmp_.spin) != 0) {
            LOG_INFO("%s : CMD_COMPOUND : Item Compound Failed : %s", char_name_, c->name.c_str());
            fail(-1);
            return;
        }
        if (cmp_.mode < 0 || cmp_.mode > 3) { fail(-1); return; }
        // The results: mode 1..3 -> each ResID x the successes, the WasteItem x the failures; mode 0 none.
        std::vector<QuestItem> made;
        std::vector<uint32_t> res_ids;
        uint32_t waste = 0;
        if (cmp_.mode != 0) {
            for (uint32_t r : c->res) {
                made.push_back({ r, cmp_.success, 0 });
                res_ids.push_back(r);
            }
            waste = c->waste;
        }
        // hItemCompound::Send (sub_77F970)
        for (uint32_t r : res_ids) if (!r) { LOG_INFO("hItemCompound invalid itemID name : %s", char_name_); fail(-1); return; }
        made.push_back({ waste, cmp_.mode != 0 ? cmp_.fail : 0, 0 });
        std::vector<QuestItem> given;
        for (int i = 0; i < cnt; ++i) given.push_back({ cmp_.items[i].item, cmp_.items[i].cnt, cmp_.items[i].uid });
        if (!given_merge(given) || !compound_given_ok(c, given, cmp_.spin)) {   // Hardening (not retail): as the roll's
            LOG_INFO("%s : CMD_COMPOUND : items given twice / short : %s", char_name_, c->name.c_str());
            fail(-1);
            return;
        }
        int new_items = 0;
        quest_bag_records(given, made, &new_items, false);
        if (kBagCapacity - static_cast<int>(bag_.size()) < new_items) {
            LOG_INFO("hItemCompound::Send - HasAcceptableSpace returns false : %s, calc.cnt: %d", char_name_, new_items);
            fail(0);
            return;
        }
        LOG_DEBUG("%s : Try Item Compound : fee = %d", char_name_, fee);
        // OnOk (sub_77FED0)
        const std::vector<BagRecord> recs = quest_bag_records(given, made);
        if (fee) {
            money_ -= fee;
            send_money();
        }
        PacketBuffer p(static_cast<Opcode>(0x00BE), is_paula());      // S2C 190 (sub_5FC410)
        p.write<uint8_t>(static_cast<uint8_t>(res_ids.size()));
        for (uint32_t r : res_ids) p.write<uint32_t>(r);
        p.write<uint32_t>(waste);
        p.write<uint8_t>(static_cast<uint8_t>(cmp_.success));
        p.write<uint8_t>(static_cast<uint8_t>(cmp_.fail));
        p.write<uint8_t>(static_cast<uint8_t>(recs.size()));
        for (const BagRecord& r : recs) write_bag_record(p, r);
        send(p);
        save_position();
        LOG_INFO("%s : Item Compound OK ; Money: %d (compound %d x%d: %d made, %d failed)", char_name_, money_, cmp_.id,
                 cmp_.spin, cmp_.success, cmp_.fail);
        cmp_.mode = -1;                                  // (safety: one compound per roll)
    } catch (...) {}
}

// C2S 187 (any state): the state 13.
void GameClientSession::handle_compound_open()
{
    if (dead_) return;
    if (talk_npc_) npc_end_talk();
    compound_enter();
}

// C2S 192 (any state): state 0, S2C 193 (sub_5FC3F0: the compound window closes).
void GameClientSession::handle_compound_close()
{
    compound_leave();
    if (talk_npc_) npc_end_talk();
    PacketBuffer p(static_cast<Opcode>(0x00C1), is_paula());
    send(p);
}
