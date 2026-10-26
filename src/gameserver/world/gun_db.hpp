#pragma once
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include "logger.hpp"
#include "yaml_scalar.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  GunDb — db/gun_db.yml (the original Item_GunParam2): the gauge and the attack ranges of a gun
//  (an item of SubType 54). Retail: GameInfoMgr hash list by item id (loader sub_62E470, lookup
//  sub_625C30); a gun's ItemParam +88 points to its row (sub_624380, read by sub_719AE0).
// ─────────────────────────────────────────────────────────────────────────────

struct GunDef {
    uint32_t id = 0;                 // the gun's item id                               (+4)
    int gun_type = 0;                // GunType                                         (+8)
    int gauge_speed = 0;             // GaugeSpeed: the client's reload gauge rate     (+12)
    int pow_range = 0;               // PowRange: the power-shot window's end           (+16)
    int pow_start_point = 0;         // PowStartPoint: the power-shot window's start    (+20)
    int min_attack_range = 0;        // MinAttackRange                                  (+24)
    int max_attack_range = 0;        // MaxAttackRange                                  (+28)
    std::string name;
};

class GunDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("GunDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        GunDef cur;
        bool in_record = false;
        auto commit = [&]() { if (in_record && cur.id > 0) guns_.emplace(cur.id, cur); };   // the first row wins
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                cur = GunDef();
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
        LOG_INFO("GunDb: loaded %zu guns from %s", guns_.size(), path.c_str());
        return !guns_.empty();
    }

    const GunDef* get(uint32_t id) const
    {
        auto it = guns_.find(id);
        return it == guns_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<uint32_t, GunDef> guns_;

    static void kv(const std::string& s, GunDef& d)
    {
        const size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const std::string v = s.substr(c + 1);
        const int n = std::atoi(v.c_str());
        if      (k == "ID")             d.id               = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "GunType")        d.gun_type         = n;
        else if (k == "GaugeSpeed")     d.gauge_speed      = n;
        else if (k == "PowRange")       d.pow_range        = n;
        else if (k == "PowStartPoint")  d.pow_start_point  = n;
        else if (k == "MinAttackRange") d.min_attack_range = n;
        else if (k == "MaxAttackRange") d.max_attack_range = n;
        else if (k == "Name")           d.name             = yaml_scalar(v);
    }
};
