#pragma once
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include "logger.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  MemoryPortDb — db/memory_port_db.yml (the original Item_MemoryPortParam): a Memory Port item's slot list type
//  (0 = the 5-slot one, 1 = the 10-slot one) and slot count (retail GameInfoMgr sub_633E10: +4 the type).
// ─────────────────────────────────────────────────────────────────────────────
class MemoryPortDb
{
public:
    struct Info { uint32_t item{ 0 }; int type{ 0 }; int slots{ 0 }; };

    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("MemoryPortDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        Info cur;
        bool in = false;
        auto commit = [&]() { if (in && cur.item) rows_[cur.item] = cur; cur = Info{}; in = false; };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            std::string body;
            if (ns == 2 && line.compare(ns, 2, "- ") == 0) { commit(); in = true; body = line.substr(ns + 2); }
            else if (in && ns >= 4) body = line.substr(ns);
            else { if (line.compare(ns, 7, "Footer:") == 0) commit(); continue; }
            const size_t c = body.find(':');
            if (c == std::string::npos) continue;
            const std::string k = body.substr(0, c);
            const int v = std::atoi(body.c_str() + c + 1);
            if (k == "ItemId") cur.item = static_cast<uint32_t>(v);
            else if (k == "Type") cur.type = v;
            else if (k == "SlotNum") cur.slots = v;
        }
        commit();
        LOG_INFO("MemoryPortDb: loaded %zu memory ports from %s", rows_.size(), path.c_str());
        return !rows_.empty();
    }

    // sub_633E10: the item's row, or null.
    const Info* get(uint32_t item) const
    {
        auto it = rows_.find(item);
        return it == rows_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<uint32_t, Info> rows_;
};
