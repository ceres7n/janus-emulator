#include "game_session.hpp"
#include "game_server.hpp"
#include "combat_util.hpp"
#include "logger.hpp"
#include <cstdlib>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Drilling — retail GameServer (Player state 3, DrillState), verified against the KR client.
//
//  The world: every zone whose MapInfoEx ItemPercentage is not 0 has buried items (MapItemManager,
//  sub_850040): the diggable cells (byte 0 != 1 and ground nature != 6) are counted and random ones
//  (100 tries per item, sub_850CB0) each get an item at a depth of 10 * rand[Min/10, Max/10 + 1) until
//  they are ItemPercentage % of them; a dug item is replaced at once (sub_8504E0 -> sub_850570). WHAT
//  is dug is rolled only on a find (PopItemByRandom sub_850890, db/map_item_db.yml).
//
//  The protocol (client sub_6B4E00 = the avatar, state 5 = its drill state):
//    C2S 0x002C [u16 x][u16 y][u8 moveType] (Player case 44, any state but 3 / 8 / 15): the drill must
//      be equipped (slot 7) and in the bag, else S2C 0x002E (sub_5F4120; the client clears its drill
//      flags). The point's cell is ours -> state 3, +0x67C = our position, S2C 0x002D (sub_5F4100; the
//      client enters its state 5), S2C 0x0019 halt to the area and event 30008 at once; another cell ->
//      the walk (Player vtbl+100), state 3, +0x67C = its end, S2C 0x002D, S2C 0x0018; a failed walk ->
//      S2C 0x002E + S2C 0x0019.
//    event 30008 (the walk ended, DrillState::OnStopMove): only once (+4); stopped elsewhere -> stop 1;
//      ground nature 6 -> stop 7 ("NotDigableGround"); the sense roll rand()%100 < DA + 10: with no item
//      in a cell other than the last dig's, the probe's EXP (kind 7: the map's middle depth x
//      (DA + 200) / 100, S2C 0x0026); then the seed, the start time and S2C 371 to us:
//      [u16 me][u16 x][u16 y][u8 0][u8 MaxItemDepth/5][u8 itemDepth/5 (0 = none)][u8 nature]
//      [u16 seed | sensed][u8 Crazy Drill gauge %], and S2C 47 to the area (the same up to the nature,
//      then [u8 has PSAbility 6]). The client digs in its gauge (sub_6B4240: a sensed cell without an
//      item says "Speech_NoExistItem" and cancels) and reports:
//    C2S 0x0031 [u16 cnt][u16][u16 depth][u16 crc] (state 3 case 49, CMD_END_DRILL): the CRC
//      crc ^ BarSpeed<<1 ^ BarEffArea ^ ScoopMass<<5 ^ depth<<8 ^ cnt<<12, & 0xFFFE, must be the seed,
//      and 0 < cnt, depth <= DigDepth and <= MaxItemDepth — else the session is cut; the drill loses
//      cnt (2 x cnt on a ground of another nature) of its life; no item -> stop 2 "Nothing"; an item
//      deeper than reached -> stop 4 "LowDrillGrade"; reached past it -> stop 2 (no EXP); reached it ->
//      the shortest dig time (sub_5E8F20) is checked ("Drill Hack Detected": the cut), the item pops
//      up at us (S2C 14, owned by us; a drill comes with its DrillLife), stop 3 "DiggedOut", TM EXP
//      4 * (3 * Lv + depth) * Grade (S2C 0x011D), the cell's item is replaced; the dig's EXP (event
//      30003 kind 6: ExpRate(Grade) * depth * (Lv + 400) * 0.0005, S2C 0x0406) on every end but the
//      "past it" one.
//    C2S 0x0030 [u16 cnt][u16] (state 3 case 48): cancelled — the life spent so far, stop 1.
//    event 30002 (hit, hit or missed, while in state 3): stop 10 unless PSAbility 6 (skill 3001
//      "발굴 삼매경").
//    stop (sub_75C4A0): S2C 0x0032 [u8 reason][u16 drill life][u32 item] to us (client vtbl+268
//      sub_6B4600: 2/9 "Nothing", 3 "DiggedOut", 4 "LowDrillGrade", 6 "EmptyDrillLife", 7
//      "NotDigableGround"), S2C 0x0033 [u16 me][u8 reason] to the area (others leave their state 5),
//      state 0.
//  The Crazy Drill Mode (skill 5019; CCrazyDrillMgr, Player +6212) is at the end of this file: the
//  gauge the results feed, its instant digs (Player::DoDrill sub_769640) and the BigBang (S2C 1074).
//  Not modelled (no source of it in the KR data / no client trigger yet): the premium / auto-drill
//  (Player +0x668 bits 0 / 2, C2S 785 / 791 / 794, S2C 1206), the wedding flag (CCrazyDrillMgr +60),
//  the drill-event zones (zone +5660); the timed drop events that may replace a find (World
//  +258184); the scenario quests' items in the type-5 boost (only the normal quests' so far); the
//  server-side test hook C2S 1029 [0x1392] (CDM on — the client never sends it).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// sub_61E000: n * rand() / 0x8000 -> [0, n)
int rand_n(int n) { return n * std::rand() / 0x8000; }
// sub_5CC080: a + (b - a) * rand() / 0x8000 -> [a, b)
int rand_range(int a, int b) { return a + (b - a) * std::rand() / 0x8000; }

// sub_5E8F20(BarSpeed, BarEffArea, depth, mapDepth, ScoopMass): the shortest time (ms) a dig to
// `depth` can take in the client's gauge — x87 with float stores, as the retail.
int32_t shortest_dig_ms(const DrillParam& dp, int depth, int map_depth)
{
    int a3 = depth;
    if (a3 <= 0) a3 = map_depth;
    const float v8 = static_cast<float>(static_cast<double>(100 * (200 - dp.bar_eff_area)) /
                                        (static_cast<double>(dp.bar_speed) * 0.1666666716337204));
    const float v9 = static_cast<float>(dp.scoop_mass ? 120 * a3 / dp.scoop_mass : 0);   // (no drill has 0)
    const float v6 = v9 + v8;
    if (v6 < 0.0f) return 0;
    return static_cast<int32_t>(static_cast<int64_t>(static_cast<float>(static_cast<double>(v6) * 0.699999988079071)));
}

} // namespace

// ─── The zones' buried items (MapItemManager) ────────────────────────────────

// sub_850040 on the zone's first use: nothing when ItemPercentage is 0 (sub_850230); else the
// diggable cells are counted (sub_850440) and the items laid (sub_850570). (Called with
// map_items_mutex_ held.)
cGameServer::ZoneMapItems& cGameServer::zone_map_items(int16_t zone)
{
    ZoneMapItems& z = map_items_[zone];
    if (z.init) return z;
    z.init = true;
    const MapDef* md = maps_db_.get(zone);
    if (!md || md->item_percentage == 0.0f) return z;
    const MapGrid* g = zone_grid(zone);
    if (!g || g->w <= 0 || g->h <= 0) return z;
    z.w = g->w;
    z.h = g->h;
    z.depth.assign(static_cast<size_t>(g->w) * static_cast<size_t>(g->h), ZoneMapItems::kNoItem);
    for (int y = 0; y < g->h; ++y)
        for (int x = 0; x < g->w; ++x)
            if (g->diggable_cell(x, y)) ++z.unblocked;
    map_items_fill(zone, z);
    const double laid = z.unblocked ? static_cast<double>(z.laid) * 100.0 / z.unblocked : 0.0;
    LOG_DEBUG("DrillItemInfo - ZoneId:%d, ZoneName:%s, SetedItemPercent:%.2f%%, UnblockTileCount:%d, "
              "LayedItemCount:%zu, LayedItemPercent:%.2f%%", zone, md->name.c_str(),
              static_cast<double>(md->item_percentage), z.unblocked, z.laid, laid);
    return z;
}

