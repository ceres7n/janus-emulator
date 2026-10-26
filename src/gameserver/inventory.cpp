#include "game_session.hpp"
#include "game_server.hpp"
#include "combat_util.hpp"
#include "item_option.hpp"
#include "item_wire.hpp"
#include "logger.hpp"
#include <cmath>
#include <cstring>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
//  Inventory and picking items up — retail GameServer, verified against the KR client.
//
//  The bag (Player +1716) is a plain list of up to 300 items keyed by uid (sub_6DABE0(…, 300));
//  the client lays them out itself. At login it is S2C 0x00E7 (sub_5F69D0 / client sub_6E23D0).
//
//  Pick-up, Player state machine sub_73D2A0:
//    C2S 0x0021 [u16 item][u8 moveType] (case 33): stop the running action, find the item; an event
//      item (+149) only for a player allowed to see it (+1628), an item with ItemParam Options 0x800
//      only for the actor that dropped it. Item and player cells (pos/16) more than 4 apart
//      (sqrtf of the squared cell distance, sub_42F1B0): walk to a free cell near the item (spiral
//      sub_422200, inner 3 / outer 7) or onto it — S2C 0x0018 — and pick it up when the move ends
//      (the move strategy posts 30008, sub_42C330). Near: S2C 0x0019 halt and pick it up at once.
//    state 4, event 30008: still within 4 cells; an owned item (+184, its first 30 s) only for its
//      owner (or a party member sharing loot); then
//      gelder (ItemParam Type 30): money + amount must stay within 0..2,100,000,000 (sub_765960;
//        else S2C 0x0355 [u16 1] "Word_PickMoney_Over"); money += amount (sub_73B280);
//        S2C 0x0022 to the area (count 1), S2C 0x0029 [u32 money], the item leaves (0x000F);
//      any other item: the weight allows Player::GetAcceptableCount (sub_73BA10) and the bag takes
//        it (sub_6DAEA0) -> the DB stores it (hItemPickUp) -> Player::OnPickupItem (sub_73B2B0):
//        bag add, S2C 0x0022 to the area, S2C 0x00A0 to us, the item leaves (0x000F) or keeps
//        the rest; else S2C 0x0023 ("OverLimitedWt").
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr size_t  kBagCapacity     = 300;          // sub_6DABE0(this + 1716, 300)
constexpr int64_t kMaxMoney        = 2100000000;   // sub_765960: 0x7D2B7500
constexpr double  kPickupCellRange = 4.0;          // dbl_988970

// sub_422100 + sub_42F1B0: the distance in 16-unit cells (C int16 division), sqrtf of the squared one.
float cell_distance(int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
    const int dx = static_cast<int16_t>(bx) / 16 - static_cast<int16_t>(ax) / 16;
    const int dy = static_cast<int16_t>(by) / 16 - static_cast<int16_t>(ay) / 16;
    return std::sqrt(static_cast<float>(dy * dy + dx * dx));
}

} // namespace

