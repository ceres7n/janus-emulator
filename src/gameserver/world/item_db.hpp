#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <fstream>
#include "logger.hpp"
#include "yaml_scalar.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  ItemDb — the item table, loaded from db/item_db.yml (the four item tables of the original
//  server data — ItemParam2, ItemParamEx2, ItemParamCM2, ItemParamEx — that the retail
//  GameInfoMgr merges and looks up by id, sub_624DF0).
//
//  Class is the item's kind (param +4): 1 = common (CommonItem), 2 = rare (RareItem). It is the
//  kind byte of an item on the wire (sub_60CCA0) and a rare item always drops one by one
//  (sub_729220). Type 30 = gelder (money): a gelder pile lies on the ground 60 s, any other
//  item 90 s once it is free (ItemActor sub_71C760).
// ─────────────────────────────────────────────────────────────────────────────

// One option of a rare item — retail 12-byte record (sub_61A2C0): {u32 type, u16 id, u16 value,
// u32 extra}, kept per item uid (Paula tbl_item_option: option_type, option_index, option_value,
// mixed_item; type 0~9 compound slot, 10 base (+random) stat, 11 refine, 12~ others).
struct ItemOption {
    uint32_t type{ 10 };
    int16_t  id{ 0 };
    int16_t  value{ 0 };
    int32_t  extra{ 0 };
};

// The 12 stat bonuses of an item (param +178, int16 each), in the order of the ItemParam columns
// APPlus..HVPlus; item option ids 10000 + index (sub_6EC320 / sub_6EC4E0).
enum ItemPlus { IP_AP, IP_AC, IP_DX, IP_MAXMP, IP_MA, IP_MD, IP_MAXWT, IP_DA, IP_LK, IP_MAXHP, IP_DP, IP_HV,
                IP_COUNT };

struct ItemDef {
    uint32_t id = 0;
    int cls = 0, type = 0, sub_type = 0;
    int16_t weight = 0;            // Weight (param +72): the inventory weight of one
    int32_t value = 0;             // Value (param +76): the base price (shops: sub_720160 / sub_6DE0E0)
    uint32_t options = 0;          // Options (param +56): 2 = usable (sub_6DE120), 0x800 = only the
                                   //   actor that dropped it may pick it up (sub_6DE2A0), 0x1000 = not
                                   //   droppable (hItemDrop), 0x4000 = zone-limited use (sub_6DE1A0)
    uint32_t options_ex = 0;       // OptionsEx (param +148): 1 = rolls random stats when it becomes a
                                   //   new rare item (sub_6E02D0), 64 = permanent (use does not consume)
    int16_t min_level = 0;         // MinLevel (param +80)
    int effect = 0;                // Effect (param +84): the use effect id
    float delay = 0.0f;            // Delay (param +100): seconds before the use takes effect
    int16_t ap = 0;                // AP (param +104)
    int16_t hp = 0;                // HP (param +106): potion HP
    int16_t hp_con = 0;            // HPCon (param +108): HP needed to use it on a target (sub_73C940)
    float hp_recovery_rate = 0.0f; // HPRecoveryRate (param +112): item option 24000
    int16_t mp = 0;                // MP (param +116): potion MP
    int16_t mp_con = 0;            // MPCon (param +118)
    float mp_recovery_rate = 0.0f; // MPRecoveryRate (param +120): item option 24001
    int32_t money = 0;             // Money (param +124)
    int16_t plus[IP_COUNT]{};      // APPlus..HVPlus (param +178)
    int16_t compound_slot = 0;     // CompoundSlot (param +172)
    int refine_type = 0;           // RefineType (param +164): 1..7 = the refined stat (refine.cpp)
    int refine_index = 0;          // RefineIndex (param +168): the RefineLevelTable row
    uint32_t chr_type_flags = 0;   // ChrTypeFlags (param +60): the character types (bit type-1) that may
                                   //   use/equip an Options 0x4000 item (sub_6DE080)
    int min_stat_type = 0;         // MinStatType (param +152): the stat index an item needs ...
    int min_stat_lv = 0;           // MinStatLv (param +156): ... at least this value to be equipped
    std::string name;

