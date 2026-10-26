#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <fstream>
#include "logger.hpp"
#include "yaml_scalar.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  MobDb — monster stat table, loaded from db/mob_db.yml (generated from the
//  original server data kto_libconfig.xml, table MonsterParamEx2). The *Lv columns
//  are the monster's ACTUAL stats at its own Level — the client stores MaxHpLv
//  directly (sub_614A50 @0x614A50, no level-curve), so our MaxHp == the client's
//  HP-bar maximum and the two track 1:1.
//
//  Speeds are units/s (retail actor +156 walk / +160 run). ChaseTime follows
//  GameInfoMgr::InitializeMonsterParam (server decomp line 268302): param+28 =
//  (int)(ChaseTime * 1000) ms, and <= 0 becomes 3000.
//
//  The file is the project's simple flat YAML (Header/Body list of `- Id:` records
//  with `    Key: Value` fields), so a tiny hand-rolled parser suffices — no YAML
//  dependency. Unknown keys are ignored; Name is the only string value (yaml_scalar).
// ─────────────────────────────────────────────────────────────────────────────

struct MobDef {
    int id = 0, level = 1, max_hp = 1, max_mp = 0;
    int type = 0;                    // MonsterParamEx2 Type (+4; sub_6223A0): its class; 3 is never regenerated
    int ap = 0, ac = 0, dx = 0, dp = 0, hv = 0, lk = 0, ma = 0, md = 0;
    int da = 0;                      // DaLv (stat 7)
    int debuff_set = 0;              // DebuffSet: the MonsterDebuffResist row of a boss (+2462)
    int acc_formula = 0;             // AccFormula (+2460): 1 = its melee hit is sub_5E5680, unblockable
    int exp = 0, tm = 0;
    float walk = 0.0f, run = 0.0f;   // WalkSpeed / RunSpeed (units/s)
    int chase_time_ms = 3000;        // ChaseTime -> ms (retail rule, see above)
    // Resistances in %, MonsterParamEx2 column order: FireR WaterR WindR EarthR ElecR LightR DarkR
    // NoPropR PhysicalR GunR ShadowR (param +2296..+2316). See MonsterEntity::resist_slot.
    int16_t resist[11] = {};
    // Drops (CommonMonster die handler sub_7296E0): ItemDropIdx = the MonItemDropInfo list (param
    // +32), ItemDropCnt = item rolls (+40), GelderRate = gelder chance (+36, float), GMin/GMax =
    // gelder amount GMin + rand() % (GMax - GMin) (+2288/+2292).
    int item_drop_idx = 0, item_drop_cnt = 0;
    float gelder_rate = 0.0f;
    int gmin = 0, gmax = 0;
    std::string name;
};

class MobDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("MobDb: cannot open %s", path.c_str()); return false; }

        std::string line;
        MobDef cur;
        bool in_record = false;
        auto commit = [&]() { if (in_record && cur.id > 0) mobs_[cur.id] = cur; };

        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos) continue;        // blank
            if (line[ns] == '#') continue;                 // comment

            if (line.compare(ns, 2, "- ") == 0) {          // new record: "- Id: N"
                commit();
                cur = MobDef();
                in_record = true;
                parse_kv(line.substr(ns + 2), cur);
            } else if (in_record && ns >= 4) {             // a field of the current record
                parse_kv(line.substr(ns), cur);
            } else if (line.compare(ns, 7, "Footer:") == 0) {
                commit();
                in_record = false;
            }
            // (Header:/Body:/Type:/Version: lines before any record are ignored.)
        }
        commit();
        LOG_INFO("MobDb: loaded %zu monsters from %s", mobs_.size(), path.c_str());
        return !mobs_.empty();
    }

    const MobDef* get(int id) const
    {
        auto it = mobs_.find(id);
        return it == mobs_.end() ? nullptr : &it->second;
    }
    size_t size() const { return mobs_.size(); }

private:
    std::unordered_map<int, MobDef> mobs_;

    static void parse_kv(const std::string& s, MobDef& m)
    {
        size_t c = s.find(':');
        if (c == std::string::npos) return;
        std::string k = s.substr(0, c);
        std::string v = s.substr(c + 1);
        size_t a = v.find_first_not_of(' ');
        if (a == std::string::npos) return;
        v = v.substr(a);
        auto num = [&]() -> int { try { return std::stoi(v); } catch (...) { return 0; } };
        auto real = [&]() -> float { try { return std::stof(v); } catch (...) { return 0.0f; } };

        if      (k == "Id")        m.id     = num();
        else if (k == "Name")      m.name   = yaml_scalar(v);
        else if (k == "Level")     m.level  = num();
        else if (k == "MaxHp")     m.max_hp = num();
        else if (k == "MaxMp")     m.max_mp = num();
        else if (k == "Ap")        m.ap     = num();
        else if (k == "Ac")        m.ac     = num();
        else if (k == "Dx")        m.dx     = num();
        else if (k == "Dp")        m.dp     = num();
        else if (k == "Hv")        m.hv     = num();
        else if (k == "Lk")        m.lk     = num();
        else if (k == "Da")        m.da     = num();
        else if (k == "Type")      m.type   = num();
        else if (k == "DebuffSet") m.debuff_set = num();
        else if (k == "AccFormula") m.acc_formula = num();
        else if (k == "Ma")        m.ma     = num();
        else if (k == "Md")        m.md     = num();
        else if (k == "Exp")       m.exp    = num();
        else if (k == "Tm")        m.tm     = num();
        else if (k == "WalkSpeed") m.walk   = real();
        else if (k == "RunSpeed")  m.run    = real();
        else if (k == "ChaseTime") { const int ms = static_cast<int>(real() * 1000.0);
                                     m.chase_time_ms = (ms <= 0) ? 3000 : ms; }
        else if (k == "ItemDropIdx") m.item_drop_idx = num();
        else if (k == "ItemDropCnt") m.item_drop_cnt = num();
        else if (k == "GelderRate")  m.gelder_rate   = static_cast<float>(std::strtod(v.c_str(), nullptr));
        else if (k == "GMin")        m.gmin          = num();
        else if (k == "GMax")        m.gmax          = num();
        else {
            static const char* const kResist[11] = { "FireR", "WaterR", "WindR", "EarthR", "ElecR", "LightR",
                                                     "DarkR", "NoPropR", "PhysicalR", "GunR", "ShadowR" };
            for (int i = 0; i < 11; ++i)
                if (k == kResist[i]) { m.resist[i] = static_cast<int16_t>(num()); break; }
        }
    }
};