void GameClientSession::load_inventory()
{
    bag_.clear();
    bag_weight_ = 0;
    for (uint64_t& e : equip_) e = 0;
    mag_ = Magazine{};                                     // Player::Initialize: sub_76CCA0(+4596), +4608 = 0
    throw_item_ = 0;
    // The rare items' options (uspm_item_option_select_by_where), by item uid.
    std::unordered_map<uint64_t, std::vector<ItemOption>> opts;
    for (const ItemOptionRow& o : server_.item_store().LoadItemOptions(char_uid_))
        opts[o.item_uid].push_back({ o.type, o.index, o.value, o.mixed_item });
    for (const InvItem& r : server_.item_store().LoadInventory(char_uid_)) {
        const ItemDef* d = server_.item_db().get(r.item_id);
        if (!d) {
            LOG_ERROR("GameClientSession %u: inventory item %u (uid %llu) is not in the item tables — skipped",
                      session_id(), r.item_id, static_cast<unsigned long long>(r.uid));
            continue;
        }
        if (bag_.size() >= kBagCapacity) break;
        BagItem b;
        b.uid     = r.uid;
        b.item_id = r.item_id;
        b.count   = r.amount;
        b.kind    = static_cast<uint8_t>(d->cls);
        if (b.kind == 2) {
            b.rare[0] = r.refine ? static_cast<uint8_t>(d->refine_type) : 0;   // rf_type (byte +48)
            b.rare[1] = r.refine;                          // tbl_item.refine_level (byte +49)
            b.rare[2] = r.refine_state;                    // tbl_item.refine_state (byte +50)
            b.rare[3] = r.item_slot;                       // tbl_item.item_slot: the compound slots
            auto o = opts.find(r.uid);
            if (o != opts.end()) b.opts = o->second;
        }
        bag_.push_back(b);
        bag_weight_ += d->weight * d->item_count(r.amount);      // (a drill's amount is its life)
        if (r.equip_slot == 20) {
            // Player::SetEquipedItems (sub_75DB60), the common slot: a Throw item (Type 16) or a throw
            // card (Type 11 SubType 60) -> +4608; bullets (Type 29) -> the magazine {1, count, stack}.
            if (d->type == 16 || (d->type == 11 && d->sub_type == 60)) throw_item_ = r.uid;
            else if (d->type == 29) { mag_.per_shot = 1; mag_.loaded = r.amount; mag_.item = r.uid; }
        } else if (r.equip_slot >= 0 && r.equip_slot < 23) {
            equip_[r.equip_slot] = r.uid;                  // item_where 0, item_pos
            // the equipped items' effects (sub_75DEA0 -> the helper's vtbl+12): a weapon's
            // (WeaponItemHelper sub_6E70B0) also restarts the shot timer (sub_765480)
            if (weapon_helper_type(d->type)) last_shot_tick_ = tick_ms();
        }
    }
    LOG_INFO("GameClientSession %u: inventory %zu item(s), weight %d", session_id(), bag_.size(), bag_weight_);
}

// S2C 0x00E7 (sub_5F69D0): [u16 count] + per item [u8 kind][item data] (+ the extra info of a rare
// item). Client sub_6E23D0 -> GameWorld+10568, the avatar's inventory.
void GameClientSession::send_item_list()
{
    PacketBuffer p(static_cast<Opcode>(0x00E7), is_paula());
    p.write<uint16_t>(static_cast<uint16_t>(bag_.size()));
    for (const BagItem& b : bag_) {
        p.write<uint8_t>(b.kind);
        write_item_data(p, b.kind, b.item_id, b.uid, b.count, b.rare);
        if (b.kind == 2) write_rare_extra(p, server_.item_db().get(b.item_id), b.opts);
    }
    send(p);
}

void GameClientSession::send_money()
{
    // S2C 0x0029 [u32 money] (sub_5F4E80) — the avatar's money (client sub_6B4E00 case 0x29: +26704).
    PacketBuffer p(static_cast<Opcode>(0x0029), is_paula());
    p.write<uint32_t>(static_cast<uint32_t>(money_));
    send(p);
}

// Player::GetAcceptableCount (sub_73BA10): how many of `count` the free weight (MaxWT - weight) takes.
int GameClientSession::acceptable_count(const ItemDef& d, int count) const
{
    const int free = st_maxwt_ - bag_weight_;
    if (free <= 0) return 0;
    const int w = d.weight;
    if (w < 0) {
        LOG_ERROR("%s : Player::GetAcceptableCount - itemwt < 0, item %u,%s", char_name_, d.id, d.name.c_str());
        return 0;
    }
    if (w > free) return 0;
    const int n = (w > 0) ? free / w : count;
    return n > count ? count : n;
}

// The bag takes the item (sub_6DAEA0): a common item joins the first item of the same id (sub_6DCC50 +
// vtbl+88 sub_6DE4D0) if its whole amount fits the weight; otherwise it needs a free place (fewer
// than 300 items, sub_6DAFC0) and the weight.
bool GameClientSession::can_add_item(const ItemDef& d, int count) const
{
    const int add = count * d.weight;
    if (d.stackable())
        for (const BagItem& b : bag_)
            if (b.item_id == d.id) return add + bag_weight_ <= st_maxwt_;
    if (bag_.size() >= kBagCapacity) return false;
    return add == 0 || add + bag_weight_ <= st_maxwt_;
}

