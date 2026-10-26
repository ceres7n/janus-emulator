#include "game_server.hpp"
#include "game_session.hpp"
#include "item_wire.hpp"
#include "logger.hpp"
#include "paula_crypt.hpp"
#include <cmath>
#include <cstdlib>
#include <utility>

// ─────────────────────────────────────────────────────────────────────────────
//  Monster loot and ground items — retail GameServer, CommonMonster die handler sub_7296E0:
//  when the killed monster has an owner (+1212) that is a player, after the owner's TM:
//    1. the items are rolled (sub_729220): ItemDropCnt rolls of 10000*rand()/0x8000 against the
//       bands of its MonItemDropInfo list; a roll that hits no item falls through to the
//       sub-tables (sub_72B920 / sub_72BA00); at most 10 items, a rare item is always one;
//    2. gelder: (int)(GelderRate*10000) > 10000*rand()/0x8000 -> GMin + rand()%(GMax-GMin),
//       split into piles of items 200..203 (sub_7FEA40), dropped first (S2C 617, sub_7FF500);
//    3. the items (S2C 12, sub_7FF3E0).
//  Every item becomes an ItemActor owned by the monster's owner, placed on a free cell around
//  the monster (sub_7FF180 -> spiral sub_422200), shown to the zone at once.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr int kMaxDrops = 10;

// sub_5CC080(0, 10000)
int roll10000() { return 10000 * std::rand() / 0x8000; }

// CommonMonster::Initialize (sub_7287E0): +149 = 1 for the monster ids 16000..16999 (0x3E80..0x4267,
// MonsterEntity::shadow). It is the u16 after the position in the drop packets, and the dropped
// items' own +149 (only a player with the Third Eye picks such an item up).
uint8_t monster_event_flag(uint32_t mob_id) { return (mob_id >= 16000 && mob_id <= 16999) ? 1 : 0; }

// The ItemActor body after its id/position (vtbl+48, sub_71C6F0): sub_7146E0 [u8 0 (vtbl+96)]
// [u8 dir*256/2pi][u8 speed/4] — a ground item has neither — then the item (sub_60CCA0):
// [u8 kind] + its data (item_wire.hpp; the 4 rare bytes are 0 on a monster's drop, sub_6DE5E0, and
// a player's own on a rare item they threw away).
// Client: ItemActor sub_6D13D0 -> sub_58F690(kind) -> CommonItem sub_58F1F0 / RareItem sub_58F2E0.
void write_item_body(PacketBuffer& p, const GroundItemEntity& g)
{
    p.write<uint8_t>(0);
    p.write<uint8_t>(0);
    p.write<uint8_t>(0);
    p.write<uint8_t>(g.kind);
    write_item_data(p, g.kind, g.item_id, g.uid, g.count, g.rare);
}

// sub_8040D0 (max range 80.0): the point a thrown item lands on. The target is pulled back onto the
// line from the thrower when it is farther than the range; when its cell is blocked (byte 1) the point
// steps back toward the thrower 4 units at a time; false = no free cell on the way (the item lands at
// the thrower's position). The x87 arithmetic (53-bit precision, float stores) as in the retail.
bool drop_point(const MapGrid* grid, int16_t fx, int16_t fy, int16_t& tx, int16_t& ty)
{
    constexpr float kRange = 80.0f;
    auto free_cell = [&](int16_t x, int16_t y) {                       // sub_422100 + vtbl+12 + sub_422570
        return !grid || grid->at(static_cast<int16_t>(x / 16), static_cast<int16_t>(y / 16)) != 1;
    };
    float dx = static_cast<float>(static_cast<int>(tx) - static_cast<int>(fx));
    float dy = static_cast<float>(static_cast<int>(ty) - static_cast<int>(fy));
    const float sx = static_cast<float>(static_cast<double>(dx) * static_cast<double>(dx));
    const float sy = static_cast<float>(static_cast<double>(dy) * static_cast<double>(dy));
    float dist = std::sqrt(static_cast<float>(static_cast<double>(sy) + static_cast<double>(sx)));   // sqrtf
    if (kRange < dist) {
        const float k = static_cast<float>(static_cast<double>(kRange) / static_cast<double>(dist));
        dx = static_cast<float>(static_cast<double>(dx) * static_cast<double>(k));
        dy = static_cast<float>(static_cast<double>(dy) * static_cast<double>(k));
        tx = static_cast<int16_t>(fx + static_cast<int16_t>(static_cast<int>(dx)));
        ty = static_cast<int16_t>(fy + static_cast<int16_t>(static_cast<int>(dy)));
        dist = kRange;
    }
    if (free_cell(tx, ty)) return true;
    float d = dist;
    for (;;) {
        d = static_cast<float>(static_cast<double>(d) - 4.0);
        const float f = static_cast<float>(static_cast<double>(d) / static_cast<double>(dist));
        if (d <= 0.0f || !(f >= 0.0f)) return false;
        const float px = static_cast<float>(static_cast<double>(dx) * static_cast<double>(f));
        const float py = static_cast<float>(static_cast<double>(dy) * static_cast<double>(f));
        tx = static_cast<int16_t>(fx + static_cast<int16_t>(static_cast<int>(px)));
        ty = static_cast<int16_t>(fy + static_cast<int16_t>(static_cast<int>(py)));
        if (free_cell(tx, ty)) return true;
    }
}

} // namespace

