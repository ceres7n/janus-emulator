#include "game_server.hpp"
#include "game_session.hpp"
#include "item_wire.hpp"
#include "logger.hpp"
#include <cstring>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
//  The NeoWarehouse — the account's bank — 1:1 with the retail GameServer (GameServerRelease), its DB
//  side (MasterServer ItemMoneyStore: OnICmdLoadBank, ICMD_NWH_EXCHANGE / _EXCHANGE_MONEY) and the KR
//  client.
//
//  The bank belongs to the account (tbl_item_user item_where 3 and user_info.user_money — here
//  char_inventory rows with item_where 3 and user_uid, and accounts.user_money): Player +1920, an item
//  list of capacity 80 (sub_6DABE0) that the DB fills with the account's first 24 items by uid
//  (uspm_item_select_by_where 3), and +1984 its gelder.
//  NeoWarehouse_Start (NpcScript 17, sub_7E3060): its list still loading (m_SyncFlag 0x1000000) -> 0;
//    level < 10 -> 2; money < 100 -> 3 (the script's 2nd / 3rd argument); else S2C 117 (the dialog
//    ends), 100 gelder paid and S2C 41, the state 11; the list sent already -> S2C 196 [u8 1]; else the
//    DB (4112 -> 4113 sub_761DC0: at most 80 items and the gelder; their options 4106 -> 4107
//    sub_761B50) -> S2C 196 [u8 0][u32 gelder][u16 n]{[u8 kind][item data][u8 0 = no expiry, else 21
//    bytes]}[u16 m]{[u64 uid][u8 k]{u32 type, u32 id | value << 16, u32 extra}} (sub_5F9990; client
//    sub_6F0960 case 196: the bank's list and gelder, the window — the client's state 17).
//  State 11 (NeoWarehouseState, Player jumptable 007529E5): 0 and 64 swallowed, everything else goes on
//    as anywhere; C2S 197 (c2s_197) -> state 0; event 30002 (a hit) -> S2C 197 and state 0 (the KR
//    client has no handler for 197: not sent).
//  C2S 198 [u64 uid][u32 item][u16 cnt] (hItemAddNeoWareHouse sub_77A690): the item's flags (a rare
//    item's type-13 option, else ItemParam Options) holding both 0x200 and 0x400, or 0x20000
//    (eItemOption_NotAddableToWarehouse), or an id of 6900..6999 (sub_6DAF40) -> S2C 269 (no KR client
//    handler: not sent); 24 items in the bank unless the item stacks onto one of its own (sub_6DAF70),
//    the bag not having it (HasAll), the loaded bullets' / throw item's id -> S2C 200; a duplicated uid
//    -> the session ends; the records as the warehouse's (a common stack over 32000 -> 200); DB 4029
//    (uspm_item_char_update_move_inven_to_bank) -> OnOk (sub_77AD30): both applied, S2C 199 [bag
//    record][bank record]; refused (sub_77AE00) -> 200.
//  C2S 201 (hItemRemoveNeoWareHouse sub_77AE40): the bank not having it, the bag full / too heavy
//    (sub_6DB080), the bullets' / throw item's id -> S2C 203; OnOk (sub_77B400): S2C 202 [bag record]
//    [bank record]; refused -> 203.
//  C2S 204 [u32 gelder] (hMoneyAddToNeoWarehouse sub_77B510): <= 0, above the money, the bank above
//    99999999 after -> S2C 206; DB 4032 [money - gelder][bank + gelder] (uspm_user_info_update_move_
//    money) -> OnOk (sub_77B830): S2C 205 [u32 bank][u32 money]; refused -> 206.
//  C2S 207 [u32 gelder] (sub_77B930): <= 0 -> S2C 206; more than the bank holds -> S2C 209 [u16 0];
//    the money + gelder reaching 2100000000 -> 209 [u16 1]; DB 4032 [money + gelder][bank - gelder]
//    -> OnOk (sub_77BC40): S2C 208 [u32 bank][u32 money]; refused -> 206.
//  S2C 206: the recovered EXE sends it empty, the KR client reads [u16] (sub_99B420:
//    "eAddMoneyToNeoWarehouseErr_%d" — 0 a bad amount, 1 more than the money, 2 over the bank's limit:
//    sub_77B510's three checks), so it carries that code (0 for the others).
//  All VERIFIED in the client (sub_6F0960 cases 196 / 199 / 200 / 202 / 203 / 205 / 206 / 208 / 209;
//  c2s_197 / c2s_198 / c2s_201 / c2s_204 / c2s_207).
//  Safety as the warehouse's: the request's item id must be its uid's item; an equipped item is refused.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr int kFee        = 100;
constexpr int kBankItems  = 24;          // the bank's entries (uspm_item_select_by_where: top 24)
constexpr int kBankLimit  = 99999999;    // the bank's gelder
constexpr int kMoneyLimit = 2100000000;  // 0x7D2B7500

