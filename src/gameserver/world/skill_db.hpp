#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <fstream>
#include "logger.hpp"
#include "yaml_scalar.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  SkillDb — class skill table, loaded from db/skill_db.yml (generated from the original
//  server data kto_libconfig.xml, table SkillParam2). The client validates the learned-skill
//  records of S2C 271 against its own SkillParam2 table (sub_605E10 -> sub_604750), so only
//  these ids may be sent.
//
//  Retail SkillParam layout (GameInfoMgr loader sub_62D0D0): +4 Type, +40 SkillOption (flags),
//  +42 AffectionRel, +46 MaxSKLV, +72 ESAIC, +76 EState, +80 ESClass, +84 Grade, +88 ESAction,
//  +92 ESAUI, +96 eSkillSymbolType, +100 UseStyle, +104 SkillExp, +144 StateType, +152 DebuffBitCode; the per-level
//  ESAII / ESAction rows come from the ESAIITable / ESATable tables (db/esa_db.yml).
// ─────────────────────────────────────────────────────────────────────────────

struct SkillDef {
    int id = 0, type = 0, max_level = 1, master_tm = 0, card = 0;
    int skill_option = 0;    // SkillOption flags (0x10 never usable, 0x80/0x100 checks, 0x200 not in towns...)
    int affection_rel = 0;   // AffectionRel
    int esaic = 0;           // ESAIC: invocation condition (2 = ESAICChase, 3 = Move, 4 = Item, else Normal)
    int estate = 0;          // EState
    int esclass = 0;         // ESClass (-1 = none)
    int grade = 0;           // Grade
    int esaction = 0;        // ESAction: the effect handler (GameServer sub_5BE9A0 switch)
    int esaui = 0;           // ESAUI: the client's selection UI (0 = none, 1 = select a target object)
    int symbol_type = 0;     // eSkillSymbolType (+96): 8 = a dark skill (ESABloodyOath's cost, sub_8911A0)
    int use_style = 0;       // UseStyle
    int state_type = 0;      // StateType
    int skill_exp = 0;       // SkillExp
    int ps_ability = 0;      // PSAbility (+116): what a learned passive (Type 1) unlocks in the
                             //   player's ESLimiter (1 = guns, Gun Carrier 3008; sub_525220)
    bool level_up = false;   // bLevelUp (+44): the skill can be raised (C2S 0x01E1, sub_760930)
    bool support = false;    // Surpport (+120): a passive that supports another skill (Arrow Rush 2007
                             //   for Mana Arrow, sub_50EFE0 / sub_50F140)
    uint32_t debuff_bits = 0;  // DebuffBitCode (+152): the MonsterDebuffResist columns it is (sub_891F80)
    std::string name, esaii_table, esaui_table, esa_table;
};

class SkillDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("SkillDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        SkillDef cur;
        bool in_record = false;
        auto commit = [&]() { if (in_record && cur.id > 0) skills_[cur.id] = cur; };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                cur = SkillDef();
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
        LOG_INFO("SkillDb: loaded %zu skills from %s", skills_.size(), path.c_str());
        return !skills_.empty();
    }

    const SkillDef* get(int id) const
    {
        auto it = skills_.find(id);
        return it == skills_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<int, SkillDef> skills_;

    static void kv(const std::string& s, SkillDef& d)
    {
        size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const std::string v = s.substr(c + 1);
        const int n = std::atoi(v.c_str());
        if      (k == "ID" || k == "Id") d.id            = n;
        else if (k == "Name")            d.name          = yaml_scalar(v);
        else if (k == "Type")            d.type          = n;
        else if (k == "MaxSKLV")         d.max_level     = n;
        else if (k == "MasterTMPoint")   d.master_tm     = n;
        else if (k == "SkillCardID")     d.card          = n;
        else if (k == "SkillOption")     d.skill_option  = n;
        else if (k == "AffectionRel")    d.affection_rel = n;
        else if (k == "ESAIC")           d.esaic         = n;
        else if (k == "EState")          d.estate        = n;
        else if (k == "ESClass")         d.esclass       = n;
        else if (k == "Grade")           d.grade         = n;
        else if (k == "ESAction")        d.esaction      = n;
        else if (k == "ESAUI")           d.esaui         = n;
        else if (k == "eSkillSymbolType") d.symbol_type  = n;
        else if (k == "UseStyle")        d.use_style     = n;
        else if (k == "StateType")       d.state_type    = n;
        else if (k == "SkillExp")        d.skill_exp     = n;
        else if (k == "PSAbility")       d.ps_ability    = n;
        else if (k == "bLevelUp")        d.level_up      = v.find("true") != std::string::npos || n != 0;
        else if (k == "Surpport")        d.support       = v.find("true") != std::string::npos || n != 0;
        else if (k == "DebuffBitCode")   d.debuff_bits   = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "ESAIITable")      d.esaii_table   = yaml_scalar(v);
        else if (k == "ESAUITable")      d.esaui_table   = yaml_scalar(v);
        else if (k == "ESATable")        d.esa_table     = yaml_scalar(v);
    }
};
