#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>

// ─────────────────────────────────────────────────────────────────────────────
//  Small combat helpers shared by the melee (game_session.cpp) and the skills (skill.cpp),
//  each the equivalent of a retail GameServer primitive.
// ─────────────────────────────────────────────────────────────────────────────

// rand() % n (sub_420AB0(0, n) = 0 + rand() % (n - 0)): uniform in [0, n).
inline int rnd(int n) { return (n <= 0) ? 0 : (std::rand() % n); }

// sub_4274E0(&lo, &hi, &v): v clamped to [lo, hi].
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// GetTickCount (sub_410150): a millisecond tick (the gun's seed and shot timer).
inline uint32_t tick_ms()
{
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The item Type whose helper is WeaponItemHelper (dword_AB617C: only Type 2, the weapons — guns
// included; 3-5/7/8/17-27/34 have GeneralItemHelper): equipping one (vtbl+16 sub_6E70F0, at login
// vtbl+12 sub_6E70B0) also restarts the player's shot timer (sub_765480).
inline bool weapon_helper_type(int type) { return type == 2; }

// Euclidean distance (logs only — decisions use dist2/cmp_r like the retail).
inline int dist_i(int ax, int ay, int bx, int by)
{
    const double dx = ax - bx, dy = ay - by;
    return static_cast<int>(std::lround(std::sqrt(dx * dx + dy * dy)));
}

// sub_42F1B0: the distance of two positions as a float, sqrtf((float)(dx² + dy²)) (its sqrtf wrapper
// sub_417F80 is named "log" by IDA's signatures).
inline float dist_f(int ax, int ay, int bx, int by)
{
    const int dx = static_cast<int16_t>(bx) - static_cast<int16_t>(ax);
    const int dy = static_cast<int16_t>(by) - static_cast<int16_t>(ay);
    return std::sqrt(static_cast<float>(dy * dy + dx * dx));
}

// sub_41ED00: squared distance of two positions.
inline int64_t dist2(int ax, int ay, int bx, int by)
{
    const int64_t dx = static_cast<int64_t>(bx) - ax, dy = static_cast<int64_t>(by) - ay;
    return dx * dx + dy * dy;
}

// sub_41ED80: sign(d2 - r²).
inline int cmp_r(int64_t d2, int r)
{
    const int64_t rr = static_cast<int64_t>(r) * r;
    return d2 > rr ? 1 : (d2 >= rr ? 0 : -1);
}

// The basic-melee rolls of sub_5E5700 (rand() % n = sub_41ED70):
// sub_5E43E0 — a player's hit: rand(1000) < (int)(min((AC - 50) * 0.4 + 80, 99) * 10).
inline bool melee_hit_player(int ac)
{
    const int roll = rnd(1000);
    double v = static_cast<double>(ac - 50) * 0.4000000059604645 + 80.0;
    if (v > 99.0) v = 99.0;
    return roll < static_cast<int>(v * 10.0);
}
// sub_716960(this, 2) — the crit: two rolls of rand(100) < min(LK + 30, 90), the last one counts. (A
// player's chance first goes through its ability ratios, sub_5E4680 — none exist here.)
inline bool crit_roll(int lk)
{
    bool r = false;
    for (int i = 0; i < 2; ++i) { const int roll = rnd(100); r = roll < std::min(lk + 30, 90); }
    return r;
}
// sub_7168C0(this, 2) — a crit is a strong one: two rolls of rand(100) < min(AC - 50, 90), the last counts.
inline bool strong_roll(int ac)
{
    bool r = false;
    for (int i = 0; i < 2; ++i) { const int roll = rnd(100); r = roll < std::min(ac - 50, 90); }
    return r;
}
// The melee damage, AP against DP, times the PvP factor sub_5E7050 (1.0 unless both are players):
// sub_5E4B30 normal (int)(AP - 0.75 DP), at least 0; sub_5E4B80 crit (int)(1.5 AP - 0.75 DP), at least
// 1; sub_5E4BD0 strong = (int)(crit * 1.3).
inline int32_t melee_damage(int ap, int dp, float f = 1.0f)
{
    const int v = static_cast<int>(static_cast<double>(static_cast<int>(ap * 1.0 - dp * 0.75)) * f);
    return v < 0 ? 0 : v;
}
inline int32_t melee_crit_damage(int ap, int dp, float f = 1.0f)
{
    const int v = static_cast<int>(static_cast<double>(static_cast<int>(ap * 1.5 - dp * 0.75)) * f);
    return v <= 0 ? 1 : v;
}
inline int32_t melee_strong_damage(int ap, int dp, float f = 1.0f)
{
    return static_cast<int32_t>(static_cast<double>(melee_crit_damage(ap, dp, f)) * 1.299999952316284);
}

// The target's resistance against the hit, retail damage post-processing:
//   melee  sub_5E6510: dmg -= (int)(R * dmg * 0.01)      (R = slot 20 PhysicalR; the attacker's
//                      elemental-damage slots 0..8 add nothing without equipment/buffs)
//   skill  sub_5E6760: R > 100 -> R -= 100, dmg = 0; dmg -= (int)(R * dmgBefore * 0.01)
// A negative result is the target absorbing the hit: event 30182 heals it by -dmg and dmg = 0.
// Returns the damage dealt; `absorbed` receives the healed amount (0 normally).
inline int32_t melee_resist(int32_t dmg, int resist, int32_t& absorbed)
{
    dmg -= static_cast<int32_t>(static_cast<double>(resist * dmg) * 0.009999999776482582);
    absorbed = dmg < 0 ? -dmg : 0;
    return dmg < 0 ? 0 : dmg;
}
inline int32_t skill_resist(int32_t dmg, int resist, int32_t& absorbed)
{
    const int32_t before = dmg;
    if (resist > 100) { resist -= 100; dmg = 0; }
    dmg -= static_cast<int32_t>(static_cast<double>(resist * before) * 0.009999999776482582);
    absorbed = dmg < 0 ? -dmg : 0;
    return dmg < 0 ? 0 : dmg;
}

// sub_893E50: the server's "angle" of a vector — normalized (sqrtf), then acos(cosf(0 * x + y * y)) * 57.29578
// (the dot product is taken against (0, 1) but with y * y, and passed through cosf: so y² * 57.3 in [0, 57.3]),
// + 180 when the normalized x < 0. Retail math, kept (a vector and its mirror over the x axis share a value).
inline float server_angle(float x, float y)
{
    const float len = std::sqrt(x * x + y * y);
    x = x / len;
    y = y / len;
    const float dot = 0.0f * x + y * y;
    const float c = std::cos(dot);
    float a = static_cast<float>(std::acos(static_cast<double>(c))) * 57.29578018188477f;
    if (x < 0.0) a = static_cast<float>(static_cast<double>(a) + 180.0);
    return a;
}

// sub_893D30(a, b, c, spread): the angle of b - a and of c - b (sub_893E50); c lies within +-spread of it.
inline bool server_in_line(int16_t ax, int16_t ay, int16_t bx, int16_t by, int16_t cx, int16_t cy, float spread)
{
    const float a1 = server_angle(static_cast<float>(static_cast<double>(bx) - static_cast<double>(ax)),
                                  static_cast<float>(static_cast<double>(by) - static_cast<double>(ay)));
    const float a2 = server_angle(static_cast<float>(static_cast<double>(cx) - static_cast<double>(bx)),
                                  static_cast<float>(static_cast<double>(cy) - static_cast<double>(by)));
    const float lo = a1 - spread;
    const float hi = a1 + spread;
    return a2 >= lo && a2 <= hi;
}

// sub_893840: rand()%100 < clamp((int)(ratio * 100.0), 0, 100) — of the float ratio, so 0.7 -> 69.
inline bool apply_chance(float ratio)
{
    const int roll = rnd(100);
    const int v = clampi(static_cast<int>(static_cast<double>(ratio) * 100.0), 0, 100);
    return roll < v;
}

// sub_510060 (InitESAction vtbl+112 sub_50F890): 1 applied, 2 the ApplyRatio roll failed,
// 3 resisted. A zero ApplyRatio always applies (no resist roll); otherwise the resist r resists
// unless rand()%100 < clamp(100 - r, 0, 100) (sub_8937D0 / sub_8937F0).
inline int apply_roll(float apply_ratio, int resist)
{
    if (apply_ratio == 0.0f) return 1;
    if (!apply_chance(apply_ratio)) return 2;
    const int roll = rnd(100);
    return (roll < clampi(100 - resist, 0, 100)) ? 1 : 3;
}
