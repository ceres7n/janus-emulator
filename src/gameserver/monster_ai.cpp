#include "game_server.hpp"
#include "game_session.hpp"
#include "logger.hpp"
#include "combat_util.hpp"
#include "paula_crypt.hpp"
#include <algorithm>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
//  Monster AI — the idle side of the retail CommonMonster (state 0 of OnProcessState sub_729E20):
//  its NormalWalkStrategy (+1640) and, for an AggressiveMonster, its AggressiveAttackStrategy
//  (+332, id 3). A monster in combat (target_oid) is run by its target's session (the
//  NormalBattleFollowingStrategy, game_session.cpp).
//
//  The class by MonsterParamEx2 Type (MonsterManager::CreateChr sub_858E40): Type 0 / 11 =
//  TestMonster (sub_7379F0: attack strategy 2 = PassiveAttackStrategy — it only fights back),
//  Type 1 = AggressiveMonster (sub_721D50: view range +432 = 200, attack strategy 3).
//
//  Sectors: the Paula GameServer.ini sets no SectorWidth / SectorHeight, so a zone is ONE sector of
//  0x7FFF (sub_7EDC50 -> sub_7EFDA0): a player anywhere in the zone makes its monsters "near"
//  (sub_714A50 -> the sector's player count); no MoveRatio either (0), so the monsters of a zone
//  without players never wander. Only the zones with players are run.
//
//  NormalWalkStrategy (sub_4331E0; init sub_4330E0: delays 3000..6000 ms, move type 1 = walk;
//  start sub_433120: the timer = 3000, state 1):
//    state 1 — the timer counts down (a player near); at 0: the destination (CommonMonster
//      vtbl+272 sub_72ADA0, the monster has no area: +1660 = 0): an offset of rand(60..250) on each
//      axis (y rolled first), each sign at random; the 32 x 32 box (±32) around it meeting a portal
//      of the zone (sub_810B90 / sub_804070) reverses the offset; then clamped into the map bounds
//      (sub_71B050). The move (sub_4341F0 -> MoveTo vtbl+100 with the walk speed): S2C 0x18 to the
//      area and state 2; a refused move: S2C 0x19 [id][pos][type] (sub_5F52F0) and the timer =
//      rand(1000..2000).
//    state 2 — on the arrival (30008): the timer = rand(3000..6000), state 1.
//    (The arrival also posts 30014 to the monster, whose handler sends S2C 63 [u16 id][u8 0] to the
//    area — not sent: its client side is not verified yet.)
//  The retail MoveTo is a path search on the zone grid (PathFindingMoveStrategy sub_42BC10 ->
//  sub_415C60); here the move is refused when the destination cell is blocked and is otherwise a
//  straight segment (no path search yet).
//
//  AggressiveAttackStrategy (sub_424660), idle state: every 500 ms the first player of the sector
//  list (sub_4279F0) that is not dead, in the world and within the view range (squared distance
//  <= range², zone vtbl+60 sub_7FE100) — after the Type-12 monsters it would also take
//  (sub_427DA0) — gets 30001: the monster adds it to its hate list (5000 ms) and enters combat.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
constexpr int32_t kWalkMinDelayMs = 3000;   // NormalWalkStrategy +52 (sub_4330E0)
constexpr int32_t kWalkMaxDelayMs = 6000;   // +56
constexpr int32_t kAggroScanMs    = 500;    // AggressiveAttackStrategy: +8 >= 500

bool paula_wire() { return janus::PaulaCrypt::Instance().IsLoaded(); }

// sub_420AB0(a, b) = a + rand() % (b - a)
int rand_between(int a, int b) { return a + std::rand() % (b - a); }

// A fault that persists throws on every 100 ms tick: the first one is logged, then at most one line a minute
// with how many were left out in between. (Only the zone tick calls it, one call at a time.)
void log_tick_fault(const char* what)
{
    static std::chrono::steady_clock::time_point last{};
    static unsigned suppressed = 0;
    const auto now = std::chrono::steady_clock::now();
    if (last != std::chrono::steady_clock::time_point{} && now - last < std::chrono::seconds(60)) {
        ++suppressed;
        return;
    }
    if (suppressed)
        LOG_ERROR("cGameServer::monster_ai_tick aborted: %s (%u more since the last line)", what, suppressed);
    else
        LOG_ERROR("cGameServer::monster_ai_tick aborted: %s", what);
    last       = now;
    suppressed = 0;
}
}  // namespace

// (A function-try-block: a throw in here is logged — rate-limited, log_tick_fault — and must not stop the 100 ms
// world tick, drop.cpp.)
void cGameServer::monster_ai_tick(int32_t elapsed_ms)
try {
    std::unordered_map<int16_t, std::vector<std::shared_ptr<GameClientSession>>> zones;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto& [sid, s] : sessions_) {
            if (!s) continue;
            int32_t x, y;
            if (s->ai_targetable(x, y)) zones[s->zone_id()].push_back(s);
        }
    }
    if (zones.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    for (auto& [zone, players] : zones) {
        for (uint32_t oid : entity_mgr_.monsters_in_zone(zone)) {
            MonsterEntity* m = entity_mgr_.get_monster(oid);
            if (!m || !m->alive || m->hp <= 0 || m->target_oid) continue;
            monster_walk_tick(*m, elapsed_ms, now);
            if (m->aggressive && !m->target_oid) monster_aggro_tick(*m, elapsed_ms, players);
        }
    }
} catch (const std::exception& e) {
    log_tick_fault(e.what());
} catch (...) {
    log_tick_fault("unknown exception");
}

