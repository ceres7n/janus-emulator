#include "game_server.hpp"
#include "game_session.hpp"
#include "logger.hpp"
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
//  Exchange shops — 1:1 with the retail GameServer (GameServerRelease):
//
//  ExchangeShop_Start (NpcScript 22, sub_7E3410): the NPC's ExcShopTable (NpcTalk +120; none -> -1) ->
//    S2C 117, the player's state 18, S2C 249 [u8 n]{u16 ExchangeID} (sub_5FC780). 0.
//  State 18 (the Player state machine, jumptable 0074EADF) swallows C2S 0x18 (move), 0x1D (attack),
//  0x21 (pick-up), 0x2C (drill) and 0; a drop (67) -> S2C 0x45.
//    C2S 251 [u16 ExchangeID] -> its ExchangeShopContents row (sub_62C180: the first of the id — any id, not
//      only the NPC's) -> S2C 252 [u16 id][u32 Fee][u8 n]{u32 ResID}[u8 m]{u32 ReqID, u8 ReqNum}
//      (sub_5FC910); none -> S2C 253.
//    C2S 254 [u16 ExchangeID][u8 n]{u64 uid, u32 item, u16 count}[u8 times] -> no row: S2C 256; Fee < 0 or
//      more than our money: S2C 191 [i32 -1]; each item given must be that bag item with at least its count
//      (sub_6DB270) and the gift must match the row (sub_62C370: times 1, as many items as ReqIDs, each
//      ReqID given with at least ReqNum) else S2C 191 [-1]; the bag records (sub_6DC0E0: one of each ResID
//      times `times`, the items given taken by uid; not enough free entries -> S2C 256); hItemExchangeShop
//      -> OnOk (sub_780A90): the records, the Fee paid (S2C 41), S2C 255 [u8 n]{record} (sub_5FC850).
//  C2S 257 (any state; the client's exchange window closes, c2s_257) -> state 0, S2C 258.
//  All VERIFIED in the client (sub_6F0960 cases 249 / 252 / 253 / 255 / 256 / 258 / 191; c2s_251 /
//  c2s_254 / c2s_257).
// ─────────────────────────────────────────────────────────────────────────────

int GameClientSession::npc_exchange_shop_start(NpcEntity& n)
{
    const std::string* t = n.row ? n.row->get("ExcShopTable") : nullptr;
    const std::vector<int>* ids = (t && !t->empty()) ? server_.exchange_shop_db().table(*t) : nullptr;
    if (!ids) return -1;
    exit_npc_dialog();                                   // S2C 117 (sub_5F3BA0) ...
    exchange_ = true;                                    // ... and state 18 (sub_414B50(player, 18))
    PacketBuffer p(static_cast<Opcode>(0x00F9), is_paula());
    p.write<uint8_t>(static_cast<uint8_t>(ids->size()));
    for (int id : *ids) p.write<uint16_t>(static_cast<uint16_t>(id));
    send(p);
    LOG_DEBUG("GameClientSession %u: exchange shop %s (%zu exchanges)", session_id(), t->c_str(), ids->size());
    return 0;
}

// C2S 251 (state 18).
void GameClientSession::handle_exchange_info(PacketBuffer& pkt)
{
    try {
        if (!exchange_) return;
        const uint16_t id = pkt.read<uint16_t>();
        const ExchangeInfo* e = server_.exchange_shop_db().get(id);
        if (!e) {
            PacketBuffer c(static_cast<Opcode>(0x00FD), is_paula());
            send(c);
            return;
        }
        PacketBuffer p(static_cast<Opcode>(0x00FC), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(e->id));
        p.write<uint32_t>(static_cast<uint32_t>(e->fee));
        p.write<uint8_t>(static_cast<uint8_t>(e->res.size()));
        for (uint32_t r : e->res) p.write<uint32_t>(r);
        p.write<uint8_t>(static_cast<uint8_t>(e->req.size()));
        for (const ExchangeInfo::Req& r : e->req) {
            p.write<uint32_t>(r.item);
            p.write<uint8_t>(static_cast<uint8_t>(r.num));
        }
        send(p);
    } catch (...) {}
}

