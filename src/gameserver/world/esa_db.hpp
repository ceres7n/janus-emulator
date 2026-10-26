#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <fstream>
#include "logger.hpp"
#include "yaml_scalar.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  EsaDb — the skill effect tables (ESAII_* / ESAUI_* / ESAction_*) of the original server data,
//  loaded from db/esa_db.yml: one record per (Table, Level) row, every column kept in the table's
//  column order (the retail loader turns each ESAII column into an invocation condition in that
//  order — GameInfoMgr sub_524160 maps the column name to the condition type).
// ─────────────────────────────────────────────────────────────────────────────

struct EsaRow {
    std::vector<std::pair<std::string, double>> fields;   // column order, Level excluded (a text one counts 0)
    std::vector<std::pair<std::string, std::string>> texts;   // the String columns (BeginEffect, ...)

    const double* find(const char* key) const
    {
        for (const auto& f : fields) if (f.first == key) return &f.second;
        return nullptr;
    }
    double get(const char* key, double def = 0.0) const
    {
        const double* v = find(key);
        return v ? *v : def;
    }
    // A String column's text ("" when the row has none — the generator leaves empty ones out).
    const std::string& text(const char* key) const
    {
        static const std::string kNone;
        for (const auto& t : texts) if (t.first == key) return t.second;
        return kNone;
    }
};

class EsaDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("EsaDb: cannot open %s", path.c_str()); return false; }
        std::string line, table;
        int level = 0;
        EsaRow cur;
        bool in_record = false;
        size_t rows = 0;
        auto commit = [&]() {
            if (in_record && !table.empty()) { tables_[table][level] = cur; ++rows; }
        };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                cur = EsaRow();
                table.clear();
                level = 0;
                in_record = true;
                kv(line.substr(ns + 2), table, level, cur);
            } else if (in_record && ns >= 4) {
                kv(line.substr(ns), table, level, cur);
            } else if (line.compare(ns, 7, "Footer:") == 0) {
                commit();
                in_record = false;
            }
        }
        commit();
        LOG_INFO("EsaDb: loaded %zu tables (%zu rows) from %s", tables_.size(), rows, path.c_str());
        return !tables_.empty();
    }

    // The row of `table` for skill level `level`, or nullptr.
    const EsaRow* row(const std::string& table, int level) const
    {
        auto t = tables_.find(table);
        if (t == tables_.end()) return nullptr;
        auto r = t->second.find(level);
        return r == t->second.end() ? nullptr : &r->second;
    }

private:
    std::unordered_map<std::string, std::unordered_map<int, EsaRow>> tables_;

    static void kv(const std::string& s, std::string& table, int& level, EsaRow& r)
    {
        size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const std::string v = s.substr(c + 1);
        if (k == "Table")       table = yaml_scalar(v);
        else if (k == "Level")  level = std::atoi(v.c_str());
        else {
            const std::string sv = yaml_scalar(v);
            if (sv == "true")       r.fields.emplace_back(k, 1.0);
            else if (sv == "false") r.fields.emplace_back(k, 0.0);
            else {
                char* end = nullptr;
                const double d = std::strtod(sv.c_str(), &end);
                r.fields.emplace_back(k, d);
                if (end == sv.c_str() || *end != '\0') r.texts.emplace_back(k, sv);
            }
        }
    }
};
