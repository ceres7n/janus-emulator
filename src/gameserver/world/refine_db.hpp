#pragma once
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "logger.hpp"
#include "drill_db.hpp"
#include "yaml_scalar.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  RefineDb — the Item_Refine data of the ORIGINAL server data (kto_libconfig.xml):
//    db/item_refine_db.yml        RefineLevelTable (sub_630210, by ItemParam RefineIndex): the
//                                 accumulated bonus % and success rate of each refine level 1..13;
//    db/item_refine_table_db.yml  ItemRefineTable0..6 (sub_630380, by RefineType-1 and the item-level
//                                 band): the material item + count of each level;
//    db/refine_support_db.yml     RefineSupportItemLevel (sub_63D030, by the C2S row Id): the
//                                 optional support item and its rate bonus.
//  (ItemRefineCostDiscount and Maturing_Compound_CostDiscount are both ratio 0 in the data — the
//  GameInfoMgr "discount" the cost formulas subtract; kept as constants here.)
//
//  MaturingDb — the Item_Growth (mature compound, "remix") data:
//    db/equip_property_db.yml     Equip_Property (sub_6308D0, by the TARGET equip): the lucky-roll
//                                 base rate + the up-to-10 option ids the equip accepts;
//    db/maturing_compound_db.yml  Maturing_Compound (sub_6309A0, by the MATERIAL item): the option
//                                 it gives, its floor/top values, count, min equip level and the
//                                 DistrTblInfoMgr Import curve of the lucky roll (sub_6184A0);
//    db/maturing_help_db.yml      Maturing_Compound_Help (sub_630A70): the aid elixirs.
// ─────────────────────────────────────────────────────────────────────────────

struct RefineLevelRow {                 // RefineLevelTable
    int32_t index = 0;                  // ItemParam RefineIndex
    int32_t accumulation[13]{};         // the type-11 option value at level L = acc[L-1] * base / 100
    float   ratio[13]{};                // the success rate (%) of refining TO level L
};

struct RefineMaterialRow {              // ItemRefineTable%d
    int32_t  table = 0;                 // 0..6 = RefineType 1..7
    int32_t  min_lv = 0, max_lv = 0;    // the ItemParam Level band
    uint32_t item[13]{};                // the material of refine level 1..13
    int32_t  cnt[13]{};
};

struct RefineSupportRow {               // RefineSupportItemLevel
    int32_t  id = 0;
    int32_t  min_level = 0, max_level = 0;
    uint32_t item = 0;
    int32_t  count = 0;
    float    prob = 0.0f;               // added to the success rate, %
};

class RefineDb
{
public:
    // GameInfoMgr::InitializeRefineCostDiscount / flt_F44BD0 (FortuneRuleParam "RefineSeed"): the
    // retail data has no discount and seed 100 (the fortune card's refine stat applies 1:1).
    static constexpr float kCostDiscount = 0.0f;
    static constexpr float kRefineSeed   = 100.0f;
    static constexpr int   kMaxLevel     = 13;      // sub_782F20: refine while the level is under 13

