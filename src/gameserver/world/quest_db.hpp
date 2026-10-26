#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "logger.hpp"
#include "yaml_scalar.hpp"
#include "drill_db.hpp"
#include "item_db.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  The normal quests (original server data, kto_libconfig.xml):
//    db/quest_db.yml               QuestInfo             (GameInfoMgr sub_625D00, sub_626830 by Id)
//    db/quest_result_db.yml        QuestResult_*         (sub_626190: a quest's results)
//    db/random_result_item_db.yml  RandomResultItemInfo  (sub_639B50, sub_639E40 by ID)
//    db/scenario_quest_db.yml      ScenarioQuestInfo     (sub_626D20 by Id; QuestResult tables too)
//    db/monster_quest_db.yml       MonsterQuestInfo      (GameInfoMgr sub_629D10 area, sub_6284C0 by Id)
//    db/monster_quest_result_db.yml MonsterQuestResult_* (sub_627F60: a monster quest's results by Grade)
//  (QuestItemInfo — one row: quest 142 asks for item 500000 refined to level 3, an item ItemParam
//  does not have — is not loaded: sub_7E0870 only fixes the uid of such an item.)
// ─────────────────────────────────────────────────────────────────────────────

// A quest item (the 16-byte record sub_647220: {u64 uid, u32 item, u32 count}); the count is 1 for an
// item whose ItemParam Options has bit 1 (sub_41C8F0(param + 56, 1)).
struct QuestItem {
    uint32_t item = 0;
    int32_t  cnt  = 0;
    uint64_t uid  = 0;                       // a given item (an exchange); 0 = the first item of the id
};

// A QuestResult row (the 120-byte record of sub_626190).
struct QuestResult {
    int32_t count = 0;                       // [0]  Count
    int32_t exp = 0;                         // [1]  Exp
    int32_t tmxp = 0;                        // [2]  Tmxp
    int32_t type = 0;                        // [3]  Type: 1 = one item (and one random table) at random
    std::vector<QuestItem> items;            // [4..19] Item0..3 that are set, [20] their number
    int32_t skill = 0;                       // [21] Skill
    int32_t skill_lv = 0;                    // [22] SkillLv
    std::vector<std::string> msgs;           // [23] / [24]: Msg0 .. the last non-empty one
    int32_t gelder = 0;                      // [25] Gelder
    std::vector<int32_t> random_tables;      // [26] / [27..29]: RandomTableID0..2 that are set
};

// A QuestInfo row (sub_625D00).
struct QuestDef {
    int32_t     id = 0;                      // +0   Id
    std::string name;                        // +4   Name
    std::string client;                      // +36  Client
    std::string pos;                         // +68  Pos
    std::string comment;                     // +132 Comment
    int16_t     ref_id = 0;                  // +388 RefId
    std::vector<QuestItem> src;              // +392 SrcItem0..5 that are set (packed), +488 their number
    int32_t     src_skill = 0;               // +492 SrcSkill0
    int32_t     src_skill_lv = 0;            // +496 SrcSkillLv0
    int32_t     options = 0;                 // +500 Options (2 = the reward tier follows SrcItem0's count)
    int16_t     min_level = 0;               // +504 MinLevel (< 0 -> 0)
    int16_t     max_level = 0;               // +506 MaxLevel (< MinLevel -> MinLevel)
    int32_t     exp = 0;                     // +508 Exp (< 0 -> 0)
    int32_t     tmxp = 0;                    // +512 Tmxp
    uint8_t     max_count = 25;              // +516 MaxCount (<= 0 -> 25), a byte
    std::vector<QuestResult> results;        // +520 / +524
    int32_t     src_gelder = 0;              // +528 SrcGelder

    // sub_6D8680: the result of a run — the row with the largest Count up to `count` (Counts above 0),
    // else the first row. nullptr when the quest has no result table (6 rows name a blank one).
    const QuestResult* result_for(int count) const
    {
        if (results.empty()) return nullptr;
        int best = 0;
        size_t at = 0;
        for (size_t i = 0; i < results.size(); ++i)
            if (results[i].count <= count && results[i].count > best) { best = results[i].count; at = i; }
        return &results[at];
    }

