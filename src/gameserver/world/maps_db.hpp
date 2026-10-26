#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
#include <fstream>
#include "logger.hpp"
#include "yaml_scalar.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  MapsDb — the map (zone) table, loaded from db/maps_db.yml (generated from the
//  original server data: MapInfoEx in kto_libconfig.xml + the arrival points of each
//  map's .md3, read the way Zone::BuildPoints sub_8113C0 does). Only these maps exist
//  for the GameServer (retail BuildWorld sub_818B60 builds one Zone per MapInfoEx row).
//
//  Class / Type are the retail Zone +20 / +16 (BuildWorld passes MapInfoEx Class, ID,
//  Type). Arrival (retail sub_8108F0, used for a restart or an invalid stored
//  position): a random type-2 point (sub_8133D0); if the map has none, a random type-1
//  point (sub_810D70); otherwise (500,400) (sub_810830).
// ─────────────────────────────────────────────────────────────────────────────

struct MapPoint { int id = 0, param = 0, x = 0, y = 0; };

struct MapDef {
    int id = 0, cls = 0, type = 0;
    int option = 0;     // Option (zone +1128, flags): e.g. 32 = no resurrection item (UseItemHelper case 29)
    int town = 0;  // the map's own town for `restart_town: zone` (derived, not retail; 0 = none)
    std::string name;
    std::string file;   // FileName: the map's .md3 (data\map\...), read for the zone attributes
    int monster_avr_num = 0;      // MonsterAvrNum (+280): the map's monster count the regeneration keeps
    std::string monster_table;    // MonsterTable: its Mon_<area> table (db/mon_spawn_db.yml)
    std::string npc_table;        // NpcTable: its NpcTalk_<area> table (db/npc_db.yml)
    std::string boss_monster_table;   // BossMonsterTable: its BossMon_* table (db/boss_mon_db.yml)
    std::string shop_table;           // ShopTable: its Shop_* table (db/shop_db.yml) — the ShopNpcs
    std::string skill_shop_table;     // SkillShopTable (+1060): its SkillShopTalk_* table (db/skill_shop_db.yml)
    float shop_price_rate = 1.0f;     // ShopPriceRate (+1032; sub_623D20: <= 0 -> 1.0)
    // The buried items (MapItemManager, drill.cpp): MinItemDepth (+292) / MaxItemDepth (+296, raised to
    // the min when below it; also the map's depth, ZoneMapAttr +8), ItemPercentage (+300, float: the
    // share of the diggable cells holding an item; 0 = none) and ItemDelaySec (+304, x1000 ms — loaded,
    // read by nothing in the 2014 server).
    int   min_item_depth = 0;
    int   max_item_depth = 0;
    float item_percentage = 0.0f;
    int   item_delay_ms = 0;
    std::vector<MapPoint> type2, type1, type7, type9;

    // Player vtbl+60 (sub_73AA70): entering such a map makes it the player's return town.
    bool is_return_town() const { return (type == 1 || type == 2) && cls != 3; }
};

class MapsDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("MapsDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        MapDef cur;
        bool in_record = false;
        auto commit = [&]() {
            if (in_record && cur.id > 0) {
                if (cur.max_item_depth < cur.min_item_depth) cur.max_item_depth = cur.min_item_depth;   // sub_623D20
                maps_[cur.id] = cur;
            }
        };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                cur = MapDef();
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
        LOG_INFO("MapsDb: loaded %zu maps from %s", maps_.size(), path.c_str());
        return !maps_.empty();
    }

    const std::unordered_map<int, MapDef>& all() const { return maps_; }

    const MapDef* get(int id) const
    {
        auto it = maps_.find(id);
        return it == maps_.end() ? nullptr : &it->second;
    }

    // The map's default arrival position (retail sub_8108F0 -> sub_8133D0 / sub_810D70 / (500,400)).
    void default_arrival(int map_id, int32_t& x, int32_t& y) const
    {
        x = 500; y = 400;
        const MapDef* m = get(map_id);
        if (!m) return;
        const std::vector<MapPoint>& pts = !m->type2.empty() ? m->type2 : m->type1;
        if (pts.empty()) return;
        const MapPoint& p = pts[static_cast<size_t>(std::rand()) % pts.size()];
        x = p.x; y = p.y;
    }

    // The arrival through a portal (changeZoneType 2: sub_819350 case 2 -> sub_810730): the
    // destination's type-1 point whose Param is the map we come from and whose Id is the portal's
    // connection id (sub_810CF0); else a random type-1 point (sub_810D70); else (500,400).
    void portal_arrival(int map_id, int from_map, int conn_id, int32_t& x, int32_t& y) const
    {
        x = 500; y = 400;
        const MapDef* m = get(map_id);
        if (!m) return;
        if (from_map) {
            for (const MapPoint& p : m->type1)
                if (p.param == from_map && p.id == conn_id) { x = p.x; y = p.y; return; }
        }
        if (m->type1.empty()) return;
        const MapPoint& p = m->type1[static_cast<size_t>(std::rand()) % m->type1.size()];
        x = p.x; y = p.y;
    }

    // The arrival of an NPC teleport (changeZoneType 3: sub_819350 case 3 -> sub_810A60 with the
    // position (0,0) -> sub_810A10): a random type-7 point (sub_813420); none -> the default
    // arrival (sub_8108F0).
    void teleport_arrival(int map_id, int32_t& x, int32_t& y) const
    {
        const MapDef* m = get(map_id);
        if (m && !m->type7.empty()) {
            const MapPoint& p = m->type7[static_cast<size_t>(std::rand()) % m->type7.size()];
            x = p.x; y = p.y;
            return;
        }
        default_arrival(map_id, x, y);
    }

    // The type-9 point `id` of a map (the zone list +0x7680, keyed by id; NpcMoveInfo DestId,
    // GameInfoMgr sub_84F1D0). False when the map has none.
    bool type9_point(int map_id, int id, int32_t& x, int32_t& y) const
    {
        const MapDef* m = get(map_id);
        if (!m) return false;
        for (const MapPoint& p : m->type9) if (p.id == id) { x = p.x; y = p.y; return true; }
        return false;
    }

