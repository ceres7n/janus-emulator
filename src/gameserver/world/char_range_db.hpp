#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <fstream>
#include "logger.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  CharRangeDb — per character/monster type attack ranges, from db/char_range_db.yml
//  (generated from the original server data kto_libconfig.xml, table CharacterInfo).
//  Keyed by the CharacterInfo ID: the player's character type (1..9) or the monster
//  species.
//
//  The retail defaults are applied exactly as GameInfoMgr::InitializeChrInfo does
//  (server decomp line 268748): Max < 16 -> 86; Min <= 0 or Min >= Max -> Max - 15;
//  Chase < 16 -> 200. An unknown ID gets the same defaults (as an all-zero row would).
//  The attack motion times come from the char's .nri (sub_623200, default 1000 ms).
// ─────────────────────────────────────────────────────────────────────────────

struct CharRange {
    int min_atk = 71;     // MinAtkRange
    int max_atk = 86;     // MaxAtkRange
    int chase   = 200;    // ChaseRange
    // Summed frame durations of the .nri motions '공격' / '공격2' / '공격준비' (ChrInfo +280/+284/+288,
    // sub_623200; 1000 when the motion is missing) — the attack cooldown base.
    int attack_ms       = 1000;
    int attack2_ms      = 1000;
    int attack_ready_ms = 1000;
    int approach_atk    = -1;   // set apart for an equipped gun: 3*(Min+Max)>>2 (sub_717E70)
    // actor +1044 (Character init, decomp 416885: (Min+Max)/2)
    int approach() const { return approach_atk >= 0 ? approach_atk : (min_atk + max_atk) / 2; }
};

class CharRangeDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("CharRangeDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        Raw cur;
        bool in_record = false;
        auto commit = [&]() { if (in_record && cur.id > 0) rows_[cur.id] = make(cur); };
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
        LOG_INFO("CharRangeDb: loaded %zu rows from %s", rows_.size(), path.c_str());
        return !rows_.empty();
    }

    CharRange get(int id) const
    {
        auto it = rows_.find(id);
        return it == rows_.end() ? make(Raw()) : it->second;
    }

private:
    struct Raw { int id = 0, mn = 0, mx = 0, ch = 0, a = 1000, a2 = 1000, ar = 1000; };
    std::unordered_map<int, CharRange> rows_;

    static CharRange make(const Raw& w)
    {
        CharRange r;
        r.max_atk = (w.mx < 16) ? 86 : w.mx;
        r.min_atk = (w.mn <= 0 || w.mn >= r.max_atk) ? r.max_atk - 15 : w.mn;
        r.chase   = (w.ch < 16) ? 200 : w.ch;
        r.attack_ms       = w.a;
        r.attack2_ms      = w.a2;
        r.attack_ready_ms = w.ar;
        return r;
    }

    static void kv(const std::string& s, Raw& w)
    {
        size_t c = s.find(':');
        if (c == std::string::npos) return;
        std::string k = s.substr(0, c);
        int v = 0;
        try { v = std::stoi(s.substr(c + 1)); } catch (...) { return; }
        if      (k == "Id")            w.id = v;
        else if (k == "MinAtkRange")   w.mn = v;
        else if (k == "MaxAtkRange")   w.mx = v;
        else if (k == "ChaseRange")    w.ch = v;
        else if (k == "AttackMs")      w.a  = v;
        else if (k == "Attack2Ms")     w.a2 = v;
        else if (k == "AttackReadyMs") w.ar = v;
    }
};
