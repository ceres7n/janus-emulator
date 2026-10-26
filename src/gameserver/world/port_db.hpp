#pragma once
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "logger.hpp"
#include "yaml_scalar.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  The item teleports' tables:
//  RandomPortDb — db/random_port_db.yml (the original RandomPortParam): a broom / port item's (Type 15,
//    Effect 28) destinations. Retail loader (GameInfoMgr, key ID): only a ZoneID 1..999 counts, its slots
//    packed in order with their Ratio summed; the pick sub_6E0010 = rand() % sum against them in order.
//  TeleportAdvDb — db/teleport_adv_db.yml (the original TeleportAdvInfo): the Portable Port AD (Effect 20)
//    destinations by the index the client sends (loader sub_62E7D0: {ZoneIndex, ZoneID}).
// ─────────────────────────────────────────────────────────────────────────────

struct RandomPortDef {
    uint32_t id = 0;                         // ID (the item)
    std::string notice;                      // Notice (S2C 758), UTF-8
    std::vector<std::pair<int, int>> zones;  // the counted {ZoneID, Ratio}, packed in order
    int sum = 0;                             // the sum of their Ratio

    // sub_6E0010: rand() % sum, the first slot whose running sum passes it (0: none).
    int pick() const
    {
        if (sum <= 0) return 0;                                   // (rand() % 0 in the retail)
        const int r = std::rand() % sum;
        int acc = 0;
        for (const auto& z : zones) {
            acc += z.second;
            if (acc > r) return z.first;
        }
        return 0;
    }
};

class RandomPortDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("RandomPortDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        Raw cur;
        bool in_record = false;
        auto commit = [&]() { if (in_record && cur.id > 0 && !rows_.count(cur.id)) rows_.emplace(cur.id, build(cur)); };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t ns = line.find_first_not_of(' ');
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
        LOG_INFO("RandomPortDb: loaded %zu port items from %s", rows_.size(), path.c_str());
        return !rows_.empty();
    }

    // sub_632DB0: the item's row.
    const RandomPortDef* get(uint32_t item_id) const
    {
        auto it = rows_.find(item_id);
        return it == rows_.end() ? nullptr : &it->second;
    }

private:
    struct Raw { uint32_t id = 0; std::string notice; int zone[20]{}; int ratio[20]{}; };
    std::unordered_map<uint32_t, RandomPortDef> rows_;

    static RandomPortDef build(const Raw& r)
    {
        RandomPortDef d;
        d.id = r.id;
        d.notice = r.notice;
        for (int i = 0; i < 20; ++i) {
            if (r.zone[i] > 0 && r.zone[i] < 1000) {
                d.zones.emplace_back(r.zone[i], r.ratio[i]);
                d.sum += r.ratio[i];
            }
        }
        return d;
    }

    static void kv(const std::string& s, Raw& r)
    {
        const size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const std::string v = s.substr(c + 1);
        if (k == "ID")          { r.id = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10)); return; }
        if (k == "Notice")      { r.notice = yaml_scalar(v); return; }
        if (k.compare(0, 7, "ZoneID_") == 0) {
            const int i = std::atoi(k.c_str() + 7);
            if (i >= 0 && i < 20) r.zone[i] = std::atoi(v.c_str());
        } else if (k.compare(0, 6, "Ratio_") == 0) {
            const int i = std::atoi(k.c_str() + 6);
            if (i >= 0 && i < 20) r.ratio[i] = std::atoi(v.c_str());
        }
    }
};

class TeleportAdvDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("TeleportAdvDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        int index = -1, zone = 0;
        bool in_record = false;
        auto commit = [&]() { if (in_record && index >= 0 && !zones_.count(index)) zones_.emplace(index, zone); };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            std::string body;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                index = -1; zone = 0;
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
            if (k == "ZoneIndex") index = std::atoi(body.c_str() + c + 1);
            else if (k == "ZoneID") zone = std::atoi(body.c_str() + c + 1);
        }
        commit();
        LOG_INFO("TeleportAdvDb: loaded %zu destinations from %s", zones_.size(), path.c_str());
        return !zones_.empty();
    }

    // sub_62E8F0: the zone of an index, 0 when there is no such row.
    int zone(int index) const
    {
        auto it = zones_.find(index);
        return it == zones_.end() ? 0 : it->second;
    }
    bool has(int index) const { return zones_.count(index) != 0; }

private:
    std::unordered_map<int, int> zones_;
};
