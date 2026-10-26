#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <array>
#include <fstream>
#include <sstream>
#include "logger.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  StatTable / ExpTable — the retail Trickster character progression tables.
//
//  StatTable loads data\gulim13.fnt2 (the GLOBAL per-graph-level stat table, same
//  file the retail GameServer loads in GameInfoMgr::InitializeChrStatus). Each line
//  is one graph-level with 12 "<gl> 4 <value>" triplets, one per stat, in the
//  engine's canonical order:
//     0 AP, 1 AC, 2 DX, 3 MaxMP, 4 MA, 5 MD, 6 MaxWT, 7 DA, 8 LK, 9 MaxHP, 10 DP, 11 HV
//  A character's effective stat i (naked, no equip/buff) = table[ AccPts[i]/4 + 1 ][ i ]
//  (GetChrStatusValue). graph-level is 1-based.
//
//  ExpTable loads data\gulim14.fnt2 (GetChrNextExp): per base level, the EXP needed
//  to reach the next level (column 3). Levels 1..400.
// ─────────────────────────────────────────────────────────────────────────────

enum StatIdx {
    ST_AP = 0, ST_AC, ST_DX, ST_MAXMP, ST_MA, ST_MD,
    ST_MAXWT, ST_DA, ST_LK, ST_MAXHP, ST_DP, ST_HV, ST_COUNT
};

class StatTable
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("StatTable: cannot open %s", path.c_str()); return false; }
        rows_.clear();
        rows_.push_back(std::array<int, ST_COUNT>{});   // index 0 unused (graph-level is 1-based)
        std::string line;
        while (std::getline(f, line)) {
            if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
            std::istringstream ss(line);
            std::vector<long> tok; long v;
            while (ss >> v) tok.push_back(v);
            if (tok.size() < ST_COUNT * 3) continue;    // need 12 triplets
            std::array<int, ST_COUNT> r{};
            for (int i = 0; i < ST_COUNT; ++i) r[i] = static_cast<int>(tok[i * 3 + 2]);  // 3rd of each triplet
            rows_.push_back(r);
        }
        LOG_INFO("StatTable: loaded %zu graph-levels from %s", rows_.size() - 1, path.c_str());
        return rows_.size() > 1;
    }

    // Effective value of stat `i` at graph-level `gl` (1-based; clamped to the loaded range).
    int value(int gl, int i) const
    {
        if (rows_.size() <= 1 || i < 0 || i >= ST_COUNT) return 0;
        if (gl < 1) gl = 1;
        if (gl >= static_cast<int>(rows_.size())) gl = static_cast<int>(rows_.size()) - 1;
        return rows_[gl][i];
    }
    bool loaded() const { return rows_.size() > 1; }

private:
    std::vector<std::array<int, ST_COUNT>> rows_;
};

class ExpTable
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("ExpTable: cannot open %s", path.c_str()); return false; }
        next_.assign(1, 0);                              // index 0 unused (level is 1-based)
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::vector<long> tok; long v;
            while (ss >> v) tok.push_back(v);
            if (tok.size() >= 3) next_.push_back(static_cast<uint32_t>(tok[2]));  // col3 = exp to next level
        }
        LOG_INFO("ExpTable: loaded %zu levels from %s", next_.size() - 1, path.c_str());
        return next_.size() > 1;
    }

    // EXP required to go from `level` to level+1 (0 if out of range).
    uint32_t next_exp(int level) const
    {
        if (level < 1 || level >= static_cast<int>(next_.size())) return 0;
        return next_[level];
    }

private:
    std::vector<uint32_t> next_;
};

// TmTable loads db/tm_level_db.yml (table TM_LEVEL2 of the original data: Level, NextTmxp). The retail
// GameInfoMgr (sub_62A7E0) keeps Levels 0..500; sub_719AB0 returns -1 past 500.
class TmTable
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("TmTable: cannot open %s", path.c_str()); return false; }
        next_.assign(501, 0);
        std::string line;
        int level = -1;
        size_t n = 0;
        while (std::getline(f, line)) {
            const size_t a = line.find_first_not_of(" -");
            if (a == std::string::npos) continue;
            if (line.compare(a, 6, "Level:") == 0)          level = std::atoi(line.c_str() + a + 6);
            else if (line.compare(a, 9, "NextTmxp:") == 0 && level >= 0 && level <= 500) {
                next_[level] = std::atoi(line.c_str() + a + 9);
                ++n;
            }
        }
        LOG_INFO("TmTable: loaded %zu TM levels from %s", n, path.c_str());
        return n > 0;
    }

    // sub_719AB0: TM EXP needed at TM level `tm` (0..500), -1 beyond.
    int32_t next(int tm) const { return (tm < 0 || tm > 500) ? -1 : next_[tm]; }

private:
    std::vector<int32_t> next_;
};
