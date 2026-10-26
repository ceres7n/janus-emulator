#pragma once
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <fstream>
#include "logger.hpp"
#include "yaml_scalar.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  The data of the NPC script functions (original server data, kto_libconfig.xml):
//    TeleportDb       db/teleport_db.yml          TeleportInfo   (GameInfoMgr sub_628590)
//    NpcMoveDb        db/npc_move_db.yml          NpcMoveInfo    (sub_6288A0)
//    NpcCheckLvMsgDb  db/npc_check_lv_msg_db.yml  NpcCheckLvMsg  (sub_632E80)
// ─────────────────────────────────────────────────────────────────────────────

namespace npcdb_detail {
// One flat `- Key: value` record file: calls `row` with each record's (key, value) pairs.
inline size_t read_records(const std::string& path, const char* who,
                           const std::function<void(const std::vector<std::pair<std::string, std::string>>&)>& row)
{
    std::ifstream f(path);
    if (!f) { LOG_ERROR("%s: cannot open %s", who, path.c_str()); return 0; }
    std::string line;
    std::vector<std::pair<std::string, std::string>> cur;
    bool in_record = false;
    size_t n = 0;
    auto kv = [&](const std::string& s) {
        size_t c = s.find(':');
        if (c != std::string::npos) cur.emplace_back(s.substr(0, c), yaml_scalar(s.substr(c + 1)));
    };
    auto commit = [&]() { if (in_record && !cur.empty()) { row(cur); ++n; } cur.clear(); };
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t ns = line.find_first_not_of(' ');
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
    return n;
}
inline int ival(const std::vector<std::pair<std::string, std::string>>& r, const std::string& k, int def = 0)
{
    for (const auto& p : r) if (p.first == k) return std::atoi(p.second.c_str());
    return def;
}
inline std::string sval(const std::vector<std::pair<std::string, std::string>>& r, const std::string& k)
{
    for (const auto& p : r) if (p.first == k) return p.second;
    return {};
}
}  // namespace npcdb_detail

// TeleportInfo, as GameInfoMgr sub_628590 keeps it: Id, MinLevel (+4) and the slots Zone0..Zone6
// whose Zone is not 0, in order — each {Zone (+8), Cost (+12), LimitCnt (+16), LoadImg (+20)} at
// +8 + 44*i, their count at +316. Zone7..Zone10 of the table are not read.
struct TeleportSlot { int zone = 0, cost = 0, limit_cnt = 0; std::string load_img; };
struct TeleportInfo {
    int id = 0, min_level = 0;
    std::vector<TeleportSlot> slots;
};

class TeleportDb
{
public:
    bool load(const std::string& path)
    {
        using namespace npcdb_detail;
        const size_t n = read_records(path, "TeleportDb", [&](const auto& r) {
            TeleportInfo t;
            t.id        = ival(r, "Id");
            t.min_level = ival(r, "MinLevel");
            for (int i = 0; i < 7; ++i) {
                const std::string s = std::to_string(i);
                const int zone = ival(r, "Zone" + s);
                if (!zone) continue;
                t.slots.push_back({ zone, ival(r, "Cost" + s), ival(r, "LimitCnt" + s), sval(r, "LoadImg" + s) });
            }
            map_[t.id] = t;
        });
        LOG_INFO("TeleportDb: loaded %zu teleport tables from %s", n, path.c_str());
        return n > 0;
    }
    // sub_628840: by the NPC's TeleportId (the u16 record field, passed on as a byte — the key
    // compare in the retail map is on the int value).
    const TeleportInfo* get(int id) const { auto it = map_.find(id); return it == map_.end() ? nullptr : &it->second; }

private:
    std::unordered_map<int, TeleportInfo> map_;
};

// NpcMoveInfo (sub_6288A0): rows with Zone in 1..999 only; MinLevel / MaxLevel clamped (negative
// -> 0, Min > Max -> both 0); up to 4 source items (SrcItem%d != 0, in order).
struct NpcMoveInfo {
    int id = 0, zone = 0, dest_id = 0;
    struct Src { int item = 0, cnt = 0; };
    std::vector<Src> items;
    int min_level = 0, max_level = 0, ftype = 0, chr_type = 0, take_item = 0;
};

class NpcMoveDb
{
public:
    bool load(const std::string& path)
    {
        using namespace npcdb_detail;
        const size_t n = read_records(path, "NpcMoveDb", [&](const auto& r) {
            NpcMoveInfo m;
            m.id      = ival(r, "Id");
            m.zone    = ival(r, "Zone");
            m.dest_id = ival(r, "DestId");
            if (m.zone < 1 || m.zone >= 1000) return;
            for (int i = 0; i < 4; ++i) {
                const std::string s = std::to_string(i);
                const int item = ival(r, "SrcItem" + s);
                if (item) m.items.push_back({ item, ival(r, "Cnt" + s) });
            }
            m.min_level = ival(r, "MinLevel");
            m.max_level = ival(r, "MaxLevel");
            if (m.min_level < 0) m.min_level = 0;
            if (m.max_level < 0) m.max_level = 0;
            if (m.min_level > m.max_level) { m.min_level = 0; m.max_level = 0; }
            m.ftype     = ival(r, "FType");
            m.chr_type  = ival(r, "ChrType");
            m.take_item = ival(r, "TakeItem");
            map_[m.id] = m;
        });
        LOG_INFO("NpcMoveDb: loaded %zu moves from %s", n, path.c_str());
        return n > 0;
    }
    const NpcMoveInfo* get(int id) const { auto it = map_.find(id); return it == map_.end() ? nullptr : &it->second; }

private:
    std::unordered_map<int, NpcMoveInfo> map_;
};

// NpcCheckLvMsg (sub_632E80): ID, RejectMsg, six level ranges {Min%d, Max%d, Msg%d}.
struct NpcCheckLvMsg {
    int id = 0, reject_msg = 0;
    int min[6]{}, max[6]{}, msg[6]{};
};

class NpcCheckLvMsgDb
{
public:
    bool load(const std::string& path)
    {
        using namespace npcdb_detail;
        const size_t n = read_records(path, "NpcCheckLvMsgDb", [&](const auto& r) {
            NpcCheckLvMsg c;
            c.id         = ival(r, "ID");
            c.reject_msg = ival(r, "RejectMsg");
            for (int i = 0; i < 6; ++i) {
                const std::string s = std::to_string(i);
                c.min[i] = ival(r, "Min" + s);
                c.max[i] = ival(r, "Max" + s);
                c.msg[i] = ival(r, "Msg" + s);
            }
            map_[c.id] = c;
        });
        LOG_INFO("NpcCheckLvMsgDb: loaded %zu tables from %s", n, path.c_str());
        return n > 0;
    }
    const NpcCheckLvMsg* get(int id) const { auto it = map_.find(id); return it == map_.end() ? nullptr : &it->second; }

private:
    std::unordered_map<int, NpcCheckLvMsg> map_;
};
