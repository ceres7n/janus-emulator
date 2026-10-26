#include "game_server.hpp"
#include "game_session.hpp"
#include "logger.hpp"
#include "combat_util.hpp"
#include <algorithm>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
//  Monster regeneration — the retail MonsterManager of each map (GameServerRelease).
//
//  Data: a map's MapInfoEx MonsterAvrNum (+280) and MonsterTable (its Mon_<area> table,
//  db/mon_spawn_db.yml), grouped by GameInfoMgr sub_6236D0: one entry per MonsterID (first-seen
//  order, the Ratio of its LAST row), one region slot per row {Avr, Max, Region}; the manager
//  (sub_857F10) keeps a live count per slot (sub_85AC40).
//  Positions (Zone::BuildPoints sub_8113C0, from the map's .md3): region R > 0 -> a random type-6
//  point whose id is R (sub_811170, at most 128 a region); else, or when the region has none, a
//  random type-3 point (sub_858A50 -> sub_85ABF0, at most 1024); else the first type-1 point; else
//  (600, 400).
//  Tick (MonsterManager::Process sub_859AA0 on the zone tick): while the map holds fewer monsters
//  than MonsterAvrNum (sub_859A50), the regeneration pass (sub_859760) walks the entries and their
//  slots: below the slot's Max, a slot with Avr > 0 wants Avr monsters, else (Ratio > 0)
//  (int)(MonsterAvrNum * Ratio / (float)sum of the entries' (int)Ratio) (sub_6D8430); a slot under
//  its target gets ONE monster (vtbl+40 sub_858B00): MonsterParamEx2 Type 3 never; else
//  CreateMonsterByRegionId (sub_858C90) -> CreateMonster (sub_8595E0: the class by Type, its stats,
//  its position) and the appear S2C 9 (sub_5F6FC0, with the warp-in effect), and the slot counts it.
//  A monster leaving the world counts down its slot (vtbl+44 sub_858B70).
//  The boss table (BossMonsterTable -> db/boss_mon_db.yml; entries built by sub_857F10): every
//  60 s (MonsterManager::Process sub_859AA0: this[94]) each entry counts a minute (sub_85A3F0: the
//  count BEFORE the increment is compared) and at its BossMonCheckTime the missing ones (BossMonCnt
//  - alive) come — when the last of them died more than 20 s ago — in its region, and the count
//  restarts (sub_85A480). A boss leaving the world (Type 3 / 13, vtbl+44 sub_858B70) counts out of
//  its entry and stamps the time. (Their notices go to Master, 3088 — not done; the event entries
//  of a time table, sub_645520, neither: no Mon_ row of this data has a TimeTable.)
// ─────────────────────────────────────────────────────────────────────────────

void cGameServer::build_regen()
{
    std::lock_guard<std::recursive_mutex> lk(regen_mutex_);
    regen_.clear();
    regen_index_.clear();
    size_t slots = 0, monsters = 0;
    size_t bosses = 0;
    for (const auto& [id, md] : maps_db_.all()) {
        const std::vector<MonSpawnRow>* rows =
            md.monster_table.empty() ? nullptr : mon_spawn_db_.table(md.monster_table);
        if (!md.monster_table.empty() && (!rows || rows->empty()))   // (an empty Mon_ table: RowCount 0)
            LOG_INFO("Regen: map %d MonsterTable %s has no rows", id, md.monster_table.c_str());
        const std::vector<BossMonRow>* boss_rows =
            md.boss_monster_table.empty() ? nullptr : boss_mon_db_.table(md.boss_monster_table);
        if ((!rows || rows->empty()) && (!boss_rows || boss_rows->empty())) continue;
        ZoneRegen z;
        z.zone    = static_cast<int16_t>(id);
        z.avr_num = md.monster_avr_num;
        if (boss_rows)
            for (const BossMonRow& b : *boss_rows) { BossEntry e; e.row = b; z.bosses.push_back(e); ++bosses; }
        if (rows) for (const MonSpawnRow& r : *rows) {
            auto it = std::find_if(z.entries.begin(), z.entries.end(), [&](const RegenEntry& e) { return e.mob == r.mob; });
            if (it == z.entries.end()) {
                z.entries.push_back(RegenEntry{});
                it = z.entries.end() - 1;
                it->mob = r.mob;
            }
            it->ratio = r.ratio;                              // the last row's Ratio
            if (it->slots.size() < 9) it->slots.push_back(RegenSlot{ r.region, r.avr, r.max, 0 });
        }
        for (const RegenEntry& e : z.entries) {
            z.ratio_sum = static_cast<int>(static_cast<float>(z.ratio_sum) + static_cast<float>(static_cast<int>(e.ratio)));
            slots += e.slots.size();
        }
        monsters += static_cast<size_t>(z.avr_num);
        regen_index_[z.zone] = regen_.size();
        regen_.push_back(std::move(z));
    }
    LOG_INFO("Regen: %zu maps with a MonsterTable or a BossMonsterTable (%zu region slots, MonsterAvrNum total %zu, "
             "%zu bosses)", regen_.size(), slots, monsters, bosses);
}