// dword_58FB78 (MasterServer): a record's type -> the procedure's flag (0 delete, 1 insert, 2 update).
int db_flag(uint8_t type)
{
    static const int k[9] = { 255, 2, 1, 0, 1, 0, 1, 0, 1 };
    return type < 9 ? k[type] : 255;
}

} // namespace

// The bank's list (DB 4112 OnICmdLoadBank -> 4113, Player::LoadNeoWarehouse sub_761DC0: at most 80 kept)
// and its gelder, then its rare items' options (4106 -> 4107, sub_761B50).
void GameClientSession::bank_load()
{
    bank_.clear();
    bank_weight_ = 0;
    std::vector<InvItem> rows = server_.item_store().LoadBank(account_id_);
    std::unordered_map<uint64_t, std::vector<ItemOption>> opts;
    for (const ItemOptionRow& o : server_.item_store().LoadBankOptions(account_id_))
        opts[o.item_uid].push_back({ o.type, o.index, o.value, o.mixed_item });
    for (const InvItem& r : rows) {
        const ItemDef* d = server_.item_db().get(r.item_id);
        if (!d) {
            LOG_ERROR("GameClientSession %u: bank item %u (uid %llu) is not in the item tables — skipped", session_id(),
                      r.item_id, static_cast<unsigned long long>(r.uid));
            continue;
        }
        BagItem b;
        b.uid     = r.uid;
        b.item_id = r.item_id;
        b.count   = r.amount;
        b.kind    = static_cast<uint8_t>(d->cls);
        if (b.kind == 2) {
            b.rare[0] = r.refine ? static_cast<uint8_t>(d->refine_type) : 0;
            b.rare[1] = r.refine;
            b.rare[2] = r.refine_state;
            b.rare[3] = r.item_slot;
            auto o = opts.find(r.uid);
            if (o != opts.end()) b.opts = o->second;
        }
        bank_.push_back(b);
        bank_weight_ += d->weight * d->item_count(r.amount);
    }
    const DBResult m = server_.db().query("SELECT user_money FROM accounts WHERE account_id=%u", account_id_);
    bank_money_ = m.empty() ? 0 : std::atoi(m[0].at("user_money").c_str());
}

// S2C 196 (sub_5F9990).
void GameClientSession::send_bank()
{
    PacketBuffer p(static_cast<Opcode>(0x00C4), is_paula());
    p.write<uint8_t>(bank_sent_ ? 1 : 0);
    if (!bank_sent_) {
        p.write<uint32_t>(static_cast<uint32_t>(bank_money_));
        p.write<uint16_t>(static_cast<uint16_t>(bank_.size()));
        std::vector<const BagItem*> rare;
        for (const BagItem& b : bank_) {
            p.write<uint8_t>(b.kind);
            write_item_data(p, b.kind, b.item_id, b.uid, b.count, b.rare);
            p.write<uint8_t>(0);                             // no expiry (sub_6EE590 finds none)
            if (b.kind == 2) rare.push_back(&b);            // sub_6ECB50: class 2
        }
        p.write<uint16_t>(static_cast<uint16_t>(rare.size()));
        for (const BagItem* b : rare) {
            p.write<uint64_t>(b->uid);
            p.write<uint8_t>(static_cast<uint8_t>(b->opts.size()));        // sub_761A20: all of them
            for (const ItemOption& o : b->opts) {
                p.write<uint32_t>(o.type);
                p.write<uint32_t>(static_cast<uint32_t>(static_cast<uint16_t>(o.id)) |
                                  (static_cast<uint32_t>(static_cast<uint16_t>(o.value)) << 16));
                p.write<uint32_t>(static_cast<uint32_t>(o.extra));
            }
        }
        bank_sent_ = true;                                   // sub_60CD10(1)
    }
    send(p);
}