// sub_729220 / sub_72BA00: a rare item (Class 2) drops one, anything else its Cnt.
uint16_t cGameServer::drop_amount(const DropItem& e) const
{
    const ItemDef* it = item_db_.get(e.item);
    return static_cast<uint16_t>((it && it->cls == 2) ? 1 : e.cnt);
}

// sub_729220: the monster's item rolls.
void cGameServer::roll_item_drops(const MobDef& d, std::vector<DropLoot>& loot) const
{
    const DropList* dl = drop_db_.list(static_cast<uint32_t>(d.item_drop_idx));
    if (!dl) return;
    // sub_718D70: the owner may win one extra roll (a % chance at Player +1496, from items/buffs —
    // none of them exists here yet, so 0); no extra roll when the total would reach 10.
    int extra = 0;
    if (extra + d.item_drop_cnt >= 10) extra = 0;
    for (int i = 0; i < extra + d.item_drop_cnt; ++i) {
        const int roll = roll10000();
        bool hit = false;
        for (const DropItem& e : dl->items) {
            if (!e.hi) break;
            if (roll < e.hi) { loot.push_back({ e.item, drop_amount(e) }); hit = true; break; }
        }
        if (static_cast<int>(loot.size()) >= kMaxDrops) break;
        if (!hit) roll_sub_tables(roll, *dl, d.item_drop_cnt, loot);
    }
}

// sub_72B920: the SAME roll against the sub-table bands (which follow the items' bands); the hit
// sub-table fills the remaining rolls. Repeated (ItemDropCnt - drops so far) times.
void cGameServer::roll_sub_tables(int roll, const DropList& dl, int rolls, std::vector<DropLoot>& loot) const
{
    const int n = rolls - static_cast<int>(loot.size());
    for (int i = 0; i < n; ++i) {
        for (const DropTableRef& t : dl.tables) {
            if (!t.hi) break;
            if (roll < t.hi) {
                if (const DropSubTable* st = drop_db_.table(t.table)) roll_sub_table(*st, rolls, loot);
                break;
            }
        }
        if (static_cast<int>(loot.size()) >= kMaxDrops) break;
    }
}

// sub_72BA00: (ItemDropCnt - drops so far) rolls of rand() % total against the sub-table's bands
// (slots 0..count-1).
void cGameServer::roll_sub_table(const DropSubTable& t, int rolls, std::vector<DropLoot>& loot) const
{
    const int n = rolls - static_cast<int>(loot.size());
    for (int i = 0; i < n; ++i) {
        const int r = (t.total > 0) ? std::rand() % t.total : 0;   // sub_420AB0(0, total)
        for (int j = 0; j < t.count; ++j) {
            const DropItem& e = t.slot[j];
            if (!e.hi) break;
            if (r < e.hi) { loot.push_back({ e.item, drop_amount(e) }); break; }
        }
        if (static_cast<int>(loot.size()) >= kMaxDrops) break;
    }
}

