#include "game_server.hpp"
#include "game_session.hpp"
#include "item_wire.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cstring>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
//  The warehouse — 1:1 with the retail GameServer (GameServerRelease), its DB side (MasterServer
//  cItemWarehouseStore) and the KR client.
//
//  The tabs (WHInventory, Player +1804): char_state.wh_slots (the login block +57, which the client
//    reads too: sub_6ABD20 -> sub_596F60) split into tabs of 80 slots with the ids 2, 21..29 (the
//    client's table, sub_596E70; at most 800 slots, sub_6F2BA0). A tab is an item list of capacity 80
//    (sub_6DABE0) read from the DB (uspm_item_select_by_where: by item uid, at most 80 kept,
//    sub_761C40) on the first opening. (WHInventory +112, sub_62B7E0: a weight limit — 2500 up to
//    level 5, else (200 (level - 5) + 2500) / 80 x slots; hItemAddToWarehouse compares against it and
//    jumps on regardless, so the server never enforces it — the client checks it itself.)
//  Warehouse_Start (NpcScript 16 = sub_7E2E30(player, 1)): a warehouse in use (WHUseType +5060) -> 0;
//    tabs still loading (m_SyncFlag 0x800000) -> 0; level < 5 -> 2; money < 50 -> 3 (the script's
//    2nd / 3rd argument); else S2C 117 (the dialog ends), 50 gelder paid (money log 3619) and S2C 41,
//    the state 10, WHUseType 1; every tab's list already sent -> S2C 137 for tab 2 alone (flag 1:
//    the client shows what it has); else each unsent tab is read (DB 4109 -> 4110, its options 4106
//    -> 4107) and goes out as S2C 137 with its list, the loading flag cleared once all are sent.
//    (A character without tabs keeps the loading flag: its later Warehouse_Start answers 0.)
//  S2C 137 (sub_5F9710): [u8 tab][u8 sent] + when not sent: [u16 n]{[u8 kind][item data][u8 0 = no
//    expiry, else 21 bytes]} [u16 m]{[u64 uid][u8 k]{u32 type, u32 id | value << 16, u32 extra}} —
//    every option of each rare item (Player +4764, uspm_item_option_select_by_where; not filtered
//    like the bag records' sub_6EC150). Client sub_6F0960 case 137: the tab's list (sub_58E3D0, the
//    expiry sub_595C10, the options sub_592320), the window (sub_49EC70(16)) once every tab came.
//  State 10 (WarehouseState, Player jumptable 00752483): everything else goes on as anywhere (C2S 64
//    sitting and 0 are swallowed); event 30002 (a hit) -> S2C 138 and state 0, WHUseType kept (only
//    C2S 138 resets it, so the warehouse stays shut until then); C2S 138 (the window closed,
//    c2s_138) -> state 0, WHUseType 0. S2C 138 is not sent: the KR client has no handler for it
//    (sub_6F0960 default -> dispatch_packet_main, no route).
//  C2S 139 [u8 tab][u64 uid][u32 item][u16 cnt] (hItemAddToWarehouse sub_7782A0): cnt < 1, the item's
//    flag 0x10000 (ItemParam Options, or a rare item's type-13 option: eItemOption_NotAddableTo
//    Warehouse), the bag not having it (HasAll sub_50CD80: by uid, else the first of the id, with at
//    least cnt), a full tab (items >= its slots) unless the item stacks onto one of its own, the id
//    of the loaded bullets or the throw item (sub_778800) -> S2C 141 [u8 tab]; no such tab -> nothing.
//    The bag's record (PrepareToRemoveItem sub_6DBF00: 3 = all of it, 1 = what stays) and the tab's
//    (sub_6DB7C0: 1 = onto its stack of that id, else 2 = the item itself; a common stack over 32000
//    -> 141); a part moving to a new place gets a new uid (2 with uid 0), then 2 -> 8 and 3 -> 7
//    (sub_6DC6C0). DB 4026 (uspm_item_char_update_move_inven_to_warehouse) -> OnOk (sub_778860): both
//    records applied, S2C 140 [u8 tab][bag record][tab record]; refused -> 141.
//  C2S 142 (hItemRemoveFromWarehouse sub_778CD0): the same the other way — cnt < 1, no tab, the tab not
//    having it, the bag full (300 items) or too heavy (sub_6DB080: cnt x Weight + its weight > MaxWT),
//    the bullets' / throw item's id -> S2C 144 [u8 tab]; OnOk (sub_779380): S2C 143 [u8 tab][bag
//    record][tab record].
//  C2S 745 [u8 from][u8 to][u64 uid][u32 item][u16 cnt] (hExchangeWarehouseItem sub_779540; the client
//    only moves between tabs): cnt < 1, from == to, an unknown place -> nothing; the source not having
//    it, the destination full (80) or its weight + cnt x Weight over MaxWT (sub_6DB080 on the tab),
//    MakeExchangeItemOp failing, a new entry into a tab whose items reached its slots -> S2C 747; DB
//    4137 -> OnOk (sub_779920): S2C 746 [u8 from][record][u8 to][record]; refused -> 747.
//  The records (client sub_58E540): 1 sets the stack's count, 7 removes the item (its options stay with
//  it), 8 adds it (2 / 4 with a new rare item's options).
//  All VERIFIED in the client (sub_6F0960 cases 137 / 140 / 141 / 143 / 144, 746 / 747 in its second
//  switch; c2s_138 / c2s_139 / c2s_142 / c2s_745).
//  Safety: a request whose item id is not its uid's item is refused like a missing item (the retail
//  trusts the id: its type flags / exchange record would come from it); an equipped item is refused
//  like the loaded bullets (the client never offers one); a rare item moved between tabs keeps its
//  stored count (the retail's sub_6EBAD0 gives a drill its full DrillParam life in memory only — a
//  free repair); a uid-less exchange is refused (the retail would add an item of uid 0). The
//  NeoWarehouse (NpcScript 17, place 3): bank.cpp. The MyCamp warehouse is not done.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr int kTabCapacity = 80;     // sub_6DABE0(tab + 12, 80)
constexpr int kBagCapacity = 300;    // sub_6DABE0(player + 1716, 300)
constexpr int kMaxSlots    = 800;    // sub_6F2BA0
constexpr int kFee         = 50;

