#pragma once
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "logger.hpp"
#include "yaml_scalar.hpp"
#include "drill_db.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  The exchange shops (original server data, kto_libconfig.xml):
//    db/exchange_shop_db.yml        ExchangeShopContents (GameInfoMgr sub_62BD60; sub_62C180 takes the first
//                                   row of an ExchangeID, in the table's order)
//    db/exchange_shop_table_db.yml  the NpcTalk ExcShopTable tables (sub_62C060: an NPC's ExchangeIDs)
// ─────────────────────────────────────────────────────────────────────────────

// An ExchangeShopContents row (the record sub_65DF10 stores).
struct ExchangeInfo {
    int32_t id = 0;                          // +68 ExchangeID
    std::vector<uint32_t> res;               // +72 / +76: ResID1..3 up to the first blank one
    struct Req { uint32_t item = 0; int16_t num = 0; };
    std::vector<Req> req;                    // +88 / +92: ReqID1..5 (and ReqNum) up to the first blank ReqID
    int32_t fee = 0;                         // +132 Fee
};

class ExchangeShopDb
{
public:
    bool load(const std::string& contents_path, const std::string& tables_path)
    {
        {
            std::ifstream f(contents_path);
            if (!f) { LOG_ERROR("ExchangeShopDb: cannot open %s", contents_path.c_str()); return false; }
            DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                ExchangeInfo e;
                uint32_t res[3]{}, req[5]{};
                int req_num[5]{};
                for (const auto& [k, v] : kvs) {
                    if (k == "ExchangeID") e.id = std::atoi(v.c_str());
                    else if (k == "Fee")   e.fee = std::atoi(v.c_str());
                    else if (k.size() == 6 && k.compare(0, 5, "ResID") == 0 && k[5] >= '1' && k[5] <= '3')
                        res[k[5] - '1'] = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    else if (k.size() == 6 && k.compare(0, 5, "ReqID") == 0 && k[5] >= '1' && k[5] <= '5')
                        req[k[5] - '1'] = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    else if (k.size() == 7 && k.compare(0, 6, "ReqNum") == 0 && k[6] >= '1' && k[6] <= '5')
                        req_num[k[6] - '1'] = std::atoi(v.c_str());
                }
                for (int i = 0; i < 3 && res[i]; ++i) e.res.push_back(res[i]);
                for (int i = 0; i < 5 && req[i]; ++i) e.req.push_back({ req[i], static_cast<int16_t>(req_num[i]) });
                rows_.push_back(std::move(e));
            });
        }
        {
            std::ifstream f(tables_path);
            if (!f) { LOG_ERROR("ExchangeShopDb: cannot open %s", tables_path.c_str()); return false; }
            DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                std::string t;
                int id = 0;
                for (const auto& [k, v] : kvs) {
                    if (k == "Table") t = yaml_scalar(v);
                    else if (k == "ExchangeID") id = std::atoi(v.c_str());
                }
                if (!t.empty()) tables_[t].push_back(id);
            });
        }
        LOG_INFO("ExchangeShopDb: %zu exchanges, %zu shop tables", rows_.size(), tables_.size());
        return !rows_.empty();
    }

    // sub_62C180: the first row of an ExchangeID.
    const ExchangeInfo* get(int id) const
    {
        for (const ExchangeInfo& e : rows_) if (e.id == id) return &e;
        return nullptr;
    }

    // An NPC's ExcShopTable list (NpcTalk +120), nullptr when it has none.
    const std::vector<int>* table(const std::string& name) const
    {
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }

private:
    std::vector<ExchangeInfo> rows_;
    std::unordered_map<std::string, std::vector<int>> tables_;
};
