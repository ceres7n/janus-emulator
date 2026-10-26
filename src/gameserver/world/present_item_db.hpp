#pragma once
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "logger.hpp"
#include "drill_db.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  PresentItemDb — db/present_item_db.yml (PresentItemParam2 of the original server data, GameInfoMgr's
//  loader: one 264-byte record per Id): the gift boxes (ItemParam Type 15 Effect 22). The record: +4 Id,
//  +8 the slots that have an item, +12 DropCnt, +16 the rates' sum, +20 Type, +24 the slots {DropId,
//  DropRate, ItemCnt (0 -> 1)} packed in column order (a DropId 0 column is skipped).
// ─────────────────────────────────────────────────────────────────────────────

struct PresentItem {
    struct Slot {
        uint32_t item = 0;
        int32_t  rate = 0;
        int32_t  cnt  = 1;
    };
    uint32_t id = 0;
    int32_t  type = 0;               // 0 random, 1 random and distinct (32 tries), 2 the slots in order
    int32_t  drop_cnt = 0;           // the picks
    int32_t  rate_sum = 0;
    std::vector<Slot> slots;
};

class PresentItemDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("PresentItemDb: cannot open %s", path.c_str()); return false; }
        DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
            PresentItem b;
            uint32_t item[20]{};
            int32_t  rate[20]{};
            int32_t  cnt[20]{};
            for (const auto& [k, v] : kvs) {
                if      (k == "Id")      b.id = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                else if (k == "Type")    b.type = std::atoi(v.c_str());
                else if (k == "DropCnt") b.drop_cnt = std::atoi(v.c_str());
                else {
                    int i = -1;
                    const char* field = nullptr;
                    for (const char* p : { "DropId_", "DropRate_", "ItemCnt_" })
                        if (k.compare(0, std::char_traits<char>::length(p), p) == 0) {
                            field = p;
                            i = std::atoi(k.c_str() + std::char_traits<char>::length(p));
                            break;
                        }
                    if (!field || i < 0 || i >= 20) continue;
                    if (field[4] == 'I')      item[i] = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    else if (field[4] == 'R') rate[i] = std::atoi(v.c_str());
                    else                      cnt[i]  = std::atoi(v.c_str());
                }
            }
            if (!b.id) return;
            for (int i = 0; i < 20; ++i) {
                if (!item[i]) continue;
                b.slots.push_back({ item[i], rate[i], cnt[i] ? cnt[i] : 1 });
                b.rate_sum += rate[i];
            }
            boxes_[b.id] = std::move(b);
        });
        LOG_INFO("PresentItemDb: %zu gift boxes", boxes_.size());
        return !boxes_.empty();
    }

    // sub_62FA20: the box's record, nullptr when it has none.
    const PresentItem* get(uint32_t id) const
    {
        auto it = boxes_.find(id);
        return it == boxes_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<uint32_t, PresentItem> boxes_;
};