void GameClientSession::handle_pickup(PacketBuffer& pkt)
{
    try {
        const uint16_t item_oid = pkt.read<uint16_t>();
        const uint8_t  mt       = pkt.read<uint8_t>();
        if (dead_) return;
        if (rest_state_) return;                          // sitting / resting (states 8 / 15) swallow it
        if (exchange_) return;                            // so does the exchange shop state 18
        if (compound_) return;                            // and the compound state 13
        if (pcond_test(128)) return;                      // case 33: the condition 128 ignores it
        head_notice_end();                                // the conditions 2 / 3 / 8 end (megaphone.cpp)
        end_attack(true);                                 // the running action strategy stops (sub_41C5D0)
        pick_ = Pickup{};
        drill_ = Drill{};                                 // (state 4 replaces the drill state 3,
        wh_leave_state();                                 //  the warehouse state 10
        skill_shop_leave();                               //  and the skill master state 25)

        GroundItemEntity g;
        if (!server_.entity_mgr().get_ground_item(item_oid, g) || g.zone != zone_id_) return;
        if (g.event_flag && !third_eye_) return;           // a shadow item (+149) needs the Third Eye (+1628)
        const ItemDef* d = server_.item_db().get(g.item_id);
        if (d && (d->options & 0x800) && g.dropper != static_cast<uint16_t>(object_id_)) return;

        const auto now = std::chrono::steady_clock::now();
        int32_t px, py;
        cur_pos(now, px, py);
        const uint16_t self = static_cast<uint16_t>(object_id_);

        if (std::fabs(cell_distance(g.pos_x, g.pos_y, px, py)) > kPickupCellRange) {
            // Too far: a free cell near the item (or the item itself), then Player vtbl+100 MoveTo
            // with the request's move type and S2C 0x0018 (sub_5F5240: [id][cur][dest][type]).
            int32_t tx = g.pos_x, ty = g.pos_y;
            if (const MapGrid* grid = server_.zone_grid(zone_id_)) {
                int16_t cx = static_cast<int16_t>(static_cast<int16_t>(g.pos_x) / 16);
                int16_t cy = static_cast<int16_t>(static_cast<int16_t>(g.pos_y) / 16);
                if (grid->find_drop_cell(3, 7, cx, cy, {})) { tx = 16 * cx + 8; ty = 16 * cy + 8; }
            }
            const uint8_t mtype = ((mt & 7) == 0) ? 2 : static_cast<uint8_t>(mt & 7);
            apply_move_type_speed(mtype);
            motion_.start(px, py, tx, ty, cur_speed_, now, mtype);
            pos_x_ = tx; pos_y_ = ty;
            if (auto* p = server_.entity_mgr().get_player(object_id_)) { p->pos_x = tx; p->pos_y = ty; }
            pick_.active = true;
            pick_.item   = item_oid;
            broadcast_move(self, px, py, tx, ty, mt);
            ensure_world_tick();
            LOG_DEBUG("GameClientSession %u: pick-up item %u: walk (%d,%d) -> (%d,%d)", session_id(), item_oid,
                      px, py, tx, ty);
            return;
        }
        // Near: S2C 0x0019 halt [u16 id][u16 x][u16 y][u8 moveType] (sub_5F52F0) and the pick-up now
        // (state 4 + event 30008 to ourselves).
        motion_.rest(px, py);
        pos_x_ = px; pos_y_ = py;
        if (auto* p = server_.entity_mgr().get_player(object_id_)) { p->pos_x = px; p->pos_y = py; }
        PacketBuffer h(static_cast<Opcode>(0x0019), is_paula());
        h.write<uint16_t>(self);
        h.write<uint16_t>(static_cast<uint16_t>(px));
        h.write<uint16_t>(static_cast<uint16_t>(py));
        h.write<uint8_t>(mt);
        server_.broadcast_zone(zone_id_, h, 0);
        pick_.active = true;
        pick_.item   = item_oid;
        do_pickup();
    } catch (...) {}
}

// The walk to the item ended: the move strategy posts 30008 (sub_42C330).
void GameClientSession::pickup_tick(std::chrono::steady_clock::time_point now)
{
    if (motion_.moving(now)) return;
    do_pickup();
}

