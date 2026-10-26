#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include "logger.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  DebuffResistDb — db/monster_debuff_resist_db.yml (the original MonsterDebuffResist): what a boss
//  (MonsterParamEx2 DebuffSet = the row's ID) shrugs off. Retail: GameInfoMgr map by ID (getter
//  sub_644B10) {+0 u16 ID, +2 + 2*i u16 column i}; sub_891F80 reads column i for bit i of a skill's
//  DebuffBitCode.
// ─────────────────────────────────────────────────────────────────────────────

class DebuffResistDb
{
public:
    using Row = std::array<int16_t, 31>;

    bool load(const std::string& path)
    {
        static const char* const kCols[31] = {
            "Stun", "Paralysis", "Stone", "MagicPoison", "Poison", "RestrictAvoid", "RestrictAttack", "RestrictSkill",
            "RestrictHeal", "AP", "AC", "DX", "MA", "MD", "DA", "LK", "DP", "HV", "SPD", "Cancler", "APResist",
            "GPResist", "MAResist", "ThunderResist", "AirResist", "WaterResist", "EarthResist", "FireResist",
            "DarkResist", "LightResist", "Dimension" };
        std::ifstream f(path);
        if (!f) { LOG_ERROR("DebuffResistDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        int id = 0;
        Row row{};
        bool in_record = false;
        auto commit = [&]() { if (in_record && id > 0 && !rows_.count(id)) rows_.emplace(id, row); };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            std::string body;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                id = 0; row = Row{};
                in_record = true;
                body = line.substr(ns + 2);
            } else if (in_record && ns >= 4) {
                body = line.substr(ns);
            } else {
                if (line.compare(ns, 7, "Footer:") == 0) { commit(); in_record = false; }
                continue;
            }
            const size_t c = body.find(':');
            if (c == std::string::npos) continue;
            const std::string k = body.substr(0, c);
            const int v = std::atoi(body.c_str() + c + 1);
            if (k == "ID") { id = v; continue; }
            for (int i = 0; i < 31; ++i)
                if (k == kCols[i]) { row[i] = static_cast<int16_t>(v); break; }
        }
        commit();
        LOG_INFO("DebuffResistDb: loaded %zu debuff sets from %s", rows_.size(), path.c_str());
        return !rows_.empty();
    }

    const Row* get(int id) const
    {
        auto it = rows_.find(id);
        return it == rows_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<int, Row> rows_;
};