void cGameServer::drop_monster_loot(const MonsterEntity& m, int32_t x, int32_t y)
{
    // The die handler drops only for an owner that is (still) a player; the spawn order's
    // no-reward flag (+2028) is 0 for every monster spawned here.
    const uint32_t owner = m.owner_oid;
    if (!owner || !entity_mgr_.get_player(owner)) return;
    const MobDef* d = mob_db_.get(static_cast<int>(m.mob_id));
    if (!d) return;

    std::vector<DropLoot> loot;
    roll_item_drops(*d, loot);
    const uint8_t ev = monster_event_flag(m.mob_id);

    // Gelder (sub_62FB70: only when GMin or GMax is set) — sub_729B30 -> sub_7FF500.
    const int groll = roll10000();
    if (static_cast<int>(static_cast<double>(d->gelder_rate) * 10000.0) > groll && (d->gmin || d->gmax)) {
        int lo = d->gmin, hi = d->gmax;
        if (hi < lo) std::swap(lo, hi);
        if (hi > lo) {                                  // (equal: the retail rand() % 0 — not in the data)
            const int amount = lo + std::rand() % (hi - lo);   // sub_420AB0
            const std::vector<DropLoot> piles = split_gelder(amount);
            // a pile's init byte +0x24 = the monster's +149, its own +149 = 0; S2C 617, inner 7 / outer 10
            if (!piles.empty()) place_drops(m, piles, owner, ev != 0, 0, 7, 10, 617, x, y);
        }
    }

    // Items — sub_729AB0 -> sub_7FF3E0. Every item is built with the amount of the FIRST rolled one
    // (`movsx edx, word ptr [ecx+8]` on the array base, not on entry j — retail behaviour, kept).
    if (!loot.empty()) {
        const uint16_t first_count = loot.front().count;
        for (DropLoot& l : loot) l.count = first_count;
        // an item's init byte +0x24 = 1, its own +149 = the monster's; S2C 12, inner 4 / outer 6
        place_drops(m, loot, owner, true, ev, 4, 6, 12, x, y);
    }
}

// sub_7FF180 (+ sub_801330 / ItemActor::OnInitialize sub_71C3B0): each item is placed on a free cell
// around the monster (cell = pos/16, centre = cell*16+8; at the monster's position when no cell is
// found), owned (state 1) when its owner is a player of this zone, and shown to the zone:
// S2C 12 (items, sub_5F7420) / 617 (gelder, sub_5F7510) = [u16 dropper][u16 item][u16 x][u16 y]
// [u16 monster +149][item body]. (The 2012 server also wrote the owner's id before that u16; the
// KR client does not read it — sub_6F0550 reads id, position, u16 -> ItemActor +384, body.)
void cGameServer::place_drops(const MonsterEntity& m, const std::vector<DropLoot>& loot, uint32_t owner_oid,
                              bool drop_flag, uint8_t item_event_flag, int inner, int outer, uint16_t opcode,
                              int32_t x, int32_t y)
{
    const MapGrid* grid = zone_grid(m.zone);
    const uint8_t  ev   = monster_event_flag(m.mob_id);
    const PlayerEntity* op = entity_mgr_.get_player(owner_oid);
    const bool owned = op && op->zone == m.zone;
    const bool paula = janus::PaulaCrypt::Instance().IsLoaded();
    std::vector<std::pair<int16_t, int16_t>> taken;    // the retail marks the batch's cells (byte 0 = 1), then clears them

    for (const DropLoot& l : loot) {
        const ItemDef* it = item_db_.get(l.item);
        int32_t px = x, py = y;
        if (grid) {
            int16_t cx = static_cast<int16_t>(static_cast<int16_t>(x) / 16);   // sub_422100
            int16_t cy = static_cast<int16_t>(static_cast<int16_t>(y) / 16);
            if (grid->find_drop_cell(inner, outer, cx, cy, taken)) {
                taken.emplace_back(cx, cy);
                px = 16 * cx + 8;                                              // sub_422150
                py = 16 * cy + 8;
            }
        }
        GroundItemEntity g;
        g.zone       = m.zone;
        g.pos_x      = px;
        g.pos_y      = py;
        g.item_id    = l.item;
        g.uid        = 0;
        g.count      = l.count;
        g.kind       = static_cast<uint8_t>(it ? it->cls : 1);
        g.item_type  = it ? it->type : 0;
        g.dropper    = static_cast<uint16_t>(m.object_id);
        g.owner_oid  = owned ? owner_oid : 0;
        g.owned      = owned;
        g.event_flag = item_event_flag;
        g.drop_flag  = drop_flag;
        g.object_id  = entity_mgr_.spawn_ground_item(g);

        PacketBuffer p(static_cast<Opcode>(opcode), paula);
        p.write<uint16_t>(static_cast<uint16_t>(m.object_id));
        p.write<uint16_t>(static_cast<uint16_t>(g.object_id));
        p.write<uint16_t>(static_cast<uint16_t>(px));
        p.write<uint16_t>(static_cast<uint16_t>(py));
        p.write<uint16_t>(ev);
        write_item_body(p, g);
        broadcast_zone(m.zone, p, 0);
        LOG_DEBUG("cGameServer: mob %u (%u) drops %s %u x%u (%s) as item %u at (%d,%d)%s", m.object_id,
                  m.mob_id, opcode == 617 ? "gelder" : "item", l.item, l.count, it ? it->name.c_str() : "?",
                  g.object_id, px, py, owned ? " [owned]" : "");
    }
}

