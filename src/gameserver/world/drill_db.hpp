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
//  The drill tables (original server data, kto_libconfig.xml):
//    DrillDb   — db/drill_db.yml (Item_DrillParam, GameInfoMgr sub_625910: the drill of a Type-6
//                item, ItemParam +88), db/drill_grade_exp_db.yml (DrillGradeExpRate, sub_639580) and
//                db/stress_point_db.yml (StressPointInfo, sub_63DB20);
//    MapItemDb — what a drill digs up in a map: db/map_item_db.yml (MapItem_All) with the tables of
//                db/map_item_table_db.yml (MapItem_ETC_Table) and the rate bonuses of
//                db/map_item_event_add_db.yml (MapItem_Event_Add, sub_629FF0), built per map the way
//                GameInfoMgr::LoadMapItemInfo (sub_640AE0) builds the map info +308.
// ─────────────────────────────────────────────────────────────────────────────

struct DrillParam {
    uint32_t id = 0;                    // Id                                         (+4)
    int16_t  grade = 0;                 // Grade                                      (+8, i16)
    int32_t  ground_nature = 0;         // GroundNature: 0 any ground                 (+12)
    int16_t  dig_depth = 0;             // DigDepth                                   (+16)
    int16_t  bar_eff_area = 0;          // BarEffArea                                 (+18)
    int16_t  bar_speed = 0;             // BarSpeed                                   (+20)
    int16_t  scoop_mass = 0;            // ScoopMass                                  (+22)
    int16_t  drill_life = 0;            // DrillLife: a new drill's count             (+24)
    std::string effect;                 // DrillEffect                                (+26)
    float    cdm_gauge_max = 0.0f;      // CDM_GaugeMax                               (+288)
    float    cdm_stress_bonus = 0.0f;   // CDM_StressBonus                            (+292)
    int32_t  cdm_seconds = 0;           // CDM_Seconds                                (+296)
    float    cdm_ratio_bang = 0.0f;     // CDM_RatioBang                              (+300)
    float    wedding_stress = 0.0f;     // Wedding_Stress                             (+304)
    float    wedding_ratio_bang = 0.0f; // Wedding_RatioBang                          (+308)
    float    random_spot_ratio = 0.0f;  // RandomSpot_Ratio                           (+312)
    std::string name;
};

// The values a client keys the C2S 0x31 result's CRC with and stops its gauge at — its own libconfig's
// Item_DrillParam (the server's drill_db row, or one of db/drill_client_db.yml: a Season 2 client).
struct DrillCheck {
    std::string source;                 // "server", or the drill_client_db.yml Source
    int16_t  dig_depth = 0;
    int16_t  bar_eff_area = 0;
    int16_t  bar_speed = 0;
    int16_t  scoop_mass = 0;
};