private:
    std::unordered_map<int, MapDef> maps_;

    static void kv(const std::string& s, MapDef& m)
    {
        size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const std::string v = s.substr(c + 1);
        if      (k == "ID" || k == "Id") m.id   = std::atoi(v.c_str());
        else if (k == "Class")           m.cls  = std::atoi(v.c_str());
        else if (k == "Type")            m.type = std::atoi(v.c_str());
        else if (k == "Option")          m.option = std::atoi(v.c_str());
        else if (k == "Name")            m.name = yaml_scalar(v);
        else if (k == "FileName")        m.file = yaml_scalar(v);
        else if (k == "MonsterAvrNum")   m.monster_avr_num = std::atoi(v.c_str());
        else if (k == "MonsterTable")    m.monster_table = yaml_scalar(v);
        else if (k == "NpcTable")        m.npc_table = yaml_scalar(v);
        else if (k == "BossMonsterTable") m.boss_monster_table = yaml_scalar(v);
        else if (k == "ShopTable")       m.shop_table = yaml_scalar(v);
        else if (k == "SkillShopTable")  m.skill_shop_table = yaml_scalar(v);
        else if (k == "ShopPriceRate") {
            m.shop_price_rate = std::strtof(v.c_str(), nullptr);
            if (m.shop_price_rate <= 0.0f) m.shop_price_rate = 1.0f;
        }
        else if (k == "MinItemDepth")    m.min_item_depth = std::atoi(v.c_str());
        else if (k == "MaxItemDepth")    m.max_item_depth = std::atoi(v.c_str());
        else if (k == "ItemPercentage")  m.item_percentage = std::strtof(v.c_str(), nullptr);
        else if (k == "ItemDelaySec")    m.item_delay_ms = static_cast<int>(static_cast<long long>(
                                             static_cast<double>(std::strtof(v.c_str(), nullptr)) * 1000.0));
        else if (k == "Town")            m.town = std::atoi(v.c_str());
        else if (k == "Type2Points")     m.type2 = points(v, 32);   // sub_815480: at most 32
        else if (k == "Type1Points")     m.type1 = points(v);
        else if (k == "Type7Points")     m.type7 = points(v, 32);
        else if (k == "Type9Points")     m.type9 = points(v);
    }

    // "[[id, param, x, y], ...]" -> points (every 4 integers make one point), at most `cap`
    static std::vector<MapPoint> points(const std::string& v, size_t cap = static_cast<size_t>(-1))
    {
        std::vector<int> n;
        const char* p = v.c_str();
        while (*p) {
            if ((*p >= '0' && *p <= '9') || (*p == '-' && p[1] >= '0' && p[1] <= '9')) {
                char* end = nullptr;
                n.push_back(static_cast<int>(std::strtol(p, &end, 10)));
                p = end;
            } else {
                ++p;
            }
        }
        std::vector<MapPoint> out;
        for (size_t i = 0; i + 3 < n.size() && out.size() < cap; i += 4)
            out.push_back({ n[i], n[i + 1], n[i + 2], n[i + 3] });
        return out;
    }
};
