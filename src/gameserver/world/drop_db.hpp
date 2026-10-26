#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
#include <fstream>
#include "logger.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  DropDb — the monster drop tables of the original server data:
//    db/mon_item_drop_db.yml   MonItemDropInfo        a monster's drop list (MonsterParamEx2
//                                                     ItemDropIdx): 10 items + 5 sub-tables
//    db/mon_item_table_db.yml  MonItemTable_DropInfo  the sub-tables: 20 items each
//
//  Built exactly like the retail GameInfoMgr loaders, float arithmetic included:
//    drop list (server decomp ~279725): every present Item/Table (id != 0) is appended in slot
//      order with its chance as a float (Cnt 0 -> 1); the chances (items then tables) are summed
//      in float; a sum over 1.0 divides each of them by it; then each becomes a band
//      [lo, hi) of (int)(chance * 10000.0), items first, continuing with the tables.
//    sub-table (sub_6368E0): the rows are read in file order into ONE 20-slot buffer that is
//      never cleared; an item is stored at its own slot number, `count` = the number of filled
//      slots, total = (int)(sum * 10000.0) (10000 and the chances divided by the sum when it is
//      over 1.0); the bands are built over slots 0..count-1. A row with an empty slot in the
//      middle therefore uses the values the earlier rows left in that slot and never uses its
//      last item (retail behaviour, kept: table 200 of monsters 2186..2190).
// ─────────────────────────────────────────────────────────────────────────────

struct DropItem {
    uint32_t item = 0;
    float    rate = 0.0f;
    int32_t  lo = 0, hi = 0;   // band in 0..10000 (sub-table: 0..total)
    int32_t  cnt = 0;          // amount (Cnt, 0 -> 1)
};

struct DropTableRef {
    uint32_t table = 0;        // MonItemTable_DropInfo id
    float    rate = 0.0f;
    int32_t  lo = 0, hi = 0;
};

struct DropList {              // retail struct: item count, table count, items (+8), tables (+208)
    std::vector<DropItem>     items;
    std::vector<DropTableRef> tables;
};

struct DropSubTable {          // retail struct: 20 items (20 bytes each), count (+400), total (+404)
    DropItem slot[20];
    int      count = 0;
    int      total = 0;
};

// One rolled drop: an item (or gelder pile) id and its amount (the retail 16-byte drop entry
// {u32 id, u16 owner, u8 1, u16 count, ...} of sub_729220 / the 8-byte pile of sub_7FEA40).
struct DropLoot {
    uint32_t item = 0;
    uint16_t count = 0;
};

// GameInfoMgr GelderUnit (sub_62FB00): the gelder items 200..203 and the most each pile holds.
// sub_7FEA40 splits an amount into amount/10000 piles of item 203, then the rest as one pile of
// the smallest kind that holds it. When the rest is 0 it returns 0 piles — an exact multiple of
// 10000 drops no gelder at all (retail behaviour, kept).
inline std::vector<DropLoot> split_gelder(int amount)
{
    static constexpr uint32_t kItem[4] = { 200, 201, 202, 203 };
    static constexpr int      kMax[4]  = { 50, 500, 2000, 10000 };
    std::vector<DropLoot> out;
    const int n = amount / kMax[3];
    for (int i = 0; i < n; ++i) out.push_back({ kItem[3], static_cast<uint16_t>(kMax[3]) });
    const int rest = amount % kMax[3];
    if (rest == 0) return {};
    for (int t = 0; t < 4; ++t) {
        const bool fits = (t == 0) ? (rest <= kMax[0]) : (kMax[t - 1] < rest && kMax[t] >= rest);
        if (fits) { out.push_back({ kItem[t], static_cast<uint16_t>(rest) }); return out; }
    }
    return {};
}

class DropDb
{
public:
    bool load(const std::string& lists_path, const std::string& tables_path)
    {
        const bool a = load_lists(lists_path);
        const bool b = load_tables(tables_path);
        return a && b;
    }