    bool load(const std::string& level_path, const std::string& table_path, const std::string& support_path)
    {
        bool ok = true;
        {
            std::ifstream f(level_path);
            if (!f) { LOG_ERROR("RefineDb: cannot open %s", level_path.c_str()); ok = false; }
            else DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                RefineLevelRow r;
                for (const auto& [k, v] : kvs) {
                    if      (k == "RefineIndex")  r.index = std::atoi(v.c_str());
                    else if (k == "Accumulation") read_ints(v, r.accumulation, 13);
                    else if (k == "Ratio")        read_floats(v, r.ratio, 13);
                }
                if (r.index) levels_.emplace(r.index, r);
            });
        }
        {
            std::ifstream f(table_path);
            if (!f) { LOG_ERROR("RefineDb: cannot open %s", table_path.c_str()); ok = false; }
            else DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                RefineMaterialRow r;
                int32_t it[13]{};
                for (const auto& [k, v] : kvs) {
                    if      (k == "Table")   r.table = std::atoi(v.c_str());
                    else if (k == "MinLv")   r.min_lv = std::atoi(v.c_str());
                    else if (k == "MaxLv")   r.max_lv = std::atoi(v.c_str());
                    else if (k == "ItemId")  read_ints(v, it, 13);
                    else if (k == "ItemCnt") read_ints(v, r.cnt, 13);
                }
                for (int i = 0; i < 13; ++i) r.item[i] = static_cast<uint32_t>(it[i]);
                if (r.table >= 0 && r.table < 7) materials_[r.table].push_back(r);
            });
        }
        {
            std::ifstream f(support_path);
            if (!f) { LOG_ERROR("RefineDb: cannot open %s", support_path.c_str()); ok = false; }
            else DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                RefineSupportRow r;
                for (const auto& [k, v] : kvs) {
                    if      (k == "Id")             r.id = std::atoi(v.c_str());
                    else if (k == "MinRefineLevel") r.min_level = std::atoi(v.c_str());
                    else if (k == "MaxRefineLevel") r.max_level = std::atoi(v.c_str());
                    else if (k == "ItemId")         r.item = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    else if (k == "ItemCount")      r.count = std::atoi(v.c_str());
                    else if (k == "Prob")           r.prob = std::strtof(v.c_str(), nullptr);
                }
                if (r.id) supports_.emplace(r.id, r);
            });
        }
        size_t mats = 0;
        for (const auto& t : materials_) mats += t.size();
        LOG_INFO("RefineDb: %zu refine indexes, %zu material bands, %zu support items",
                 levels_.size(), mats, supports_.size());
        return ok && !levels_.empty() && mats != 0;
    }

    // sub_630210(RefineIndex, level 1..13): the level row's accumulation / ratio, or null.
    const RefineLevelRow* level_row(int refine_index) const
    {
        auto it = levels_.find(refine_index);
        return it == levels_.end() ? nullptr : &it->second;
    }

    // sub_630380(RefineType 1..7, ItemParam Level, next level 1..13): the material of that level.
    const RefineMaterialRow* material(int refine_type, int item_level) const
    {
        if (refine_type < 1 || refine_type > 7) return nullptr;
        for (const RefineMaterialRow& r : materials_[refine_type - 1])
            if (r.min_lv <= item_level && item_level <= r.max_lv) return &r;
        return nullptr;
    }

    // sub_63D030(the C2S support row id).
    const RefineSupportRow* support(int id) const
    {
        auto it = supports_.find(id);
        return it == supports_.end() ? nullptr : &it->second;
    }

private:
    static void read_ints(const std::string& v, int32_t* out, int n)
    {
        const char* p = v.c_str();
        while (*p && *p != '[') ++p;
        if (*p) ++p;
        for (int i = 0; i < n && *p; ++i) {
            out[i] = static_cast<int32_t>(std::strtol(p, const_cast<char**>(&p), 10));
            while (*p == ',' || *p == ' ') ++p;
            if (*p == ']') break;
        }
    }
    static void read_floats(const std::string& v, float* out, int n)
    {
        const char* p = v.c_str();
        while (*p && *p != '[') ++p;
        if (*p) ++p;
        for (int i = 0; i < n && *p; ++i) {
            out[i] = std::strtof(p, const_cast<char**>(&p));
            while (*p == ',' || *p == ' ') ++p;
            if (*p == ']') break;
        }
    }

    std::unordered_map<int, RefineLevelRow>   levels_;
    std::vector<RefineMaterialRow>            materials_[7];
    std::unordered_map<int, RefineSupportRow> supports_;
};

struct EquipPropertyRow {               // Equip_Property (the growth-compound TARGET)
    uint32_t item = 0;
    float    apply_ratio = 0.0f;        // the lucky roll's base rate (0..1)
    int32_t  props[10]{};               // the option ids the equip accepts
};

struct MaturingMaterialRow {            // Maturing_Compound
    uint32_t item = 0;
    int32_t  property = 0;              // the option id it gives
    int32_t  min_ability = 0;           // the floor value / the lucky range's bottom
    int32_t  max_ability = 0;           // the lucky range's top
    int32_t  item_num = 1;              // materials consumed
    int32_t  equip_lv = 0;              // the equip's ItemParam Level must reach it
    std::string import_;                // the lucky roll's RandomStatImportTable curve
};

struct MaturingHelpRow {                // Maturing_Compound_Help (the aid elixirs)
    uint32_t item = 0;
    float    apply_ratio = 0.0f;        // + to the lucky-roll rate
    float    min_ability = 0.0f;        // x the material's MinAbility (floor bonus)
    float    max_ability = 0.0f;        // x the material's MaxAbility (top bonus)
};