void GameClientSession::do_pickup()
{
    const uint16_t oid = pick_.item;
    pick_ = Pickup{};                                     // leaving state 4 clears the target (event 3)
    if (dead_ || !oid) return;

    GroundItemEntity g;
    if (!server_.entity_mgr().get_ground_item(oid, g) || g.zone != zone_id_) return;
    int32_t px, py;
    cur_pos(std::chrono::steady_clock::now(), px, py);
    if (std::fabs(cell_distance(g.pos_x, g.pos_y, px, py)) > kPickupCellRange) return;
    // An owned item (state 1) whose owner is still here (Player sub_73D2A0 @0x749725 / @0x75053F) is for the
    // owner — or a member of its party's share group when the party shares items (the owner's
    // PartyMemberJudge sub_716EF0 + PartyShareItemJudge sub_760CD0, S2C 7056). An owner gone frees it.
    if (g.owned && g.owner_oid != object_id_) {
        const PlayerEntity* op = server_.entity_mgr().get_player(g.owner_oid);
        if (op && !server_.party().loot_shared(op->char_uid, char_uid_)) return;
    }
    const ItemDef* d = server_.item_db().get(g.item_id);
    if (!d) return;
    const uint16_t self = static_cast<uint16_t>(object_id_);

    // S2C 0x0022 to the area (sub_5F5580): [u16 picker][u16 item][u8 kind][item data] — the ground
    // item's data with `count` for a common item. Client: Character sub_6BEC50 case 0x22 -> vtbl+228
    // (the pick-up effect; the avatar also writes "Result_GetItem" / "Result_GetGelder").
    auto pickup_packet = [&](uint16_t count) {
        PacketBuffer p(static_cast<Opcode>(0x0022), is_paula());
        p.write<uint16_t>(self);
        p.write<uint16_t>(oid);
        p.write<uint8_t>(g.kind);
        write_item_data(p, g.kind, g.item_id, g.uid, (g.kind == 1) ? count : g.count, g.rare);
        server_.broadcast_zone(zone_id_, p, 0);
    };

    if (d->type == 30) {
        // Gelder -> money.
        const int64_t total = static_cast<int64_t>(money_) + g.count;
        if (total < 0 || total > kMaxMoney) {
            LOG_DEBUG("%s Cannot Pickup Money : Money Is Full", char_name_);
            PacketBuffer p(static_cast<Opcode>(0x0355), is_paula());   // sub_6043D0(this, 1)
            p.write<uint16_t>(1);
            send(p);
            return;
        }
        bool removed = false;
        if (!server_.entity_mgr().take_ground_item(oid, g.count, removed)) return;
        money_ = static_cast<int32_t>(total);
        pet_speech(13);                                    // (@0x750652) our pet's line
        pickup_packet(1);                                  // sub_5F5580(this, item, 1)
        send_money();
        if (removed) server_.ground_item_gone(zone_id_, oid);
        save_position();                                   // the money is saved at once (DB message 3619)
        LOG_DEBUG("GameClientSession %u: picked up %u gelder -> money %d", session_id(), g.count, money_);
        return;
    }

    // (the item's count, vtbl+48: a rare item — a drill with its life too — is one)
    const int have = d->item_count(g.count);
    const int cnt = acceptable_count(*d, have);
    if (cnt <= 0 || !can_add_item(*d, have)) {
        PacketBuffer p(static_cast<Opcode>(0x0023), is_paula());       // sub_5F3970: "OverLimitedWt"
        send(p);
        LOG_DEBUG("GameClientSession %u: cannot pick up item %u (%u x%u): weight %d/%d, %zu items", session_id(),
                  oid, g.item_id, g.count, bag_weight_, st_maxwt_, bag_.size());
        return;
    }
    bool removed = false;
    if (!server_.entity_mgr().take_ground_item(oid, d->stackable() ? static_cast<uint16_t>(cnt) : g.count,
                                               removed)) return;

    // hItemPickUp -> Player::OnPickupItem (sub_73B2B0): the bag takes it, S2C 0x0022 to the area,
    // S2C 0x00A0 to us, then the item leaves the world (or keeps the rest).
    const BagRecord rec = bag_add(*d, (g.kind == 1) ? static_cast<uint16_t>(cnt) : g.count, &g);   // sub_6DD090
    pet_speech(13);       // our pet's line: the DB's pick-up kind 0 (1 unique -> 21, 2 -> 20: not modelled)
    pickup_packet(static_cast<uint16_t>(cnt));
    send_bag_record(rec);
    if (removed) server_.ground_item_gone(zone_id_, oid);
    LOG_DEBUG("GameClientSession %u: picked up item %u x%d (%s) -> uid %llu (%s), weight %d/%d", session_id(),
              g.item_id, cnt, d->name.c_str(), static_cast<unsigned long long>(rec.uid),
              rec.type == 1 ? "stack" : "new", bag_weight_, st_maxwt_);
}