// NeoWarehouse_Start (sub_7E3060): 0 opened, 2 under level 10, 3 not enough money.
int GameClientSession::npc_bank_start()
{
    if (level_ < 10) return 2;
    if (money_ < kFee) return 3;
    exit_npc_dialog();                                       // S2C 117 (sub_5F3BA0)
    money_ -= kFee;                                          // sub_73B280(-100) + the money log
    save_position();
    send_money();                                            // S2C 41 (sub_5F4E80)
    bank_state_ = true;                                      // state 11
    if (!bank_sent_) bank_load();                            // DB 4112 -> 4113 -> 4106 -> 4107
    send_bank();
    LOG_DEBUG("GameClientSession %u: bank open (%zu items, %d gelder), money %d", session_id(), bank_.size(),
              bank_money_, money_);
    return 0;
}

// DB 4029 (MasterServer ItemMoneyStore::Exchange sub_463980 -> uspm_item_char_update_move_inven_to_bank):
// from the bag unless the bag's record takes and the bank's gives; a common stack split off gets the
// new uid.
// Hardening (not retail): the bank is the account's and another of its characters may move the same rows — the move
// runs in one transaction, after a check that the bank rows it touches still hold what our cache (reloaded by the
// step) says; the procedure's absolute counts are then right.
bool GameClientSession::bank_db_move(const WhPlace& from, BagRecord& fr, const WhPlace& to, BagRecord& tr)
{
    DBTransaction tx(server_.db());
    auto current = [&](const WhPlace& p, uint64_t uid) {
        if (p.where != 3 || !uid) return true;
        for (const BagItem& b : *p.items) {
            if (b.uid != uid) continue;
            const DBResult r = server_.db().query("SELECT amount FROM char_inventory WHERE uid=%llu AND item_where=3 "
                                                  "AND user_uid=%u FOR UPDATE", static_cast<unsigned long long>(uid),
                                                  account_id_);
            return !r.empty() && std::atoi(r[0].at("amount").c_str()) == static_cast<int>(b.count);
        }
        return true;                                         // (not in the bank: the item that comes)
    };
    if (!current(from, fr.uid) || !current(to, tr.uid)) {
        LOG_INFO("%s : the bank changed meanwhile (another character of the account) - refused", char_name_);
        return false;
    }
    uint64_t new_uid = 0;
    const int err = server_.item_store().MoveBankItem(account_id_, char_uid_, fr.item_id, fr.kind,
                                                      from.where, fr.uid, fr.count, static_cast<uint8_t>(db_flag(fr.type)),
                                                      to.where, tr.uid, tr.count, static_cast<uint8_t>(db_flag(tr.type)),
                                                      new_uid);
    if (err) {
        LOG_ERROR("ItemMoneyStore::Exchange Return failure : chrUid:%u, spRet:%d", char_uid_, err);
        return false;                                        // (the transaction rolls back)
    }
    tx.commit();
    if (fr.kind == 1 && db_flag(fr.type) == 2 && db_flag(tr.type) == 1) tr.uid = new_uid;
    return true;
}