// Player::OnDropItem (sub_73B760) -> sub_7FEE10: a new ItemActor (object id from the pool; dropper =
// the thrower, sub_60D270) -> sub_7FE1B0: its position (sub_8040D0 from the thrower's position toward
// the target, else the thrower's position), no owner (state 0: free at once, 90 s), and S2C 0x000D to
// the area (sub_5F7290): [u16 item][u16 x][u16 y][u16 fromX][u16 fromY][u16 0][item body] — client
// case 13 flies the item from the thrower (+108) to its spot (+112).
uint32_t cGameServer::drop_player_item(int16_t zone, uint16_t thrower, int32_t fx, int32_t fy, int32_t tx,
                                       int32_t ty, GroundItemEntity g)
{
    int16_t x = static_cast<int16_t>(tx), y = static_cast<int16_t>(ty);
    if (!drop_point(zone_grid(zone), static_cast<int16_t>(fx), static_cast<int16_t>(fy), x, y)) {
        x = static_cast<int16_t>(fx);
        y = static_cast<int16_t>(fy);
    }
    const ItemDef* it = item_db_.get(g.item_id);
    g.zone       = zone;
    g.pos_x      = x;
    g.pos_y      = y;
    g.item_type  = it ? it->type : 0;
    g.dropper    = thrower;
    g.owner_oid  = 0;
    g.owned      = false;
    g.event_flag = 0;
    g.drop_flag  = false;
    g.timer_ms   = 0;
    g.object_id  = entity_mgr_.spawn_ground_item(g);

    PacketBuffer p(static_cast<Opcode>(0x000D), janus::PaulaCrypt::Instance().IsLoaded());
    p.write<uint16_t>(static_cast<uint16_t>(g.object_id));
    p.write<uint16_t>(static_cast<uint16_t>(x));
    p.write<uint16_t>(static_cast<uint16_t>(y));
    p.write<uint16_t>(static_cast<uint16_t>(fx));
    p.write<uint16_t>(static_cast<uint16_t>(fy));
    p.write<uint16_t>(0);
    write_item_body(p, g);
    broadcast_zone(zone, p, 0);
    LOG_DEBUG("cGameServer: player %u threw %u x%u (%s) as item %u: (%d,%d) -> target (%d,%d) -> lands (%d,%d)",
              thrower, g.item_id, g.count, it ? it->name.c_str() : "?", g.object_id, fx, fy, tx, ty, x, y);
    return g.object_id;
}

