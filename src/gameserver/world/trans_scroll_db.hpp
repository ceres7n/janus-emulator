#pragma once
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include "logger.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  TransScrollDb — db/trans_scroll_db.yml (the original TransScrollInfo): the model a disguise kit
//  (Type 15, SubType 71) turns its user into. Retail: GameInfoMgr map by item id {+0 ID, +4
//  TransChrType} (getter sub_63D7C0, 0 = none).
// ─────────────────────────────────────────────────────────────────────────────

class TransScrollDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("TransScrollDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        uint32_t id = 0;
        int model = 0;
        bool in_record = false;
        auto commit = [&]() { if (in_record && id > 0 && !models_.count(id)) models_.emplace(id, model); };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            std::string body;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                id = 0; model = 0;
                in_record = true;
                body = line.substr(ns + 2);
            } else if (in_record && ns >= 4) {
                body = line.substr(ns);
            } else {
                if (line.compare(ns, 7, "Footer:") == 0) { commit(); in_record = false; }
                continue;
            }
            const size_t c = body.find(':');
            if (c == std::string::npos) continue;
            const std::string k = body.substr(0, c);
            if (k == "ID") id = static_cast<uint32_t>(std::strtoul(body.c_str() + c + 1, nullptr, 10));
            else if (k == "TransChrType") model = std::atoi(body.c_str() + c + 1);
        }
        commit();
        LOG_INFO("TransScrollDb: loaded %zu disguise kits from %s", models_.size(), path.c_str());
        return !models_.empty();
    }

    // sub_63D7C0: the item's TransChrType, 0 when it has no row.
    int model(uint32_t item_id) const
    {
        auto it = models_.find(item_id);
        return it == models_.end() ? 0 : it->second;
    }

private:
    std::unordered_map<uint32_t, int> models_;
};