class DrillDb
{
public:
    // db/drill_client_db.yml: other clients' values of some drills (optional — none = the server's only).
    bool load_client(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_INFO("DrillDb: no %s (only the server's drill values are accepted)", path.c_str()); return false; }
        size_t n = 0;
        each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
            uint32_t id = 0;
            DrillCheck c;
            for (const auto& [k, v] : kvs) {
                if      (k == "Id")         id = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                else if (k == "Source")     c.source = yaml_scalar(v);
                else if (k == "DigDepth")   c.dig_depth = static_cast<int16_t>(std::atoi(v.c_str()));
                else if (k == "BarEffArea") c.bar_eff_area = static_cast<int16_t>(std::atoi(v.c_str()));
                else if (k == "BarSpeed")   c.bar_speed = static_cast<int16_t>(std::atoi(v.c_str()));
                else if (k == "ScoopMass")  c.scoop_mass = static_cast<int16_t>(std::atoi(v.c_str()));
            }
            if (!id) return;
            client_[id].push_back(c);
            ++n;
        });
        LOG_INFO("DrillDb: %zu client drill rows (%zu drills) from %s", n, client_.size(), path.c_str());
        return true;
    }

    // The client rows of a drill (nullptr = none).
    const std::vector<DrillCheck>* client_checks(uint32_t item_id) const
    {
        auto it = client_.find(item_id);
        return it == client_.end() ? nullptr : &it->second;
    }

    // The C2S 0x31 check value (Player CMD_END_DRILL): the result's crc undone with the gauge values; it must
    // give back the dig's seed (& 0xFFFE).
    static uint16_t result_calc(const DrillCheck& k, uint16_t crc, int depth, int cnt)
    {
        const uint16_t v = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(k.bar_speed) << 1) ^
                                                 static_cast<uint16_t>(k.bar_eff_area) ^
                                                 (static_cast<uint16_t>(k.scoop_mass) << 5) ^ (depth << 8) ^ (cnt << 12));
        return static_cast<uint16_t>(v & 0xFFFE);
    }

    // Which values the client keyed a result with — the CRC is the client's own Item_DrillParam (client
    // sub_494880): the server's row, else the first client row of this drill whose check gives the seed (a
    // Season 2 client's libconfig_twn has other values for some drills; the Season 2 GameServer checks the
    // same CRC with its own data, sub_4EF5A0 CMD_END_DRILL). false = none (a forged / broken result).
    bool keyed_with(const DrillParam& dp, uint16_t crc, int depth, int cnt, uint16_t seed, DrillCheck& out) const
    {
        out = DrillCheck{ "server", dp.dig_depth, dp.bar_eff_area, dp.bar_speed, dp.scoop_mass };
        if (result_calc(out, crc, depth, cnt) == seed) return true;
        if (const std::vector<DrillCheck>* alts = client_checks(dp.id))
            for (const DrillCheck& k : *alts)
                if (result_calc(k, crc, depth, cnt) == seed) { out = k; return true; }
        return false;
    }

    bool load(const std::string& drill_path, const std::string& grade_exp_path, const std::string& stress_path)
    {
        bool ok = true;
        {
            std::ifstream f(drill_path);
            if (!f) { LOG_ERROR("DrillDb: cannot open %s", drill_path.c_str()); ok = false; }
            else {
                each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                    DrillParam d;
                    for (const auto& [k, v] : kvs) {
                        if      (k == "Id")                d.id = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                        else if (k == "Name")              d.name = yaml_scalar(v);
                        else if (k == "Grade")             d.grade = static_cast<int16_t>(std::atoi(v.c_str()));
                        else if (k == "GroundNature")      d.ground_nature = std::atoi(v.c_str());
                        else if (k == "DigDepth")          d.dig_depth = static_cast<int16_t>(std::atoi(v.c_str()));
                        else if (k == "BarEffArea")        d.bar_eff_area = static_cast<int16_t>(std::atoi(v.c_str()));
                        else if (k == "BarSpeed")          d.bar_speed = static_cast<int16_t>(std::atoi(v.c_str()));
                        else if (k == "ScoopMass")         d.scoop_mass = static_cast<int16_t>(std::atoi(v.c_str()));
                        else if (k == "DrillLife")         d.drill_life = static_cast<int16_t>(std::atoi(v.c_str()));
                        else if (k == "DrillEffect")       d.effect = yaml_scalar(v);
                        else if (k == "CDM_GaugeMax")      d.cdm_gauge_max = std::strtof(v.c_str(), nullptr);
                        else if (k == "CDM_StressBonus")   d.cdm_stress_bonus = std::strtof(v.c_str(), nullptr);
                        else if (k == "CDM_Seconds")       d.cdm_seconds = std::atoi(v.c_str());
                        else if (k == "CDM_RatioBang")     d.cdm_ratio_bang = std::strtof(v.c_str(), nullptr);
                        else if (k == "Wedding_Stress")    d.wedding_stress = std::strtof(v.c_str(), nullptr);
                        else if (k == "Wedding_RatioBang") d.wedding_ratio_bang = std::strtof(v.c_str(), nullptr);
                        else if (k == "RandomSpot_Ratio")  d.random_spot_ratio = std::strtof(v.c_str(), nullptr);
                    }
                    if (d.id) drills_.emplace(d.id, d);   // the hash map keeps the first row of a key
                });
            }
        }
        {
            std::ifstream f(grade_exp_path);
            if (!f) { LOG_ERROR("DrillDb: cannot open %s", grade_exp_path.c_str()); ok = false; }
            else {
                each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                    float r = 0.0f;
                    for (const auto& [k, v] : kvs) if (k == "ExpRate") r = std::strtof(v.c_str(), nullptr);
                    grade_exp_.push_back(r);          // sub_639580: row i -> rate[i]
                });
            }
        }
        {
            std::ifstream f(stress_path);
            if (!f) { LOG_ERROR("DrillDb: cannot open %s", stress_path.c_str()); ok = false; }
            else {
                each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                    int t = -1; float p = 0.0f;
                    for (const auto& [k, v] : kvs) {
                        if (k == "StressType") t = std::atoi(v.c_str());
                        else if (k == "Point") p = std::strtof(v.c_str(), nullptr);
                    }
                    if (t >= 0 && t < 5) stress_[t] = p;   // sub_63DB20: 5 rows by StressType
                });
            }
        }
        LOG_INFO("DrillDb: %zu drills, %zu grade EXP rates", drills_.size(), grade_exp_.size());
        return ok && !drills_.empty();
    }

    // sub_625B60: the drill parameters of an item id (nullptr when it is no drill).
    const DrillParam* get(uint32_t item_id) const
    {
        auto it = drills_.find(item_id);
        return it == drills_.end() ? nullptr : &it->second;
    }

    // sub_62B9F0: rate[Grade], a Grade above the row count reading the last row. (Grade == the row
    // count reads the retail array's uninitialised extra element — no drill has such a Grade; the
    // last row is used.)
    double grade_exp_rate(int grade) const
    {
        if (grade_exp_.empty()) return 0.0;
        const int n = static_cast<int>(grade_exp_.size());
        if (grade >= n) grade = n - 1;
        if (grade < 0) grade = 0;
        return grade_exp_[static_cast<size_t>(grade)];
    }

    // sub_63DB00: the Crazy Drill gauge points of a stress type (0..4).
    float stress_point(int type) const { return (type >= 0 && type < 5) ? stress_[type] : 0.0f; }