// A drill's find (Player::OnEndDrill -> sub_7FEF10): a new ItemActor (sub_8011F0: the init struct
// +0x20 owner, +0x22 the actor, +0x24 = 1) placed by sub_7FE2F0(zone, item, 1, actor, 4, 6) — the spiral
// (inner 4 / outer 6) around the actor's cell, centre of the cell found, else the actor's position —
// and shown with its case 1, sub_5F7350: S2C 0x000E [u16 actor][u16 item][u16 x][u16 y][u16 actorX]
// [u16 actorY][u16 0][item body] to the area (client case 14 -> sub_6EA1D0: the item flies from the
// actor, +0x6C, to its spot, +0x70).
uint32_t cGameServer::pop_up_item(int16_t zone, uint16_t popper, int32_t x, int32_t y, GroundItemEntity g)
{
    int32_t ix = x, iy = y;
    if (const MapGrid* grid = zone_grid(zone)) {
        int16_t cx = static_cast<int16_t>(static_cast<int16_t>(x) / 16);   // sub_422100
        int16_t cy = static_cast<int16_t>(static_cast<int16_t>(y) / 16);
        if (grid->find_drop_cell(4, 6, cx, cy, {})) { ix = 16 * cx + 8; iy = 16 * cy + 8; }   // sub_422150
    }
    const ItemDef* it = item_db_.get(g.item_id);
    g.zone      = zone;
    g.pos_x     = ix;
    g.pos_y     = iy;
    g.item_type = it ? it->type : 0;
    g.timer_ms  = 0;
    g.object_id = entity_mgr_.spawn_ground_item(g);

    PacketBuffer p(static_cast<Opcode>(0x000E), janus::PaulaCrypt::Instance().IsLoaded());
    p.write<uint16_t>(popper);
    p.write<uint16_t>(static_cast<uint16_t>(g.object_id));
    p.write<uint16_t>(static_cast<uint16_t>(ix));
    p.write<uint16_t>(static_cast<uint16_t>(iy));
    p.write<uint16_t>(static_cast<uint16_t>(x));
    p.write<uint16_t>(static_cast<uint16_t>(y));
    p.write<uint16_t>(0);
    write_item_body(p, g);
    broadcast_zone(zone, p, 0);
    LOG_DEBUG("cGameServer: %u x%u (%s) pops up at actor %u as item %u at (%d,%d)%s", g.item_id, g.count,
              it ? it->name.c_str() : "?", popper, g.object_id, ix, iy, g.owned ? " [owned]" : "");
    return g.object_id;
}

// sub_7FE670: the BigBang's finds — the ItemActors first (sub_8011F0: count = the entry's 1, dropper =
// the digger, owner = the entry's, init byte +0x24 = 1), then each placed around the digger's cell
// (sub_422200(…, 4, 6), the found cell marked taken until the batch ends) — and S2C 1074 (sub_607740) to
// the area: [u32 n][u16 digger] + n x {[u16 item][u16 fromX][u16 fromY][u16 toX][u16 toY][item body]}
// + [u8 the wedding flag] (client case 1074: at most 10, the items fly from -> to, "bigbang" /
// "LoveDrillBang" at the digger).
void cGameServer::bigbang_drop(int16_t zone, uint16_t digger, int32_t x, int32_t y, uint32_t owner_oid,
                               const std::vector<BigBangFind>& finds, bool wedding)
{
    const MapGrid* grid = zone_grid(zone);
    std::vector<std::pair<int16_t, int16_t>> taken;
    const PlayerEntity* op = entity_mgr_.get_player(owner_oid);
    const bool owned = op && op->zone == zone;
    PacketBuffer p(static_cast<Opcode>(0x0432), janus::PaulaCrypt::Instance().IsLoaded());
    p.write<uint32_t>(static_cast<uint32_t>(finds.size()));
    p.write<uint16_t>(digger);
    for (const BigBangFind& f : finds) {
        const ItemDef* it = item_db_.get(f.item);
        int32_t ix = x, iy = y;
        if (grid) {
            int16_t cx = static_cast<int16_t>(static_cast<int16_t>(x) / 16);
            int16_t cy = static_cast<int16_t>(static_cast<int16_t>(y) / 16);
            if (grid->find_drop_cell(4, 6, cx, cy, taken)) {
                taken.emplace_back(cx, cy);
                ix = 16 * cx + 8;
                iy = 16 * cy + 8;
            }
        }
        GroundItemEntity g;
        g.zone       = zone;
        g.pos_x      = ix;
        g.pos_y      = iy;
        g.item_id    = f.item;
        g.uid        = 0;
        g.count      = 1;
        g.kind       = static_cast<uint8_t>(it ? it->cls : 1);
        g.item_type  = it ? it->type : 0;
        g.dropper    = digger;
        g.owner_oid  = owned ? owner_oid : 0;
        g.owned      = owned;
        g.event_flag = 0;
        g.drop_flag  = true;
        g.object_id  = entity_mgr_.spawn_ground_item(g);
        p.write<uint16_t>(static_cast<uint16_t>(g.object_id));
        p.write<uint16_t>(static_cast<uint16_t>(f.from_x));
        p.write<uint16_t>(static_cast<uint16_t>(f.from_y));
        p.write<uint16_t>(static_cast<uint16_t>(ix));
        p.write<uint16_t>(static_cast<uint16_t>(iy));
        write_item_body(p, g);
        LOG_DEBUG("cGameServer: BigBang %u (%s) from (%d,%d) as item %u at (%d,%d)", f.item, it ? it->name.c_str() : "?",
                  f.from_x, f.from_y, g.object_id, ix, iy);
    }
    p.write<uint8_t>(wedding ? 1 : 0);
    broadcast_zone(zone, p, 0);
}