// C2S 198: the bag -> the bank.
void GameClientSession::handle_bank_add(PacketBuffer& pkt)
{
    try {
        WhReq q;
        q.uid  = pkt.read<uint64_t>();
        q.item = pkt.read<uint32_t>();
        q.cnt  = pkt.read<uint16_t>();
        if (!bank_state_ || dead_) return;                   // a message of the state 11 only
        // Hardening (not retail): the account's other characters may have changed the bank since it was read (retail
        // reads it once a login) — each step starts from the DB.
        bank_load();
        auto fail = [&]() {                                  // S2C 200 (__fputchar_39)
            PacketBuffer c(static_cast<Opcode>(0x00C8), is_paula());
            send(c);
        };
        const ItemDef* d = server_.item_db().get(q.item);
        if (!d) { fail(); return; }                          // (the retail reads no ItemParam: refused)
        uint32_t flags = d->options;                         // sub_6ECC20
        if (q.uid && d->cls == 2)
            if (const BagItem* b = bag_find_uid(q.uid))
                for (const ItemOption& o : b->opts) if (o.type == 13) { flags = static_cast<uint32_t>(o.extra); break; }
        if ((flags & 1536) == 1536 || (flags & 0x20000) || (q.item > 6899 && q.item < 7000)) {
            // S2C 269 (__fputchar_40): the KR client has no handler for it — not sent
            LOG_INFO("%s addToWh:%llu:%u:%s:%d - eItemOption_NotAddableToWarehouse", char_name_,
                     static_cast<unsigned long long>(q.uid), q.item, d->name.c_str(), q.cnt);
            return;
        }
        WhPlace bag, bk;
        wh_place(1, bag);
        wh_place(3, bk);
        if (static_cast<int>(bank_.size()) >= kBankItems) {    // sub_6DAF70: it stacks onto one of its own
            bool stacks = false;
            for (const BagItem& b : bank_)
                if (b.item_id == q.item) { stacks = b.kind == 1; break; }
            if (!stacks) { fail(); return; }
        }
        if (!wh_has(bag, q)) {
            LOG_INFO("%s addToNWh:%llu:%u:%s:%d - HasAll == false", char_name_, static_cast<unsigned long long>(q.uid),
                     q.item, d->name.c_str(), q.cnt);
            fail();
            return;
        }
        if (wh_ammo_item(q.item)) { fail(); return; }
        if (q.uid)
            for (uint64_t e : equip_) if (e == q.uid) { fail(); return; }   // (safety: an equipped item)
        for (const BagItem& b : bank_)
            if (q.uid && b.uid == q.uid) {
                LOG_ERROR("[ITEMEX]:hItemAddNeoWareHouse::Send - duplicated uid. player:%s itemId:%u, itemUid:%llu",
                          char_name_, q.item, static_cast<unsigned long long>(q.uid));
                close();
                return;
            }
        BagRecord br, tr;
        if (!wh_take_record(bag, q, br)) {
            LOG_INFO("%s addToNWh:%llu:%u:%s:%d - PrepareToRemoveItem_MakeItemOp failed", char_name_,
                     static_cast<unsigned long long>(q.uid), q.item, d->name.c_str(), q.cnt);
            fail();
            return;
        }
        tr = br;
        tr.type = 0;
        if (tr.kind == 1) tr.count = static_cast<uint16_t>(q.cnt);   // sub_6DD090
        wh_put_record(bk, tr);
        if (tr.kind == 1 && (tr.type == 2 || tr.type == 1) && tr.count > 32000) {
            LOG_INFO("%s addToNwh:%llu:%u:%s:%d - itemcount > 32000", char_name_, static_cast<unsigned long long>(q.uid),
                     q.item, d->name.c_str(), q.cnt);
            fail();
            return;
        }
        if (br.type == 1 && tr.type == 2) tr.uid = 0;       // a part moves: a new item
        for (BagRecord* r : { &br, &tr }) {                 // sub_6DC6C0
            if (r->type == 2) r->type = 8;
            if (r->type == 3) r->type = 7;
        }
        LOG_INFO("%s addToNwh:%llu:%u:%s:%d", char_name_, static_cast<unsigned long long>(q.uid), q.item,
                 d->name.c_str(), q.cnt);
        const bool ok = (db_flag(br.type) != 1 && db_flag(tr.type) != 0) ? bank_db_move(bag, br, bk, tr)
                                                                          : bank_db_move(bk, tr, bag, br);
        if (!ok) {
            LOG_INFO("%s : OCMD_ITEM_ADD_TO_NEOWAREHOUSE_ACK_CANCEL", char_name_);
            fail();
            return;
        }
        // OnOk (sub_77AD30)
        wh_apply(bag, br);
        wh_apply(bk, tr);
        PacketBuffer p(static_cast<Opcode>(0x00C7), is_paula());   // S2C 199 (sub_5F3C00)
        write_bag_record(p, br);
        write_bag_record(p, tr);
        send(p);
    } catch (...) {}
}