    // sub_6D87B0: a user-choice quest (Options 2) takes the row whose Count is its tier exactly
    // ("ERROR: QuestInfo::GetQuestResultByGrade() - UserChoiceQuest has no matched QuestResult." and
    // the first row when none); any other quest result_for().
    const QuestResult* result_by(int count, bool user_choice) const
    {
        if (!user_choice) return result_for(count);
        if (results.empty()) return nullptr;
        for (const QuestResult& r : results) if (r.count == count) return &r;
        LOG_ERROR("ERROR: QuestInfo::GetQuestResultByGrade() - UserChoiceQuest has no matched QuestResult.");
        return &results[0];
    }

    // sub_6D8710: the tier of a user-choice quest — the source item index whose Cnt is the largest up
    // to `have` (the items while their Cnt is set), -1 when none.
    int user_choice_tier(int have) const
    {
        int best = 0, tier = -1;
        for (size_t i = 0; i < src.size() && i < 6 && src[i].cnt; ++i)
            if (have >= src[i].cnt && src[i].cnt > best) { tier = static_cast<int>(i); best = src[i].cnt; }
        return best ? tier : -1;
    }
};

// A RandomResultItemInfo row (sub_639B50): the entries by their column (0..9), each taking
// (int)(Rate * 10000) of the range after the previous ones; `n` = the columns that have an item.
struct RandomResultItems {
    struct Entry { uint32_t item = 0; int32_t cnt = 0; int32_t lo = 0, hi = 0; };
    int32_t n = 0;                           // [0]
    int32_t total = 0;                       // [1]
    Entry   e[10];                           // [2 + 4i] item, [3 + 4i] count (0 -> 1), [4 + 4i] lo, [5 + 4i] hi

    // sub_7EB220: rand() % total, the first of the first `n` columns whose [lo, hi] holds it; nullptr
    // when none (a total of 0 would divide by zero in the retail — no table has one).
    const Entry* pick() const
    {
        if (total <= 0) return nullptr;
        const int v = std::rand() % total;                  // sub_420AB0(0, total)
        for (int i = 0; i < n && i < 10; ++i)
            if (v >= e[i].lo && v <= e[i].hi) return &e[i];
        return nullptr;
    }
};

// A MonsterQuestResult row (the 104-byte record of sub_627F60).
struct MonsterQuestResult {
    int32_t grade = 0;                       // [0]  Grade
    int32_t type = 0;                        // [1]  Type: 1 = one item (and one random table) at random
    std::vector<QuestItem> items;            // [2..17] Item0..3 that are set, [18] their number
    std::vector<std::string> msgs;           // [19] / [20]: Msg0 .. the last non-empty one
    int32_t gelder = 0;                      // [21] Gelder
    std::vector<int32_t> random_tables;      // [22] / [23..25]
};

// A MonsterQuestInfo grade (20 bytes): kill `cnt` of `src_monster` within `time` minutes, for the
// levels min_lv..max_lv.
struct MonsterQuestGrade {
    int32_t src_monster = 0;                 // +0  SrcMonsterN
    int16_t cnt = 0;                         // +4  CntN
    int16_t time = 0;                        // +6  TimeN (minutes)
    int16_t min_lv = 0;                      // +8  MinLvN
    int16_t max_lv = 0;                      // +10 MaxLvN
    int32_t exp = 0;                         // +12 ExpN (< 0 -> 0)
    int32_t tmxp = 0;                        // +16 TmxpN
};

// A MonsterQuestInfo row.
struct MonsterQuestDef {
    int32_t     id = 0;                      // +0  Id
    std::string name;                        // +4  Name
    std::string client;                      // +8  Client
    std::string pos;                         // +12 Pos
    std::string comment;                     // +16 Comment (the client's format)
    std::vector<MonsterQuestGrade> grades;   // +24 / +28: as many as SrcMonster columns are set, read
                                             //   from column 0 on
    int16_t     min_level = 0;               // +32 MinLevel
    int16_t     max_level = 0;               // +34 MaxLevel
    uint8_t     max_count = 25;              // +36 MaxCount (<= 0 -> 25)
    std::vector<MonsterQuestResult> results; // +40: indexed by Grade (max Grade + 1 slots)
    int32_t     result_count = 0;            // +44: the table's row count
    int32_t     return_map = 0;              // +48 ReturnMapID