private:
    std::unordered_map<uint32_t, DrillParam> drills_;
    std::unordered_map<uint32_t, std::vector<DrillCheck>> client_;
    std::vector<float> grade_exp_;
    float stress_[5]{};

    template <typename Fn>
    static void each_record(std::ifstream& f, Fn&& fn)
    {
        std::string line;
        std::vector<std::pair<std::string, std::string>> kvs;
        bool in_record = false;
        auto commit = [&]() { if (in_record) fn(kvs); kvs.clear(); };
        auto kv = [&](const std::string& s) {
            const size_t c = s.find(':');
            if (c != std::string::npos) kvs.emplace_back(s.substr(0, c), s.substr(c + 1));
        };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                in_record = true;
                kv(line.substr(ns + 2));
            } else if (in_record && ns >= 4) {
                kv(line.substr(ns));
            } else if (line.compare(ns, 7, "Footer:") == 0) {
                commit();
                in_record = false;
            }
        }
        commit();
    }

    friend class MapItemDb;
    friend class QuestDb;
    friend class ExchangeShopDb;
    friend class RefineDb;
    friend class MaturingDb;
    friend class CompoundDb;
    friend class SkillShopDb;
    friend class PresentItemDb;
};

// The item list of a map item type (the retail phList<MapItemDropInfo>: insertion order, duplicates
// kept, {u32 item, u16 rate}).
struct MapItemEntry { uint32_t item = 0; uint16_t rate = 0; };

// The map info +308 (sub_6D8490): [0] the total rate, [1..6] each type's rate, [7..12] the sum of each
// type's item rates, +52 + 112*type the item lists. Types 0..4 = SpecialT, EquipT, Normal1T..3T;
// type 5 = the map's own Item0..29.
struct MapItemInfo {
    int32_t total = 0;
    int32_t type_rate[6]{};
    int32_t item_sum[6]{};
    std::vector<MapItemEntry> items[6];
};