// The bag record of hItemPickUp (sub_771C60 -> sub_6DB7C0) and its effect (sub_6DBA50): a common item
// joins the first item of the same id (type 1: that item's uid with the new total, sub_6DB890 — the
// u16 wraps like the retail); an item that still has its uid (a rare item a player threw away) is
// type 2 — its DB row gets the picker as its owner (uspm_item_update_uid_item_rare drop_flag 1) and it
// keeps its bytes and options; anything else is a new item that gets its uid from the DB (type 4).
// A new rare item rolls its compound slots when the record is made (sub_6EC2B0: the MixSlot curve of
// RandomStatValueTable, else ItemParam CompoundSlot) and its options before the DB insert
// (MakeItemDefaultStatHelper sub_6E0490 -> sub_6E02D0: its base stats, re-rolled when OptionsEx & 1),
// stored with it (uspm_item_insert_get_item_rare).
GameClientSession::BagRecord GameClientSession::bag_add(const ItemDef& d, uint16_t count, const GroundItemEntity* from)
{
    BagItem* stack = nullptr;
    if (d.stackable())
        for (BagItem& b : bag_)
            if (b.item_id == d.id) { stack = &b; break; }
    if (!stack) return bag_new(d, count, from);
    BagRecord r;
    r.kind    = static_cast<uint8_t>(d.cls);
    r.item_id = d.id;
    r.type    = 1;
    r.uid     = stack->uid;
    r.count   = static_cast<uint16_t>(stack->count + count);
    stack->count = r.count;
    server_.item_store().SetAmount(stack->uid, r.count);
    bag_weight_ += d.weight * d.item_count(count);
    return r;
}

// The new item of a record (type 2: it keeps the uid it had on the ground, type 4: the DB gives it one;
// a new rare item rolls its slots and options first, sub_6EBAD0 / sub_6E0490) — it joins the bag.
GameClientSession::BagRecord GameClientSession::bag_new(const ItemDef& d, uint16_t count, const GroundItemEntity* from)
{
    BagRecord r;
    r.kind    = static_cast<uint8_t>(d.cls);
    r.item_id = d.id;
    BagItem b;
    if (from && from->uid) {
        r.type  = 2;
        r.uid   = from->uid;
        std::memcpy(r.rare, from->rare, 4);
        r.opts  = from->opts;
        server_.item_store().SetOwner(r.uid, char_uid_);
    } else {
        r.type  = 4;
        if (d.cls == 2) {
            int slots = server_.random_stat_db().roll_mix_slot(d.id);           // sub_630B90
            if (static_cast<uint8_t>(slots) == 127) slots = d.compound_slot;
            r.rare[3] = static_cast<uint8_t>(slots);
            r.opts = make_item_options(d, server_.random_stat_db(), (d.options_ex & 1) != 0);
            std::vector<ItemOptionRow> rows;
            for (const ItemOption& o : r.opts)
                rows.push_back({ 0, static_cast<uint8_t>(o.type), o.id, o.value, o.extra });
            r.uid = server_.item_store().AddRareItem(char_uid_, d.id, count, r.rare[3], rows);
        } else {
            r.uid = server_.item_store().AddItem(char_uid_, d.id, count);
        }
    }
    r.count   = count;
    b.uid     = r.uid;
    b.item_id = d.id;
    b.count   = count;
    b.kind    = r.kind;
    std::memcpy(b.rare, r.rare, 4);
    b.opts    = r.opts;
    bag_.push_back(b);
    bag_weight_ += d.weight * d.item_count(count);
    return r;
}