// C2S 201: the bank -> the bag.
void GameClientSession::handle_bank_remove(PacketBuffer& pkt)
{
    try {
        WhReq q;
        q.uid  = pkt.read<uint64_t>();
        q.item = pkt.read<uint32_t>();
        q.cnt  = pkt.read<uint16_t>();
        if (!bank_state_ || dead_) return;
        bank_load();                                         // Hardening (not retail): from the DB (handle_bank_add)
        auto fail = [&]() {                                  // S2C 203 (__fputchar_42)
            PacketBuffer c(static_cast<Opcode>(0x00CB), is_paula());
            send(c);
        };
        WhPlace bag, bk;
        wh_place(1, bag);
        wh_place(3, bk);
        const ItemDef* d = server_.item_db().get(q.item);
        if (!d || !wh_has(bk, q)) {
            LOG_INFO("%s removeFromNWh:%llu:%u:%d - HasAll == false", char_name_, static_cast<unsigned long long>(q.uid),
                     q.item, q.cnt);
            fail();
            return;
        }
        if (!wh_can_take(bag, q) || wh_ammo_item(q.item)) { fail(); return; }
        for (const BagItem& b : bag_)
            if (q.uid && b.uid == q.uid) {
                LOG_ERROR("[ITEMEX]:hItemRemoveNeoWareHouse::Send - duplicated uid. player:%s itemId:%u, itemUid:%llu",
                          char_name_, q.item, static_cast<unsigned long long>(q.uid));
                close();
                return;
            }
        BagRecord tr, br;
        if (!wh_take_record(bk, q, tr)) {
            LOG_INFO("%s removeFromNWh:%llu:%u:%s:%d - PrepareToRemoveItem_MakeItemOp failed", char_name_,
                     static_cast<unsigned long long>(q.uid), q.item, d->name.c_str(), q.cnt);
            fail();
            return;
        }
        br = tr;
        br.type = 0;
        if (br.kind == 1) br.count = static_cast<uint16_t>(q.cnt);
        wh_put_record(bag, br);
        if (br.kind == 1 && (br.type == 2 || br.type == 1) && br.count > 32000) {
            LOG_INFO("%s removeFromNWh:%llu:%u:%s:%d - itemcount > 32000", char_name_,
                     static_cast<unsigned long long>(q.uid), q.item, d->name.c_str(), q.cnt);
            fail();
            return;
        }
        if (tr.type == 1 && br.type == 2) br.uid = 0;
        for (BagRecord* r : { &br, &tr }) {
            if (r->type == 2) r->type = 8;
            if (r->type == 3) r->type = 7;
        }
        LOG_INFO("%s removeFromNWh:%llu:%u:%s:%d", char_name_, static_cast<unsigned long long>(q.uid), q.item,
                 d->name.c_str(), q.cnt);
        const bool ok = (db_flag(br.type) != 1 && db_flag(tr.type) != 0) ? bank_db_move(bag, br, bk, tr)
                                                                          : bank_db_move(bk, tr, bag, br);
        if (!ok) {
            LOG_INFO("%s : OCMD_ITEM_REMOVE_FROM_NEOWAREHOUSE_ACK_CANCEL", char_name_);
            fail();
            return;
        }
        // OnOk (sub_77B400)
        wh_apply(bag, br);
        wh_apply(bk, tr);
        PacketBuffer p(static_cast<Opcode>(0x00CA), is_paula());   // S2C 202 (sub_5F3CF0)
        write_bag_record(p, br);
        write_bag_record(p, tr);
        send(p);
    } catch (...) {}
}

