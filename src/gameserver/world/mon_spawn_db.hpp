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
//  MonSpawnDb — the Mon_<area> monster tables of the original server data (kto_libconfig.xml),
//  loaded from db/mon_spawn_db.yml: each table's rows in their order. A map's MapInfoEx
//  MonsterTable names its table; the retail loader (GameInfoMgr sub_6236D0) groups the rows by
//  MonsterID (first-seen order; the Ratio of a MonsterID's LAST row is kept) and gives each
//  MonsterID one region slot per row {Avr, Max, Region}.
// ─────────────────────────────────────────────────────────────────────────────

struct MonSpawnRow {
    int   mob = 0;          // MonsterID
    float ratio = 0.0f;     // Ratio
    int   avr = 0;          // Avr
    int   max = 0;          // Max
    int   region = 0;       // Region (0 = the map's type-3 points)
};

class MonSpawnDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("MonSpawnDb: cannot open %s", path.c_str()); return false; }
        std::string line, table;
        MonSpawnRow cur;
        bool in_record = false;
        size_t rows = 0;
        auto commit = [&]() {
            if (in_record && !table.empty() && cur.mob > 0) { tables_[table].push_back(cur); ++rows; }
        };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                cur = MonSpawnRow();
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
        LOG_INFO("MonSpawnDb: loaded %zu tables (%zu rows) from %s", tables_.size(), rows, path.c_str());
        return !tables_.empty();
    }

    const std::vector<MonSpawnRow>* table(const std::string& name) const
    {
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<std::string, std::vector<MonSpawnRow>> tables_;

    static void kv(const std::string& s, std::string& table, MonSpawnRow& r)
    {
        size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const std::string v = s.substr(c + 1);
        if      (k == "Table")     table   = yaml_scalar(v);
        else if (k == "MonsterID") r.mob   = std::atoi(v.c_str());
        else if (k == "Ratio")     r.ratio = static_cast<float>(std::atof(v.c_str()));
        else if (k == "Avr")       r.avr   = std::atoi(v.c_str());
        else if (k == "Max")       r.max   = std::atoi(v.c_str());
        else if (k == "Region")    r.region = std::atoi(v.c_str());
    }
};