// C2S 254 (state 18).
void GameClientSession::handle_exchange(PacketBuffer& pkt)
{
    try {
        if (!exchange_) return;
        const uint16_t id = pkt.read<uint16_t>();
        const ExchangeInfo* e = server_.exchange_shop_db().get(id);
        auto fail = [&]() { PacketBuffer c(static_cast<Opcode>(0x0100), is_paula()); send(c); };   // S2C 256
        auto invalid = [&]() {                                                                        // S2C 191
            PacketBuffer c(static_cast<Opcode>(0x00BF), is_paula());
            c.write<int32_t>(-1);
            send(c);
        };
        if (!e) { fail(); return; }
        const int n = pkt.read<uint8_t>();
        std::vector<QuestItem> given;
        for (int i = 0; i < n; ++i) {
            QuestItem g;
            g.uid  = pkt.read<uint64_t>();
            g.item = pkt.read<uint32_t>();
            g.cnt  = pkt.read<uint16_t>();
            given.push_back(g);
        }
        const int times = pkt.read<uint8_t>();
        if (n > 5) { invalid(); return; }                // (the retail reads into five entries)
        if (e->fee < 0 || money_ < e->fee) { invalid(); return; }
        // sub_6DB270: each item given is that bag item (uid), of that id, with at least its count (> 0).
        for (const QuestItem& g : given) {
            if (!g.item) continue;
            const BagItem* b = bag_find_uid(g.uid);
            const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
            const int have = b ? (d ? d->item_count(b->count) : b->count) : 0;
            if (!b || b->item_id != g.item || have < g.cnt || g.cnt <= 0) { invalid(); return; }
        }
        // sub_62C370: times 1, no more ReqIDs than items, every ReqID given with at least its ReqNum, and as
        // many items as ReqIDs.
        {
            int matched = 0;
            const int m = static_cast<int>(e->req.size());
            bool ok = (times == 1) && m <= n;
            if (ok) {
                for (int i = 0; i < m; ++i)
                    for (int j = 0; j < n; ++j)
                        if (e->req[static_cast<size_t>(i)].item == given[static_cast<size_t>(j)].item &&
                            e->req[static_cast<size_t>(i)].num <= given[static_cast<size_t>(j)].cnt) { ++matched; break; }
                ok = matched == m && m >= n;
            }
            if (!ok) { invalid(); return; }
        }
        // Hardening (not retail): one uid given twice passed each check on its own and was taken once
        // (quest_bag_records), and sub_62C370 lets one entry answer several ReqIDs of its id — one rare item paid for
        // five. A uid's counts add up against its bag item, and each ReqID's item must come in its total ReqNum.
        if (!given_merge(given)) { invalid(); return; }
        for (const auto& r : e->req) {
            int need = 0, got = 0;
            for (const auto& x : e->req) if (x.item == r.item) need += x.num;
            for (const QuestItem& g : given) if (g.item == r.item) got += g.cnt;
            if (got < need) {
                LOG_INFO("hItemExchangeShop::Send - item %u given %d of %d : %s", r.item, got, need, char_name_);
                invalid();
                return;
            }
        }
        std::vector<QuestItem> rewards;
        for (uint32_t r : e->res) rewards.push_back({ r, times, 0 });
        // hItemExchangeShop::Send (sub_780190): every item a known one; the new entries must fit.
        for (const QuestItem& r : rewards) if (!r.item) { fail(); return; }
        int new_items = 0;
        quest_bag_records(given, rewards, &new_items, false);
        if (300 - static_cast<int>(bag_.size()) < new_items) {
            LOG_INFO("hItemExchangeShop::Send - HasAcceptableSpace returns false : %s, calc.cnt: %d", char_name_,
                     new_items);
            fail();
            return;
        }
        const std::vector<BagRecord> recs = quest_bag_records(given, rewards);
        if (e->fee) {
            money_ -= e->fee;
            send_money();
        }
        PacketBuffer p(static_cast<Opcode>(0x00FF), is_paula());
        p.write<uint8_t>(static_cast<uint8_t>(recs.size()));
        for (const BagRecord& r : recs) write_bag_record(p, r);
        send(p);
        save_position();
        LOG_INFO("%s : Item Exchange OK ; Money: %d (exchange %d, %zu records)", char_name_, money_, e->id, recs.size());
    } catch (...) {}
}

// Hardening (not retail): the items given to an exchange or a compound, one entry per uid (their counts add up, a
// blank entry goes) — each still that bag item, of that id, with the summed count. False: refused.
bool GameClientSession::given_merge(std::vector<QuestItem>& given) const
{
    std::vector<QuestItem> m;
    for (const QuestItem& g : given) {
        if (!g.item) continue;
        auto it = std::find_if(m.begin(), m.end(), [&](const QuestItem& x) { return x.uid == g.uid; });
        if (it == m.end()) { m.push_back(g); continue; }
        if (it->item != g.item) return false;
        it->cnt += g.cnt;
    }
    for (const QuestItem& g : m) {
        const BagItem* b = nullptr;
        for (const BagItem& x : bag_) if (x.uid == g.uid) { b = &x; break; }
        const ItemDef* d = b ? server_.item_db().get(b->item_id) : nullptr;
        const int have = b ? (d ? d->item_count(b->count) : b->count) : 0;
        if (!b || b->item_id != g.item || g.cnt <= 0 || have < g.cnt) return false;
    }
    given.swap(m);
    return true;
}

// C2S 257 (any state): state 0, S2C 258 (client: the exchange window closes, the avatar's state 0).
void GameClientSession::handle_exchange_close()
{
    exchange_ = false;
    shop_     = 0;
    if (talk_npc_) npc_end_talk();
    PacketBuffer p(static_cast<Opcode>(0x0102), is_paula());
    send(p);
}
