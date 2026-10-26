#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
#include <fstream>
#include "logger.hpp"
#include "yaml_scalar.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  BossMonDb — the BossMon_* tables of the original server data (kto_libconfig.xml), loaded from
//  db/boss_mon_db.yml: a map's MapInfoEx BossMonsterTable (GameInfoMgr sub_623590: one entry per
//  BossMonID {BossMonID, BossMonCnt, BossMonCheckTime, BossMonRegionID}).
// ─────────────────────────────────────────────────────────────────────────────

struct BossMonRow {
    int mob = 0;          // BossMonID
    int cnt = 0;          // BossMonCnt (i16)
    int check_time = 0;   // BossMonCheckTime (minutes)
    int region = 0;       // BossMonRegionID (i16)
};

class BossMonDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("BossMonDb: cannot open %s", path.c_str()); return false; }
        std::string line, table;
        BossMonRow cur;
        bool in_record = false;
        size_t rows = 0;
        auto commit = [&]() {
            if (!in_record || table.empty() || cur.mob <= 0) return;
            std::vector<BossMonRow>& t = tables_[table];
            for (BossMonRow& r : t) if (r.mob == cur.mob) { r = cur; return; }   // keyed by BossMonID
            t.push_back(cur);
            ++rows;
        };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                cur = BossMonRow();
                table.clear();
                in_record = true;
                kv(line.substr(ns + 2), table, cur);
            } else if (in_record && ns >= 4) {
                kv(line.substr(ns), table, cur);
            } else if (line.compare(ns, 7, "Footer:") == 0) {
                commit();
                in_record = false;
            }
        }
        commit();
        LOG_INFO("BossMonDb: loaded %zu tables (%zu bosses) from %s", tables_.size(), rows, path.c_str());
        return !tables_.empty();
    }

    const std::vector<BossMonRow>* table(const std::string& name) const
    {
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<std::string, std::vector<BossMonRow>> tables_;

    static void kv(const std::string& s, std::string& table, BossMonRow& r)
    {
        size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const std::string v = s.substr(c + 1);
        if      (k == "Table")            table        = yaml_scalar(v);
        else if (k == "BossMonID")        r.mob        = std::atoi(v.c_str());
        else if (k == "BossMonCnt")       r.cnt        = static_cast<int16_t>(std::atoi(v.c_str()));
        else if (k == "BossMonCheckTime") r.check_time = std::atoi(v.c_str());
        else if (k == "BossMonRegionID")  r.region     = static_cast<int16_t>(std::atoi(v.c_str()));
    }
};