// The tab ids (the client's sub_596E70; the server's dword_F57684 table): 2, then 21..29.
uint8_t tab_id_at(size_t i) { return static_cast<uint8_t>(i == 0 ? 2 : 20 + i); }

// dword_58FE18 (MasterServer): a record's type -> the procedure's flag (0 delete, 1 insert, 2 update).
int db_flag(uint8_t type)
{
    static const int k[9] = { 255, 2, 1, 0, 1, 0, 1, 0, 1 };
    return type < 9 ? k[type] : 255;
}

// cItemWarehouseStore::Exchange: the bag (1), the warehouse tabs (2, 21..29).
bool db_place(int w) { return w == 1 || w == 2 || (w >= 21 && w <= 29); }

} // namespace

// sub_6F2BA0: `slots` more slots, filling the last tab to 80 and opening the next ones.
void GameClientSession::wh_build(int slots)
{
    if (slots <= 0 || slots + wh_slots_ > kMaxSlots) return;
    size_t i = wh_tabs_.empty() ? 0 : wh_tabs_.size() - 1;
    for (int left = slots; left > 0; ++i) {
        if (i >= wh_tabs_.size()) {
            WhTab t;
            t.id = tab_id_at(i);
            wh_tabs_.push_back(t);
        }
        WhTab& t = wh_tabs_[i];
        const int add = std::min(left, kTabCapacity - t.slots);
        t.slots += add;
        left -= add;
    }
    wh_slots_ += slots;
}

GameClientSession::WhTab* GameClientSession::wh_tab(int id)
{
    for (WhTab& t : wh_tabs_) if (t.id == id) return &t;
    return nullptr;
}

// sub_75DAF0: 1 = the bag (+1716), 3 = the NeoWarehouse (the bank, +1920; bank.cpp), else a tab's list.
bool GameClientSession::wh_place(int id, WhPlace& p)
{
    if (id == 1) {
        p.items  = &bag_;
        p.weight = &bag_weight_;
        p.cap    = kBagCapacity;
        p.where  = 1;
        p.tab    = nullptr;
        return true;
    }
    if (id == 3) {
        p.items  = &bank_;
        p.weight = &bank_weight_;
        p.cap    = kTabCapacity;                         // sub_6DABE0(player + 1920, 80)
        p.where  = 3;
        p.tab    = nullptr;
        return true;
    }
    WhTab* t = wh_tab(id);
    if (!t) return false;
    p.items  = &t->items;
    p.weight = &t->weight;
    p.cap    = kTabCapacity;
    p.where  = t->id;
    p.tab    = t;
    return true;
}

