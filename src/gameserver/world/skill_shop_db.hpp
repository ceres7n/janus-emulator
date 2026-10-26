#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "logger.hpp"
#include "yaml_scalar.hpp"
#include "drill_db.hpp"
#include "shop_db.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  The skill / job masters (original server data, kto_libconfig.xml):
//    db/skill_shop_db.yml          the SkillShopTalk_* tables a map's SkillShopTable names (GameInfoMgr
//                                  sub_62A3C0: a 2352-byte record per row)
//    db/skill_shop_item_db.yml     their ItemTable tables (sub_62A1C0: the first 128 rows, +16)
//    db/limit_skill_master_db.yml  their LimitSkillMaster tables (sub_62A2E0: the first 128, +2096)
//    db/skill_master_db.yml        SkillMasterInfo (sub_62E630: a skill's mastery item and count)
// ─────────────────────────────────────────────────────────────────────────────

struct SkillShopMsg {
    std::string text;                        // Message<i> (+2068 [i].text)
    int32_t     stype = 0;                   // NpcSType<i> (+2068 [i] + 4)
};

struct SkillShopRow {
    std::string table;
    int32_t     type = 0;                    // +0 Type (its CharacterInfo id, its model)
    std::string name;                        // the CharacterInfo name of Type (sub_622CD0)
    int32_t     pos_id = 0;                  // +4 PosId: the .md3 type-10 point
    int16_t     job_quest_id = 0;            // +2076 JobQuestID
    int32_t     limit_job_type = 0;          // +2080 LimitJobType
    int32_t     change_type = 0;             // +2084 ChangeType
    int32_t     change_job = 0;              // +2088 ChangeJob
    ShopRow     shop;                        // +8 PurchaseRate, +12 SellingRate, +16 the items (+2064 count)
    std::string limit_table;
    std::vector<int32_t> limit_skills;       // +2092 / +2096: LimitSkillMaster's SkillIDs
    std::vector<SkillShopMsg> msgs;          // +2068 / +2072: up to the last message written (at least 1)

    // sub_6D8980: the master masters this skill.
    bool masters(int skill) const
    {
        return std::find(limit_skills.begin(), limit_skills.end(), skill) != limit_skills.end();
    }
};

// A SkillMasterInfo row (sub_62E630): the item (and count) mastering a skill asks for.
struct SkillMasterInfo {
    int32_t  skill = 0;
    uint32_t item = 0;
    int32_t  count = 0;
};