    // sub_7EB790 / sub_6D8850: the grade whose MinLv..MaxLv holds `level` (-1 none).
    int grade_for(int level) const
    {
        for (size_t i = 0; i < grades.size(); ++i)
            if (grades[i].min_lv <= level && level <= grades[i].max_lv) return static_cast<int>(i);
        return -1;
    }
    // sub_6D8560: the grade of a monster id (-1 none).
    int grade_of_monster(int mob) const
    {
        for (size_t i = 0; i < grades.size(); ++i) if (grades[i].src_monster == mob) return static_cast<int>(i);
        return -1;
    }
    // sub_40FAA0: a grade's SrcMonster (0 for a negative grade).
    int monster_of(int grade) const
    {
        return (grade >= 0 && grade < static_cast<int>(grades.size())) ? grades[static_cast<size_t>(grade)].src_monster : 0;
    }
    // sub_7EB740: MinLevel and MaxLevel both 0 -> any level.
    bool level_ok(int level) const
    {
        if (min_level <= 0 && max_level <= 0) return true;
        return level >= min_level && level <= max_level;
    }
    // sub_60C9D0: the result of a grade (a grade out of the row count reads the last row; nullptr when
    // the quest has no result table).
    const MonsterQuestResult* result_of(int grade) const
    {
        if (result_count <= 0 || results.empty()) return nullptr;
        int i = (grade >= 0 && grade < result_count) ? grade : result_count - 1;
        if (i >= static_cast<int>(results.size())) i = static_cast<int>(results.size()) - 1;
        return &results[static_cast<size_t>(i)];
    }
};

