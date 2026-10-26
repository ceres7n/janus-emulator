#pragma once
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include "logger.hpp"
#include "yaml_scalar.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  ItemRebirthDb — db/item_rebirth_db.yml (the original Item_RebirthParam): what a resurrection item
//  (Type 15, Effect 29) restores. Retail: GameInfoMgr map by item id (getter sub_62E3A0), read by
//  UseItemHelper::Use (sub_6E1AE0) case 29; the row's floats go to the revive (sub_764260) as
//  +8 HP, +12 MP, +16 EXP, +20 TM EXP.
// ─────────────────────────────────────────────────────────────────────────────

struct ItemRebirthDef {
    uint32_t item_id = 0;            // ItemId                                       (+4)
    float hp_ratio = 0.0f;           // fHpRestoreRatio                              (+8)
    float mp_ratio = 0.0f;           // fMpRestoreRatio                              (+12)
    float exp_ratio = 0.0f;          // fExpRestoreRatio                             (+16)
    float tmxp_ratio = 0.0f;         // fTmxpRestoreRatio                            (+20)
    std::string name;
};

class ItemRebirthDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("ItemRebirthDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        ItemRebirthDef cur;
        bool in_record = false;
        auto commit = [&]() { if (in_record && cur.item_id > 0) items_.emplace(cur.item_id, cur); };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                cur = ItemRebirthDef();
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
        LOG_INFO("ItemRebirthDb: loaded %zu resurrection items from %s", items_.size(), path.c_str());
        return !items_.empty();
    }

    const ItemRebirthDef* get(uint32_t item_id) const
    {
        auto it = items_.find(item_id);
        return it == items_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<uint32_t, ItemRebirthDef> items_;

    static void kv(const std::string& s, ItemRebirthDef& d)
    {
        const size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const std::string v = s.substr(c + 1);
        if      (k == "ItemId")            d.item_id    = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "fHpRestoreRatio")   d.hp_ratio   = std::strtof(v.c_str(), nullptr);
        else if (k == "fMpRestoreRatio")   d.mp_ratio   = std::strtof(v.c_str(), nullptr);
        else if (k == "fExpRestoreRatio")  d.exp_ratio  = std::strtof(v.c_str(), nullptr);
        else if (k == "fTmxpRestoreRatio") d.tmxp_ratio = std::strtof(v.c_str(), nullptr);
        else if (k == "Name")              d.name       = yaml_scalar(v);
    }
};
