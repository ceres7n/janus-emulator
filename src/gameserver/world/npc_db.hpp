#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <fstream>
#include "logger.hpp"
#include "yaml_scalar.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  NpcDb — the NpcTalk_<area> tables of the original server data (kto_libconfig.xml), loaded
//  from db/npc_db.yml: each table's rows in their order (a map's MapInfoEx NpcTable names its
//  table; GameInfoMgr sub_628CB0 loads it into 328-byte records). Type = the NPC's CharacterInfo
//  id, PosId = its .md3 type-4 point; every other column is kept as text for the dialogs, shops
//  and quests.
// ─────────────────────────────────────────────────────────────────────────────

struct NpcRow {
    int type = 0;           // Type (+0): CharacterInfo id
    int pos_id = 0;         // PosId (+4)
    std::string name;       // Name (the CharacterInfo CommonName of Type)
    std::vector<std::pair<std::string, std::string>> fields;   // the other columns, in order

    const std::string* get(const char* key) const
    {
        for (const auto& f : fields) if (f.first == key) return &f.second;
        return nullptr;
    }

    // A numeric column (kept as text; DataType Enum values may be '/'-separated flag lists that the
    // loader ORs, libconfig rule). 0 when absent.
    int num(const char* key) const
    {
        const std::string* v = get(key);
        if (!v) return 0;
        if (v->find('/') == std::string::npos) return std::atoi(v->c_str());
        int acc = 0;
        size_t a = 0;
        while (a <= v->size()) {
            size_t b = v->find('/', a);
            if (b == std::string::npos) b = v->size();
            acc |= std::atoi(v->substr(a, b - a).c_str());
            a = b + 1;
        }
        return acc;
    }

    // sub_60CC70: QuestId[i] (u16 at +8 + 2i, i = 0..4) as an int16, else -1.
    int quest_id(int i) const
    {
        static const char* kKeys[5] = { "QuestId", "QuestId1", "QuestId2", "QuestId3", "QuestId4" };
        if (i < 0 || i > 4) return -1;
        return static_cast<int16_t>(num(kKeys[i]));
    }
};

class NpcDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("NpcDb: cannot open %s", path.c_str()); return false; }
        std::string line, table;
        NpcRow cur;
        bool in_record = false;
        size_t rows = 0;
        auto commit = [&]() {
            if (in_record && !table.empty()) { tables_[table].push_back(cur); ++rows; }
        };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                cur = NpcRow();
                table.clear();
                in_record = true;
                kv(line.substr(ns + 2), table, cur);
            } else if (in_record && ns >= 4) {
                kv(line.substr(ns), table, cur);
            } else if (line.compare(ns, 7, "Footer:") == 0) {
                commit();
                in_record = false;
            }
        }
        commit();
        LOG_INFO("NpcDb: loaded %zu tables (%zu rows) from %s", tables_.size(), rows, path.c_str());
        return !tables_.empty();
    }

    const std::vector<NpcRow>* table(const std::string& name) const
    {
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<std::string, std::vector<NpcRow>> tables_;

    static void kv(const std::string& s, std::string& table, NpcRow& r)
    {
        size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const std::string v = yaml_scalar(s.substr(c + 1));
        if      (k == "Table") table    = v;
        else if (k == "Type")  r.type   = std::atoi(v.c_str());
        else if (k == "PosId") r.pos_id = std::atoi(v.c_str());
        else if (k == "Name")  r.name   = v;
        else                   r.fields.emplace_back(k, v);
    }
};