class QuestDb
{
public:
    bool load(const std::string& quest_path, const std::string& result_path, const std::string& random_path,
              const std::string& scenario_path, const ItemDb& items)
    {
        // An item's count as the loaders keep it: 1 for an Options & 1 item.
        auto item_cnt = [&](uint32_t id, int32_t cnt) {
            const ItemDef* d = items.get(id);
            return (d && (d->options & 1)) ? 1 : cnt;
        };
        // QuestResult_* (sub_626190), grouped by table in row order.
        std::unordered_map<std::string, std::vector<QuestResult>> tables;
        {
            std::ifstream f(result_path);
            if (!f) { LOG_ERROR("QuestDb: cannot open %s", result_path.c_str()); return false; }
            DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                std::string table;
                QuestResult r;
                uint32_t it[4]{};
                int32_t  cnt[4]{};
                std::string msg[10];
                bool has_msg[10]{};
                int32_t rnd[3]{};
                for (const auto& [k, v] : kvs) {
                    if      (k == "Table")   table = yaml_scalar(v);
                    else if (k == "Count")   r.count = std::atoi(v.c_str());
                    else if (k == "Type")    r.type = std::atoi(v.c_str());
                    else if (k == "Gelder")  r.gelder = std::atoi(v.c_str());
                    else if (k == "Skill")   r.skill = std::atoi(v.c_str());
                    else if (k == "SkillLv") r.skill_lv = std::atoi(v.c_str());
                    else if (k == "Exp")     r.exp = std::atoi(v.c_str());
                    else if (k == "Tmxp")    r.tmxp = std::atoi(v.c_str());
                    else if (k.size() == 5 && k.compare(0, 4, "Item") == 0 && k[4] >= '0' && k[4] <= '3')
                        it[k[4] - '0'] = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    else if (k.size() == 4 && k.compare(0, 3, "Cnt") == 0 && k[3] >= '0' && k[3] <= '3')
                        cnt[k[3] - '0'] = std::atoi(v.c_str());
                    else if (k.size() == 4 && k.compare(0, 3, "Msg") == 0 && k[3] >= '0' && k[3] <= '9') {
                        msg[k[3] - '0'] = yaml_scalar(v);
                        has_msg[k[3] - '0'] = true;
                    }
                    else if (k.size() == 14 && k.compare(0, 13, "RandomTableID") == 0 && k[13] >= '0' && k[13] <= '2')
                        rnd[k[13] - '0'] = std::atoi(v.c_str());
                }
                if (table.empty()) return;
                for (int i = 0; i < 4; ++i)
                    if (it[i]) r.items.push_back({ it[i], item_cnt(it[i], cnt[i]) });
                // The messages: Msg0 .. the last non-empty one (a lone space counts).
                int last = 0;
                for (int i = 0; i < 10; ++i) if (has_msg[i] && !msg[i].empty()) last = i;
                for (int i = 0; i <= last; ++i) r.msgs.push_back(msg[i]);
                for (int i = 0; i < 3; ++i) if (rnd[i]) r.random_tables.push_back(rnd[i]);
                tables[table].push_back(std::move(r));
            });
        }
        // QuestInfo (sub_625D00) and ScenarioQuestInfo (the same record without the skill, its results
        // QuestResult tables too).
        auto load_info = [&](const std::string& path, std::unordered_map<int, QuestDef>& into) {
            std::ifstream f(path);
            if (!f) { LOG_ERROR("QuestDb: cannot open %s", path.c_str()); return false; }
            DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                QuestDef d;
                uint32_t it[6]{};
                int32_t  cnt[6]{};
                int max_count = 0, min_level = 0, max_level = 0;
                for (const auto& [k, v] : kvs) {
                    if      (k == "Id")          d.id = std::atoi(v.c_str());
                    else if (k == "Name")        d.name = yaml_scalar(v);
                    else if (k == "Client")      d.client = yaml_scalar(v);
                    else if (k == "Pos")         d.pos = yaml_scalar(v);
                    else if (k == "Comment")     d.comment = yaml_scalar(v);
                    else if (k == "MinLevel")    min_level = std::atoi(v.c_str());
                    else if (k == "MaxLevel")    max_level = std::atoi(v.c_str());
                    else if (k == "Exp")         d.exp = std::atoi(v.c_str());
                    else if (k == "Tmxp")        d.tmxp = std::atoi(v.c_str());
                    else if (k == "SrcSkill0")   d.src_skill = std::atoi(v.c_str());
                    else if (k == "SrcSkillLv0") d.src_skill_lv = std::atoi(v.c_str());
                    else if (k == "SrcGelder")   d.src_gelder = std::atoi(v.c_str());
                    else if (k == "ResultTable") d.results = tables[yaml_scalar(v)];
                    else if (k == "MaxCount")    max_count = std::atoi(v.c_str());
                    else if (k == "RefId")       d.ref_id = static_cast<int16_t>(std::atoi(v.c_str()));
                    else if (k == "Options")     d.options = std::atoi(v.c_str());
                    else if (k.size() == 8 && k.compare(0, 7, "SrcItem") == 0 && k[7] >= '0' && k[7] <= '5')
                        it[k[7] - '0'] = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    else if (k.size() == 4 && k.compare(0, 3, "Cnt") == 0 && k[3] >= '0' && k[3] <= '5')
                        cnt[k[3] - '0'] = std::atoi(v.c_str());
                }
                if (!d.id) return;
                d.min_level = static_cast<int16_t>(min_level);
                d.max_level = static_cast<int16_t>(max_level);
                const int16_t mc = static_cast<int16_t>(max_count);
                d.max_count = static_cast<uint8_t>(mc <= 0 ? 25 : mc);
                if (d.min_level < 0) d.min_level = 0;
                if (d.max_level < d.min_level) d.max_level = d.min_level;
                if (d.max_level < 0) d.max_level = 0;
                if (d.exp < 0) d.exp = 0;
                for (int i = 0; i < 6; ++i)
                    if (it[i]) d.src.push_back({ it[i], item_cnt(it[i], cnt[i]) });
                share_messages(d);
                into.emplace(d.id, std::move(d));      // the hash map keeps the first row of a key
            });
            return true;
        };
        if (!load_info(quest_path, quests_)) return false;
        load_info(scenario_path, scenario_);
        // RandomResultItemInfo (sub_639B50).
        {
            std::ifstream f(random_path);
            if (!f) { LOG_ERROR("QuestDb: cannot open %s", random_path.c_str()); return false; }
            DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                int id = 0;
                uint32_t it[10]{};
                float rate[10]{};
                int32_t cnt[10]{};
                for (const auto& [k, v] : kvs) {
                    if (k == "ID") id = std::atoi(v.c_str());
                    else if (k.size() >= 5 && k.compare(0, 4, "Item") == 0) {
                        const int i = std::atoi(k.c_str() + 4);
                        if (i >= 0 && i < 10) it[i] = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    } else if (k.size() >= 5 && k.compare(0, 4, "Rate") == 0) {
                        const int i = std::atoi(k.c_str() + 4);
                        if (i >= 0 && i < 10) rate[i] = std::strtof(v.c_str(), nullptr);
                    } else if (k.size() >= 4 && k.compare(0, 3, "Cnt") == 0) {
                        const int i = std::atoi(k.c_str() + 3);
                        if (i >= 0 && i < 10) cnt[i] = std::atoi(v.c_str());
                    }
                }
                if (!id) return;
                RandomResultItems t;
                int32_t run = 0;
                for (int i = 0; i < 10; ++i) {
                    if (!it[i]) continue;
                    // the float Rate times the double 10000.0, truncated (__ftol2)
                    const int32_t share = static_cast<int32_t>(static_cast<double>(rate[i]) * 10000.0);
                    t.e[i].lo   = run;
                    t.e[i].hi   = share + run;
                    run         = t.e[i].hi;
                    t.e[i].item = it[i];
                    t.e[i].cnt  = static_cast<int16_t>(cnt[i]) ? cnt[i] : 1;
                    t.total    += share;
                    ++t.n;
                }
                random_.emplace(id, t);
            });
        }
        LOG_INFO("QuestDb: %zu quests, %zu scenario quests, %zu result tables, %zu random result tables",
                 quests_.size(), scenario_.size(), tables.size(), random_.size());
        return !quests_.empty();
    }

    // MonsterQuestInfo (sub_629D10 area) with its MonsterQuestResult tables (sub_627F60).
    bool load_monster(const std::string& quest_path, const std::string& result_path, const ItemDb& items)
    {
        if (!load_mq(quest_path, result_path, items, monster_, false)) return false;
        LOG_INFO("QuestDb: %zu monster quests", monster_.size());
        return !monster_.empty();
    }

    // PartyMonQuestInfo (the GameInfoMgr loader at "GameInfoMgr-Init PartyQuestInfo") with its PMonQuestResult
    // tables (the same sub_627F60) and PartyQuestExpTable (Ratio in row order).
    bool load_party(const std::string& quest_path, const std::string& result_path, const std::string& exp_path,
                    const ItemDb& items)
    {
        if (!load_mq(quest_path, result_path, items, party_, true)) return false;
        std::ifstream f(exp_path);
        if (!f) { LOG_ERROR("QuestDb: cannot open %s", exp_path.c_str()); return false; }
        party_ratio_.clear();
        DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
            for (const auto& [k, v] : kvs)
                if (k == "Ratio") party_ratio_.push_back(static_cast<float>(std::atof(v.c_str())));
        });
        LOG_INFO("QuestDb: %zu party quests, %zu party quest EXP ratios", party_.size(), party_ratio_.size());
        return !party_.empty();
    }

    // sub_627410: the EXP / TM ratio of a party quest done `times` times — rows 1..49 by index, 50 from 50 on
    // (0 reads the float before the table in the retail; no reward asks for it).
    double party_exp_ratio(int times) const
    {
        if (times < 0 || party_ratio_.empty()) return 0.0;
        if (times < 50) return times >= 1 && times <= static_cast<int>(party_ratio_.size())
                               ? static_cast<double>(party_ratio_[static_cast<size_t>(times - 1)]) : 0.0;
        return static_cast<double>(party_ratio_[(std::min)(party_ratio_.size(), size_t{ 50 }) - 1]);
    }

    // sub_627340: PartyMonQuestInfo by id.
    const MonsterQuestDef* party(int id) const
    {
        auto it = party_.find(id);
        return it == party_.end() ? nullptr : &it->second;
    }