// sub_850570: items are laid until they cover ItemPercentage % of the diggable cells (the share a float,
// sub_850820) or 100 random cells (sub_850CB0: rand_n(w), rand_n(h); diggable and still empty) give
// none. The depth: sub_850E10 = 10 * sub_5CC080(Min / 10, Max / 10 + 1).
void cGameServer::map_items_fill(int16_t zone, ZoneMapItems& z)
{
    const MapDef* md = maps_db_.get(zone);
    const MapGrid* g = zone_grid(zone);
    if (!md || !g || z.depth.empty()) return;
    for (;;) {
        const float share = static_cast<float>(static_cast<double>(z.laid) * 100.0 /
                                               static_cast<double>(static_cast<uint32_t>(z.unblocked)));
        if (share >= md->item_percentage) break;          // (0 diggable cells: NaN, the cells run out)
        bool laid = false;
        for (int i = 0; i < 100; ++i) {
            const int16_t cx = static_cast<int16_t>(rand_n(z.w));
            const int16_t cy = static_cast<int16_t>(rand_n(z.h));
            if (!g->diggable_cell(cx, cy)) continue;
            uint16_t* c = z.at(cx, cy);
            if (!c || *c != ZoneMapItems::kNoItem) continue;
            const int depth = 10 * rand_range(md->min_item_depth / 10, md->max_item_depth / 10 + 1);
            *c = static_cast<uint16_t>(depth);
            ++z.laid;
            laid = true;
            break;
        }
        if (!laid) break;
    }
}

// The world build (BuildWorld -> each zone's build sub_8111D0 -> sub_850040): every map lays its items.
void cGameServer::build_map_items()
{
    std::lock_guard<std::recursive_mutex> lock(map_items_mutex_);
    size_t zones = 0, items = 0;
    for (const auto& [id, md] : maps_db_.all()) {
        if (md.item_percentage == 0.0f) continue;
        ZoneMapItems& z = zone_map_items(static_cast<int16_t>(id));
        ++zones;
        items += z.laid;
    }
    LOG_INFO("cGameServer: buried items laid in %zu maps (%zu items)", zones, items);
}

// sub_850260 / sub_850110 / sub_8504E0: the cell of (x, y) (sub_422100: C int16 / 16).
bool cGameServer::map_item_has(int16_t zone, int16_t x, int16_t y)
{
    std::lock_guard<std::recursive_mutex> lock(map_items_mutex_);
    ZoneMapItems& z = zone_map_items(zone);
    const uint16_t* c = z.at(static_cast<int16_t>(x / 16), static_cast<int16_t>(y / 16));
    return c && *c != ZoneMapItems::kNoItem;
}

int16_t cGameServer::map_item_depth(int16_t zone, int16_t x, int16_t y)
{
    std::lock_guard<std::recursive_mutex> lock(map_items_mutex_);
    ZoneMapItems& z = zone_map_items(zone);
    const uint16_t* c = z.at(static_cast<int16_t>(x / 16), static_cast<int16_t>(y / 16));
    return (c && *c != ZoneMapItems::kNoItem) ? static_cast<int16_t>(*c) : static_cast<int16_t>(-1);
}

void cGameServer::map_item_stats(int16_t zone, size_t& laid, int32_t& unblocked)
{
    std::lock_guard<std::recursive_mutex> lock(map_items_mutex_);
    ZoneMapItems& z = zone_map_items(zone);
    laid = z.laid;
    unblocked = z.unblocked;
}

void cGameServer::map_items_refill(int16_t zone)
{
    std::lock_guard<std::recursive_mutex> lock(map_items_mutex_);
    map_items_fill(zone, zone_map_items(zone));
}

void cGameServer::cdm_put(uint32_t char_uid, const CdmCarry& c)
{
    std::lock_guard<std::mutex> lock(cdm_carry_mutex_);
    CdmCarry e = c;
    e.at = std::chrono::steady_clock::now();
    cdm_carry_[char_uid] = e;
}

bool cGameServer::cdm_take(uint32_t char_uid, CdmCarry& out)
{
    std::lock_guard<std::mutex> lock(cdm_carry_mutex_);
    auto it = cdm_carry_.find(char_uid);
    if (it == cdm_carry_.end()) return false;
    const CdmCarry e = it->second;
    cdm_carry_.erase(it);
    if (std::chrono::steady_clock::now() - e.at > std::chrono::seconds(60)) return false;
    out = e;
    return true;
}

void cGameServer::map_item_remove(int16_t zone, int16_t x, int16_t y, bool refill)
{
    std::lock_guard<std::recursive_mutex> lock(map_items_mutex_);
    ZoneMapItems& z = zone_map_items(zone);
    uint16_t* c = z.at(static_cast<int16_t>(x / 16), static_cast<int16_t>(y / 16));
    if (c && *c != ZoneMapItems::kNoItem) { *c = ZoneMapItems::kNoItem; --z.laid; }
    if (refill) map_items_fill(zone, z);                 // sub_850570
}

// MapItemManager::PopItemByRandom (sub_850890). The type: rand_n(total) against the six type rates in
// order (sub_850EF0; none -> 7, "Invalid Map Item Type"); the item: rand_n(sum of the type's item rates
// (+ the quest boost)) against the items' cumulative rates in list order. Type 5 first adds Rate_Add
// (Type 3) times its rate to the first entry of each item an open quest needs (sub_850FA0 / sub_850FD0).
uint32_t cGameServer::map_item_pop(int16_t zone, const std::vector<uint32_t>& quest_items)
{
    const MapItemInfo* info = map_item_db_.get(zone);
    if (!info || info->total == 0) {                      // sub_6D8D60
        LOG_ERROR("MapItemManager::PopItemByRandom - Invalid MapItem Ini Info, ZoneId:%d", zone);
        return 0;
    }
    const int r = rand_n(info->total);
    int type = 7, acc = 0;
    for (int i = 0; i < 6; ++i) {
        const int rate = info->type_rate[i];
        if (acc <= r && rate + acc > r) { type = i; break; }
        acc += rate;
    }
    if (type == 7) {
        LOG_ERROR("MapItemManager::PopItemByRandom - Invalid Map Item Type");
        return 0;
    }
    std::vector<MapItemEntry> list = info->items[type];
    int extra = 0;
    if (type == 5) {
        const double add = static_cast<double>(map_item_db_.rate_add(3));
        for (uint32_t q : quest_items)
            for (MapItemEntry& e : list)
                if (e.item == q) {
                    const int v = e.rate;
                    extra += static_cast<int>(static_cast<double>(v) * add);
                    e.rate = static_cast<uint16_t>(e.rate + static_cast<int>(static_cast<double>(v) * add));
                    break;
                }
    }
    const int pick = rand_n(extra + info->item_sum[type]);
    int prev = 0;
    for (const MapItemEntry& e : list) {
        const int cur = prev + e.rate;
        if (prev <= pick && cur > pick) return e.item;
        prev = cur;
    }
    return 0;
}