// S2C 0x00A0 to us (sub_5F7160): [u8 type][u8 kind][item data] (+ the extra info of a new rare item,
// sub_60C220) + [u8 flag] (1 = two u64 expiry times, 3 = unique-item message; 0 here — no such item
// drops). Client sub_6EEC50 -> sub_6B3DC0 + sub_58E540: type 1 sets that item's amount, 2/4 adds it.
void GameClientSession::send_bag_record(const BagRecord& r)
{
    PacketBuffer p(static_cast<Opcode>(0x00A0), is_paula());
    write_bag_record(p, r);
    p.write<uint8_t>(0);
    send(p);
}

// The bag record on the wire (sub_60C220 -> sub_60C070 [+ sub_6EC150]; client sub_6B3DC0): [u8 type]
// [u8 kind][item data] + a new rare item's extra info — in S2C 0x00A0, 0x0049 and 0x004E alike.
void GameClientSession::write_bag_record(PacketBuffer& p, const BagRecord& r) const
{
    p.write<uint8_t>(r.type);
    p.write<uint8_t>(r.kind);
    write_item_data(p, r.kind, r.item_id, r.uid, r.count, r.rare);
    if ((r.type == 4 || r.type == 2) && r.kind == 2) write_rare_extra(p, server_.item_db().get(r.item_id), r.opts);
}

GameClientSession::BagItem* GameClientSession::bag_find_uid(uint64_t uid)
{
    for (BagItem& b : bag_) if (b.uid == uid) return &b;
    return nullptr;
}

GameClientSession::BagItem* GameClientSession::bag_find_id(uint32_t item_id)
{
    for (BagItem& b : bag_) if (b.item_id == item_id) return &b;
    return nullptr;
}

// sub_6E4460 -> sub_6DB8D0 (and sub_6DBF00 for a drop): the record of `count` of item `b` leaving the
// bag — type 3, the item goes (a common item's record carries the amount, sub_6DD090), or, for a common
// item that has more, type 1 with what remains (sub_6DB9A0).
GameClientSession::BagRecord GameClientSession::bag_decrease(const BagItem& b, uint16_t count)
{
    BagRecord r;
    r.type    = 3;
    r.kind    = b.kind;
    r.item_id = b.item_id;
    r.uid     = b.uid;
    r.count   = (b.kind == 1) ? count : b.count;
    std::memcpy(r.rare, b.rare, 4);
    const ItemDef* d = server_.item_db().get(b.item_id);
    if (d && d->stackable() && r.count < b.count) {
        r.type  = 1;
        r.count = static_cast<uint16_t>(b.count - r.count);
    }
    return r;
}

// sub_6DBA50: a type 1 / 3 record on the bag (the inventory weight follows the amount).
void GameClientSession::bag_apply(const BagRecord& r)
{
    for (auto it = bag_.begin(); it != bag_.end(); ++it) {
        if (it->uid != r.uid) continue;
        const ItemDef* d = server_.item_db().get(it->item_id);
        const int32_t w = d ? d->weight : 0;
        if (r.type == 1) {
            bag_weight_ -= w * (static_cast<int32_t>(it->count) - static_cast<int32_t>(r.count));
            it->count = r.count;
        } else if (r.type == 3 || r.type == 5 || r.type == 7) {
            bag_weight_ -= w * (d ? d->item_count(it->count) : static_cast<int32_t>(it->count));
            bag_.erase(it);
        }
        return;
    }
}