private:
    // The monster quests and the party quests share the layout (the 20-byte grades, the result rows). A party
    // quest's columns are the quest's own Time / MinLevel / MaxLevel / Exp / Tmxp, given to every grade.
    bool load_mq(const std::string& quest_path, const std::string& result_path, const ItemDb& items,
                 std::unordered_map<int, MonsterQuestDef>& out, bool party)
    {
        auto item_cnt = [&](uint32_t id, int32_t cnt) {
            const ItemDef* d = items.get(id);
            return (d && (d->options & 1)) ? 1 : cnt;
        };
        std::unordered_map<std::string, std::vector<MonsterQuestResult>> tables;
        {
            std::ifstream f(result_path);
            if (!f) { LOG_ERROR("QuestDb: cannot open %s", result_path.c_str()); return false; }
            DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                std::string table;
                MonsterQuestResult r;
                uint32_t it[4]{};
                int32_t  cnt[4]{};
                std::string msg[10];
                bool has_msg[10]{};
                int32_t rnd[3]{};
                for (const auto& [k, v] : kvs) {
                    if      (k == "Table")  table = yaml_scalar(v);
                    else if (k == "Grade")  r.grade = std::atoi(v.c_str());
                    else if (k == "Type")   r.type = std::atoi(v.c_str());
                    else if (k == "Gelder") r.gelder = std::atoi(v.c_str());
                    else if (k.size() == 5 && k.compare(0, 4, "Item") == 0 && k[4] >= '0' && k[4] <= '3')
                        it[k[4] - '0'] = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
                    else if (k.size() == 4 && k.compare(0, 3, "Cnt") == 0 && k[3] >= '0' && k[3] <= '3')
                        cnt[k[3] - '0'] = std::atoi(v.c_str());
                    else if (k.size() == 4 && k.compare(0, 3, "Msg") == 0 && k[3] >= '0' && k[3] <= '9') {
                        msg[k[3] - '0'] = yaml_scalar(v);
                        has_msg[k[3] - '0'] = true;
                    }
                    else if (k.size() == 14 && k.compare(0, 13, "RandomTableID") == 0 && k[13] >= '0' && k[13] <= '2')
                        rnd[k[13] - '0'] = std::atoi(v.c_str());
                }
                if (table.empty()) return;
                for (int i = 0; i < 4; ++i)
                    if (it[i]) r.items.push_back({ it[i], item_cnt(it[i], cnt[i]) });
                int last = 0;
                for (int i = 0; i < 10; ++i) if (has_msg[i] && !msg[i].empty()) last = i;
                for (int i = 0; i <= last; ++i) r.msgs.push_back(msg[i]);
                for (int i = 0; i < 3; ++i) if (rnd[i]) r.random_tables.push_back(rnd[i]);
                tables[table].push_back(std::move(r));
            });
        }
        {
            std::ifstream f(quest_path);
            if (!f) { LOG_ERROR("QuestDb: cannot open %s", quest_path.c_str()); return false; }
            DrillDb::each_record(f, [&](const std::vector<std::pair<std::string, std::string>>& kvs) {
                MonsterQuestDef d;
                MonsterQuestGrade g[5];
                int min_level = 0, max_level = 0, max_count = 0;
                int q_time = 0, q_exp = 0, q_tmxp = 0;            // (a party quest's own columns)
                std::string rt;
                for (const auto& [k, v] : kvs) {
                    if      (party && k == "Time") q_time = std::atoi(v.c_str());
                    else if (party && k == "Exp")  q_exp  = std::atoi(v.c_str());
                    else if (party && k == "Tmxp") q_tmxp = std::atoi(v.c_str());
                    else if (k == "Id")          d.id = std::atoi(v.c_str());
                    else if (k == "Name")        d.name = yaml_scalar(v);
                    else if (k == "Client")      d.client = yaml_scalar(v);
                    else if (k == "Pos")         d.pos = yaml_scalar(v);
                    else if (k == "Comment")     d.comment = yaml_scalar(v);
                    else if (k == "MinLevel")    min_level = std::atoi(v.c_str());
                    else if (k == "MaxLevel")    max_level = std::atoi(v.c_str());
                    else if (k == "MaxCount")    max_count = std::atoi(v.c_str());
                    else if (k == "ResultTable") rt = yaml_scalar(v);
                    else if (k == "ReturnMapID") d.return_map = std::atoi(v.c_str());
                    else {
                        // the grade columns: SrcMonsterN, CntN, TimeN, MinLvN, MaxLvN, ExpN, TmxpN (N = 0..4)
                        static const char* kCols[7] = { "SrcMonster", "Cnt", "Time", "MinLv", "MaxLv", "Exp", "Tmxp" };
                        for (int c = 0; c < 7; ++c) {
                            const size_t len = std::char_traits<char>::length(kCols[c]);
                            if (k.size() != len + 1 || k.compare(0, len, kCols[c]) != 0) continue;
                            const int i = k[len] - '0';
                            if (i < 0 || i > 4) break;
                            const int n = std::atoi(v.c_str());
                            switch (c) {
                            case 0: g[i].src_monster = n; break;
                            case 1: g[i].cnt = static_cast<int16_t>(n); break;
                            case 2: g[i].time = static_cast<int16_t>(n); break;
                            case 3: g[i].min_lv = static_cast<int16_t>(n); break;
                            case 4: g[i].max_lv = static_cast<int16_t>(n); break;
                            case 5: g[i].exp = n < 0 ? 0 : n; break;
                            case 6: g[i].tmxp = n; break;
                            }
                            break;
                        }
                    }
                }
                if (!d.id) return;
                // the grade count = the SrcMonster columns that are set; columns 0.. that many are read
                int used = 0;
                for (int i = 0; i < 5; ++i) if (g[i].src_monster) ++used;
                for (int i = 0; i < used; ++i) d.grades.push_back(g[i]);
                if (party)
                    for (MonsterQuestGrade& gr : d.grades) {
                        gr.time   = static_cast<int16_t>(q_time);
                        gr.min_lv = static_cast<int16_t>(min_level);
                        gr.max_lv = static_cast<int16_t>(max_level);
                        gr.exp    = q_exp < 0 ? 0 : q_exp;
                        gr.tmxp   = q_tmxp;
                    }
                const int16_t mc = static_cast<int16_t>(max_count);
                d.max_count = static_cast<uint8_t>(mc <= 0 ? 25 : mc);
                d.min_level = static_cast<int16_t>(min_level < 0 ? 0 : min_level);
                d.max_level = static_cast<int16_t>(max_level);
                if (d.max_level < d.min_level) d.max_level = d.min_level;
                if (d.max_level < 0) d.max_level = 0;
                // the results: max Grade + 1 slots, each row at its Grade; the count = the rows
                auto t = tables.find(rt);
                if (!rt.empty() && t != tables.end() && !t->second.empty()) {
                    int maxg = 0;
                    for (const MonsterQuestResult& r : t->second) if (r.grade > maxg) maxg = r.grade;
                    d.results.resize(static_cast<size_t>(maxg + 1));
                    for (const MonsterQuestResult& r : t->second)
                        if (r.grade >= 0) d.results[static_cast<size_t>(r.grade)] = r;
                    d.result_count = static_cast<int>(t->second.size());
                }
                out.emplace(d.id, std::move(d));
            });
        }
        LOG_INFO("QuestDb: %zu %s result tables from %s", tables.size(), party ? "party quest" : "monster quest",
                 result_path.c_str());
        return true;
    }

