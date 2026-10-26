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
//  PetSpeechDb — db/pet_param_db.yml (PetParam, by the pet's CharacterInfo id) + db/pet_speech_db.yml (the
//  PetSpeech / PetSpeech_* tables it names): a pet's lines per Type (the situation - 1, sub_7390B0), its Voice flag
//  and SpeechColor. Retail loader sub_620C50: per row of the table (keyed by Type; a repeated Type is left out by the
//  generator — the lookup finds the first row), up to three slots, stopping at the first empty Speech; each slot's
//  bound = the previous one + (int)(Ratio * 1000.0) (x87: the float Ratio times the double 1000.0, so 0.7 -> 699).
//  The pick sub_60CDB0: no row / no line -> none; one line -> always it; more -> rand() % 1000 against the bounds
//  (none passed -> none).
// ─────────────────────────────────────────────────────────────────────────────
class PetSpeechDb
{
public:
    struct Line  { std::string text; std::string voice; int hi{ 0 }; };
    struct Slot  { std::vector<Line> lines; };
    struct Table { std::unordered_map<int, Slot> types; };
    struct Param { const Table* table{ nullptr }; bool voice{ false }; uint32_t color{ 0 }; };

    bool load(const std::string& param_path, const std::string& speech_path)
    {
        if (!load_lines(speech_path)) return false;
        return load_params(param_path);
    }

    // PetParam of a species (sub_622750 — Pet::Init's +1660), or null.
    const Param* find(int species) const
    {
        auto it = params_.find(species);
        return it == params_.end() ? nullptr : &it->second;
    }

    // sub_60CDB0: the line of a Type, or null.
    static const Line* pick(const Param& p, int type)
    {
        if (!p.table) return nullptr;
        auto it = p.table->types.find(type);
        if (it == p.table->types.end()) return nullptr;
        const std::vector<Line>& ls = it->second.lines;
        if (ls.empty()) return nullptr;
        if (ls.size() == 1) return &ls[0];
        const int r = std::rand() % 1000;                      // sub_41ED70(1000)
        for (const Line& l : ls) if (r < l.hi) return &l;
        return nullptr;
    }

private:
    // Each entry starts with "  - Key: value" at column 2; its other keys are indented 4.
    template <class Fn>
    static bool for_each_entry(const std::string& path, const char* what, Fn&& fn)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("PetSpeechDb: cannot open %s (%s)", path.c_str(), what); return false; }
        std::unordered_map<std::string, std::string> kv;
        bool in = false;
        auto commit = [&]() { if (in) fn(kv); kv.clear(); in = false; };
        std::string line;
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
            kv[body.substr(0, c)] = yaml_scalar(body.substr(c + 1));
        }
        commit();
        return true;
    }

    bool load_lines(const std::string& path)
    {
        size_t n = 0;
        const bool ok = for_each_entry(path, "lines", [&](std::unordered_map<std::string, std::string>& kv) {
            const std::string& name = kv["Table"];
            if (name.empty() || kv["Type"].empty()) return;
            Slot& s = tables_[name].types[std::atoi(kv["Type"].c_str())];
            if (s.lines.size() >= 3) return;
            const float ratio = static_cast<float>(std::atof(kv["Ratio"].c_str()));
            const int lo = s.lines.empty() ? 0 : s.lines.back().hi;
            s.lines.push_back(Line{ kv["Speech"], kv["Voice"],
                                    lo + static_cast<int>(static_cast<double>(ratio) * 1000.0) });
            ++n;
        });
        if (ok) LOG_INFO("PetSpeechDb: loaded %zu lines of %zu tables from %s", n, tables_.size(), path.c_str());
        return ok && !tables_.empty();
    }

    bool load_params(const std::string& path)
    {
        size_t missing = 0;
        const bool ok = for_each_entry(path, "params", [&](std::unordered_map<std::string, std::string>& kv) {
            const int id = std::atoi(kv["ID"].c_str());
            if (id <= 0 || params_.count(id)) return;
            Param p;
            const std::string& name = kv["Speech"];
            auto t = tables_.find(name);
            if (t != tables_.end()) p.table = &t->second;
            else if (!name.empty()) ++missing;                 // (PetSpeech_FairyFreesia: no such table — silent)
            p.voice = (kv["Voice"] == "true");
            p.color = static_cast<uint32_t>(std::strtoul(kv["SpeechColor"].c_str(), nullptr, 10));
            params_.emplace(id, p);
        });
        if (ok) LOG_INFO("PetSpeechDb: loaded %zu pets from %s (%zu name a missing table)", params_.size(),
                         path.c_str(), missing);
        return ok && !params_.empty();
    }

    std::unordered_map<std::string, Table> tables_;
    std::unordered_map<int, Param> params_;
};