// sub_858B00: one monster of `mob` for the slot (Type 3 is never regenerated); its object id or 0.
uint32_t cGameServer::regen_spawn(int16_t zone, int mob, int region, int entry, int slot)
{
    const MobDef* d = mob_db_.get(mob);
    if (!d) return 0;
    if (d->type == 3) return 0;
    return create_monster(zone, mob, region, entry, slot, -1);
}

// CreateMonsterByRegionId (sub_858C90) -> CreateMonster (sub_8595E0): its object id or 0.
uint32_t cGameServer::create_monster(int16_t zone, int mob, int region, int entry, int slot, int boss)
{
    const MobDef* d = mob_db_.get(mob);
    if (!d) return 0;
    // MonsterManager::CreateChr (sub_858E40): a chrType of 2000..3434 or 16000..16998 and a Type
    // with a monster class (0, 1, 3..13) — else no monster ("invalid atype").
    if ((mob < 2000 || mob >= 3435) && (mob < 16000 || mob >= 16999)) return 0;
    if (d->type < 0 || d->type == 2 || d->type > 13) {
        static std::unordered_map<int, bool> warned;
        if (!warned[mob]) {
            warned[mob] = true;
            LOG_INFO("MonsterManager::CreateChr - zone:%d: invalid atype %d (monster %d)", zone, d->type, mob);
        }
        return 0;
    }
    int16_t x = 600, y = 400;
    if (const MapGrid* g = zone_grid(zone)) {
        const std::vector<MapGrid::Pt>* list = nullptr;
        if (region > 0 && region < 64 && !g->region6[region].empty()) list = &g->region6[region];
        else if (!g->spawn3.empty())                                  list = &g->spawn3;
        if (list) {
            const MapGrid::Pt& p = (*list)[static_cast<size_t>(rnd(static_cast<int>(list->size())))];
            x = p.x; y = p.y;
        } else if (!g->start1.empty()) {
            x = g->start1.front().x; y = g->start1.front().y;
        }
    }
    MonsterEntity m;
    m.zone = zone; m.pos_x = x; m.pos_y = y;
    m.species = static_cast<uint16_t>(mob); m.mob_id = static_cast<uint32_t>(mob);
    m.name = d->name;
    m.level = static_cast<int16_t>(d->level);
    m.hp = m.hp_max = (d->max_hp > 0 ? d->max_hp : 100);
    m.ap = d->ap; m.ac = d->ac; m.dx = d->dx; m.dp = d->dp; m.hv = d->hv; m.lk = d->lk;
    m.da = d->da; m.ma = d->ma; m.md = d->md; m.debuff_set = d->debuff_set;
    m.acc_formula = d->acc_formula;
    std::copy(std::begin(d->resist), std::end(d->resist), std::begin(m.resist));
    m.exp_reward = static_cast<uint32_t>(d->exp);
    m.tm_reward  = static_cast<uint32_t>(d->tm);
    m.walk_speed    = d->walk;                                // CommonMonster::Initialize sub_7287E0
    m.run_speed     = d->run;
    m.chase_speed   = (d->walk + d->run) / 2.0f;
    m.cur_speed     = d->walk;
    m.move_speed    = static_cast<uint8_t>(std::min(255, static_cast<int>(m.cur_speed / 4.0f)));
    m.chase_time_ms = d->chase_time_ms;
    m.regen_entry   = static_cast<int16_t>(entry);
    m.regen_slot    = static_cast<int8_t>(slot);
    m.boss_entry    = static_cast<int16_t>(boss);
    m.aggressive    = (d->type == 1);                         // AggressiveMonster (sub_721D50)
    m.view_range    = 200;                                    // its +432
    m.motion.rest(m.pos_x, m.pos_y);
    const uint32_t oid = entity_mgr_.spawn_monster(m);
    broadcast_entity_spawn(oid, 0x0009);                      // sub_5F6FC0: S2C 9
    return oid;
}

