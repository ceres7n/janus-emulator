#include "game_session.hpp"
#include "game_server.hpp"
#include "item_wire.hpp"
#include "item_option.hpp"
#include "client_text.hpp"
#include "logger.hpp"
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  NPC shops — retail GameServer, verified against the KR client.
//
//  A map's ShopTable rows are ShopNpcs (object kind 5, shop.cpp's world part: cGameServer::
//  spawn_shops); the client sends C2S 0x0046 when one is clicked (sub_6B96C0: a kind-5 object).
//    C2S 0x0046 [u16 shop] (Player case 70, a message of every state): S2C 0x00A8 [u16 shop][cstr the
//      shop's Script] (sub_5F8C00; client sub_6F00D0 opens the shop dialog with the greeting and the
//      CharacterInfo name), the shop state 5, S2C 0x02D9 [u16 us][pos][u8 move bits] to the area
//      (sub_5F5440 — the stop), and the running action stops (sub_41C5D0).
//  The shop state 5 (sub_73D2A0 case 5) swallows moves (0x18) and actions (0x40) and answers:
//    C2S 0x00A4 [u16 shop] -> S2C 0x0047 [u16 n]{u32 ItemId, u32 price} of the items not OutOfStock
//      (sub_5F8C90; client sub_6EC130 fills the buy list); no shop -> S2C 0x00A5 (__fputchar_48)
//    C2S 0x00A6 [u16 shop] -> S2C 0x00A9 [f32 ShopPriceRate x the shop's SellingRate][u16 n]{u32
//      ItemId, f32 its SellingRate} of the items whose SellingRate > 0 (sub_5F8D90; client sub_6EC1F0);
//      no shop -> S2C 0x00A7 (__fputchar_49). (The KR client handles neither 0x00A5 nor 0x00A7 —
//      not sent.)
//    C2S 0x0048 buy (client c2s_72, sub_A4F120 / sub_A5D920): [u16 shop][u16 n] n x {u32 ItemId,
//      u16 count} -> S2C 0x0049 [u32 money][u16 n] + the bag records (sub_5F9BE0; client case 73:
//      the money, then sub_6B3DC0 per record — the S2C 0x00A0 record) or S2C 0x004A (sub_5F3930) /
//      0x004B weight (sub_5F3950)
//    C2S 0x039F inner 76 sell (client sub_5AC3A0 through the second-password wrapper): [u16 shop]
//      [u16 n] n x {u64 uid, u32 ItemId, u16 count} -> S2C 0x004E [u32 money][u16 n] + the bag records
//      (sub_5F9C90) or S2C 0x004D (sub_5F3990); money past 2,100,000,000 -> S2C 0x0355 [u16 0]
//      (sub_6043D0, "Money Is Full") alone
//    C2S 0x004F (client c2s_79, its avatar in its shop state 12): back to state 0
//  Prices: buy (sub_720160) = (int)(map ShopPriceRate x ItemParam Value x rate + 0.5), rate = the
//  shop's item row PurchaseRate when > 0, else the shop's PurchaseRate; sell (sub_720210) =
//  (int)(ShopPriceRate x the item's value x rate), at least 1, rate = the item row's SellingRate when
//  > 0, else the shop's. (A drill's value scales with its durability, sub_6DE7C0 — no drills here.)
// ─────────────────────────────────────────────────────────────────────────────

namespace {
constexpr int64_t kMaxMoneyShop = 2100000000;              // sub_765960: 0x7D2B7500
constexpr int     kMaxShopLines = 20;                      // cntItem 1..20 (and the client's cap)
}

// sub_720160: the buy price of an item in a shop; a skill master's (sub_721480) is not rounded.
int32_t GameClientSession::shop_buy_price(const ShopRow& s, uint32_t item_id, bool round) const
{
    const ShopItemRow* r = s.find(item_id);
    const float rate = (r && r->purchase_rate > 0.0f) ? r->purchase_rate : s.purchase_rate;
    const ItemDef* d = server_.item_db().get(item_id);
    const MapDef* z = server_.maps_db().get(zone_id_);
    const float spr = z ? z->shop_price_rate : 1.0f;
    const double v = d ? static_cast<double>(d->value) : 0.0;
    return static_cast<int32_t>(static_cast<double>(spr) * v * static_cast<double>(rate) + (round ? 0.5 : 0.0));
}