// ─── The drill state ─────────────────────────────────────────────────────────

// sub_76ECB0 (equipment slot 7) + sub_60C610 (the item is in the bag).
GameClientSession::BagItem* GameClientSession::equipped_drill()
{
    return equip_[7] ? bag_find_uid(equip_[7]) : nullptr;
}

// C2S 0x002C (Player global case 44).
void GameClientSession::handle_drill_start(PacketBuffer& pkt)
{
    try {
        const uint16_t tx = pkt.read<uint16_t>();
        const uint16_t ty = pkt.read<uint16_t>();
        const uint8_t  mt = pkt.read<uint8_t>();
        if (dead_ || leaving_) return;
        if (rest_state_) return;                          // the rest states 8 / 15 swallow it
        if (exchange_) return;                            // so does the exchange shop state 18
        if (compound_) return;                            // and the compound state 13
        if (drill_.active) return;                        // and so does the drill state 3
        head_notice_end();                                // case 44: the conditions 2 / 3 / 8 end (megaphone.cpp)
        auto send_empty = [this](uint16_t op) { PacketBuffer p(static_cast<Opcode>(op), is_paula()); send(p); };
        const uint16_t self = static_cast<uint16_t>(object_id_);
        auto halt = [&](int32_t x, int32_t y) {          // S2C 0x0019 (sub_5F52F0) to the area
            PacketBuffer h(static_cast<Opcode>(0x0019), is_paula());
            h.write<uint16_t>(self);
            h.write<uint16_t>(static_cast<uint16_t>(x));
            h.write<uint16_t>(static_cast<uint16_t>(y));
            h.write<uint8_t>(mt);
            server_.broadcast_zone(zone_id_, h, 0);
        };
        // (sub_73C9C0 / sub_73CAF0 / sub_73CCD0 end a personal shop / trade — none here.)
        if (!equipped_drill()) {                          // sub_41C7D0(+0x848, 7) + sub_60C610
            send_empty(0x002E);                           // sub_5F4120
            return;
        }
        // The conditions (+0x118) 0x4E8 (e.g. Berserker): S2C 0x002E (sub_5F4120). (The form flags +1060 —
        // a transformed player: S2C 0x01E4 [16] / [22] — are not modelled here.)
        if (pcond_test(0x4E8)) {
            send_empty(0x002E);
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        int32_t px, py;
        cur_pos(now, px, py);
        const bool same_cell = static_cast<int16_t>(px) / 16 == static_cast<int16_t>(tx) / 16 &&
                               static_cast<int16_t>(py) / 16 == static_cast<int16_t>(ty) / 16;   // sub_76ED30
        const MapGrid* grid = server_.zone_grid(zone_id_);
        const bool walkable = !grid || grid->at(static_cast<int16_t>(tx) / 16, static_cast<int16_t>(ty) / 16) != 1;
        if (!same_cell && walkable) {
            // Player vtbl+100 MoveTo with the request's move type; state 3, +0x67C = the walk's end.
            end_attack(true);
            pick_ = Pickup{};
            shop_ = 0;
            wh_leave_state();
            skill_shop_leave();
            const uint8_t mtype = ((mt & 7) == 0) ? 2 : static_cast<uint8_t>(mt & 7);
            apply_move_type_speed(mtype);
            motion_.start(px, py, tx, ty, cur_speed_, now, mtype);
            pos_x_ = tx; pos_y_ = ty;
            if (auto* p = server_.entity_mgr().get_player(object_id_)) { p->pos_x = tx; p->pos_y = ty; }
            drill_ = Drill{};
            drill_.active = true;
            drill_.x = static_cast<int16_t>(tx);
            drill_.y = static_cast<int16_t>(ty);
            pet_speech(17);                               // state 3: our pet's line (@0x7459A3)
            send_empty(0x002D);                           // sub_5F4100
            broadcast_move(self, px, py, tx, ty, mt);     // sub_5F5240
            ensure_world_tick();
            LOG_DEBUG("GameClientSession %u: drill at (%u,%u): walk from (%d,%d)", session_id(), tx, ty, px, py);
            return;
        }
        if (same_cell) {
            end_attack(true);
            pick_ = Pickup{};
            shop_ = 0;
            wh_leave_state();
            skill_shop_leave();
            motion_.rest(px, py);
            pos_x_ = px; pos_y_ = py;
            if (auto* p = server_.entity_mgr().get_player(object_id_)) { p->pos_x = px; p->pos_y = py; }
            drill_ = Drill{};
            drill_.active = true;
            drill_.x = static_cast<int16_t>(px);          // +0x67C = our position
            drill_.y = static_cast<int16_t>(py);
            pet_speech(17);                               // (@0x7458EF)
            send_empty(0x002D);
            halt(px, py);
            LOG_DEBUG("GameClientSession %u: drill at (%d,%d)", session_id(), px, py);
            drill_on_stop_move();                         // event 30008
            return;
        }
        send_empty(0x002E);
        halt(px, py);
    } catch (...) {}
}

// The walk to the point ended: the move strategy posts 30008.
void GameClientSession::drill_tick(std::chrono::steady_clock::time_point now)
{
    if (!drill_.active || drill_.started) return;
    if (motion_.moving(now)) return;
    drill_on_stop_move();
}

// DrillState::OnStopMove (state 3, event 30008).
void GameClientSession::drill_on_stop_move()
{
    if (!drill_.active || drill_.started) return;         // +4: once
    int32_t px, py;
    cur_pos(std::chrono::steady_clock::now(), px, py);
    const int16_t cx = static_cast<int16_t>(static_cast<int16_t>(px) / 16);
    const int16_t cy = static_cast<int16_t>(static_cast<int16_t>(py) / 16);
    if (drill_.x / 16 != cx || drill_.y / 16 != cy) { drill_stop(1, -1, 0); return; }   // stopped elsewhere
    drill_.started = true;
    BagItem* drill = equipped_drill();
    if (!drill) { drill_stop(1, -1, 0); return; }
    // The Crazy Drill Mode (sub_7AFC20) digs at once: Player::DoDrill. (So would a premium drill.)
    if (cdm_enabled() && cdm_.active) { drill_do_crazy(); return; }
    const MapGrid* grid = server_.zone_grid(zone_id_);
    const uint8_t nature = grid ? grid->nature_of(drill_.x, drill_.y) : 0;
    if (nature == 6) { drill_stop(7, -1, 0); return; }   // "NotDigableGround"
    const bool sensed = rnd(100) < st_da_ + 10;           // sub_716A40: rand() % 100 < DA (+588) + 10
    const MapDef* md = server_.maps_db().get(zone_id_);
    if (sensed && !server_.map_item_has(zone_id_, drill_.x, drill_.y) &&
        (drill_cell_x_ != cx || drill_cell_y_ != cy)) {
        const DrillParam* dp = server_.drill_db().get(drill->item_id);   // sub_6DEAE0
        if (!dp) {
            LOG_ERROR("%s:DrillState:OnStopMove - drill paramext is null", char_name_);
            drill_stop(1, -1, 0);
            return;
        }
        if (!md || zone_id_ < 1 || zone_id_ >= 1000) {
            LOG_ERROR("%s:DrillState:OnStopMove - invalid shell type:%d or id:%d", char_name_, 1, zone_id_);
            drill_stop(1, -1, 0);
            return;
        }
        // sub_5E4CE0 kind 7 -> sub_5E74A0: the probe's EXP — the map's middle item depth x (DA + 200)
        // x 0.01, at least 1; AddExp; no level-up -> S2C 0x0026 [u32 EXP] (sub_5F4DA0).
        const int mid = (md->min_item_depth + md->max_item_depth) / 2;
        const float v = static_cast<float>(static_cast<double>(mid) * static_cast<double>(st_da_ + 200) *
                                           0.009999999776482582);
        const int32_t gain = (v < 1.0f) ? 1 : static_cast<int32_t>(v);
        if (!server_.party().reward(char_uid_, gain, 0, false) &&   // (30003: the party's share first)
            !add_exp(gain, true)) {
            PacketBuffer e(static_cast<Opcode>(0x0026), is_paula());
            e.write<uint32_t>(static_cast<uint32_t>(exp_));
            send(e);
        }
        cdm_add(server_.drill_db().stress_point(0));      // the Crazy Drill gauge (StressPoint 0)
        LOG_DEBUG("GameClientSession %u: drill probe of an empty cell -> EXP +%d", session_id(), gain);
    }
    const bool ps6 = has_ps_ability(6);                   // sub_5255F0(+4504, 6): skill 3001
    const uint32_t ms = server_.uptime_ms();
    drill_.start_ms = ms;                                 // +0xC44 (sub_435960)
    drill_.seed = static_cast<uint16_t>(ms & 0xFFFE);     // +0xC40 = sub_410150() & 0xFFFE
    drill_cell_x_ = cx; drill_cell_y_ = cy;               // +0xC48
    const int16_t map_depth  = md ? static_cast<int16_t>(md->max_item_depth) : 0;   // ZoneMapAttr +8
    const int16_t item_depth = server_.map_item_depth(zone_id_, drill_.x, drill_.y); // sub_850110 (-1)
    const uint16_t self = static_cast<uint16_t>(object_id_);
    // S2C 371 (sub_5F5B90) to us.
    {
        PacketBuffer p(static_cast<Opcode>(0x0173), is_paula());
        p.write<uint16_t>(self);
        p.write<uint16_t>(static_cast<uint16_t>(px));
        p.write<uint16_t>(static_cast<uint16_t>(py));
        p.write<uint8_t>(0);
        p.write<uint8_t>(static_cast<uint8_t>(map_depth / 5));
        p.write<uint8_t>(static_cast<uint8_t>(item_depth / 5));
        p.write<uint8_t>(nature);
        p.write<uint16_t>(static_cast<uint16_t>((sensed ? 1 : 0) | (drill_.seed & 0xFFFE)));
        p.write<uint8_t>(static_cast<uint8_t>(cdm_gauge_percent()));   // the Crazy Drill gauge %
        send(p);
    }
    // S2C 47 to the area.
    {
        PacketBuffer q(static_cast<Opcode>(0x002F), is_paula());
        q.write<uint16_t>(self);
        q.write<uint16_t>(static_cast<uint16_t>(px));
        q.write<uint16_t>(static_cast<uint16_t>(py));
        q.write<uint8_t>(0);
        q.write<uint8_t>(static_cast<uint8_t>(map_depth / 5));
        q.write<uint8_t>(static_cast<uint8_t>(item_depth / 5));
        q.write<uint8_t>(nature);
        q.write<uint8_t>(ps6 ? 1 : 0);
        server_.broadcast_zone(zone_id_, q, 0);
    }
    LOG_DEBUG("GameClientSession %u: drill %u digs (%d,%d): nature %u, map depth %d, item depth %d%s",
              session_id(), drill->item_id, px, py, nature, map_depth, item_depth, sensed ? " [sensed]" : "");
}

// sub_75C4A0: the end of a dig.
void GameClientSession::drill_stop(int reason, int life, uint32_t item)
{
    if (life == -1) {
        const BagItem* d = equipped_drill();
        life = d ? d->count : 0;
    }
    // (sub_7B3B60: the server's statistics.) An interrupted dig feeds the Crazy Drill gauge.
    if (reason == 10) cdm_add(server_.drill_db().stress_point(3));
    {
        PacketBuffer p(static_cast<Opcode>(0x0032), is_paula());   // sub_5F5DA0
        p.write<uint8_t>(static_cast<uint8_t>(reason));
        p.write<uint16_t>(static_cast<uint16_t>(life));
        p.write<uint32_t>(item);
        send(p);
    }
    {
        PacketBuffer q(static_cast<Opcode>(0x0033), is_paula());   // sub_5F5E30
        q.write<uint16_t>(static_cast<uint16_t>(object_id_));
        q.write<uint8_t>(static_cast<uint8_t>(reason));
        server_.broadcast_zone(zone_id_, q, 0);
    }
    drill_ = Drill{};                                     // sub_414B50(this, 0)
    LOG_DEBUG("GameClientSession %u: drill stop %d (life %d, item %u)", session_id(), reason, life, item);
}

// C2S 0x0030 (state 3 case 48): the client cancelled — [u16 cnt][u16] when it had dug.
void GameClientSession::handle_drill_cancel(PacketBuffer& pkt)
{
    try {
        if (!drill_.active) return;                       // other states swallow it
        int cnt = 0;
        if (pkt.size() >= pkt.rpos() + 4) {
            cnt = pkt.read<uint16_t>();
            (void)pkt.read<uint16_t>();
        }
        BagItem* drill = equipped_drill();
        if (!drill) { drill_stop(1, 0, 0); return; }
        const DrillParam* dp = server_.drill_db().get(drill->item_id);
        const MapGrid* grid = server_.zone_grid(zone_id_);
        const uint8_t nature = grid ? grid->nature_of(drill_.x, drill_.y) : 0;
        int life = drill->count;
        life -= (!dp || dp->ground_nature == 0 || nature == dp->ground_nature) ? cnt : 2 * cnt;
        if (life <= 0) life = 0;
        drill->count = static_cast<uint16_t>(life);
        server_.item_store().SetAmount(drill->uid, drill->count);   // sub_765730 (the delayed save)
        drill_stop(1, drill->count, 0);
    } catch (...) {}
}

// C2S 0x0031 (state 3 case 49, CMD_END_DRILL).
void GameClientSession::handle_drill_end(PacketBuffer& pkt)
{
    try {
        if (!drill_.active) return;
        const size_t avail = (pkt.size() > pkt.rpos()) ? pkt.size() - pkt.rpos() : 0;
        if (avail < 8) {
            LOG_ERROR("%s CMD_END_DRILL: msg size is illegal %zu", char_name_, avail);
            drill_stop(2, -1, 0);
            return;
        }
        const uint16_t cnt   = pkt.read<uint16_t>();      // cntStarting
        (void)pkt.read<uint16_t>();
        const uint16_t depth = pkt.read<uint16_t>();      // curDepth
        const uint16_t crc   = pkt.read<uint16_t>();
        BagItem* drill = equipped_drill();
        if (!drill) { drill_stop(2, 0, 0); return; }
        const DrillParam* dp = server_.drill_db().get(drill->item_id);
        if (!dp) {                                        // (every drill has its row: CheckValidItems)
            LOG_ERROR("%s CMD_END_DRILL: item %u has no Item_DrillParam row", char_name_, drill->item_id);
            drill_stop(2, 0, 0);
            return;
        }
        const MapGrid* grid = server_.zone_grid(zone_id_);
        const uint8_t nature = grid ? grid->nature_of(drill_.x, drill_.y) : 0;
        const MapDef* md = server_.maps_db().get(zone_id_);
        const int map_depth = md ? static_cast<int16_t>(md->max_item_depth) : 0;
        const int c = depth;
        // The CRC is keyed with the CLIENT's own Item_DrillParam: the server's row, or a row of
        // db/drill_client_db.yml for this drill (a Season 2 client) — DrillDb::keyed_with. The row that
        // matches also gives the depth limit and the shortest dig time below; the life, the ground and the
        // Grade stay the server's.
        DrillCheck chk;
        const bool crc_ok = server_.drill_db().keyed_with(*dp, crc, c, cnt, drill_.seed, chk);
        const uint16_t calc = crc_ok ? drill_.seed
                                     : DrillDb::result_calc(DrillCheck{ "server", dp->dig_depth, dp->bar_eff_area,
                                                                        dp->bar_speed, dp->scoop_mass }, crc, c, cnt);
        if (crc_ok && chk.source != "server")
            LOG_DEBUG("%s CMD_END_DRILL: drill %u keyed with the %s data (DigDepth %d, BarEffArea %d, BarSpeed %d, "
                      "ScoopMass %d)", char_name_, drill->item_id, chk.source.c_str(), chk.dig_depth, chk.bar_eff_area,
                      chk.bar_speed, chk.scoop_mass);
        if (calc != drill_.seed) {
            LOG_ERROR("%s CMD_END_DRILL: crc is invalid %u : org %u, cntStarting:%d, curDepth:%d, dp.digDepth:%d, "
                      "mapDepth:%d, drillId:%u, map:%d", char_name_, calc, drill_.seed, cnt, c, dp->dig_depth,
                      map_depth, drill->item_id, zone_id_);
            close();                                      // sub_8AA290: the session is cut
            return;
        }
        drill_.seed = 0;
        if (cnt == 0 || c > chk.dig_depth || c > map_depth) {
            LOG_ERROR("%s : invalid drill result : cntStarting:%d, curDepth:%d, dp.digDepth:%d, mapDepth:%d, "
                      "drillId:%u, map:%d", char_name_, cnt, c, chk.dig_depth, map_depth, drill->item_id, zone_id_);
            close();
            return;
        }
        drill_depth_sum_ += c;                            // +0x1414
        if (drill->count == 0) { drill_stop(6, 0, 0); return; }   // "EmptyDrillLife"
        int life = drill->count;
        life -= (dp->ground_nature == 0 || nature == dp->ground_nature) ? cnt : 2 * cnt;
        if (life <= 0) life = 0;
        drill->count = static_cast<uint16_t>(life);
        server_.item_store().SetAmount(drill->uid, drill->count);   // sub_765730
        // (sub_894750: the drill-event zones; sub_76AE60: the event pet — none here.)
        const int16_t x = drill_.x, y = drill_.y;
        const int grade = dp->grade;
        if (!server_.map_item_has(zone_id_, x, y)) {
            drill_stop(2, drill->count, 0);               // "Nothing"
            cdm_add(server_.drill_db().stress_point(2));  // (sub_7AFE20)
            drill_exp(grade, c);
            return;
        }
        const uint32_t item = server_.map_item_pop(zone_id_, quest_items());   // (+ the open quests' items)
        const int depth5 = (server_.map_item_depth(zone_id_, x, y) / 5) * 5;
        if (!item) {
            if (c == map_depth) {
                cdm_add(server_.drill_db().stress_point(2));
                drill_stop(2, drill->count, 0);
            } else {
                drill_stop(4, drill->count, 0);
            }
            drill_exp(grade, c);
            return;
        }
        if (c > depth5) { drill_stop(2, drill->count, 0); return; }   // dug past it (no EXP)
        if (c != depth5) {                                // "LowDrillGrade": not deep enough
            drill_stop(4, drill->count, 0);
            drill_exp(grade, c);
            return;
        }
        // The find. The shortest dig time first (a faster report is a hack: logged, the session cut,
        // and the find still made — the retail goes on).
        DrillParam keyed = *dp;                           // (with the client's gauge values the CRC matched)
        keyed.bar_eff_area = chk.bar_eff_area;
        keyed.bar_speed    = chk.bar_speed;
        keyed.scoop_mass   = chk.scoop_mass;
        const uint32_t predicted = static_cast<uint32_t>(shortest_dig_ms(keyed, depth5, map_depth));
        const uint32_t elapsed = server_.uptime_ms() - drill_.start_ms;
        if (elapsed < predicted) {
            LOG_ERROR("Drill Hack Detected ! => Name : %s, ChrUid: %u, PreDictTime : %u, MessageTime : %u",
                      char_name_, char_uid_, predicted, elapsed);
            close();                                      // sub_6042C0(this, 2) + sub_8AA290
        }
        ++drill_finds_;                                   // +0x1418
        // A drill comes with its life (sub_625B60 -> DrillLife), anything else one. (The world's timed
        // drop events — none are set — could replace the find.)
        uint16_t amount = 1;
        if (const DrillParam* fp = server_.drill_db().get(item)) amount = static_cast<uint16_t>(fp->drill_life);
        const ItemDef* it = server_.item_db().get(item);
        int32_t px, py;
        cur_pos(std::chrono::steady_clock::now(), px, py);
        GroundItemEntity g;
        g.item_id    = item;
        g.uid        = 0;
        g.count      = amount;
        g.kind       = static_cast<uint8_t>(it ? it->cls : 1);
        g.dropper    = static_cast<uint16_t>(object_id_);
        g.owner_oid  = object_id_;                        // ours (state 1 for 30 s)
        g.owned      = true;
        g.event_flag = 0;
        g.drop_flag  = true;                              // the init struct +0x24 = 1
        server_.pop_up_item(zone_id_, static_cast<uint16_t>(object_id_), px, py, g);   // sub_7FEF10
        LOG_DEBUG("GameClientSession %u: drill finds %u x%u (%s) at depth %d", session_id(), item, amount,
                  it ? it->name.c_str() : "?", c);
        cdm_add(server_.drill_db().stress_point(1));      // the Crazy Drill gauge (StressPoint 1)
        drill_stop(3, -1, item);                          // "DiggedOut"
        // TM EXP (TM below 500): 4 * (3 * Lv + depth) * Grade (+0x14EC); AddTMExp; no TM level-up ->
        // S2C 0x011D [u8 ftype][u32 TM EXP] (sub_5ED650(this, +1028)). (An auto-drill multiplies it.)
        if (tm_level_ < 500) {
            const int32_t tm = 4 * (3 * level_ + c) * grade;
            if (!add_tm_exp(tm, true)) {
                PacketBuffer p(static_cast<Opcode>(0x011D), is_paula());
                p.write<uint8_t>(f_type_);
                p.write<uint32_t>(static_cast<uint32_t>(tm_exp_));
                send(p);
            }
        }
        server_.map_item_remove(zone_id_, x, y, true);    // sub_8504E0: and another one is laid
        drill_exp(grade, c);
    } catch (...) {}
}

// State 3, event 30002: a hit (or a miss) while drilling ends the dig — unless the player has
// PSAbility 6 (skill 3001; or a premium drill with option 0x400).
void GameClientSession::drill_interrupted()
{
    if (!drill_.active) return;
    if (!equipped_drill()) { drill_stop(1, -1, 0); return; }
    if (has_ps_ability(6)) return;
    drill_stop(10, -1, 0);                                // (+ the CDM gauge StressPoint 3; sub_764C10)
}

// Event 30003 kind 6 (Player @0x7431C8): sub_5E4CE0 -> sub_5E7450 = ExpRate(Grade) * depth *
// (Lv + 400) * 0.0005, at least 1; AddExp; no level-up -> S2C 0x0406 [u32 gain] (sub_5F4E10).
void GameClientSession::drill_exp(int grade, int depth)
{
    if (dead_) return;
    const float v = static_cast<float>(server_.drill_db().grade_exp_rate(grade) * static_cast<double>(depth) *
                                       static_cast<double>(level_ + 400) * 0.0005000000237487257);
    const int32_t gain = (v < 1.0f) ? 1 : static_cast<int32_t>(v);
    if (server_.party().reward(char_uid_, gain, 0, false)) return;   // sub_716F40: the party's share group
    if (!add_exp(gain, true)) {
        PacketBuffer p(static_cast<Opcode>(0x0406), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(gain));
        send(p);
    }
    LOG_DEBUG("GameClientSession %u: drill EXP +%d (grade %d, depth %d)", session_id(), gain, grade, depth);
}

// ─── The Crazy Drill Mode (CCrazyDrillMgr, Player +6212) ────────────────────
//  Enabled by the wedding flag (+60, sub_7B0490) or the passive skill 5019 "크레이지 발굴" (the
//  ESLimiter +4504 holds it, sub_54BA80; its card 31019 is quest 2373's reward). The digs fill the
//  gauge with StressPointInfo points (+ the drill's CDM_StressBonus; the Boiling buff, ESAction 1391,
//  multiplies them by 1 + StressUp); full -> CDM_Seconds (x 1 + the level's Ratio of skill 3111) of
//  instant digs (Player::DoDrill) with the BigBang. S2C 0x0032 reasons: 11 start [secs], 12 / 13
//  remaining [secs] at a half / a quarter (durations of 40 s+ / 60 s+), 14 at 5 s, 15 a map without
//  it (MapInfoEx Option 512), 16 quit in a town / indoor / mini-game zone (Type 1 / 5 / 6), 17 quit by
//  equipping another drill, 8 a find, 9 nothing (or the 200 ms between digs).

namespace {
constexpr int   kCdmDigCooldownMs = 200;       // sub_79EF50: CCrazyDrillMgr +24
constexpr float kCdmDefaultMax    = 120.0f;    // sub_7AFB80 / sub_7AFFC0 defaults
constexpr float kCdmDefaultStress = 0.0099999998f;
constexpr int   kCdmDefaultMs     = 30000;
constexpr float kCdmDefaultBang   = 0.050000001f;
}

bool GameClientSession::cdm_enabled() const
{
    return cdm_.wedding || find_skill(5019) != nullptr;
}

// sub_5F5B90: (char)(int)(gauge / max * 100.0).
int GameClientSession::cdm_gauge_percent() const
{
    if (cdm_.gauge_max == 0.0f) return 0;
    return static_cast<int>(static_cast<double>(cdm_.gauge) / static_cast<double>(cdm_.gauge_max) * 100.0);
}

// sub_7AFC80: a dig's points. Nothing while active or for a point not below the max; the Boiling
// buff (ESAction 1391 in the ESLimiter, vtbl+80) takes the boosted branch unless the wedding flag is
// set. Full -> sub_7AFA20(this, 1). (An auto-drill — Player +0x668 bit 2 — adds nothing; none here.)
void GameClientSession::cdm_add(float add)
{
    if (!cdm_enabled() || cdm_.active || !(static_cast<double>(add) < static_cast<double>(cdm_.gauge_max))) return;
    if (cdm_.wedding || !es_find(1391)) {
        cdm_.gauge = cdm_.gauge + add;
        cdm_.gauge = cdm_.gauge + cdm_.stress_bonus;
        if (cdm_.wedding)
            if (const BagItem* d = equipped_drill())
                if (const DrillParam* dp = server_.drill_db().get(d->item_id)) cdm_.gauge = cdm_.gauge + dp->wedding_stress;
    } else {
        const float v = static_cast<float>((static_cast<double>(add) + static_cast<double>(cdm_.stress_bonus)) *
                                           (static_cast<double>(cdm_.boost) + 1.0));
        cdm_.gauge = cdm_.gauge + v;
    }
    if (static_cast<double>(cdm_.gauge_max) <= static_cast<double>(cdm_.gauge)) cdm_activate(1);
}

// sub_7AFA20: active, the gauge empty; with `how` the drill's values (sub_7AFFC0), the thresholds
// (a half / a quarter of the time) and their warnings by the duration, and (how 1) the start to
// announce on the next tick.
void GameClientSession::cdm_activate(int how)
{
    if (!cdm_enabled()) return;
    cdm_.active      = true;
    cdm_.gauge       = 0.0f;
    cdm_.cooldown_ms = 0;
    if (!how) return;
    cdm_setup();
    cdm_.duration_ms = cdm_.remain_ms;
    cdm_.half_ms     = cdm_.duration_ms >> 1;
    cdm_.quarter_ms  = cdm_.duration_ms >> 2;
    if (how == 1) cdm_.announce = true;
    const int d = cdm_.duration_ms;
    if (d >= 60000)      { cdm_.warn_quarter = true;  cdm_.warn_half = true;  }
    else if (d >= 40000) { cdm_.warn_quarter = false; cdm_.warn_half = true;  }
    else if (d >= 20000) { cdm_.warn_quarter = false; cdm_.warn_half = false; }
    // (below 20 s both stay as they were)
    cdm_.warn_5s = true;
    LOG_DEBUG("GameClientSession %u: Crazy Drill Mode on for %d ms (BigBang %.3f)", session_id(), cdm_.remain_ms,
              static_cast<double>(cdm_.ratio_bang));
}

// sub_7AFFC0: the equipped drill's (slot 7) CDM_GaugeMax (<= 0 -> 120), CDM_StressBonus (<= 0 -> 0.01),
// 1000 * CDM_Seconds (x 1 + skill 3111's Ratio at its level; <= 0 -> 30000), CDM_RatioBang (outside
// (0, 1] -> 0.05; the wedding adds Wedding_RatioBang); no drill -> the defaults.
void GameClientSession::cdm_setup()
{
    if (!cdm_enabled()) return;
    const BagItem* drill = equipped_drill();
    const DrillParam* dp = drill ? server_.drill_db().get(drill->item_id) : nullptr;
    if (!dp) {
        cdm_.gauge_max    = kCdmDefaultMax;
        cdm_.stress_bonus = kCdmDefaultStress;
        cdm_.remain_ms    = kCdmDefaultMs;
        cdm_.ratio_bang   = kCdmDefaultBang;
        return;
    }
    cdm_.gauge_max = dp->cdm_gauge_max;
    if (cdm_.gauge_max <= 0.0f) cdm_.gauge_max = kCdmDefaultMax;
    cdm_.stress_bonus = dp->cdm_stress_bonus;
    if (cdm_.stress_bonus <= 0.0f) cdm_.stress_bonus = kCdmDefaultStress;
    cdm_.remain_ms = 1000 * dp->cdm_seconds;
    if (const LearnedSkill* s = find_skill(3111); s && s->level > 0) {   // "Mega Crazy Drilling"
        const SkillDef* sd = server_.skill_db().get(3111);
        const EsaRow* row = sd ? server_.esa_db().row(sd->esa_table, s->level) : nullptr;
        const float r = row ? static_cast<float>(row->get("Ratio", 0.0)) : 0.0f;
        if (r > 0.0f)
            cdm_.remain_ms = static_cast<int32_t>(static_cast<double>(cdm_.remain_ms) * (static_cast<double>(r) + 1.0));
    }
    if (cdm_.remain_ms <= 0) cdm_.remain_ms = kCdmDefaultMs;
    cdm_.ratio_bang = dp->cdm_ratio_bang;
    if (cdm_.ratio_bang <= 0.0f || cdm_.ratio_bang > 1.0f) cdm_.ratio_bang = kCdmDefaultBang;
    if (cdm_.wedding) cdm_.ratio_bang = cdm_.ratio_bang + dp->wedding_ratio_bang;
}

// sub_7AFE50, on every Player tick: the start's announcement (sub_7AF890: S2C 0x0032 [11][secs] and
// S2C 0x0033 [11] to the area), the time, then the zone check (16) or one warning (12 / 13 / 14);
// out of time -> off, silently (the client times its own end).
void GameClientSession::cdm_tick(int elapsed_ms)
{
    if (!cdm_.active) return;
    auto result50 = [this](uint8_t reason, int secs) {   // sub_5F5DA0
        PacketBuffer p(static_cast<Opcode>(0x0032), is_paula());
        p.write<uint8_t>(reason);
        p.write<uint16_t>(static_cast<uint16_t>(secs));
        p.write<uint32_t>(0);
        send(p);
    };
    if (cdm_.announce) {
        cdm_.announce = false;
        result50(11, cdm_.remain_ms / 1000);
        PacketBuffer q(static_cast<Opcode>(0x0033), is_paula());   // sub_5F5E30
        q.write<uint16_t>(static_cast<uint16_t>(object_id_));
        q.write<uint8_t>(11);
        server_.broadcast_zone(zone_id_, q, 0);
    }
    cdm_.remain_ms   -= elapsed_ms;
    cdm_.cooldown_ms -= elapsed_ms;
    if (cdm_.remain_ms <= 0) {
        cdm_.remain_ms = 0;
        cdm_.active    = false;
        LOG_DEBUG("GameClientSession %u: Crazy Drill Mode over", session_id());
        return;
    }
    const MapDef* md = server_.maps_db().get(zone_id_);
    const int type = md ? md->type : 0;                   // the zone's +16 (MapInfoEx Type)
    if (type == 1 || type == 5 || type == 6) {
        result50(16, 0);                                  // "QuitByMapMove"
        cdm_.remain_ms = 0;
        cdm_.active    = false;
    } else if (cdm_.warn_half && cdm_.remain_ms <= cdm_.half_ms) {
        cdm_.warn_half = false;
        result50(12, cdm_.half_ms / 1000);
    } else if (cdm_.warn_quarter && cdm_.remain_ms <= cdm_.quarter_ms) {
        cdm_.warn_quarter = false;
        result50(13, cdm_.quarter_ms / 1000);
    } else if (cdm_.warn_5s && cdm_.remain_ms <= 5000) {
        cdm_.warn_5s = false;
        result50(14, 5);
    }
}

// The player build after a zone change (sub_40D940 -> sub_7B0520): the state the left zone sent along
// (cGameServer::cdm_put at leave_field) — running: its time, thresholds and warnings (no new
// announcement; the drill's values stay the defaults until it is equipped again); else its gauge.
void GameClientSession::cdm_restore_carry()
{
    cGameServer::CdmCarry c;
    if (!server_.cdm_take(char_uid_, c)) return;
    cdm_.drill_uid = c.drill_uid;
    if (c.active) {
        cdm_.remain_ms    = c.remain_ms;
        cdm_.half_ms      = c.half_ms;
        cdm_.quarter_ms   = c.quarter_ms;
        cdm_.warn_half    = c.warn_half;
        cdm_.warn_quarter = c.warn_quarter;
        cdm_.warn_5s      = c.warn_5s;
        cdm_.gauge        = 0.0f;
        cdm_.active       = true;
        cdm_.cooldown_ms  = 0;
    } else {
        cdm_.remain_ms = 0;
        cdm_.gauge     = c.gauge;
    }
    LOG_DEBUG("GameClientSession %u: Crazy Drill state carried (%s, gauge %.2f, %d ms)", session_id(),
              cdm_.active ? "active" : "idle", static_cast<double>(cdm_.gauge), cdm_.remain_ms);
}

// Player::OnDie -> sub_7B0230: off; the gauge + StressPoint 4 (-999), within [0, max - 1].
void GameClientSession::cdm_on_death()
{
    if (!cdm_enabled()) return;
    cdm_.active = false;
    cdm_.gauge  = static_cast<float>(static_cast<double>(server_.drill_db().stress_point(4)) +
                                     static_cast<double>(cdm_.gauge));
    if (cdm_.gauge < 0.0f) cdm_.gauge = 0.0f;
    if (cdm_.gauge_max < cdm_.gauge) cdm_.gauge = cdm_.gauge_max - 1.0f;
    cdm_.remain_ms   = 0;
    cdm_.cooldown_ms = 0;
}

// DrillItemHelper::OnEquip (sub_6E6EF0): another drill than the gauge's (+40) while the gauge has
// something or the mode runs -> sub_7B02F0 (reset, sub_7AFB80, + S2C 0x0032 [17] "QuitByEquipDrill");
// then SetCrazyInfo (sub_7B04B0): its uid, CDM_StressBonus, Grade and CDM_GaugeMax.
void GameClientSession::cdm_on_equip_drill(const BagItem& drill)
{
    const uint32_t uid = static_cast<uint32_t>(drill.uid);
    if (uid != cdm_.drill_uid && (cdm_.gauge > 0.0f || (cdm_enabled() && cdm_.active)) && cdm_enabled()) {
        cdm_ = CrazyDrill{};
        PacketBuffer p(static_cast<Opcode>(0x0032), is_paula());
        p.write<uint8_t>(17);
        p.write<uint16_t>(0);
        p.write<uint32_t>(0);
        send(p);
        LOG_DEBUG("GameClientSession %u: Crazy Drill reset — another drill equipped", session_id());
    }
    const DrillParam* dp = server_.drill_db().get(drill.item_id);
    if (!dp) { LOG_ERROR("CCrazyDrillMgr::SetCrazyInfo - invalid drill"); return; }
    cdm_.drill_uid    = uid;
    cdm_.stress_bonus = dp->cdm_stress_bonus;
    cdm_.grade        = dp->grade;
    cdm_.gauge_max    = dp->cdm_gauge_max;
}

// Player::DoDrill (sub_769640): a dig of the Crazy Drill Mode, at once. (The premium auto-drill
// branches — Player +0x668 bit 0 with a drill of OptionsEx 0x400: no cool-down, no life, stop 19,
// items without an owner, S2C 1206 — have no source here.)
void GameClientSession::drill_do_crazy()
{
    // (sub_41C5D0: the action strategy stops — we stand; an auto-drill player leaves here)
    const MapDef* md = server_.maps_db().get(zone_id_);
    if (md && (md->option & 512)) {                       // "NoCrazyModeMap": S2C 0x0032 [15], state 0
        PacketBuffer p(static_cast<Opcode>(0x0032), is_paula());
        p.write<uint8_t>(15);
        p.write<uint16_t>(0);
        p.write<uint32_t>(0);
        send(p);
        drill_ = Drill{};
        return;
    }
    BagItem* drill = equipped_drill();
    if (!drill) { drill_stop(2, 0, 0); return; }
    if (cdm_.cooldown_ms > 0) { drill_stop(9, drill->count, 0); return; }
    cdm_.cooldown_ms = kCdmDigCooldownMs;
    const DrillParam* dp = server_.drill_db().get(drill->item_id);
    if (!dp) { drill_stop(2, 0, 0); return; }
    const int16_t x = drill_.x, y = drill_.y;
    const MapGrid* grid = server_.zone_grid(zone_id_);
    const uint8_t nature = grid ? grid->nature_of(x, y) : 0;
    const int map_depth = md ? static_cast<int16_t>(md->max_item_depth) : 0;
    const int life = drill->count - ((dp->ground_nature && nature != dp->ground_nature) ? 2 : 1);
    if (life <= 0) {
        drill->count = 0;
        server_.item_store().SetAmount(drill->uid, 0);
        drill_stop(6, 0, 0);
        return;
    }
    drill->count = static_cast<uint16_t>(life);
    server_.item_store().SetAmount(drill->uid, drill->count);       // sub_765730
    if (!server_.map_item_has(zone_id_, x, y)) { drill_stop(9, drill->count, 0); return; }
    const int16_t depth = server_.map_item_depth(zone_id_, x, y);
    const uint32_t item = server_.map_item_pop(zone_id_, quest_items());
    if (dp->dig_depth < map_depth) { drill_stop(4, drill->count, 0); return; }   // reaches not the map's depth
    drill_depth_sum_ += depth;
    if (!item) { drill_stop(9, drill->count, 0); return; }
    ++drill_finds_;
    uint16_t amount = 1;
    if (const DrillParam* fp = server_.drill_db().get(item)) amount = static_cast<uint16_t>(fp->drill_life);
    const int grade = dp->grade;
    drill_stop(8, -1, item);                              // "CrazyDrillSucess", then the item pops up
    int32_t px, py;
    cur_pos(std::chrono::steady_clock::now(), px, py);
    {
        const ItemDef* it = server_.item_db().get(item);
        GroundItemEntity g;
        g.item_id    = item;
        g.count      = amount;
        g.kind       = static_cast<uint8_t>(it ? it->cls : 1);
        g.dropper    = static_cast<uint16_t>(object_id_);
        g.owner_oid  = object_id_;
        g.owned      = true;
        g.drop_flag  = true;
        server_.pop_up_item(zone_id_, static_cast<uint16_t>(object_id_), px, py, g);   // sub_7FEF10
    }
    // TM (TM below 500): Grade * (12 * Lv + 40) (+5356); AddTMExp; no level-up -> S2C 0x011D.
    if (tm_level_ < 500) {
        const int32_t tm = grade * (12 * level_ + 40);
        if (!add_tm_exp(tm, true)) {
            PacketBuffer p(static_cast<Opcode>(0x011D), is_paula());
            p.write<uint8_t>(f_type_);
            p.write<uint32_t>(static_cast<uint32_t>(tm_exp_));
            send(p);
        }
    }
    server_.map_item_remove(zone_id_, x, y, true);        // sub_8504E0
    drill_exp(grade, 10);                                 // event 30003 (6, Grade, 10)
    // The BigBang: CDM_RatioBang (+56) >= rand() / 32767.0 -> up to 10 more items from the buried
    // ones of the 7 x 7 cells from (cell - 3) (25 random tries, each cell once); a failed roll of what
    // one holds ends it all (the retail returns — nothing of the batch drops). The finds leave their
    // cells (sub_850530), the map lays new ones once (sub_850570), and they fly out (sub_7FE670).
    const double roll = static_cast<double>(std::rand()) / 32767.0;
    if (!(static_cast<double>(cdm_.ratio_bang) >= roll)) return;
    int cx = static_cast<int16_t>(x / 16), cy = static_cast<int16_t>(y / 16);
    cx = (cx <= 3) ? 0 : cx - 3;
    cy = (cy <= 3) ? 0 : cy - 3;
    const int max_x = grid ? 16 * grid->w : 0;
    const int max_y = grid ? 16 * grid->h : 0;
    std::vector<cGameServer::BigBangFind> finds;
    bool used[49] = {};
    for (int i = 0; i < 25; ++i) {
        const int dx = std::rand() % 7;
        const int dy = std::rand() % 7;
        const int tx = 16 * (cx + dx), ty = 16 * (cy + dy);
        if (tx <= max_x && ty <= max_y) {
            if (server_.map_item_has(zone_id_, static_cast<int16_t>(tx), static_cast<int16_t>(ty)) &&
                !used[7 * dx + dy]) {
                const uint32_t it = server_.map_item_pop(zone_id_, quest_items());
                if (!it) return;
                finds.push_back({ it, static_cast<int16_t>(tx), static_cast<int16_t>(ty) });
                used[7 * dx + dy] = true;
            }
            if (finds.size() >= 10) break;
        }
    }
    // (none found: still the refill and S2C 1074 with no item — the client plays the BigBang anyway)
    for (const cGameServer::BigBangFind& f : finds) server_.map_item_remove(zone_id_, f.from_x, f.from_y, false);
    server_.map_items_refill(zone_id_);
    server_.bigbang_drop(zone_id_, static_cast<uint16_t>(object_id_), px, py, object_id_, finds, cdm_.wedding);
    LOG_DEBUG("GameClientSession %u: Crazy Drill BigBang — %zu items", session_id(), finds.size());
}