// The items already lying in the zone, to a player entering it: op 10 per item — the ItemActor
// appear (vtbl+76 sub_71C690: [u16 id][u16 x][u16 y][u16 +149][item body]; client case 10 ->
// sub_6F04A0).
void cGameServer::appear_ground_items_to(GameClientSession& who)
{
    for (const GroundItemEntity& g : entity_mgr_.ground_items_in_zone(who.zone_id())) {
        PacketBuffer p(static_cast<Opcode>(0x000A), who.is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(g.object_id));
        p.write<uint16_t>(static_cast<uint16_t>(g.pos_x));
        p.write<uint16_t>(static_cast<uint16_t>(g.pos_y));
        p.write<uint16_t>(g.event_flag);
        write_item_body(p, g);
        who.send(p);
    }
}

const MapGrid* cGameServer::zone_grid(int16_t zone)
{
    const MapDef* md = maps_db_.get(zone);
    return md ? map_attr_.grid(zone, md->file) : nullptr;
}

// An ItemActor leaving the world (destroyed: free time over, or picked up — ReserveRemove sub_801E70
// -> vtbl+68 sub_71C650 -> sub_5F7610): S2C 0x000F [u16 id][u8 0] to its zone.
void cGameServer::ground_item_gone(int16_t zone, uint32_t oid)
{
    PacketBuffer p(static_cast<Opcode>(0x000F), janus::PaulaCrypt::Instance().IsLoaded());
    p.write<uint16_t>(static_cast<uint16_t>(oid));
    p.write<uint8_t>(0);
    broadcast_zone(zone, p, 0);
}

// The ground items' state machines run on the zone tick (GameTask, 100 ms).
void cGameServer::start_item_tick()
{
    item_tick_last_ = std::chrono::steady_clock::now();
    item_timer_.expires_after(std::chrono::milliseconds(100));
    item_timer_.async_wait([this](std::error_code ec) { if (!ec) item_tick(); });
}

void cGameServer::item_tick()
{
    const auto now = std::chrono::steady_clock::now();
    const int32_t elapsed = static_cast<int32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - item_tick_last_).count());
    item_tick_last_ = now;
    // Each step on its own guard: one that throws is logged (the first time, then once a minute) and the others still
    // run, and the tick re-arms — a throw used to leave the timer unarmed (monsters, respawn, item expiry stopped).
    static unsigned errors = 0;
    auto step = [](const char* what, auto&& fn) {
        try { fn(); return; }
        catch (const std::exception& e) {
            if (errors++ % 600 == 0) LOG_ERROR("cGameServer: zone tick (%s): %s (%u so far)", what, e.what(), errors);
        } catch (...) {
            if (errors++ % 600 == 0) LOG_ERROR("cGameServer: zone tick (%s): unknown exception (%u so far)", what, errors);
        }
    };
    step("ground items", [&] {
        std::vector<GroundItemEntity> gone;
        entity_mgr_.tick_ground_items(elapsed, gone);
        for (const GroundItemEntity& g : gone) {
            ground_item_gone(g.zone, g.object_id);
            LOG_DEBUG("cGameServer: ground item %u (%u x%u) expired in zone %d", g.object_id, g.item_id, g.count, g.zone);
        }
    });
    step("monster ES", [&] { monster_es_tick(elapsed); });     // the ESs on monsters (es.cpp)
    step("regen", [&] { regen_tick(elapsed); });                // the maps' monster regeneration (regen.cpp)
    step("monster AI", [&] { monster_ai_tick(elapsed); });      // the idle monsters (monster_ai.cpp)
    step("party", [&] { party_.tick(elapsed); });               // the PartyMemberDepot (party.cpp)
    item_timer_.expires_after(std::chrono::milliseconds(100));
    item_timer_.async_wait([this](std::error_code ec) { if (!ec) item_tick(); });
}