public:

    // sub_626830: QuestInfo by id.
    const QuestDef* get(int id) const
    {
        auto it = quests_.find(id);
        return it == quests_.end() ? nullptr : &it->second;
    }

    // sub_626D20: ScenarioQuestInfo by id.
    const QuestDef* scenario(int id) const
    {
        auto it = scenario_.find(id);
        return it == scenario_.end() ? nullptr : &it->second;
    }

    // sub_6284C0: MonsterQuestInfo by id.
    const MonsterQuestDef* monster(int id) const
    {
        auto it = monster_.find(id);
        return it == monster_.end() ? nullptr : &it->second;
    }

    // sub_639E40: RandomResultItemInfo by id.
    const RandomResultItems* random(int id) const
    {
        auto it = random_.find(id);
        return it == random_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<int, QuestDef> quests_;
    std::unordered_map<int, RandomResultItems> random_;
    std::unordered_map<int, MonsterQuestDef> monster_;
    std::unordered_map<int, MonsterQuestDef> party_;     // PartyMonQuestInfo
    std::vector<float>                       party_ratio_;   // PartyQuestExpTable Ratio, Times 1..50
    std::unordered_map<int, QuestDef> scenario_;

    // sub_626710: a row with at most one message takes the messages of the row with the largest Count up
    // to its own among the rows with more (sub_626790; the first row when none), in row order.
    static void share_messages(QuestDef& d)
    {
        for (QuestResult& r : d.results) {
            if (r.msgs.size() > 1) continue;
            int best = 0;
            size_t at = 0;
            for (size_t i = 0; i < d.results.size(); ++i) {
                const QuestResult& o = d.results[i];
                if (o.msgs.size() > 1 && o.count <= r.count && o.count > best) { best = o.count; at = i; }
            }
            r.msgs = d.results[at].msgs;
        }
    }
};
