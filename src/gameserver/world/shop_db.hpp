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

// ─────────────────────────────────────────────────────────────────────────────
//  ShopDb — db/shop_db.yml (the Shop_* tables a map's MapInfoEx ShopTable names) and
//  db/shop_item_db.yml (the item tables those rows name). Retail GameInfoMgr sub_629D90 builds one
//  2580-byte record per row: +0 NpcType, +4 PosId, +8 Script[512], +520 PurchaseRate (<= 0 -> 1.0),
//  +524 SellingRate (<= 0 -> 0.5), +528 the item table's rows (sub_62A0A0: the first 128, 16 bytes
//  each: ItemId, PurchaseRate, SellingRate, u8 OutOfStock), +2576 the table's row count.
// ─────────────────────────────────────────────────────────────────────────────

struct ShopItemRow {
    uint32_t item_id = 0;
    float    purchase_rate = 0.0f;   // > 0 replaces the shop's (sub_720160)
    float    selling_rate = 0.0f;    // > 0 replaces the shop's (sub_720210) and lists it in S2C 0x00A9
    bool     out_of_stock = false;   // left out of the buy list (S2C 0x0047)
};

struct ShopRow {
    std::string table;
    int         npc_type = 0;        // the ShopNpc's CharacterInfo id (its model)
    std::string name;                // the CharacterInfo name of NpcType (sub_622CD0)
    int         pos_id = 0;          // the .md3 type-5 point it stands on
    float       purchase_rate = 1.0f;
    float       selling_rate = 0.5f;
    std::string item_table;
    std::string script;              // the greeting (S2C 0x00A8)
    std::vector<ShopItemRow> items;  // the item table's first 128 rows

    // sub_6D8310: the shop's row of an item, nullptr when it has none.
    const ShopItemRow* find(uint32_t item_id) const
    {
        for (const ShopItemRow& r : items) if (r.item_id == item_id) return &r;
        return nullptr;
    }
};

class ShopDb
{
public:
    bool load(const std::string& shop_path, const std::string& item_path)
    {
        std::unordered_map<std::string, std::vector<ShopItemRow>> items;
        {
            std::ifstream f(item_path);
            if (!f) { LOG_ERROR("ShopDb: cannot open %s", item_path.c_str()); return false; }
            std::string line, table;
            ShopItemRow cur;
            bool in_record = false;
            auto commit = [&]() { if (in_record && !table.empty()) items[table].push_back(cur); };
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t ns = line.find_first_not_of(' ');
                if (ns == std::string::npos || line[ns] == '#') continue;
                if (line.compare(ns, 2, "- ") == 0) {
                    commit();
                    cur = ShopItemRow();
                    table.clear();
                    in_record = true;
                    item_kv(line.substr(ns + 2), table, cur);
                } else if (in_record && ns >= 4) {
                    item_kv(line.substr(ns), table, cur);
                } else if (line.compare(ns, 7, "Footer:") == 0) {
                    commit();
                    in_record = false;
                }
            }
            commit();
        }
        std::ifstream f(shop_path);
        if (!f) { LOG_ERROR("ShopDb: cannot open %s", shop_path.c_str()); return false; }
        std::string line;
        ShopRow cur;
        bool in_record = false;
        size_t rows = 0;
        auto commit = [&]() {
            if (!in_record || cur.table.empty()) return;
            // sub_629D90: the rates' defaults; the item table's first 128 rows (sub_62A0A0).
            if (cur.purchase_rate <= 0.0f) cur.purchase_rate = 1.0f;
            if (cur.selling_rate <= 0.0f) cur.selling_rate = 0.5f;
            auto it = items.find(cur.item_table);
            if (it != items.end()) {
                const size_t n = std::min<size_t>(it->second.size(), 128);
                cur.items.assign(it->second.begin(), it->second.begin() + static_cast<std::ptrdiff_t>(n));
            }
            tables_[cur.table].push_back(cur);
            ++rows;
        };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                cur = ShopRow();
                cur.purchase_rate = 0.0f;
                cur.selling_rate = 0.0f;
                in_record = true;
                shop_kv(line.substr(ns + 2), cur);
            } else if (in_record && ns >= 4) {
                shop_kv(line.substr(ns), cur);
            } else if (line.compare(ns, 7, "Footer:") == 0) {
                commit();
                in_record = false;
            }
        }
        commit();
        LOG_INFO("ShopDb: loaded %zu shop tables (%zu shops), %zu item tables", tables_.size(), rows, items.size());
        return !tables_.empty();
    }

    const std::vector<ShopRow>* table(const std::string& name) const
    {
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<std::string, std::vector<ShopRow>> tables_;

    static void shop_kv(const std::string& s, ShopRow& r)
    {
        const size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const std::string v = s.substr(c + 1);
        if      (k == "Table")        r.table = yaml_scalar(v);
        else if (k == "NpcType")      r.npc_type = std::atoi(v.c_str());
        else if (k == "Name")         r.name = yaml_scalar(v);
        else if (k == "PosId")        r.pos_id = std::atoi(v.c_str());
        else if (k == "PurchaseRate") r.purchase_rate = std::strtof(v.c_str(), nullptr);
        else if (k == "SellingRate")  r.selling_rate = std::strtof(v.c_str(), nullptr);
        else if (k == "ItemTable")    r.item_table = yaml_scalar(v);
        else if (k == "Script")       r.script = yaml_scalar(v);
    }

    static void item_kv(const std::string& s, std::string& table, ShopItemRow& r)
    {
        const size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const std::string v = s.substr(c + 1);
        if      (k == "Table")        table = yaml_scalar(v);
        else if (k == "ItemId")       r.item_id = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "PurchaseRate") r.purchase_rate = std::strtof(v.c_str(), nullptr);
        else if (k == "SellingRate")  r.selling_rate = std::strtof(v.c_str(), nullptr);
        else if (k == "OutOfStock")   r.out_of_stock = std::atoi(v.c_str()) != 0;
    }
};