class SkillShopDb
{
public:
    bool load(const std::string& talk_path, const std::string& item_path, const std::string& limit_path,
              const std::string& master_path)
    {
        std::unordered_map<std::string, std::vector<ShopItemRow>> items;
        std::unordered_map<std::string, std::vector<int32_t>> limits;
        {
            std::ifstream f(item_path);
            if (!f) { LOG_ERROR("SkillShopDb: cannot open %s", item_path.c_str()); return false; }
            DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                std::string t;
                ShopItemRow r;
                for (const auto& [k, v] : kvs) {
                    if      (k == "Table")        t = yaml_scalar(v);
                    else if (k == "ItemId")       r.item_id = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    else if (k == "PurchaseRate") r.purchase_rate = std::strtof(v.c_str(), nullptr);
                    else if (k == "SellingRate")  r.selling_rate = std::strtof(v.c_str(), nullptr);
                    else if (k == "OutOfStock")   r.out_of_stock = std::atoi(v.c_str()) != 0;
                }
                if (!t.empty()) items[t].push_back(r);
            });
        }
        {
            std::ifstream f(limit_path);
            if (!f) { LOG_ERROR("SkillShopDb: cannot open %s", limit_path.c_str()); return false; }
            DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                std::string t;
                int32_t id = 0;
                for (const auto& [k, v] : kvs) {
                    if (k == "Table")        t = yaml_scalar(v);
                    else if (k == "SkillID") id = std::atoi(v.c_str());
                }
                if (!t.empty()) limits[t].push_back(id);
            });
        }
        size_t rows = 0;
        {
            std::ifstream f(talk_path);
            if (!f) { LOG_ERROR("SkillShopDb: cannot open %s", talk_path.c_str()); return false; }
            DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                SkillShopRow r;
                r.shop.purchase_rate = 0.0f;         // (sub_62A3C0 keeps the rates as they are)
                r.shop.selling_rate = 0.0f;
                std::vector<SkillShopMsg> msgs(21);
                int last = 0;
                for (const auto& [k, v] : kvs) {
                    if      (k == "Table")            r.table = yaml_scalar(v);
                    else if (k == "Type")             r.type = std::atoi(v.c_str());
                    else if (k == "Name")             r.name = yaml_scalar(v);
                    else if (k == "PosId")            r.pos_id = std::atoi(v.c_str());
                    else if (k == "JobQuestID")       r.job_quest_id = static_cast<int16_t>(std::atoi(v.c_str()));
                    else if (k == "LimitJobType")     r.limit_job_type = std::atoi(v.c_str());
                    else if (k == "ChangeType")       r.change_type = std::atoi(v.c_str());
                    else if (k == "ChangeJob")        r.change_job = std::atoi(v.c_str());
                    else if (k == "PurchaseRate")     r.shop.purchase_rate = std::strtof(v.c_str(), nullptr);
                    else if (k == "SellingRate")      r.shop.selling_rate = std::strtof(v.c_str(), nullptr);
                    else if (k == "ItemTable")        r.shop.item_table = yaml_scalar(v);
                    else if (k == "LimitSkillMaster") r.limit_table = yaml_scalar(v);
                    else if (k.compare(0, 7, "Message") == 0 && k.size() > 7) {
                        const int i = std::atoi(k.c_str() + 7);
                        if (i >= 0 && i < 21) { msgs[static_cast<size_t>(i)].text = yaml_scalar(v); last = std::max(last, i); }
                    } else if (k.compare(0, 8, "NpcSType") == 0 && k.size() > 8) {
                        const int i = std::atoi(k.c_str() + 8);
                        if (i >= 0 && i < 21) msgs[static_cast<size_t>(i)].stype = std::atoi(v.c_str());
                    }
                }
                if (r.table.empty()) return;
                msgs.resize(static_cast<size_t>(last) + 1);
                r.msgs = std::move(msgs);
                r.shop.table = r.table;
                r.shop.npc_type = r.type;
                r.shop.name = r.name;
                r.shop.pos_id = r.pos_id;
                if (auto it = items.find(r.shop.item_table); it != items.end()) {
                    const size_t n = std::min<size_t>(it->second.size(), 128);
                    r.shop.items.assign(it->second.begin(), it->second.begin() + static_cast<std::ptrdiff_t>(n));
                }
                if (auto it = limits.find(r.limit_table); it != limits.end()) {
                    const size_t n = std::min<size_t>(it->second.size(), 128);
                    r.limit_skills.assign(it->second.begin(), it->second.begin() + static_cast<std::ptrdiff_t>(n));
                }
                tables_[r.table].push_back(std::move(r));
                ++rows;
            });
        }
        {
            std::ifstream f(master_path);
            if (!f) { LOG_ERROR("SkillShopDb: cannot open %s", master_path.c_str()); return false; }
            DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                SkillMasterInfo m;
                for (const auto& [k, v] : kvs) {
                    if      (k == "SkillId") m.skill = std::atoi(v.c_str());
                    else if (k == "ItemId")  m.item = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    else if (k == "ItemCnt") m.count = std::atoi(v.c_str());
                }
                if (m.skill && !masters_.count(m.skill)) masters_[m.skill] = m;
            });
        }
        LOG_INFO("SkillShopDb: %zu tables (%zu masters), %zu item tables, %zu skill lists, %zu mastery rows",
                 tables_.size(), rows, items.size(), limits.size(), masters_.size());
        return !tables_.empty();
    }

    const std::vector<SkillShopRow>* table(const std::string& name) const
    {
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }

    // sub_62E770: a skill's SkillMasterInfo.
    const SkillMasterInfo* master(int skill) const
    {
        auto it = masters_.find(skill);
        return it == masters_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<std::string, std::vector<SkillShopRow>> tables_;
    std::unordered_map<int, SkillMasterInfo> masters_;
};