// DB 4109 (uspm_item_select_by_where, by uid) -> 4110 (Player::LoadWarehouse sub_761C40: at most 80
// kept) and its rare items' options (4106 -> 4107, uspm_item_option_select_by_where).
void GameClientSession::wh_load_tab(WhTab& t)
{
    t.items.clear();
    t.weight = 0;
    std::vector<InvItem> rows = server_.item_store().LoadWhere(char_uid_, t.id);
    if (rows.size() > static_cast<size_t>(kTabCapacity)) {
        LOG_ERROR("[warehouse] Player::LoadWarehouse -- %s : invalid warehouse inventory item cnt (tabId:%d, cnd:%zu).",
                  char_name_, t.id, rows.size());
        rows.resize(kTabCapacity);
    }
    std::unordered_map<uint64_t, std::vector<ItemOption>> opts;
    for (const ItemOptionRow& o : server_.item_store().LoadItemOptions(char_uid_))
        opts[o.item_uid].push_back({ o.type, o.index, o.value, o.mixed_item });
    for (const InvItem& r : rows) {
        const ItemDef* d = server_.item_db().get(r.item_id);
        if (!d) {
            LOG_ERROR("GameClientSession %u: warehouse item %u (uid %llu) is not in the item tables — skipped",
                      session_id(), r.item_id, static_cast<unsigned long long>(r.uid));
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
        t.items.push_back(b);
        t.weight += d->weight * d->item_count(r.amount);
    }
}

// S2C 137 (sub_5F9710).
void GameClientSession::send_warehouse_tab(WhTab& t)
{
    PacketBuffer p(static_cast<Opcode>(0x0089), is_paula());
    p.write<uint8_t>(t.id);
    p.write<uint8_t>(t.sent ? 1 : 0);
    if (!t.sent) {
        p.write<uint16_t>(static_cast<uint16_t>(t.items.size()));
        std::vector<const BagItem*> rare;
        for (const BagItem& b : t.items) {
            p.write<uint8_t>(b.kind);
            write_item_data(p, b.kind, b.item_id, b.uid, b.count, b.rare);
            p.write<uint8_t>(0);                         // no expiry (sub_6EE590 finds none)
            if (b.kind == 2) rare.push_back(&b);        // sub_6ECB50: class 2
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
    }
    send(p);
}

// Warehouse_Start (sub_7E2E30): 0 opened (or nothing to do), 2 under level 5, 3 not enough money.
int GameClientSession::npc_warehouse_start(int type)
{
    if (wh_use_) {
        LOG_INFO("[warehouse] StartWarehouse() -- aleady use warehouse. (playerName:%s, playerUid:%u, curWhUseType:%d)",
                 char_name_, char_uid_, wh_use_);
        return 0;
    }
    if (!type) return 0;
    if (wh_loading_) return 0;
    if (level_ < 5) return 2;
    if (type != 2 && money_ < kFee) return 3;
    if (type == 1) exit_npc_dialog();                    // S2C 117 (sub_5F3BA0)
    if (type != 2) {
        money_ -= kFee;                                  // sub_73B280(-50) + the money log (DB 3619)
        save_position();
    }
    send_money();                                        // S2C 41 (sub_5F4E80)
    wh_state_ = true;                                    // state 10
    wh_use_   = type;
    bool all = !wh_tabs_.empty();                        // sub_6F2AC0
    for (const WhTab& t : wh_tabs_) if (!t.sent) { all = false; break; }
    if (all) {
        if (WhTab* t = wh_tab(2)) send_warehouse_tab(*t);
    } else {
        wh_loading_ = true;
        for (WhTab& t : wh_tabs_) {
            if (t.sent) continue;
            wh_load_tab(t);                              // DB 4109 -> 4110 -> 4106 -> 4107
            send_warehouse_tab(t);                       // 4107: S2C 137, then the tab is sent
            t.sent = true;
        }
        bool done = !wh_tabs_.empty();
        for (const WhTab& t : wh_tabs_) if (!t.sent) { done = false; break; }
        if (done) wh_loading_ = false;
    }
    size_t n = 0;
    for (const WhTab& t : wh_tabs_) n += t.items.size();
    LOG_DEBUG("GameClientSession %u: warehouse open (%zu tabs, %d slots, %zu items), money %d", session_id(),
              wh_tabs_.size(), wh_slots_, n, money_);
    return 0;
}

// HasAll (sub_50CD80): a uid under 2 -> the first item of the id with at least cnt; else that item with
// at least cnt (and, for safety, of the request's id).
// Hardening (not retail): only the item of that uid. "Any of the id" passed on one stack while wh_take_record took
// another (the first of the id) and the move wrote the asked count onto that smaller stack's row (a dupe, warehouse
// and bank alike). The client always sends the item's own uid: uid 0 is refused (1 is a real row here, not a
// "none" — char_inventory.uid counts from 1).
bool GameClientSession::wh_has(const WhPlace& p, const WhReq& q) const
{
    if (q.cnt <= 0 || !q.uid) return false;
    for (const BagItem& b : *p.items) {
        if (b.uid != q.uid) continue;
        const ItemDef* d = server_.item_db().get(b.item_id);
        const int have = d ? d->item_count(b.count) : b.count;
        return b.item_id == q.item && have >= q.cnt;
    }
    return false;
}

// PrepareToRemoveItem_MakeItemOp (sub_6DBF00): the item (by uid, else the first of the id) leaving
// the place — 3 = all of it (a common item's count: what is asked, then what stays, 0), or 1 = what
// stays of a common stack.
bool GameClientSession::wh_take_record(const WhPlace& p, const WhReq& q, BagRecord& r) const
{
    const BagItem* it = nullptr;
    for (const BagItem& b : *p.items)
        if (q.uid ? b.uid == q.uid : b.item_id == q.item) { it = &b; break; }
    if (!it) { r.type = 0; return false; }
    r.type    = 3;
    r.kind    = it->kind;
    r.item_id = it->item_id;
    r.uid     = it->uid;
    r.count   = it->count;
    std::memcpy(r.rare, it->rare, 4);
    r.opts    = it->opts;
    if (r.kind == 1) r.count = static_cast<uint16_t>(q.cnt);           // sub_6DD090
    const ItemDef* qd = server_.item_db().get(q.item);
    if (qd && qd->stackable()) {                                         // sub_6DDD90
        const ItemDef* d = server_.item_db().get(it->item_id);
        const int have = d ? d->item_count(it->count) : it->count;
        if (q.cnt <= have) {
            if (q.cnt < have) r.type = 1;
            r.count = static_cast<uint16_t>(have - r.count);            // sub_6DB9A0
        }
    }
    return true;
}

// sub_6DB7C0: the record of an item (type 0, count = what moves) arriving at the place — 1 onto its
// stack of that id (that stack's uid, the sum; the u16 wraps), else 2 (it has a uid) or 4.
void GameClientSession::wh_put_record(const WhPlace& p, BagRecord& r) const
{
    const ItemDef* d = server_.item_db().get(r.item_id);
    if (d && d->stackable()) {
        for (const BagItem& b : *p.items)
            if (b.item_id == r.item_id) {
                r.type  = 1;
                r.uid   = b.uid;
                r.count = static_cast<uint16_t>(b.count + r.count);    // sub_6DB890
                return;
            }
    }
    r.type = r.uid ? 2 : 4;
}

// sub_6DB080: the place takes `cnt` of the item — fewer items than its capacity and, with a weight,
// cnt x Weight + its weight within MaxWT (the player's, even for a tab).
bool GameClientSession::wh_can_take(const WhPlace& p, const WhReq& q) const
{
    if (static_cast<int>(p.items->size()) >= p.cap) return false;
    const ItemDef* d = server_.item_db().get(q.item);
    const int w = q.cnt * (d ? d->weight : 0);
    return !w || w + *p.weight <= st_maxwt_;
}

// sub_778800: the id of the loaded bullets (Player +4604) or of the throw item (+4608).
bool GameClientSession::wh_ammo_item(uint32_t item_id) const
{
    for (const BagItem& b : bag_) {
        if (mag_.item && b.uid == mag_.item && b.item_id == item_id) return true;
        if (throw_item_ && b.uid == throw_item_ && b.item_id == item_id) return true;
    }
    return false;
}

// The DB part (MasterServer cItemWarehouseStore::Exchange sub_4641D0 ->
// uspm_item_char_update_move_inven_to_warehouse): both places valid and different; the from record's
// item, class, place, uid, count and flag, the to record's; a common stack split off gets the new uid.
bool GameClientSession::wh_db_move(const WhPlace& from, BagRecord& fr, const WhPlace& to, BagRecord& tr)
{
    if (!db_place(from.where) || !db_place(to.where) || from.where == to.where) {
        LOG_ERROR("cItemWarehouseStore::Exchange failure - Invalid storage type!(from:%d, to:%d)", from.where, to.where);
        return false;
    }
    uint64_t new_uid = 0;
    const int err = server_.item_store().MoveItem(char_uid_, fr.item_id, fr.kind,
                                                  from.where, fr.uid, fr.count, static_cast<uint8_t>(db_flag(fr.type)),
                                                  to.where, tr.uid, tr.count, static_cast<uint8_t>(db_flag(tr.type)),
                                                  new_uid);
    if (err) {
        LOG_ERROR("cItemWarehouseStore::Exchange Return failure(spRet:%d) : chrUid:%u, item:%llu, from:%d, to:%d", err,
                  char_uid_, static_cast<unsigned long long>(fr.uid), from.where, to.where);
        return false;
    }
    if (fr.kind == 1 && db_flag(fr.type) == 2 && db_flag(tr.type) == 1) tr.uid = new_uid;
    return true;
}

// sub_6DBA50: a record on a place — 2/6/4/8 add the item (a full list takes nothing, sub_6DB350),
// 1 sets the stack's count, 3/5/7 remove it; the place's weight follows.
void GameClientSession::wh_apply(WhPlace& p, const BagRecord& r)
{
    const ItemDef* d = server_.item_db().get(r.item_id);
    const int32_t w = d ? d->weight : 0;
    if (r.type == 2 || r.type == 6 || r.type == 4 || r.type == 8) {
        if (static_cast<int>(p.items->size()) >= p.cap) return;
        BagItem b;
        b.uid     = r.uid;
        b.item_id = r.item_id;
        b.count   = r.count;
        b.kind    = r.kind;
        std::memcpy(b.rare, r.rare, 4);
        b.opts    = r.opts;
        p.items->push_back(b);
        *p.weight += w * (d ? d->item_count(r.count) : r.count);
        return;
    }
    for (auto it = p.items->begin(); it != p.items->end(); ++it) {
        if (it->uid != r.uid) continue;
        if (r.type == 1) {
            *p.weight -= w * (static_cast<int32_t>(it->count) - static_cast<int32_t>(r.count));
            it->count = r.count;
        } else if (r.type == 3 || r.type == 5 || r.type == 7) {
            *p.weight -= w * (d ? d->item_count(it->count) : static_cast<int32_t>(it->count));
            p.items->erase(it);
        }
        return;
    }
}

// C2S 139: the bag -> a tab.
void GameClientSession::handle_wh_add(PacketBuffer& pkt)
{
    try {
        const uint8_t tab = pkt.read<uint8_t>();
        WhReq q;
        q.uid  = pkt.read<uint64_t>();
        q.item = pkt.read<uint32_t>();
        q.cnt  = pkt.read<uint16_t>();
        if (!wh_state_ || dead_) return;                 // a message of the state 10 only
        auto fail = [&]() {                              // S2C 141 (sub_5F4080)
            PacketBuffer c(static_cast<Opcode>(0x008D), is_paula());
            c.write<uint8_t>(tab);
            send(c);
        };
        if (q.cnt < 1) {
            LOG_INFO("%s addToWh:%llu:%u - Item cnt is %d", char_name_, static_cast<unsigned long long>(q.uid), q.item, q.cnt);
            fail();
            return;
        }
        const ItemDef* d = server_.item_db().get(q.item);
        if (!d) { fail(); return; }
        // sub_6ECC20: a rare item's type-13 option holds its flags, else ItemParam Options.
        uint32_t flags = d->options;
        if (q.uid && d->cls == 2)
            if (const BagItem* b = bag_find_uid(q.uid))
                for (const ItemOption& o : b->opts) if (o.type == 13) { flags = static_cast<uint32_t>(o.extra); break; }
        if (flags & 0x10000) {
            LOG_INFO("%s addToWh:%llu:%u:%s:%d - eItemOption_NotAddableToWarehouse", char_name_,
                     static_cast<unsigned long long>(q.uid), q.item, d->name.c_str(), q.cnt);
            fail();
            return;
        }
        WhPlace bag, wt;
        wh_place(1, bag);
        if (!wh_has(bag, q)) {
            LOG_INFO("%s addToWh:%llu:%u:%s:%d - HasAll == false", char_name_, static_cast<unsigned long long>(q.uid),
                     q.item, d->name.c_str(), q.cnt);
            fail();
            return;
        }
        WhTab* t = wh_tab(tab);
        if (!t || !wh_place(tab, wt)) {
            LOG_INFO("[warehouse] hItemAddToWarehouse::Send() - can not found warehouse tab. (name:%s, tabId:%d, itemTypeId:%u)",
                     char_name_, tab, q.item);
            return;
        }
        if (static_cast<int>(t->items.size()) >= t->slots) {           // full, unless it stacks (sub_6DAF70)
            bool stacks = false;
            for (const BagItem& b : t->items)
                if (b.item_id == q.item) { stacks = b.kind == 1; break; }
            if (!stacks) { fail(); return; }
        }
        // (the weight limit sub_62B7E0 is compared here and ignored)
        if (wh_ammo_item(q.item)) { fail(); return; }
        if (q.uid)
            for (uint64_t e : equip_) if (e == q.uid) { fail(); return; }   // (safety: an equipped item)
        for (const BagItem& b : t->items)
            if (b.uid == q.uid && q.uid) {
                LOG_ERROR("[ITEMEX]:hItemAddWareHouse::Send - duplicated uid. player:%s itemId:%u, itemUid:%llu",
                          char_name_, q.item, static_cast<unsigned long long>(q.uid));
                close();
                return;
            }
        BagRecord br, tr;
        if (!wh_take_record(bag, q, br)) {
            LOG_INFO("%s addToWh:%llu:%u:%s:%d - PrepareToRemoveItem_MakeItemOp failed", char_name_,
                     static_cast<unsigned long long>(q.uid), q.item, d->name.c_str(), q.cnt);
            fail();
            return;
        }
        tr = br;
        tr.type = 0;
        if (tr.kind == 1) tr.count = static_cast<uint16_t>(q.cnt);   // sub_6DD090
        wh_put_record(wt, tr);
        if (tr.kind == 1 && (tr.type == 2 || tr.type == 1) && tr.count > 32000) {
            LOG_INFO("%s addToWh:%llu:%u:%s:%d - itemcount > 32000", char_name_, static_cast<unsigned long long>(q.uid),
                     q.item, d->name.c_str(), q.cnt);
            fail();
            return;
        }
        if (br.type == 1 && tr.type == 2) tr.uid = 0;   // a part moves: a new item
        for (BagRecord* r : { &br, &tr }) {             // sub_6DC6C0
            if (r->type == 2) r->type = 8;
            if (r->type == 3) r->type = 7;
        }
        LOG_DEBUG("%s addToWh:%d:%llu:%u:%s:%d", char_name_, tab, static_cast<unsigned long long>(q.uid), q.item,
                  d->name.c_str(), q.cnt);
        // DB 4026 (sub_464160: from the bag unless the bag takes and the tab gives)
        const bool ok = (db_flag(br.type) != 1 && db_flag(tr.type) != 0) ? wh_db_move(bag, br, wt, tr)
                                                                          : wh_db_move(wt, tr, bag, br);
        if (!ok) {
            LOG_INFO("%s : OCMD_ITEM_ADD_TO_WAREHOUSE_ACK_CANCEL", char_name_);
            fail();
            return;
        }
        // OnOk (sub_778860)
        wh_apply(bag, br);
        wh_apply(wt, tr);
        PacketBuffer p(static_cast<Opcode>(0x008C), is_paula());       // S2C 140 (sub_5F3F40)
        p.write<uint8_t>(tab);
        write_bag_record(p, br);
        write_bag_record(p, tr);
        send(p);
    } catch (...) {}
}

// C2S 142: a tab -> the bag.
void GameClientSession::handle_wh_remove(PacketBuffer& pkt)
{
    try {
        const uint8_t tab = pkt.read<uint8_t>();
        WhReq q;
        q.uid  = pkt.read<uint64_t>();
        q.item = pkt.read<uint32_t>();
        q.cnt  = pkt.read<uint16_t>();
        if (!wh_state_ || dead_) return;
        auto fail = [&]() {                              // S2C 144 (sub_5F40C0)
            PacketBuffer c(static_cast<Opcode>(0x0090), is_paula());
            c.write<uint8_t>(tab);
            send(c);
        };
        if (q.cnt < 1) {
            LOG_INFO("[warehouse] hItemRemoveFromWarehouse::Send() -- item cnt is %d. (playerName:%s)", q.cnt, char_name_);
            fail();
            return;
        }
        WhPlace bag, wt;
        wh_place(1, bag);
        if (!wh_tab(tab) || !wh_place(tab, wt)) {        // sub_6F2890: the tabs only
            LOG_INFO("[warehouse] hItemRemoveFromWarehouse::Send() -- not found tab. (playerName:%s, tabId:%d)", char_name_, tab);
            fail();
            return;
        }
        const ItemDef* d = server_.item_db().get(q.item);
        if (!d || !wh_has(wt, q)) {
            LOG_INFO("[warehouse] %s removeFromWh:%llu:%u:%d - HasAll == false", char_name_,
                     static_cast<unsigned long long>(q.uid), q.item, q.cnt);
            fail();
            return;
        }
        if (!wh_can_take(bag, q) || wh_ammo_item(q.item)) { fail(); return; }
        for (const BagItem& b : bag_)
            if (b.uid == q.uid && q.uid) {
                LOG_ERROR("[ITEMEX]:hItemRemoveWareHouse::Send - duplicated uid. player:%s itemId:%u, itemUid:%llu",
                          char_name_, q.item, static_cast<unsigned long long>(q.uid));
                close();
                return;
            }
        BagRecord tr, br;
        if (!wh_take_record(wt, q, tr)) {
            LOG_INFO("%s removeFromWh:%llu:%u:%s:%d - PrepareToRemoveItem_MakeItemOp failed", char_name_,
                     static_cast<unsigned long long>(q.uid), q.item, d->name.c_str(), q.cnt);
            fail();
            return;
        }
        br = tr;
        br.type = 0;
        if (br.kind == 1) br.count = static_cast<uint16_t>(q.cnt);
        wh_put_record(bag, br);
        if (br.kind == 1 && (br.type == 2 || br.type == 1) && br.count > 32000) {
            LOG_INFO("%s removeFromWh:%llu:%u:%s:%d - itemcount > 32000", char_name_,
                     static_cast<unsigned long long>(q.uid), q.item, d->name.c_str(), q.cnt);
            fail();
            return;
        }
        if (tr.type == 1 && br.type == 2) br.uid = 0;
        for (BagRecord* r : { &br, &tr }) {
            if (r->type == 2) r->type = 8;
            if (r->type == 3) r->type = 7;
        }
        LOG_DEBUG("%s removeFromWh:%d:%llu:%u:%s:%d", char_name_, tab, static_cast<unsigned long long>(q.uid), q.item,
                  d->name.c_str(), q.cnt);
        const bool ok = (db_flag(br.type) != 1 && db_flag(tr.type) != 0) ? wh_db_move(bag, br, wt, tr)
                                                                          : wh_db_move(wt, tr, bag, br);
        if (!ok) {
            LOG_INFO("%s : OCMD_ITEM_REMOVE_FROM_WAREHOUSE_ACK_CANCEL", char_name_);
            fail();
            return;
        }
        // OnOk (sub_779380)
        wh_apply(bag, br);
        wh_apply(wt, tr);
        PacketBuffer p(static_cast<Opcode>(0x008F), is_paula());       // S2C 143 (sub_5F3FE0)
        p.write<uint8_t>(tab);
        write_bag_record(p, br);
        write_bag_record(p, tr);
        send(p);
    } catch (...) {}
}

// C2S 745: one place -> another (the client: tab -> tab).
void GameClientSession::handle_wh_exchange(PacketBuffer& pkt)
{
    try {
        const uint8_t from_id = pkt.read<uint8_t>();
        const uint8_t to_id   = pkt.read<uint8_t>();
        WhReq q;
        q.uid  = pkt.read<uint64_t>();
        q.item = pkt.read<uint32_t>();
        q.cnt  = pkt.read<uint16_t>();
        if (!wh_state_ || dead_) return;
        auto fail = [&]() {                              // S2C 747 (sub_5F08F0)
            PacketBuffer c(static_cast<Opcode>(0x02EB), is_paula());
            send(c);
        };
        if (q.cnt < 1) {
            LOG_INFO("[warehouse] hExchangeWarehouseItem::Send() -- item cnt is %d (playerName:%s, from:%d, to:%d)", q.cnt,
                     char_name_, from_id, to_id);
            return;
        }
        if (from_id == to_id) return;
        WhPlace from, to;
        // Hardening (not retail): C2S 745 moves between warehouse tabs only (what the client sends) — the bag (1) or the
        // bank (3) here skipped their own commands' checks (an equipped item, the ammo, the flags, the bank's guards).
        if (from_id == 1 || from_id == 3 || to_id == 1 || to_id == 3 || !wh_place(from_id, from) || !wh_place(to_id, to)) {
            LOG_INFO("[warehouse] hExchangeWarehouseItem::Send() -- invalid storage id. (playerName:%s, from:%d, to:%d)",
                     char_name_, from_id, to_id);
            return;
        }
        if (!wh_has(from, q)) { fail(); return; }
        if (!wh_can_take(to, q)) { fail(); return; }
        // MakeExchangeItemOp (sub_6E08F0): the source's record, the destination's (sub_6DBBC0: the
        // request's class and item via sub_6EBAD0 — a common item's count, a rare item's four bytes
        // from its source; 1 onto a stack of the id, else 2, 4 without a uid; it never fails, so no
        // 32000 check bites).
        BagRecord fr, tr;
        if (!wh_take_record(from, q, fr)) { fail(); return; }
        const ItemDef* qd = server_.item_db().get(q.item);
        if (!qd || !q.uid) { fail(); return; }           // (safety: no record of an unknown / uid-less item)
        tr.kind    = static_cast<uint8_t>(qd->cls);
        tr.item_id = q.item;
        tr.uid     = q.uid;
        tr.count   = static_cast<uint16_t>(q.cnt);
        tr.type    = 0;
        if (qd->stackable()) {
            wh_put_record(to, tr);
        } else {
            tr.type = 2;
            if (tr.kind == 2) std::memcpy(tr.rare, fr.rare, 4);
            // sub_6EBAD0 gives a rare item the asked count — a drill (Type 6) its DrillParam full life —
            // while the DB keeps the stored one (class 2 only changes its place): a free repair. Kept
            // as stored instead (safety).
            tr.count = fr.count;
        }
        tr.opts = fr.opts;                               // (its options go with it)
        if (fr.type == 1 && tr.type == 2) tr.uid = 0;
        for (BagRecord* r : { &fr, &tr }) {
            if (r->type == 2) r->type = 8;
            if (r->type == 3) r->type = 7;
        }
        // a new entry needs a free slot of the destination tab (no tab there: the retail dereferences
        // nothing — refused instead)
        if (tr.type == 2 || tr.type == 6 || tr.type == 8) {
            if (!to.tab || static_cast<int>(to.items->size()) >= to.tab->slots) { fail(); return; }
        }
        if (!wh_db_move(from, fr, to, tr)) {
            LOG_ERROR("[warehouse] hExchangeWarehouseItem::Send()::SendICmd_Item_Exchange() failure. (playerName:%s, "
                      "storageId:%d, itemId:%u, itemCnt:%d)", char_name_, to_id, q.item, q.cnt);
            fail();
            return;
        }
        // OnOk (sub_779920)
        wh_apply(from, fr);
        wh_apply(to, tr);
        PacketBuffer p(static_cast<Opcode>(0x02EA), is_paula());       // S2C 746 (sub_5F0850)
        p.write<uint8_t>(from_id);
        write_bag_record(p, fr);
        p.write<uint8_t>(to_id);
        write_bag_record(p, tr);
        send(p);
        LOG_DEBUG("[warehouse] hExchangeWarehouseItem::OnOk() -- all ok. (playerName:%s, from:%d, to:%d, itemId:%u, itemCnt:%d)",
                  char_name_, from_id, to_id, q.item, q.cnt);
    } catch (...) {}
}

// C2S 138 (state 10): state 0, WHUseType 0. Nothing goes back.
void GameClientSession::handle_wh_close()
{
    if (!wh_state_) return;
    wh_state_ = false;
    wh_use_   = 0;
}

// State 10, event 30002 (a hit): S2C 138 (not sent — no client handler) and state 0; WHUseType stays.
// State 11 (the bank) the same with S2C 197 (no client handler either).
void GameClientSession::wh_hit()
{
    if (bank_state_) {
        bank_state_ = false;
        LOG_DEBUG("GameClientSession %u: hit while in the bank -> state 0", session_id());
    }
    if (!wh_state_) return;
    wh_state_ = false;
    LOG_DEBUG("GameClientSession %u: hit while in the warehouse -> state 0 (WHUseType %d kept)", session_id(), wh_use_);
}

// Another state replaces the state 10 (an attack, a pick-up, a dig, a talk, death): WHUseType stays.
// So for the bank's state 11.
void GameClientSession::wh_leave_state()
{
    wh_state_   = false;
    bank_state_ = false;
}