// sub_720210: the sell price of one of a bag item.
int32_t GameClientSession::shop_sell_price(const ShopRow& s, const BagItem& b) const
{
    const ShopItemRow* r = s.find(b.item_id);
    const float rate = (r && r->selling_rate > 0.0f) ? r->selling_rate : s.selling_rate;
    const ItemDef* d = server_.item_db().get(b.item_id);
    const MapDef* z = server_.maps_db().get(zone_id_);
    const float spr = z ? z->shop_price_rate : 1.0f;
    // CommonItem / RareItem vtbl+44: the Value; a rare Type-6 one (a drill, sub_6DE7C0) life * Value / DrillLife.
    int32_t value = d ? d->value : 0;
    if (d && d->cls == 2 && d->type == 6)
        if (const DrillParam* dp = server_.drill_db().get(b.item_id); dp && dp->drill_life)
            value = static_cast<int32_t>(b.count) * d->value / dp->drill_life;
    const double v = static_cast<double>(value);
    const int32_t p = static_cast<int32_t>(static_cast<double>(spr) * v * static_cast<double>(rate));
    return p > 0 ? p : 1;
}

void GameClientSession::handle_shop_open(PacketBuffer& pkt)
{
    try {
        const uint16_t id = pkt.read<uint16_t>();
        NpcEntity* s = server_.zone_shop(zone_id_, id);
        if (!s || !s->shop) {
            // (retail enters the shop state anyway; without the S2C 0x00A8 the client never opens its
            // window nor sends the close, and the state would swallow its moves — not entered here)
            LOG_INFO("GameClientSession %u: shop %u not found in zone %d", session_id(), id, zone_id_);
            return;
        }
        {
            PacketBuffer p(static_cast<Opcode>(0x00A8), is_paula());
            p.write<uint16_t>(id);
            const std::string t = to_client_text(s->shop->script);
            p.write_bytes(t.c_str(), t.size() + 1);
            send(p);
        }
        shop_       = id;                                      // the shop state 5
        rest_state_ = 0;
        drill_      = Drill{};
        wh_leave_state();                                      // (it replaces the warehouse state 10,
        compound_leave();                                      //  the compound state 13,
        refine_ui_leave();                                     //  the refine / growth / reset states
        skill_shop_leave();                                    //  and the skill master state 25)
        const auto now = std::chrono::steady_clock::now();
        int32_t cx, cy;
        cur_pos(now, cx, cy);
        {
            PacketBuffer p(static_cast<Opcode>(0x02D9), is_paula());
            p.write<uint16_t>(static_cast<uint16_t>(object_id_));
            p.write<uint16_t>(static_cast<uint16_t>(cx));
            p.write<uint16_t>(static_cast<uint16_t>(cy));
            p.write<uint8_t>(static_cast<uint8_t>(motion_.state_flags(now)));
            server_.broadcast_zone(zone_id_, p, 0);
        }
        motion_.rest(cx, cy);
        pos_x_ = cx; pos_y_ = cy;
        end_attack(true);                                      // sub_41C5D0
        LOG_DEBUG("GameClientSession %u: shop %u (%s, %s) opened", session_id(), id, s->name.c_str(),
                  s->shop->table.c_str());
    } catch (...) {}
}

// The shop state's C2S 0x00A4 (buy list) / 0x00A6 (sell rates).
void GameClientSession::handle_shop_list(PacketBuffer& pkt, bool sell)
{
    try {
        const uint16_t id = pkt.read<uint16_t>();
        if (!shop_) return;
        NpcEntity* s = server_.zone_shop(zone_id_, id);
        if (!s || !s->shop) {
            // retail: S2C 0x00A7 / 0x00A5 (__fputchar_49 / _48) — the KR client has no handler for them
            LOG_INFO("GameClientSession %u: shop list — shop %u not found", session_id(), id);
            return;
        }
        const ShopRow& row = *s->shop;
        if (!sell) {
            PacketBuffer p(static_cast<Opcode>(0x0047), is_paula());
            uint16_t n = 0;
            for (const ShopItemRow& r : row.items) if (!r.out_of_stock) ++n;
            p.write<uint16_t>(n);
            for (const ShopItemRow& r : row.items) {
                if (r.out_of_stock) continue;
                p.write<uint32_t>(r.item_id);
                p.write<uint32_t>(static_cast<uint32_t>(shop_buy_price(row, r.item_id)));
            }
            send(p);
        } else {
            const MapDef* z = server_.maps_db().get(zone_id_);
            const float rate = (z ? z->shop_price_rate : 1.0f) * row.selling_rate;
            PacketBuffer p(static_cast<Opcode>(0x00A9), is_paula());
            p.write<float>(rate);
            uint16_t n = 0;
            for (const ShopItemRow& r : row.items) if (r.selling_rate > 0.0f) ++n;
            p.write<uint16_t>(n);
            for (const ShopItemRow& r : row.items) {
                if (r.selling_rate <= 0.0f) continue;
                p.write<uint32_t>(r.item_id);
                p.write<float>(r.selling_rate);
            }
            send(p);
        }
    } catch (...) {}
}