// C2S 204 / 207: gelder into / out of the bank.
void GameClientSession::handle_bank_money(PacketBuffer& pkt, bool add)
{
    try {
        const int32_t value = static_cast<int32_t>(pkt.read<uint32_t>());
        if (!bank_state_ || dead_) return;
        auto fail = [&](uint16_t code) {                     // S2C 206 [u16 code] (see above)
            PacketBuffer c(static_cast<Opcode>(0x00CE), is_paula());
            c.write<uint16_t>(code);
            send(c);
        };
        auto refuse = [&](uint16_t why) {                    // S2C 209 [u16] (sub_5F3DE0)
            PacketBuffer c(static_cast<Opcode>(0x00D1), is_paula());
            c.write<uint16_t>(why);
            send(c);
        };
        if (value <= 0) {
            LOG_ERROR("%s : moneyAdd/DelNeoBank is zero or negative", char_name_);
            fail(0);
            return;
        }
        // Hardening (not retail): the account's gelder is shared by its characters, each with a cached copy (retail
        // reads it once a login) — it is re-read here, moves relatively and guarded in one transaction with the
        // character's money, and is read again after.
        auto read_bank = [&]() {
            const DBResult m = server_.db().query("SELECT user_money FROM accounts WHERE account_id=%u", account_id_);
            if (!m.empty()) bank_money_ = std::atoi(m[0].at("user_money").c_str());
        };
        read_bank();
        int32_t money, bank;
        if (add) {
            if (value > money_) {
                LOG_ERROR("%s : hMoneyAddToNeoWarehouse: moneyAddToWh > player.GetMoney()", char_name_);
                fail(1);
                return;
            }
            if (static_cast<int64_t>(value) + bank_money_ > kBankLimit) {
                LOG_INFO("%s : OCMD_MONEY_ADD_TO_WAREHOUSE_ACK_CANCEL - LIMIT EXCEEDED", char_name_);
                fail(2);
                return;
            }
            money = money_ - value;                          // sub_5F09F0: [money - v][bank + v]
            bank  = value + bank_money_;
        } else {
            if (bank_money_ - value < 0) {
                LOG_INFO("%s : OCMD_MONEY_REMOVE_FROM_WAREHOUSE_ACK_CANCEL - LIMIT EXCEEDED", char_name_);
                refuse(0);
                return;
            }
            if (static_cast<uint32_t>(value) + static_cast<uint32_t>(money_) >= static_cast<uint32_t>(kMoneyLimit)) {
                LOG_INFO("%s : USER GELDER + PAYMENT GELDER From WareHouse OVERFLOWS FROM INT_MAX", char_name_);
                refuse(1);
                return;
            }
            money = money_ + value;                          // (the gelder goes as -value)
            bank  = bank_money_ - value;
        }
        // DB 4032 -> uspm_user_info_update_move_money: the character's money (101), the account's (102).
        bool ok;
        {
            DBTransaction tx(server_.db());
            ok = !server_.db().query("SELECT uid FROM characters WHERE uid=%u", char_uid_).empty() &&
                 !server_.db().query("SELECT account_id FROM accounts WHERE account_id=%u", account_id_).empty();
            if (ok)                                          // (the bank keeps what leaves it / stays in its limit)
                ok = (add ? server_.db().execute("UPDATE accounts SET user_money=user_money+%d WHERE account_id=%u AND "
                                                 "user_money+%d<=%d", value, account_id_, value, kBankLimit)
                          : server_.db().execute("UPDATE accounts SET user_money=user_money-%d WHERE account_id=%u AND "
                                                 "user_money>=%d", value, account_id_, value)) == 1;
            if (ok) {
                server_.db().execute("UPDATE characters SET money=%d WHERE uid=%u", money, char_uid_);
                tx.commit();
            }
        }
        if (!ok) {
            LOG_INFO("%s : OCMD_MONEY_%s_WAREHOUSE_ACK_CANCEL", char_name_, add ? "ADD_TO" : "REMOVE_FROM");
            fail(0);
            return;
        }
        // OnOk (sub_77B830 / sub_77BC40): the bank's and the player's gelder, S2C 205 / 208.
        bank_money_ = bank;
        read_bank();                                         // (another character's move shows too)
        money_      = money;
        PacketBuffer p(static_cast<Opcode>(add ? 0x00CD : 0x00D0), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(bank_money_));
        p.write<uint32_t>(static_cast<uint32_t>(money_));
        send(p);
        LOG_INFO("GameClientSession %u: bank %s %d -> bank %d, money %d", session_id(), add ? "deposit" : "withdraw",
                 value, bank_money_, money_);
    } catch (...) {}
}

// C2S 197 (state 11): state 0.
void GameClientSession::handle_bank_close()
{
    bank_state_ = false;
}
