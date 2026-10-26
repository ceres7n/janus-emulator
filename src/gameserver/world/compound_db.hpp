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
//  The compounds (original server data, kto_libconfig.xml):
//    db/compound_db.yml    Compound_Potion / Compound_Rare / Compound_Throw (GameInfoMgr sub_62AD60:
//                          CompoundInfo, type 0 / 1 / 2 by table, keyed by CompoundID; sub_62B370)
//    db/compounder_db.yml  Compounder_Info and its lists (sub_62B0E0 / sub_62B210: at most 64 lists of
//                          {CompoundID, Probability, Hidden} with a Grade; an NPC's CompounderTable is the
//                          first list of that name, sub_628CB0)
// ─────────────────────────────────────────────────────────────────────────────

// A CompoundInfo record (sub_62AD60).
struct CompoundInfo {
    int32_t     type = 0;                    // +0: 0 Compound_Potion, 1 Compound_Rare, 2 Compound_Throw
    int16_t     res_lv = 0;                  // +4 ResLv
    std::string name;                        // +6 Name (logs)
    int32_t     id = 0;                      // +72 CompoundID
    std::vector<uint32_t> res;               // +76 / +80: ResID1..3 up to the first blank one
    struct Req { uint32_t item = 0; int16_t num = 0; };
    std::vector<Req> req;                    // +92 / +96: ReqID1..5 (and ReqNum) up to the first blank ReqID
    int16_t     probability = 0;             // +136 Probability
    int32_t     fee = 0;                     // +140 Fee
    uint32_t    waste = 0;                   // +144 WasteItem
};

// A compounder list (Compounder_Info row + its table; 2052 bytes per list in the retail).
struct CompounderList {
    std::string name;                        // +0 ListName
    int32_t     grade = 0;                   // +4 Grade
    struct Entry { int32_t id = 0; int16_t probability = 0; uint8_t hidden = 0; };
    std::vector<Entry> entries;              // +8: 8 bytes each
};

class CompoundDb
{
public:
    bool load(const std::string& compounds_path, const std::string& lists_path)
    {
        {
            std::ifstream f(compounds_path);
            if (!f) { LOG_ERROR("CompoundDb: cannot open %s", compounds_path.c_str()); return false; }
            DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                CompoundInfo c;
                uint32_t res[3]{}, req[5]{};
                int req_num[5]{};
                for (const auto& [k, v] : kvs) {
                    if (k == "Table") {
                        const std::string t = yaml_scalar(v);
                        c.type = t == "Compound_Rare" ? 1 : (t == "Compound_Throw" ? 2 : 0);
                    }
                    else if (k == "CompoundID")  c.id = std::atoi(v.c_str());
                    else if (k == "Name")        c.name = yaml_scalar(v);
                    else if (k == "ResLv")       c.res_lv = static_cast<int16_t>(std::atoi(v.c_str()));
                    else if (k == "Probability") c.probability = static_cast<int16_t>(std::atoi(v.c_str()));
                    else if (k == "Fee")         c.fee = std::atoi(v.c_str());
                    else if (k == "WasteItem")   c.waste = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    else if (k.size() == 6 && k.compare(0, 5, "ResID") == 0 && k[5] >= '1' && k[5] <= '3')
                        res[k[5] - '1'] = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    else if (k.size() == 6 && k.compare(0, 5, "ReqID") == 0 && k[5] >= '1' && k[5] <= '5')
                        req[k[5] - '1'] = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    else if (k.size() == 7 && k.compare(0, 6, "ReqNum") == 0 && k[6] >= '1' && k[6] <= '5')
                        req_num[k[6] - '1'] = std::atoi(v.c_str());
                }
                for (int i = 0; i < 3 && res[i]; ++i) c.res.push_back(res[i]);
                for (int i = 0; i < 5 && req[i]; ++i) c.req.push_back({ req[i], static_cast<int16_t>(req_num[i]) });
                if (!by_id_.count(c.id)) by_id_[c.id] = c;
            });
        }
        {
            std::ifstream f(lists_path);
            if (!f) { LOG_ERROR("CompoundDb: cannot open %s", lists_path.c_str()); return false; }
            DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                std::string name;
                int grade = 0;
                CompounderList::Entry e;
                for (const auto& [k, v] : kvs) {
                    if (k == "ListName")         name = yaml_scalar(v);
                    else if (k == "Grade")       grade = std::atoi(v.c_str());
                    else if (k == "CompoundID")  e.id = std::atoi(v.c_str());
                    else if (k == "Probability") e.probability = static_cast<int16_t>(std::atoi(v.c_str()));
                    else if (k == "Hidden")      e.hidden = static_cast<uint8_t>(std::atoi(v.c_str()));
                }
                if (name.empty()) return;
                CompounderList* l = nullptr;
                for (CompounderList& x : lists_) if (x.name == name) { l = &x; break; }
                if (!l) {
                    lists_.push_back({ name, grade, {} });
                    l = &lists_.back();
                }
                l->entries.push_back(e);
            });
        }
        LOG_INFO("CompoundDb: %zu compounds, %zu compounder lists", by_id_.size(), lists_.size());
        return !by_id_.empty();
    }

    // sub_62B370: the CompoundInfo of a CompoundID.
    const CompoundInfo* get(int id) const
    {
        auto it = by_id_.find(id);
        return it == by_id_.end() ? nullptr : &it->second;
    }

    // sub_628CB0: the first list of that name (an NPC's CompounderTable), nullptr when none.
    const CompounderList* list(const std::string& name) const
    {
        for (const CompounderList& l : lists_) if (l.name == name) return &l;
        return nullptr;
    }

private:
    std::unordered_map<int, CompoundInfo> by_id_;
    std::vector<CompounderList> lists_;
};
