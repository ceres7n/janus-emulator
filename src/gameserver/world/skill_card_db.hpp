#pragma once
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include "logger.hpp"
#include "yaml_scalar.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  SkillCardDb — db/skill_card_db.yml (the original Item_SkillCardParam): what a skill card item
//  (Type 28 SubType 55) teaches and what learning it needs. Retail: GameInfoMgr map by item id
//  (getter sub_62DD20); a skill reaches its card through SkillParam SkillCardID (+68).
// ─────────────────────────────────────────────────────────────────────────────

struct SkillCardDef {
    uint32_t id = 0;                 // the card's item id                           (+4)
    int scard_type = 0, tmlv = 0;    // SCardType / TMLV: class requirement + TM lv  (+8 / +12)
    int sub_scard_type = 0, sub_tmlv = 0;                                          // (+16 / +20)
    int pre_skill[3]{};              // PreSkill_1..3                                 (+24..+32)
    int16_t pre_skill_lv[3]{};       // PreSkillLv_1..3                               (+36..+40)
    int16_t affection_point = 0;     // AffectionPoint: TM points it costs            (+42)
    int affection_skill = 0;         // AffectionSkill: the skill it teaches          (+44)
    int16_t register_level = 0;      // RegisterLevel: the level it is learned at     (+48)
    std::string name;
};

class SkillCardDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("SkillCardDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        SkillCardDef cur;
        bool in_record = false;
        auto commit = [&]() { if (in_record && cur.id > 0) cards_.emplace(cur.id, cur); };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                cur = SkillCardDef();
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
        LOG_INFO("SkillCardDb: loaded %zu skill cards from %s", cards_.size(), path.c_str());
        return !cards_.empty();
    }

    const SkillCardDef* get(uint32_t id) const
    {
        auto it = cards_.find(id);
        return it == cards_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<uint32_t, SkillCardDef> cards_;

    static void kv(const std::string& s, SkillCardDef& d)
    {
        const size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const std::string v = s.substr(c + 1);
        const int n = std::atoi(v.c_str());
        if      (k == "ID")             d.id              = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "SCardType")      d.scard_type      = n;
        else if (k == "TMLV")           d.tmlv            = n;
        else if (k == "SubSCardType")   d.sub_scard_type  = n;
        else if (k == "SubTMLV")        d.sub_tmlv        = n;
        else if (k == "AffectionPoint") d.affection_point = static_cast<int16_t>(n);
        else if (k == "AffectionSkill") d.affection_skill = n;
        else if (k == "RegisterLevel")  d.register_level  = static_cast<int16_t>(n);
        else if (k == "Name")           d.name            = yaml_scalar(v);
        else if (k.size() == 10 && k.compare(0, 9, "PreSkill_") == 0) {
            const int i = k[9] - '1';
            if (i >= 0 && i < 3) d.pre_skill[i] = n;
        } else if (k.size() == 12 && k.compare(0, 11, "PreSkillLv_") == 0) {
            const int i = k[11] - '1';
            if (i >= 0 && i < 3) d.pre_skill_lv[i] = static_cast<int16_t>(n);
        }
    }
};