// ── C2S 0x039F: the second-password wrapper (client SecondPwdCtrl c2s_927) ───────────────────────
// [u16 inner op][cstr password][the inner command's own payload]. The client wraps the commands a
// second password can guard (drop 67, 76, 127, 172, 176, 347, 355, 766, 1009..1242) and sends them at
// once unless its state (S2C 0x0806) is Lock with that command's flag (then it asks the password).
// Retail Player case 927: only a Locked account (state 3) compares the password for a guarded command
// (wrong -> S2C 0x03A1 [u8 1] "ErrMsg_Wrong_Pwd"); otherwise the inner command is posted to the player
// as it is (at most 1000 bytes, sub_410BD0) and S2C 0x03A0 [u16 inner op] goes out at once (client
// sub_757730 closes the password dialog). Nobody has a second password here (state 0): always posted.
void GameClientSession::handle_secure_command(PacketBuffer& pkt)
{
    try {
        const uint16_t inner = pkt.read<uint16_t>();
        // Hardening (not retail): retail posts whatever inner command comes; we post only the commands the client
        // wraps (above) — never a nested 0x039F, which recursed once per 3 bytes until the stack overflowed.
        const bool guarded = inner == 67 || inner == 76 || inner == 127 || inner == 172 || inner == 176 ||
                             inner == 347 || inner == 355 || inner == 766 || (inner >= 1009 && inner <= 1242);
        if (!guarded) {
            LOG_INFO("GameClientSession %u: 0x039F with inner command %u refused", session_id(), inner);
            return;
        }
        while (!pkt.at_end() && pkt.read<uint8_t>() != 0) {}            // the password (sub_43AD60)
        if (pkt.size() - pkt.rpos() > 1000) {                            // sub_410BD0 posts at most 1000 bytes
            LOG_INFO("GameClientSession %u: 0x039F inner command %u over 1000 bytes refused", session_id(), inner);
            return;
        }
        const size_t hdr = is_paula() ? PacketBuffer::PAULA_HEADER_SIZE : PacketBuffer::STANDARD_HEADER_SIZE;
        std::vector<uint8_t> raw(hdr, 0);
        const size_t op_at = is_paula() ? 2 : 0;
        raw[op_at]     = static_cast<uint8_t>(inner & 0xFF);
        raw[op_at + 1] = static_cast<uint8_t>(inner >> 8);
        raw.insert(raw.end(), pkt.data() + pkt.rpos(), pkt.data() + pkt.size());

        PacketBuffer ack(static_cast<Opcode>(0x03A0), is_paula());
        ack.write<uint16_t>(inner);
        send(ack);

        PacketBuffer cmd(raw.data(), raw.size(), is_paula());
        on_packet(nullptr, std::move(cmd));
    } catch (...) {}
}

