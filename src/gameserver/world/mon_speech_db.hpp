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
//  MonSpeechDb — db/mon_speech_db.yml (MonsterParamEx2.Speech + its MonSpeech_* table): a monster's lines per
//  situation (the Type: 0 hit by a crit / strong crit, 1 its battle state begins, 3 its tactics state begins; 2 is
//  loaded but never said). Retail loader sub_621130: each line's Ratio * 1000 stacked into a cumulative bound;
//  the pick sub_60CF40: no line -> none; one -> always it; more -> rand() % 1000 against the bounds (none -> none).
// ─────────────────────────────────────────────────────────────────────────────
class MonSpeechDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("MonSpeechDb: cannot open %s", path.c_str()); return false; }
        std::string line;
        int mob = 0, type = -1;
        float ratio = 0.0f;
        std::string text;
        bool in = false;
        size_t n = 0;
        auto commit = [&]() {
            if (in && mob > 0 && type >= 0 && type < kTypes) {
                Slot& s = mobs_[mob].slot[type];
                if (s.lines.size() < 3) {
                    const int lo = s.lines.empty() ? 0 : s.lines.back().hi;
                    s.lines.push_back(Line{ text, lo + static_cast<int>(ratio * 1000.0f) });
                    ++n;
                }
            }
            mob = 0; type = -1; ratio = 0.0f; text.clear(); in = false;
        };
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
            const std::string v = yaml_scalar(body.substr(c + 1));
            if (k == "MonsterId") mob = std::atoi(v.c_str());
            else if (k == "Type") type = std::atoi(v.c_str());
            else if (k == "Ratio") ratio = static_cast<float>(std::atof(v.c_str()));
            else if (k == "Text") text = v;
        }
        commit();
        LOG_INFO("MonSpeechDb: loaded %zu lines of %zu monsters from %s", n, mobs_.size(), path.c_str());
        return !mobs_.empty();
    }

    // sub_60CF40: the line a monster says in a situation, or null.
    const std::string* pick(int mob, int situation) const
    {
        auto it = mobs_.find(mob);
        if (it == mobs_.end() || situation < 0 || situation >= kTypes) return nullptr;
        const Slot& s = it->second.slot[situation];
        if (s.lines.empty()) return nullptr;
        if (s.lines.size() == 1) return &s.lines[0].text;
        const int r = std::rand() % 1000;                      // sub_41ED70(1000)
        for (const Line& l : s.lines) if (r < l.hi) return &l.text;
        return nullptr;
    }

private:
    static constexpr int kTypes = 4;
    struct Line { std::string text; int hi{ 0 }; };
    struct Slot { std::vector<Line> lines; };
    struct Mob  { Slot slot[kTypes]; };
    std::unordered_map<int, Mob> mobs_;
};
