#pragma once
#include <chrono>
#include <cmath>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
//  Motion — the server-side position of a moving actor.
//
//  The retail server keeps every actor's CURRENT position (sub_4100C0) and advances
//  it along the ordered path at the actor's current speed (+132, units/s) on each
//  world tick. Every range decision (BattleFollowingStrategy sub_41DD40 / sub_41E470)
//  measures CURRENT positions, never a move's destination. One straight segment
//  from (fx,fy) to (tx,ty) started at t0 is enough here (no walk grid yet), so the
//  current position is the linear interpolation by elapsed time.
// ─────────────────────────────────────────────────────────────────────────────

struct Motion {
    using clock = std::chrono::steady_clock;

    int32_t fx{ 0 }, fy{ 0 };      // segment start
    int32_t tx{ 0 }, ty{ 0 };      // segment end (= the resting position once arrived)
    clock::time_point t0{};
    int32_t dur_ms{ 0 };           // 0 = at rest at (tx,ty)
    // Move type of the running segment, as the retail move strategy stores it in the actor's
    // state flags (+92 low 3 bits, sub_42BC10 -> sub_422980; type 0 is run = 2). The appear packet
    // carries those flags, and the client walks a moving actor from pos1 to pos2 on appear.
    uint8_t type{ 0 };

    void rest(int32_t x, int32_t y) { fx = tx = x; fy = ty = y; dur_ms = 0; }

    // Start walking (sx,sy) -> (dx,dy) at `speed` units/s from `now`, with move type `mtype`.
    void start(int32_t sx, int32_t sy, int32_t dx, int32_t dy, double speed, clock::time_point now,
               uint8_t mtype = 2)
    {
        fx = sx; fy = sy; tx = dx; ty = dy; t0 = now; type = mtype;
        const double dist = std::sqrt(double(dx - sx) * (dx - sx) + double(dy - sy) * (dy - sy));
        dur_ms = (speed > 0.0) ? static_cast<int32_t>(dist * 1000.0 / speed) : 0;
    }

    bool moving(clock::time_point now) const
    {
        return dur_ms > 0 && now < t0 + std::chrono::milliseconds(dur_ms);
    }

    // The appear packet's state-flag move bits: the running segment's type, 0 when at rest.
    uint32_t state_flags(clock::time_point now) const { return moving(now) ? (type & 7u) : 0u; }

    void pos(clock::time_point now, int32_t& x, int32_t& y) const
    {
        if (!moving(now)) { x = tx; y = ty; return; }
        const double f = std::chrono::duration<double, std::milli>(now - t0).count() / dur_ms;
        x = static_cast<int32_t>(std::lround(fx + (tx - fx) * f));
        y = static_cast<int32_t>(std::lround(fy + (ty - fy) * f));
    }
};