// ── CMD_DROP_ITEM (inner command 67 of C2S 0x039F) ───────────────────────────────────────────────
// [u32 item id][u64 uid][u16 count][u16 x][u16 y] — the point the player threw it at (the Drop button:
// near the player; dragged out of the window: the mouse position). Retail Player case 67 -> hItemDrop
// (sub_772020) -> DB -> ICMD_DROP_ACK OK (sub_7722B0) -> Player::OnDropItem (sub_73B760):
//   m_SyncFlag 0xC0020000 set (a drop already waiting for the DB) -> S2C 0x0045, nothing else;
//   hItemDrop sets the flag; the item's flags (Options, sub_6ECC20) 0x1000 = not droppable -> nothing
//     happens (the flag stays set: retail; the client never offers it);
//   the item (sub_50CD80: uid < 2 -> any item of that id, else that uid) must have 0 < count <= its
//     amount; not equipped (sub_6DA7F0); else S2C 0x0045 and the flag is cleared;
//   the record (sub_6DBF00): type 3, the item leaves (with `count`), or type 1, a common item keeps
//     the rest; the ground item is the item's data — a common item without its uid, with `count`;
//   DB: a common item is deleted (uspm_item_delete_item) or keeps the rest; a rare item keeps its row,
//     owned by nobody while it lies on the ground (uspm_item_update_uid_item_rare drop_flag 0);
//   OK: the flag is cleared, the record applied (sub_6DBA50), S2C 0x0044 [record] to us (sub_5F7220:
//     [u8 type][u8 kind][item data], client sub_6E12A0 -> the item window), and the item lands
//     (cGameServer::drop_player_item: S2C 0x000D).
void GameClientSession::handle_drop_item(PacketBuffer& pkt)
{
    auto refuse = [&]() {
        PacketBuffer p(static_cast<Opcode>(0x0045), is_paula());        // sub_60BE50(this, 69)
        send(p);
    };
    try {
        if (exchange_) { refuse(); return; }              // the exchange shop state 18: S2C 0x45
        if (compound_) { refuse(); return; }              // so does the compound state 13
        // Hardening (not retail): the trade's state 6 ignores a drop; it is refused here (S2C 0x45, so the client lets
        // the item go) and the bag stays as the exchange saw it.
        if (trade_)    { refuse(); return; }
        if (drop_sync_) {
            LOG_INFO("%s:CMD_DROP_ITEM: m_SyncFlag.HasFlag( eSyncFlag_ItemDrop )", char_name_);
            refuse();
            return;
        }
        const uint32_t id    = pkt.read<uint32_t>();
        const uint64_t uid   = pkt.read<uint64_t>();
        const uint16_t count = pkt.read<uint16_t>();
        const int16_t  tx    = pkt.read<int16_t>();
        const int16_t  ty    = pkt.read<int16_t>();
        if (!uid)
            LOG_INFO("%s:CMD_DROP_ITEM: uid == ITMUID_NONE : %u, %u", char_name_, id, count);

        drop_sync_ = true;
        const ItemDef* d = server_.item_db().get(id);
        if (d && (d->options & 0x1000)) return;

        // Hardening (not retail): sub_50CD80 takes any item of the id for uid < 2 — the check here passed on one stack
        // while another was removed (a dupe). The client always sends the item's own uid: uid 0 is refused, and the
        // item of that uid (of that id) is the one checked and dropped.
        BagItem* b = uid ? bag_find_uid(uid) : nullptr;
        if (b && (b->item_id != id || count == 0 || b->count < count)) b = nullptr;
        if (b && is_equipped(b->uid)) {
            LOG_INFO("hItemDrop::%s equiped item:%llu,%u,%s,%u", char_name_, static_cast<unsigned long long>(b->uid), id,
                     d ? d->name.c_str() : "?", count);
            drop_sync_ = false;
            refuse();
            return;
        }
        if (!b || !d) {
            LOG_INFO("hItemDrop::%s invalid item:%llu,%u,%s,%u", char_name_, static_cast<unsigned long long>(uid), id,
                     d ? d->name.c_str() : "?", count);
            drop_sync_ = false;
            refuse();
            return;
        }

        const BagRecord rec = bag_decrease(*b, count);
        GroundItemEntity g;
        g.item_id = b->item_id;
        g.kind    = b->kind;
        g.uid     = (b->kind == 1) ? 0 : b->uid;
        g.count   = (b->kind == 1) ? count : b->count;
        std::memcpy(g.rare, b->rare, 4);
        g.opts    = b->opts;

        // The DB (hItemDrop's request), then ICMD_DROP_ACK OK.
        if (b->kind == 1) {
            if (rec.type == 3) server_.item_store().RemoveItem(rec.uid);
            else               server_.item_store().SetAmount(rec.uid, rec.count);
        } else {
            server_.item_store().SetOwner(rec.uid, 0);
        }
        drop_sync_ = false;
        bag_apply(rec);
        hide_break();                                     // sub_73B760: a hidden player leaves its hide
        pet_speech(14);                                   // our pet's line, before S2C 0x0044

        PacketBuffer p(static_cast<Opcode>(0x0044), is_paula());
        p.write<uint8_t>(rec.type);
        p.write<uint8_t>(rec.kind);
        write_item_data(p, rec.kind, rec.item_id, rec.uid, rec.count, rec.rare);
        send(p);

        int32_t px, py;
        cur_pos(std::chrono::steady_clock::now(), px, py);
        server_.drop_player_item(zone_id_, static_cast<uint16_t>(object_id_), px, py, tx, ty, std::move(g));
        LOG_DEBUG("GameClientSession %u: dropped %u x%u (%s), record type %u, weight %d/%d", session_id(), id, count,
                  d->name.c_str(), rec.type, bag_weight_, st_maxwt_);
    } catch (...) {}
}

// sub_6DCC50 on the bag (Player +1716): an item of that id (an NPC check).
bool GameClientSession::bag_has_item(uint32_t item_id) const
{
    for (const BagItem& b : bag_)
        if (b.item_id == item_id) return true;
    return false;
}
