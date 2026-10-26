#pragma once
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include "logger.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  PreEsaicDb — db/pre_esaic_condition_db.yml (the original PreESAICondition): the HP / MP rates a skill
//  needs before it fires. Retail: GameInfoMgr map by skill id (loader sub_625470, getter sub_625590 —
//  {float HpRate, float MpRate}), read by sub_890DD0 in the fire checks (sub_890B30).
// ─────────────────────────────────────────────────────────────────────────────

struct PreEsaicDef {
    int   skill = 0;                 // ID
    float hp_rate = 0.0f;            // HpRate                                       (+0)
    float mp_rate = 0.0f;            // MpRate                                       (+4)
};

class PreEsaicDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("PreEsaicDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        PreEsaicDef cur;
        bool in_record = false;
        auto commit = [&]() { if (in_record && cur.skill > 0) rows_[cur.skill] = cur; };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                cur = PreEsaicDef();
                in_record = true;
                kv(line.substr(ns + 2), cur);
            } else if (in_record && ns >= 4) {
                kv(line.substr(ns), cur);
            } else if (line.compare(ns, 7, "Footer:") == 0) {
                commit();
                in_record = false;
            }
        }
        commit();
        LOG_INFO("PreEsaicDb: loaded %zu skill preconditions from %s", rows_.size(), path.c_str());
        return !rows_.empty();
    }

    const PreEsaicDef* get(int skill) const
    {
        auto it = rows_.find(skill);
        return it == rows_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<int, PreEsaicDef> rows_;

    static void kv(const std::string& s, PreEsaicDef& d)
    {
        const size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const std::string v = s.substr(c + 1);
        if      (k == "ID")     d.skill   = std::atoi(v.c_str());
        else if (k == "HpRate") d.hp_rate = std::strtof(v.c_str(), nullptr);
        else if (k == "MpRate") d.mp_rate = std::strtof(v.c_str(), nullptr);
    }
};