// The shop state's C2S 0x0048 (Player::OnNetMsg_BuyItemList, case 5 / 72) -> hItemBuy (sub_7723B0,
// OnOk sub_7726D0).
void GameClientSession::handle_shop_buy(PacketBuffer& pkt)
{
    auto fail = [&](uint16_t op) { PacketBuffer p(static_cast<Opcode>(op), is_paula()); send(p); };
    try {
        if (!shop_ && !sshop_) return;
        // The skill master state 25 (Player case 25 / 72) buys from the master's ItemTable: the same
        // flow without the message size check, its price not rounded (sub_7215F0 / sub_721480).
        const bool skill = !shop_;
        const size_t payload = (pkt.size() > pkt.rpos()) ? pkt.size() - pkt.rpos() : 0;
        const uint16_t id = pkt.read<uint16_t>();
        NpcEntity* s = skill ? server_.zone_skill_shop(zone_id_, id) : server_.zone_shop(zone_id_, id);
        const ShopRow* row = s ? (skill ? (s->sshop ? &s->sshop->shop : nullptr) : s->shop) : nullptr;
        if (!row) {
            LOG_INFO("Player::OnNetMsg_BuyItemList - shop is null (shop:%u)", id);
            fail(0x004A);
            return;
        }
        const int n = pkt.read<uint16_t>();
        if (!skill && static_cast<size_t>(n) * 6 != payload - 4) {
            LOG_ERROR("CMD_BUY_ITEM_LIST invalid msg Size name : %s, %d, %zu", char_name_, n * 6, payload - 4);
            fail(0x004A);
            close();                                           // sub_8AA290: the session is cut
            return;
        }
        if (n <= 0 || n > kMaxShopLines) {
            LOG_INFO("Player::OnNetMsg_BuyItemList - cntItem:%d", n);
            fail(0x004A);
            return;
        }
        struct Line { uint32_t id; int count; };
        std::vector<Line> lines;
        for (int i = 0; i < n; ++i) {
            const uint32_t item = pkt.read<uint32_t>();
            if (!item) {
                LOG_ERROR("CMD_BUY_ITEM_LIST invalid itemID name : %s", char_name_);
                fail(0x004A);
                return;
            }
            const int count = pkt.read<uint16_t>();
            if (count <= 0) {
                LOG_INFO("Player::CMD_BUY_ITEM_LIST - item cnt <= 0 : %s, %u, %d", char_name_, item, count);
                fail(0x004A);
                return;
            }
            lines.push_back({ item, count });
        }
        // sub_73BAD0: the weight (ItemParam Weight x count) must fit MaxWT.
        int64_t weight = 0;
        for (const Line& l : lines) {
            const ItemDef* d = server_.item_db().get(l.id);
            if (!d) { fail(0x004A); return; }                  // (retail: no such ItemParam is a crash)
            weight += static_cast<int64_t>(d->weight) * l.count;
        }
        if (weight + bag_weight_ > st_maxwt_) {
            LOG_DEBUG("Player::CMD_BUY_ITEM_LIST - weight fault : %s", char_name_);
            fail(0x004B);
            return;
        }
        // sub_73BB90: a free bag place for every line.
        if (static_cast<int>(300 - bag_.size()) < n) {
            LOG_DEBUG("Player::CMD_BUY_ITEM_LIST - capacity fault : %s", char_name_);
            fail(0x004A);
            return;
        }
        // The shop's price (sub_7202D0): every item must be one of its list; each line and the sum
        // within an int; the sum within our money.
        int64_t sum = 0;
        for (const Line& l : lines) {
            const ShopItemRow* r = row->find(l.id);
            if (!r) { fail(0x004A); return; }
            // Hardening (not retail): sub_7202D0 matches the item id only, so a forged list bought the OutOfStock rows
            // the buy lists (S2C 0x0047, the master's ItemTable) never show — many sell back for more than they cost.
            if (r->out_of_stock) {
                LOG_INFO("Player::CMD_BUY_ITEM_LIST - out of stock item : %s, shop %u, item %u", char_name_, id, l.id);
                fail(0x004A);
                return;
            }
            const int64_t each = static_cast<int64_t>(shop_buy_price(*row, l.id, !skill)) * l.count;
            if (each > 0x7FFFFFFF) {
                LOG_ERROR("each sum is exceed integer boundary. each sum:%lld,itemId:%u,cnt:%d", static_cast<long long>(each),
                          l.id, l.count);
                fail(0x004A);
                return;
            }
            sum += each;
            if (sum > 0x7FFFFFFF) {
                LOG_ERROR("sum is exceed integer boundary. sum:%lld,itemId:%u,cnt:%d", static_cast<long long>(sum), l.id,
                          l.count);
                fail(0x004A);
                return;
            }
        }
        if (sum > money_) {
            LOG_INFO("Player::CMD_BUY_ITEM_LIST - failed to pay : %s (%lld > %d)", char_name_, static_cast<long long>(sum),
                     money_);
            fail(0x004A);
            return;
        }
        // hItemBuy (sub_7723B0 -> OnOk sub_7726D0): each line becomes a bag record (sub_6DBD10: a common
        // item joins its stack — type 1 — else a new item, type 4; a rare item is always new, its
        // default stats made first, sub_6E0490), the money is what is left, S2C 0x0049.
        std::vector<BagRecord> recs;
        for (const Line& l : lines) {
            const ItemDef* d = server_.item_db().get(l.id);
            // sub_6EBAD0 builds the item: a rare Type-6 one (a drill) gets its DrillLife as its count.
            uint16_t count = static_cast<uint16_t>(l.count);
            if (d->cls == 2 && d->type == 6)
                if (const DrillParam* dp = server_.drill_db().get(l.id)) count = static_cast<uint16_t>(dp->drill_life);
            recs.push_back(bag_add(*d, count));
        }
        money_ = static_cast<int32_t>(money_ - sum);           // sub_73B260
        PacketBuffer p(static_cast<Opcode>(0x0049), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(money_));
        p.write<uint16_t>(static_cast<uint16_t>(recs.size()));
        for (const BagRecord& r : recs) write_bag_record(p, r);
        send(p);
        save_position();                                       // the money to the DB
        LOG_INFO("%s : buy:money:%d (%d lines, %lld gelder)", char_name_, money_, n, static_cast<long long>(sum));
    } catch (...) {}
}