class MaturingDb
{
public:
    static constexpr float kCostDiscount = 0.0f;   // Maturing_Compound_CostDiscount: ratio 0
    static constexpr int   kMaxSlots     = 10;     // option_type 0..9

    bool load(const std::string& equip_path, const std::string& material_path, const std::string& help_path)
    {
        bool ok = true;
        {
            std::ifstream f(equip_path);
            if (!f) { LOG_ERROR("MaturingDb: cannot open %s", equip_path.c_str()); ok = false; }
            else DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                EquipPropertyRow r;
                for (const auto& [k, v] : kvs) {
                    if      (k == "ItemId")         r.item = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    else if (k == "ApplyRatio")     r.apply_ratio = std::strtof(v.c_str(), nullptr);
                    else if (k == "EnableProperty") read_ints(v, r.props, 10);
                }
                if (r.item) equips_.emplace(r.item, r);
            });
        }
        {
            std::ifstream f(material_path);
            if (!f) { LOG_ERROR("MaturingDb: cannot open %s", material_path.c_str()); ok = false; }
            else DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                MaturingMaterialRow r;
                for (const auto& [k, v] : kvs) {
                    if      (k == "ItemId")         r.item = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    else if (k == "EnableProperty") r.property = std::atoi(v.c_str());
                    else if (k == "MinAbility")     r.min_ability = std::atoi(v.c_str());
                    else if (k == "MaxAbility")     r.max_ability = std::atoi(v.c_str());
                    else if (k == "ItemNum")        r.item_num = std::atoi(v.c_str());
                    else if (k == "EquipLv")        r.equip_lv = std::atoi(v.c_str());
                    else if (k == "Import")         r.import_ = yaml_scalar(v);
                }
                if (r.item) materials_.emplace(r.item, r);
            });
        }
        {
            std::ifstream f(help_path);
            if (!f) { LOG_ERROR("MaturingDb: cannot open %s", help_path.c_str()); ok = false; }
            else DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                MaturingHelpRow r;
                for (const auto& [k, v] : kvs) {
                    if      (k == "ItemId")     r.item = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    else if (k == "ApplyRatio") r.apply_ratio = std::strtof(v.c_str(), nullptr);
                    else if (k == "MinAbility") r.min_ability = std::strtof(v.c_str(), nullptr);
                    else if (k == "MaxAbility") r.max_ability = std::strtof(v.c_str(), nullptr);
                }
                if (r.item) helps_.emplace(r.item, r);
            });
        }
        LOG_INFO("MaturingDb: %zu equips, %zu materials, %zu aid items",
                 equips_.size(), materials_.size(), helps_.size());
        return ok && !equips_.empty() && !materials_.empty();
    }

    const EquipPropertyRow*    equip(uint32_t item_id)    const { return find(equips_, item_id); }
    const MaturingMaterialRow* material(uint32_t item_id) const { return find(materials_, item_id); }
    const MaturingHelpRow*     help(uint32_t item_id)     const { return find(helps_, item_id); }

    // sub_784AC0: the material's option id must be among the equip's 10.
    static bool allows(const EquipPropertyRow& e, int32_t prop)
    {
        for (int i = 0; i < 10; ++i) if (e.props[i] == prop) return true;
        return false;
    }

private:
    template <class M>
    static const typename M::mapped_type* find(const M& m, uint32_t id)
    {
        auto it = m.find(id);
        return it == m.end() ? nullptr : &it->second;
    }
    static void read_ints(const std::string& v, int32_t* out, int n)
    {
        const char* p = v.c_str();
        while (*p && *p != '[') ++p;
        if (*p) ++p;
        for (int i = 0; i < n && *p; ++i) {
            out[i] = static_cast<int32_t>(std::strtol(p, const_cast<char**>(&p), 10));
            while (*p == ',' || *p == ' ') ++p;
            if (*p == ']') break;
        }
    }

    std::unordered_map<uint32_t, EquipPropertyRow>    equips_;
    std::unordered_map<uint32_t, MaturingMaterialRow> materials_;
    std::unordered_map<uint32_t, MaturingHelpRow>     helps_;
};
