#pragma once
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include "logger.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  SkillScrollDb — db/skill_scroll_db.yml (the original SkillScroll_Info): the skill a scroll item
//  (Type 15, SubType 69) casts and at which level. Retail: GameInfoMgr list of {+0 SkillId, +4 ItemId,
//  +8 Level (u16)}, searched front to back for the first row of an item id (sub_636E70) by the
//  scroll use C2S 844 (Player case 844) and the SubType 69 use helper (sub_6E59C0).
// ─────────────────────────────────────────────────────────────────────────────

struct SkillScrollDef {
    uint32_t item_id = 0;            // ItemId   (+4)
    uint16_t skill = 0;              // SkillId  (+0)
    int16_t  level = 0;              // Level    (+8)
};

class SkillScrollDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("SkillScrollDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        SkillScrollDef cur;
        bool in_record = false;
        auto commit = [&]() { if (in_record && cur.item_id > 0) rows_.push_back(cur); };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                cur = SkillScrollDef();
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
        LOG_INFO("SkillScrollDb: loaded %zu skill scrolls from %s", rows_.size(), path.c_str());
        return !rows_.empty();
    }

    // sub_636E70: the first row of the item.
    const SkillScrollDef* get(uint32_t item_id) const
    {
        for (const SkillScrollDef& r : rows_)
            if (r.item_id == item_id) return &r;
        return nullptr;
    }

private:
    std::vector<SkillScrollDef> rows_;

    static void kv(const std::string& s, SkillScrollDef& d)
    {
        const size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const std::string v = s.substr(c + 1);
        if      (k == "ItemId")  d.item_id = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "SkillId") d.skill   = static_cast<uint16_t>(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "Level")   d.level   = static_cast<int16_t>(std::strtol(v.c_str(), nullptr, 10));
    }
};
