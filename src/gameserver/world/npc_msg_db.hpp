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
//  NpcMsgDb — the NpcMsg_* tables of the original server data (kto_libconfig.xml), loaded from
//  db/npc_msg_db.yml: an NPC's MsgTable (GameInfoMgr sub_634560 -> its message list, keyed by No:
//  {No, Message, NpcSType}). The message text is UTF-8 here (sent in the client's codepage).
// ─────────────────────────────────────────────────────────────────────────────

struct NpcMsg {
    int         no = 0;         // No
    int         stype = 0;      // NpcSType
    std::string text;           // Message
};

struct NpcMsgTable {
    std::vector<NpcMsg> rows;   // in table order (the list size is the message count)

    // The message list lookup by No (sub_60DBC0 / sub_60C880); the first row with that No.
    const NpcMsg* find(int no) const
    {
        for (const NpcMsg& m : rows) if (m.no == no) return &m;
        return nullptr;
    }
};

class NpcMsgDb
{
public:
    bool load(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { LOG_ERROR("NpcMsgDb: cannot open %s", path.c_str()); return false; }
        std::string line, table;
        NpcMsg cur;
        bool in_record = false;
        size_t rows = 0;
        auto commit = [&]() {
            if (in_record && !table.empty()) { tables_[table].rows.push_back(cur); ++rows; }
        };
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t ns = line.find_first_not_of(' ');
            if (ns == std::string::npos || line[ns] == '#') continue;
            if (line.compare(ns, 2, "- ") == 0) {
                commit();
                cur = NpcMsg();
                table.clear();
                in_record = true;
                kv(line.substr(ns + 2), table, cur);
            } else if (in_record && ns >= 4) {
                kv(line.substr(ns), table, cur);
            } else if (line.compare(ns, 7, "Footer:") == 0) {
                commit();
                in_record = false;
            }
        }
        commit();
        LOG_INFO("NpcMsgDb: loaded %zu tables (%zu messages) from %s", tables_.size(), rows, path.c_str());
        return !tables_.empty();
    }

    const NpcMsgTable* table(const std::string& name) const
    {
        auto it = tables_.find(name);
        return it == tables_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<std::string, NpcMsgTable> tables_;

    static void kv(const std::string& s, std::string& table, NpcMsg& m)
    {
        size_t c = s.find(':');
        if (c == std::string::npos) return;
        const std::string k = s.substr(0, c);
        const std::string v = s.substr(c + 1);
        if      (k == "Table")    table   = yaml_scalar(v);
        else if (k == "No")       m.no    = std::atoi(v.c_str());
        else if (k == "NpcSType") m.stype = std::atoi(v.c_str());
        else if (k == "Message")  m.text  = yaml_scalar(v);
    }
};