    bool stackable() const { return cls == 1; }   // sub_6DCF40 / sub_6DE040: a common item stacks
    // The item's count for weight and amounts (vtbl+48): a CommonItem's count (+40, unknown_libname_1729);
    // a RareItem is always one (unknown_libname_1725) — a drill's +40 is its life (vtbl+80 sets it raw).
    int item_count(int raw) const { return cls == 1 ? raw : 1; }
};

class ItemDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("ItemDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        ItemDef cur;
        bool in_record = false;
        auto commit = [&]() { if (in_record && cur.id > 0) items_[cur.id] = cur; };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                cur = ItemDef();
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
        LOG_INFO("ItemDb: loaded %zu items from %s", items_.size(), path.c_str());
        return !items_.empty();
    }

    const ItemDef* get(uint32_t id) const
    {
        auto it = items_.find(id);
        return it == items_.end() ? nullptr : &it->second;
    }
    size_t size() const { return items_.size(); }

private:
    std::unordered_map<uint32_t, ItemDef> items_;

    static void kv(const std::string& s, ItemDef& d)
    {
        static const char* const kPlus[IP_COUNT] = { "APPlus", "ACPlus", "DXPlus", "MaxMPPlus", "MAPlus", "MDPlus",
                                                     "MaxWTPlus", "DAPlus", "LKPlus", "MaxHPPlus", "DPPlus", "HVPlus" };
        size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const std::string v = s.substr(c + 1);
        const auto i16 = [&]() { return static_cast<int16_t>(std::atoi(v.c_str())); };
        if      (k == "ID")             d.id               = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "Class")        { d.cls              = std::atoi(v.c_str());
                                        if (d.cls == 3) d.cls = 2; }   // InitializeItemParam: Class 3 -> 2
        else if (k == "Type")           d.type             = std::atoi(v.c_str());
        else if (k == "SubType")        d.sub_type         = std::atoi(v.c_str());
        else if (k == "Weight")         d.weight           = i16();
        else if (k == "Value")          d.value            = std::atoi(v.c_str());
        else if (k == "Options")        d.options          = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "OptionsEx")      d.options_ex       = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "MinLevel")       d.min_level        = i16();
        else if (k == "Effect")         d.effect           = std::atoi(v.c_str());
        else if (k == "Delay")          d.delay            = static_cast<float>(std::atof(v.c_str()));
        else if (k == "AP")             d.ap               = i16();
        else if (k == "HP")             d.hp               = i16();
        else if (k == "HPCon")          d.hp_con           = i16();
        else if (k == "HPRecoveryRate") d.hp_recovery_rate = static_cast<float>(std::atof(v.c_str()));
        else if (k == "MP")             d.mp               = i16();
        else if (k == "MPCon")          d.mp_con           = i16();
        else if (k == "MPRecoveryRate") d.mp_recovery_rate = static_cast<float>(std::atof(v.c_str()));
        else if (k == "Money")          d.money            = std::atoi(v.c_str());
        else if (k == "CompoundSlot")   d.compound_slot    = i16();
        else if (k == "RefineType")     d.refine_type      = std::atoi(v.c_str());
        else if (k == "RefineIndex")    d.refine_index     = std::atoi(v.c_str());
        else if (k == "ChrTypeFlags")   d.chr_type_flags   = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "MinStatType")    d.min_stat_type    = std::atoi(v.c_str());
        else if (k == "MinStatLv")      d.min_stat_lv      = std::atoi(v.c_str());
        else if (k == "Name")           d.name             = yaml_scalar(v);
        else
            for (int i = 0; i < IP_COUNT; ++i)
                if (k == kPlus[i]) { d.plus[i] = i16(); break; }
    }
};