namespace {
// A fault that persists throws on every 100 ms tick: the first one is logged, then at most one line a minute
// with how many were left out in between. (Only the zone tick calls it, one call at a time.)
void log_regen_fault(const char* what)
{
    static std::chrono::steady_clock::time_point last{};
    static unsigned suppressed = 0;
    const auto now = std::chrono::steady_clock::now();
    if (last != std::chrono::steady_clock::time_point{} && now - last < std::chrono::seconds(60)) {
        ++suppressed;
        return;
    }
    if (suppressed)
        LOG_ERROR("cGameServer::regen_tick aborted: %s (%u more since the last line)", what, suppressed);
    else
        LOG_ERROR("cGameServer::regen_tick aborted: %s", what);
    last       = now;
    suppressed = 0;
}
}  // namespace

// (A function-try-block: a throw in here is logged — rate-limited, log_regen_fault — and must not stop the 100 ms
// world tick, drop.cpp.)
void cGameServer::regen_tick(int32_t elapsed_ms)
try {
    std::lock_guard<std::recursive_mutex> lk(regen_mutex_);
    const auto now = std::chrono::steady_clock::now();
    for (ZoneRegen& z : regen_) {
        if (!z.bosses.empty()) {                              // sub_859AA0: every 60 s, sub_85A3F0
            z.boss_ms += elapsed_ms;
            if (z.boss_ms > 60000) {
                z.boss_ms = 0;
                for (size_t bi = 0; bi < z.bosses.size(); ++bi) {
                    BossEntry& b = z.bosses[bi];
                    const int was = b.minutes++;
                    if (was < b.row.check_time) continue;
                    // sub_85A480: the missing ones, when the last died more than 20 s ago
                    const int missing = b.row.cnt - b.alive;
                    if (missing <= 0) continue;
                    if (b.last_death != std::chrono::steady_clock::time_point{} &&
                        now - b.last_death <= std::chrono::milliseconds(20000)) continue;
                    b.minutes = 0;
                    for (int k = 0; k < missing; ++k) {
                        create_monster(z.zone, b.row.mob, b.row.region, -1, -1, static_cast<int>(bi));
                        ++b.alive;                            // (counted even when CreateChr refuses)
                    }
                    LOG_DEBUG("Regen: map %d boss %d x%d (region %d)", z.zone, b.row.mob, missing, b.row.region);
                }
            }
        }
        if (z.alive >= z.avr_num) continue;                   // sub_859A50
        for (size_t ei = 0; ei < z.entries.size(); ++ei) {
            RegenEntry& e = z.entries[ei];
            for (size_t si = 0; si < e.slots.size(); ++si) {
                RegenSlot& s = e.slots[si];
                if (s.count >= s.max) continue;
                bool want = false;
                if (s.avr <= 0) {
                    if (e.ratio > 0.0f && z.ratio_sum > 0) {
                        const float sum = static_cast<float>(z.ratio_sum);
                        const int target = static_cast<int>(static_cast<double>(z.avr_num) * e.ratio / sum);
                        want = s.count < target;
                    }
                } else {
                    want = s.count < s.avr;
                }
                if (!want) continue;
                if (regen_spawn(z.zone, e.mob, s.region, static_cast<int>(ei), static_cast<int>(si))) {
                    ++s.count;
                    ++z.alive;
                    if (z.alive == z.avr_num)
                        LOG_DEBUG("Regen: map %d holds its %d monsters", z.zone, z.avr_num);
                }
            }
        }
    }
} catch (const std::exception& e) {
    log_regen_fault(e.what());
} catch (...) {
    log_regen_fault("unknown exception");
}

// vtbl+44 (sub_858B70): a regenerated monster left the world — its slot counts it out.
void cGameServer::monster_removed(const MonsterEntity& m)
{
    std::lock_guard<std::recursive_mutex> lk(regen_mutex_);
    auto it = regen_index_.find(m.zone);
    if (it == regen_index_.end()) return;
    ZoneRegen& z = regen_[it->second];
    if (m.boss_entry >= 0 && static_cast<size_t>(m.boss_entry) < z.bosses.size()) {   // a boss (Type 3 / 13)
        BossEntry& b = z.bosses[static_cast<size_t>(m.boss_entry)];
        if (b.alive > 0) --b.alive;
        b.last_death = std::chrono::steady_clock::now();
        return;
    }
    if (m.regen_entry < 0 || m.regen_slot < 0) return;
    if (static_cast<size_t>(m.regen_entry) >= z.entries.size()) return;
    RegenEntry& e = z.entries[static_cast<size_t>(m.regen_entry)];
    if (static_cast<size_t>(m.regen_slot) >= e.slots.size()) return;
    RegenSlot& s = e.slots[static_cast<size_t>(m.regen_slot)];
    if (s.count > 0) --s.count;
    if (z.alive > 0) --z.alive;
}