class MapItemDb
{
public:
    bool load(const std::string& map_path, const std::string& table_path, const std::string& event_path)
    {
        // MapItem_Event_Add (sub_629FF0 -> GameInfoMgr +328976, read by sub_648800(0..3)).
        {
            std::ifstream f(event_path);
            if (!f) LOG_ERROR("MapItemDb: cannot open %s", event_path.c_str());
            else DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                int t = -1; float r = 0.0f;
                for (const auto& [k, v] : kvs) {
                    if (k == "Type") t = std::atoi(v.c_str());
                    else if (k == "Rate_Add") r = std::strtof(v.c_str(), nullptr);
                }
                if (t >= 0 && t < 4) rate_add_[t] = r;
            });
        }
        // MapItem_ETC_Table rows in their order.
        struct TableRow { std::string id; std::vector<MapItemEntry> items; };
        std::vector<TableRow> tables;
        {
            std::ifstream f(table_path);
            if (!f) { LOG_ERROR("MapItemDb: cannot open %s", table_path.c_str()); return false; }
            DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                TableRow t;
                uint32_t item[30]{}; uint32_t rate[30]{};
                for (const auto& [k, v] : kvs) {
                    if (k == "Id") t.id = yaml_scalar(v);
                    else pair_field(k, v, item, rate);
                }
                for (int i = 0; i < 30; ++i)       // sub_641060: Item%d / Rate%d, both non-zero
                    if (item[i] && static_cast<uint16_t>(rate[i]))
                        t.items.push_back({ item[i], static_cast<uint16_t>(rate[i]) });
                tables.push_back(std::move(t));
            });
        }
        // MapItem_All, the first row of each map (LoadMapItemInfo stops at it).
        std::ifstream f(map_path);
        if (!f) { LOG_ERROR("MapItemDb: cannot open %s", map_path.c_str()); return false; }
        DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
            int id = 0;
            std::string tname[5];
            int16_t trate[5]{};
            uint32_t item[30]{}; uint32_t rate[30]{};
            static const char* kT[5] = { "SpecialT", "EquipT", "Normal1T", "Normal2T", "Normal3T" };
            static const char* kR[5] = { "S_Rate", "E_Rate", "N1_Rate", "N2_Rate", "N3_Rate" };
            for (const auto& [k, v] : kvs) {
                if (k == "Id") { id = std::atoi(v.c_str()); continue; }
                bool done = false;
                for (int t = 0; t < 5 && !done; ++t) {
                    if (k == kT[t]) { tname[t] = yaml_scalar(v); done = true; }
                    else if (k == kR[t]) { trate[t] = static_cast<int16_t>(std::atoi(v.c_str())); done = true; }
                }
                if (!done) pair_field(k, v, item, rate);
            }
            if (id <= 0 || maps_.count(id)) return;
            MapItemInfo info;
            for (int t = 0; t < 5; ++t) {
                if (tname[t].empty()) continue;                  // strlen(name) == 0
                // the rate x (1 + Rate_Add): Type 0 for SpecialT, 1 for EquipT, 2 for Normal1T..3T,
                // truncated to the i16 the loader keeps it in
                const double r = static_cast<double>(trate[t]);
                const int16_t scaled = static_cast<int16_t>(static_cast<int>(
                    static_cast<double>(rate_add_[t < 2 ? t : 2]) * r + r));
                for (const TableRow& row : tables) {             // sub_640E70: every row of that name
                    if (row.id != tname[t]) continue;
                    for (const MapItemEntry& e : row.items) {
                        info.items[t].push_back(e);              // sub_6D8E30
                        info.item_sum[t] += static_cast<int16_t>(e.rate);   // sub_6D8DF0 (i16 added)
                    }
                }
                info.total += scaled;                            // sub_6D8D80
                info.type_rate[t] = scaled;                      // sub_6D8DA0
            }
            for (int i = 0; i < 30; ++i) {                       // sub_640F40: the map's own items
                const uint16_t r16 = static_cast<uint16_t>(rate[i]);
                if (!item[i] || !r16) continue;
                info.items[5].push_back({ item[i], r16 });
                info.type_rate[5] += static_cast<int16_t>(r16);
                info.item_sum[5]  += static_cast<int16_t>(r16);
                info.total        += static_cast<int16_t>(r16);
            }
            maps_.emplace(id, std::move(info));
        });
        LOG_INFO("MapItemDb: dig tables of %zu maps (%zu named tables)", maps_.size(), tables.size());
        return !maps_.empty();
    }

    // GameInfoMgr::LoadMapItemInfo: the map's info, nullptr when MapItem_All has no row for it
    // ("Can't Found MapItemInfo" — PopItemByRandom then finds "Invalid MapItem Ini Info").
    const MapItemInfo* get(int map_id) const
    {
        auto it = maps_.find(map_id);
        return it == maps_.end() ? nullptr : &it->second;
    }

    // sub_648800(type): MapItem_Event_Add Rate_Add (0..3).
    float rate_add(int type) const { return (type >= 0 && type < 4) ? rate_add_[type] : 0.0f; }

private:
    std::unordered_map<int, MapItemInfo> maps_;
    float rate_add_[4]{};

    // "Item%d" / "Rate%d" (0..29)
    static void pair_field(const std::string& k, const std::string& v, uint32_t item[30], uint32_t rate[30])
    {
        const bool is_item = k.compare(0, 4, "Item") == 0;
        const bool is_rate = k.compare(0, 4, "Rate") == 0;
        if (!is_item && !is_rate) return;
        const std::string num = k.substr(4);
        if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) return;
        const int i = std::atoi(num.c_str());
        if (i < 0 || i >= 30) return;
        const uint32_t x = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        if (is_item) item[i] = x; else rate[i] = x;
    }
};