// The shop state's sell (C2S 0x039F inner 76: Player::OnNetMsg_SellItemList, case 5 / 76) -> hItemSell
// (sub_773520, OnOk sub_773790).
void GameClientSession::handle_shop_sell(PacketBuffer& pkt)
{
    auto fail = [&]() { PacketBuffer p(static_cast<Opcode>(0x004D), is_paula()); send(p); };   // sub_5F3990
    try {
        if (!shop_ && !sshop_) return;
        // The skill master state 25 (case 76): the same with the master's items and rates (sub_721530; its
        // hItemSell sub_7745C0 has no money cap — kept here for safety).
        const bool skill = !shop_;
        const uint16_t id = pkt.read<uint16_t>();
        NpcEntity* s = skill ? server_.zone_skill_shop(zone_id_, id) : server_.zone_shop(zone_id_, id);
        const ShopRow* row = s ? (skill ? (s->sshop ? &s->sshop->shop : nullptr) : s->shop) : nullptr;
        if (!row) {
            LOG_INFO("Player::OnNetMsg_SellItemList - shop not found (%u, %s, shop %u)",
                     static_cast<unsigned>(object_id_), char_name_, id);
            fail();
            return;
        }
        const int n = pkt.read<uint16_t>();
        if (n <= 0 || n > kMaxShopLines) {
            LOG_INFO("Player::OnNetMsg_SellItemList - cntItem:%d", n);
            fail();
            return;
        }
        struct Line { uint64_t uid; uint32_t id; int count; };
        std::vector<Line> lines;
        for (int i = 0; i < n; ++i) {
            const uint64_t uid = pkt.read<uint64_t>();
            const uint32_t item = pkt.read<uint32_t>();
            if (!item) {
                LOG_ERROR("CMD_SELL_ITEM_LIST invalid itemID name : %s", char_name_);
                fail();
                return;
            }
            int count = pkt.read<uint16_t>();
            const ItemDef* d = server_.item_db().get(item);
            if (!d) { fail(); return; }
            // sub_6ECC60: the item's flags — a rare item's option of type 13 holds its own — 0x200 =
            // not to be sold.
            uint32_t flags = static_cast<uint32_t>(d->options);
            if (d->cls == 2 && uid) {
                if (const BagItem* b = bag_find_uid(uid))
                    for (const ItemOption& o : b->opts) if (o.type == 13) { flags = static_cast<uint32_t>(o.extra); break; }
            }
            if (flags & 0x200) {
                LOG_INFO("Player::OnNetMsg_SellItemList - not sellable item %u (%s)", item, d->name.c_str());
                fail();
                return;
            }
            // sub_76E3D0: at least 1; a rare item one at a time.
            if (count <= 1) { if (count < 1) count = 1; }
            else if (!d->stackable()) count = 1;
            lines.push_back({ uid, item, count });
        }
        // sub_75C6E0: the same item twice.
        for (size_t i = 0; i + 1 < lines.size(); ++i)
            for (size_t j = i + 1; j < lines.size(); ++j)
                if (lines[i].uid == lines[j].uid) {
                    LOG_INFO("%s : Sell items - occupied items. item:%d", char_name_, n);
                    fail();
                    return;
                }
        // sub_6DB200: each one in the bag with at least that amount. (Not equipped either — the
        // retail leaves that to its client; an equipped item sold would leave its slot pointing nowhere.)
        for (const Line& l : lines) {
            const BagItem* b = bag_find_uid(l.uid);
            if (!b || b->item_id != l.id || b->count < l.count || l.count <= 0 || is_equipped(l.uid)) {
                LOG_INFO("%s : Sell items - invalid selling item. item:%d", char_name_, n);
                fail();
                return;
            }
        }
        // hItemSell::Send: the price (sub_720470), the money cap (sub_765960).
        int64_t sum = 0;
        for (const Line& l : lines)
            sum += static_cast<int64_t>(shop_sell_price(*row, *bag_find_uid(l.uid))) * l.count;
        const int64_t total = static_cast<int64_t>(money_) + sum;
        if (total < 0 || total > kMaxMoneyShop) {
            LOG_DEBUG("%s : Cannot Sell Item : Money Is Full (hItemSell::Send())", char_name_);
            PacketBuffer p(static_cast<Opcode>(0x0355), is_paula());   // sub_6043D0(us, 0)
            p.write<uint16_t>(0);
            send(p);
            return;
        }
        // OnOk (sub_773790): the removals (type 3 / type 1, sub_6DBB80), the money, S2C 0x004E.
        std::vector<BagRecord> recs;
        for (const Line& l : lines) {
            BagItem* b = bag_find_uid(l.uid);
            if (!b) continue;
            const BagRecord r = bag_decrease(*b, static_cast<uint16_t>(l.count));
            if (r.type == 3) server_.item_store().RemoveItem(r.uid);
            else             server_.item_store().SetAmount(r.uid, r.count);
            bag_apply(r);
            recs.push_back(r);
        }
        money_ = static_cast<int32_t>(total);                  // sub_73B280
        PacketBuffer p(static_cast<Opcode>(0x004E), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(money_));
        p.write<uint16_t>(static_cast<uint16_t>(recs.size()));
        for (const BagRecord& r : recs) write_bag_record(p, r);
        send(p);
        save_position();
        LOG_INFO("%s : sell:money:%d (%d lines, %lld gelder)", char_name_, money_, n, static_cast<long long>(sum));
    } catch (...) {}
}

// The shop state's C2S 0x004F (case 79): back to state 0.
void GameClientSession::handle_shop_close()
{
    if (!shop_) return;
    shop_ = 0;
}