    const DropList* list(uint32_t id) const
    {
        auto it = lists_.find(id);
        return it == lists_.end() ? nullptr : &it->second;
    }
    const DropSubTable* table(uint32_t id) const
    {
        auto it = tables_.find(id);
        return it == tables_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<uint32_t, DropList>     lists_;
    std::unordered_map<uint32_t, DropSubTable> tables_;

    // One YAML record: ID + ItemN / RateN / CntN (N < 20) + TableN / TableRateN (N < 5).
    struct Raw {
        uint32_t id = 0;
        uint32_t item[20]{};
        double   rate[20]{};
        int32_t  cnt[20]{};
        uint32_t table[5]{};
        double   trate[5]{};
    };

    // Reads the records of a Header/Body/Footer list file in file order.
    template <class F>
    static bool read_records(const std::string& path, const char* who, F&& on_record)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("DropDb: cannot open %s (%s)", path.c_str(), who); return false; }
        std::string line;
        Raw cur;
        bool in_record = false;
        auto commit = [&]() { if (in_record && cur.id > 0) on_record(cur); };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                cur = Raw();
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
        return true;
    }

    static void kv(const std::string& s, Raw& r)
    {
        size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const char* v = s.c_str() + c + 1;
        auto slot = [&](const char* prefix, int limit) -> int {   // "Item12" -> 12, else -1
            const size_t n = std::char_traits<char>::length(prefix);
            if (k.size() <= n || k.compare(0, n, prefix) != 0) return -1;
            for (size_t i = n; i < k.size(); ++i) if (k[i] < '0' || k[i] > '9') return -1;
            const int idx = std::atoi(k.c_str() + n);
            return idx < limit ? idx : -1;
        };
        int i;
        if (k == "ID")                              r.id = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        else if ((i = slot("TableRate", 5)) >= 0)   r.trate[i] = std::strtod(v, nullptr);
        else if ((i = slot("Table", 5)) >= 0)       r.table[i] = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        else if ((i = slot("Item", 20)) >= 0)       r.item[i]  = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        else if ((i = slot("Rate", 20)) >= 0)       r.rate[i]  = std::strtod(v, nullptr);
        else if ((i = slot("Cnt", 20)) >= 0)        r.cnt[i]   = static_cast<int16_t>(std::atoi(v));   // sub_8D60E0: i16
    }

    // MonItemDropInfo (retail loader, server decomp ~279725).
    bool load_lists(const std::string& path)
    {
        const bool ok = read_records(path, "MonItemDropInfo", [&](const Raw& r) {
            DropList dl;
            float sum = 0.0f;
            for (int k = 0; k < 10; ++k) {
                if (!r.item[k]) continue;
                DropItem e;
                e.item = r.item[k];
                e.rate = static_cast<float>(r.rate[k]);
                e.cnt  = r.cnt[k] ? r.cnt[k] : 1;
                sum = sum + e.rate;
                dl.items.push_back(e);
            }
            for (int k = 0; k < 5; ++k) {
                if (!r.table[k]) continue;
                DropTableRef t;
                t.table = r.table[k];
                t.rate  = static_cast<float>(r.trate[k]);
                sum = sum + t.rate;
                dl.tables.push_back(t);
            }
            if (sum > 1.0) {
                for (DropItem& e : dl.items)      e.rate = e.rate / sum;
                for (DropTableRef& t : dl.tables) t.rate = t.rate / sum;
            }
            int32_t acc = 0;
            for (DropItem& e : dl.items) {
                e.lo = acc;
                e.hi = acc + static_cast<int32_t>(static_cast<double>(e.rate) * 10000.0);
                acc = e.hi;
            }
            for (DropTableRef& t : dl.tables) {
                t.lo = acc;
                t.hi = acc + static_cast<int32_t>(static_cast<double>(t.rate) * 10000.0);
                acc = t.hi;
            }
            lists_[r.id] = std::move(dl);
        });
        LOG_INFO("DropDb: loaded %zu drop lists from %s", lists_.size(), path.c_str());
        return ok && !lists_.empty();
    }

    // MonItemTable_DropInfo (retail loader sub_6368E0) — file order, one persistent slot buffer.
    bool load_tables(const std::string& path)
    {
        DropItem buf[20]{};
        const bool ok = read_records(path, "MonItemTable_DropInfo", [&](const Raw& r) {
            int   count = 0;
            float sum = 0.0f;
            for (int k = 0; k < 20; ++k) {
                if (!r.item[k]) continue;
                buf[k].item = r.item[k];
                buf[k].rate = static_cast<float>(r.rate[k]);
                buf[k].cnt  = r.cnt[k] ? r.cnt[k] : 1;
                sum = sum + buf[k].rate;
                ++count;
            }
            int total = static_cast<int>(static_cast<double>(sum) * 10000.0);
            if (sum > 1.0) {
                total = 10000;
                for (int i = 0; i < count; ++i) buf[i].rate = buf[i].rate / sum;
            }
            int32_t acc = 0;
            for (int j = 0; j < count; ++j) {
                buf[j].lo = acc;
                buf[j].hi = acc + static_cast<int32_t>(static_cast<double>(buf[j].rate) * 10000.0);
                acc = buf[j].hi;
            }
            DropSubTable& t = tables_[r.id];
            for (int k = 0; k < 20; ++k) t.slot[k] = buf[k];
            t.count = count;
            t.total = total;
        });
        LOG_INFO("DropDb: loaded %zu drop sub-tables from %s", tables_.size(), path.c_str());
        return ok && !tables_.empty();
    }
};