void cGameServer::monster_walk_tick(MonsterEntity& m, int32_t elapsed_ms, std::chrono::steady_clock::time_point now)
{
    if (m.walk_state == 2) {                                  // walking: wait for the arrival (30008)
        if (m.motion.moving(now)) return;
        m.walk_timer_ms = rand_between(kWalkMinDelayMs, kWalkMaxDelayMs);
        m.walk_state    = 1;
        return;
    }
    m.walk_timer_ms -= elapsed_ms;                            // a player is near (the zone runs)
    if (m.walk_timer_ms > 0) return;

    int32_t cx, cy;
    m.motion.pos(now, cx, cy);
    // sub_72ADA0 (no area): the offset — y is rolled first (v15), then x (v8).
    int16_t oy = static_cast<int16_t>(rand_between(60, 250));
    int16_t ox = static_cast<int16_t>(rand_between(60, 250));
    if (std::rand() % 2) ox = static_cast<int16_t>(-ox);
    if (std::rand() % 2) oy = static_cast<int16_t>(-oy);
    int16_t dx = static_cast<int16_t>(cx + ox), dy = static_cast<int16_t>(cy + oy);
    const MapGrid* g = zone_grid(m.zone);
    if (g) {
        const int16_t bx1 = static_cast<int16_t>(dx - 32), by1 = static_cast<int16_t>(dy - 32);
        const int16_t bx2 = static_cast<int16_t>(dx + 32), by2 = static_cast<int16_t>(dy + 32);
        for (const MapGrid::Portal& p : g->portals) {
            if (p.x1 < bx2 && p.x2 > bx1 && p.y1 < by2 && p.y2 > by1) {   // sub_804070
                ox = static_cast<int16_t>(-ox); oy = static_cast<int16_t>(-oy);
                dx = static_cast<int16_t>(cx + ox); dy = static_cast<int16_t>(cy + oy);
                break;
            }
        }
        // sub_71B050: into the map bounds
        if (dx < g->bx1) dx = static_cast<int16_t>(g->bx1); else if (dx > g->bx2) dx = static_cast<int16_t>(g->bx2);
        if (dy < g->by1) dy = static_cast<int16_t>(g->by1); else if (dy > g->by2) dy = static_cast<int16_t>(g->by2);
    }

    const uint16_t eid = static_cast<uint16_t>(m.object_id);
    // sub_4341F0 -> MoveTo (sub_717CB0: walk and run speeds must be > 0, no condition 1) with the walk
    // type (1).
    const bool can_move = m.walk_speed > 0.0f && m.run_speed > 0.0f && !m.cond_test(1);
    const bool blocked  = g && g->at(dx / 16, dy / 16) == 1;
    if (!can_move || blocked) {
        PacketBuffer h(static_cast<Opcode>(0x0019), paula_wire());   // sub_5F52F0
        h.write<uint16_t>(eid);
        h.write<uint16_t>(static_cast<uint16_t>(cx));
        h.write<uint16_t>(static_cast<uint16_t>(cy));
        h.write<uint8_t>(1);
        broadcast_zone(m.zone, h, 0);
        m.walk_timer_ms = rand_between(1000, 2000);
        return;
    }
    if (m.cur_speed != m.walk_speed) {                        // the move type sets the speed (0x01D8)
        m.cur_speed  = m.walk_speed;
        m.move_speed = static_cast<uint8_t>(std::min(255, static_cast<int>(m.walk_speed / 4.0f)));
        PacketBuffer sp(static_cast<Opcode>(0x01D8), paula_wire());
        sp.write<uint16_t>(eid);
        sp.write<uint8_t>(static_cast<uint8_t>(static_cast<int>(m.walk_speed / 4.0f)));
        broadcast_zone(m.zone, sp, 0);
    }
    m.motion.start(cx, cy, dx, dy, m.cur_speed, now, 1);
    m.pos_x = dx; m.pos_y = dy;
    PacketBuffer mv(Opcode::C2S_MOVE, paula_wire());                // sub_5F5240: S2C 0x18
    mv.write<uint16_t>(eid);
    mv.write<uint16_t>(static_cast<uint16_t>(cx)); mv.write<uint16_t>(static_cast<uint16_t>(cy));
    mv.write<uint16_t>(static_cast<uint16_t>(dx)); mv.write<uint16_t>(static_cast<uint16_t>(dy));
    mv.write<uint8_t>(1);
    broadcast_zone(m.zone, mv, 0);
    m.walk_state = 2;
}

void cGameServer::monster_aggro_tick(MonsterEntity& m, int32_t elapsed_ms,
                                     const std::vector<std::shared_ptr<GameClientSession>>& players)
{
    m.scan_ms += elapsed_ms;
    if (m.scan_ms < kAggroScanMs) return;
    m.scan_ms = 0;
    int32_t mx, my;
    m.motion.pos(std::chrono::steady_clock::now(), mx, my);
    const int64_t r2 = static_cast<int64_t>(m.view_range) * m.view_range;
    for (const auto& s : players) {
        int32_t px, py;
        if (!s->ai_targetable(px, py)) continue;
        if (dist2(mx, my, px, py) > r2) continue;
        // sub_427CB0's last test: a hidden player (+452 bit 0) is taken only by one whose ESAOpticalEyes (+456) sees
        // it — no monster has any.
        if (s->hiding()) continue;
        // The scan's first player (sub_4241D0): a shadow monster leaves one without the Third Eye
        // alone — and this look-around ends there (sub_424660 -> sub_423B70), no other player tried.
        if (m.shadow() && !s->third_eye()) return;
        // 30001 [the player]: hate + combat — its follow runs in the player's session from here.
        m.target_oid = s->object_id();
        const uint16_t eid = static_cast<uint16_t>(m.object_id);
        auto sess = s;
        sess->run_on_strand([sess, eid]() { sess->engage_by_aggro(eid); });
        return;
    }
}
