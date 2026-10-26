#include "game_server.hpp"
#include "game_session.hpp"
#include "logger.hpp"
#include "client_text.hpp"
#include <algorithm>
#include <cstdlib>
#include <ctime>

// ─────────────────────────────────────────────────────────────────────────────
//  Normal quests — 1:1 with the retail GameServer (GameServerRelease) and its MasterServer QuestStore:
//
//  The NPC script functions (ExecuteNpcScript sub_7E85A0; the qid = the NPC record's QuestId[slot]):
//    Quest_Check (2, Quest_Check_1..6 = 90..95 -> slots 1..6) sub_7E0950: a quest in progress -> 4 (run
//      above MaxCount), the skill it asks for (3 none / 7 above / 6 below SrcSkillLv0), 3 (the items are
//      not all in the bag; Options 2: none of them) else 2; not in progress -> 5 when the money is under
//      SrcGelder, else hItemCheckQuest (DB uspm_quest_check_history): S2C 115 with the 1st argument (it can
//      be done again) or the 4th (done MaxCount times).
//    Quest_Accept (3, _1.._4 = 96..99) sub_7E0DA0: 3 level out of MinLevel..MaxLevel, -1 already taken,
//      2 no free entry; hItemAcceptQuest (uspm_quest_take: at most 6 of a type in progress, the run =
//      history + 1, refused at MaxCount + 1) -> the entry, S2C 132, S2C 115 with the 1st argument.
//      Quest_Continue (5, 106..109) is Quest_Accept answering -2 or -1; Quest_Quit (6) answers 1 (the
//      1st argument's message) and does nothing else. Quest_Once_Accept (184, 202..205) sub_7E2210 adds
//      uspm_quest_history_oneday_check: done today -> S2C 115 with the 4th argument.
//    Quest_Complete (4, 100..103) / Quest_NH_Complete (181: no history) sub_7E0F10: the run's result
//      (the row with the largest Count up to the run; Options 2: the tier of SrcItem0's count), its
//      skill (-1 when learned), the items (-1), an equipped single source item (5 with five arguments,
//      else -1), the weight and the free entries (2), then hItemCompleteQuest (uspm_quest_completed +
//      the items + the skill) -> OnOk sub_77CC60: the bag records (sub_6DC0E0), the skill (S2C 244), the
//      money (S2C 41), EXP / TM, S2C 150 (the report), S2C 115 (the 1st argument while the run is
//      under MaxCount, else the 3rd), S2C 133 and the entry goes.
//    CheckQuestState (82, _1/_2 = 104/105) sub_7E5420: two arguments; uspm_select_quest_history_check_count
//      (CheckQuestId, CheckQuestNum) -> S2C 115 with the 1st (done that often) or the 2nd.
//    ActiveQuestID_0..2 (114..116) sub_7E5E10: 1 when ActiveQuestIDn is in progress, else 2.
//  NPC PreCheck 1 (sub_71E720): the quest Arg done once -> Yes (S2C 112), else No; PreCheck 2 (sub_71E630):
//    Arg in progress -> Yes, else No.
//  A DB failure answers like the retail OnCancel handlers: S2C 116 and the talk state ends.
//  Wire (all VERIFIED in the client, Trickster.bin): S2C 234 (login, loading dispatcher sub_7457A0 ->
//  sub_6DFC00) [u16 n]{u32 qid, u16 run}; S2C 132 (sub_6F0960 case 132) [u32 qid][u16 run][cstr Name]
//  [cstr Client][cstr Pos][cstr Comment][u16 qid]; S2C 133 [u32 qid]; S2C 150 (case 150) [u16 n]{bag
//  record}[u16 m]{u32 item, u16 count}[u32 EXP][u32 TM EXP][u16 qid][u16 run][u32 gelder][u16 skill]
//  [u16 skill level]; S2C 244 (avatar sub_6B4E00 case 244) the 5-byte skill record.
//  Not modelled: tutorial quests (type 4: sub_5EF070), the pet's message (sub_738F50), guild points,
//  achievements (sub_76C740), the NPC event talk (S2C 1092), item expiry (S2C 804), QuestItemInfo
//  (quest 142 / item 500000, absent from ItemParam), the source skill removal (S2C 1113 — no quest
//  result has both SrcSkill0 and a Skill).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// sub_7EB070: the idx-th u16 of the argument stream, read from its start — false when the call has fewer
// than idx arguments, the stream ends first, or the value is 0.
bool quest_arg(const uint8_t* p, size_t size, int cnt, int idx, int& out)
{
    if (cnt < idx) return false;
    size_t pos = 0;
    for (int i = 0; i < idx; ++i) {
        if (size - pos < 2) return false;
        out = p[pos] | (p[pos + 1] << 8);
        pos += 2;
    }
    return out >= 1;
}

// The SQL quest type of the GameServer's (MasterServer dword_591AA4).
constexpr int kSqlNormal = 1;

}  // namespace

// ── The quest inventory (QuestInv, Player +2212) ──────────────────────────────────────────────────

GameClientSession::QuestEntry* GameClientSession::qinv_find(QuestEntry* inv, int qid, int cap)
{
    for (int i = 0; i < cap; ++i) if (inv[i].used && inv[i].qid == qid) return &inv[i];
    return nullptr;
}

// sub_7DBFC0: the entry of `qid` (sub_7DBF20: the first free one, run 1; its def from ScenarioQuestInfo when
// the inventory is the scenario one, +3112's flag) gets the run `count`.
GameClientSession::QuestEntry* GameClientSession::qinv_add(QuestEntry* inv, int qid, int count, bool scenario, int cap)
{
    QuestEntry* e = qinv_find(inv, qid, cap);
    if (!e) {
        for (int i = 0; i < cap; ++i) if (!inv[i].used) { e = &inv[i]; break; }
        if (!e) return nullptr;
        e->used  = true;
        e->done  = false;
        e->qid   = qid;
        e->def   = scenario ? server_.quest_db().scenario(qid) : server_.quest_db().get(qid);
        if (!e->def) LOG_ERROR("QuestInv::Add -can not found qid- qid:%d", qid);
        e->count = 1;
    }
    e->count = count;
    return e;
}

bool GameClientSession::qinv_remove(QuestEntry* inv, int qid, int cap)
{
    QuestEntry* e = qinv_find(inv, qid, cap);
    if (!e) return false;
    *e = QuestEntry{};                                   // sub_7DC170
    return true;
}

GameClientSession::QuestEntry* GameClientSession::quest_find(int qid) { return qinv_find(quests_, qid); }
GameClientSession::QuestEntry* GameClientSession::quest_add(int qid, int count) { return qinv_add(quests_, qid, count, false); }
bool GameClientSession::quest_remove(int qid) { return qinv_remove(quests_, qid); }

// The login: uspm_char_all_select_char_info_improve's normal quests (char_quest types 1 and 4) and scenario
// quests (type 6), {quest_id, com_count} each, at most 30 ("create client : invalid quest item cnt" /
// "invalid scenario quest item cnt") into their QuestInvs.
void GameClientSession::load_quests()
{
    for (QuestEntry& e : quests_) e = QuestEntry{};
    for (QuestEntry& e : squests_) e = QuestEntry{};
    if (!char_uid_) return;
    auto fill = [&](const char* types, QuestEntry* inv, bool scenario, const char* what) {
        const DBResult rows = server_.db().query(
            "SELECT quest_id, com_count FROM char_quest WHERE char_uid=%u AND quest_type IN (%s)", char_uid_, types);
        if (rows.size() > 30)
            LOG_ERROR("%s : create client : invalid %s item cnt %zu", char_name_, what, rows.size());
        size_t n = 0;
        for (const DBRow& r : rows) {
            if (n++ >= 30) break;
            auto q = r.find("quest_id");
            auto c = r.find("com_count");
            if (q == r.end() || c == r.end()) continue;
            qinv_add(inv, std::atoi(q->second.c_str()), std::atoi(c->second.c_str()), scenario);
        }
    };
    fill("1,4", quests_, false, "quest");
    fill("6", squests_, true, "scenario quest");
}

// S2C 833 (sub_6038F0, the login sequence; client sub_6E0410): [u16 n]{u32 qid, u16 run}.
void GameClientSession::send_scenario_quest_list()
{
    PacketBuffer p(static_cast<Opcode>(0x0341), is_paula());
    uint16_t n = 0;
    for (const QuestEntry& e : squests_) if (e.used) ++n;
    p.write<uint16_t>(n);
    for (const QuestEntry& e : squests_) {
        if (!e.used) continue;
        p.write<uint32_t>(static_cast<uint32_t>(e.qid));
        p.write<uint16_t>(static_cast<uint16_t>(e.count));
    }
    send(p);
}

// S2C 234 (sub_5F6BF0, the login sequence): [u16 n]{u32 qid, u16 run}.
void GameClientSession::send_quest_list()
{
    PacketBuffer p(static_cast<Opcode>(0x00EA), is_paula());
    uint16_t n = 0;
    for (const QuestEntry& e : quests_) if (e.used) ++n;
    p.write<uint16_t>(n);
    for (const QuestEntry& e : quests_) {
        if (!e.used) continue;
        p.write<uint32_t>(static_cast<uint32_t>(e.qid));
        p.write<uint16_t>(static_cast<uint16_t>(e.count));
    }
    send(p);
}

// S2C 132 (sub_5F9EA0): [u32 QuestInfo Id][u16 run][cstr Name][cstr Client][cstr Pos][cstr Comment]
// [u16 qid]. Client case 132: adds it to the quest list ("etc_quest" sound, the quest window).
void GameClientSession::send_quest_add(const QuestEntry& e)
{
    if (!e.def) return;
    PacketBuffer p(static_cast<Opcode>(0x0084), is_paula());
    p.write<uint32_t>(static_cast<uint32_t>(e.def->id));
    p.write<uint16_t>(static_cast<uint16_t>(e.count));
    for (const std::string* s : { &e.def->name, &e.def->client, &e.def->pos, &e.def->comment }) {
        const std::string t = to_client_text(*s);
        p.write_bytes(t.c_str(), t.size() + 1);
    }
    p.write<uint16_t>(static_cast<uint16_t>(e.qid));
    send(p);
}

// S2C 133 (sub_5F9F90): [u32 qid] — client case 133 takes it off the list.
void GameClientSession::send_quest_remove(int qid)
{
    PacketBuffer p(static_cast<Opcode>(0x0085), is_paula());
    p.write<uint32_t>(static_cast<uint32_t>(qid));
    send(p);
}

// The DB handlers' OnCancel (e.g. hItemAcceptQuest::OnCancel sub_77C900): S2C 116 (empty, sub_5F3B80)
// and a player in the talk state 9 leaves it.
void GameClientSession::send_npc_cancel()
{
    PacketBuffer c(static_cast<Opcode>(0x0074), is_paula());
    send(c);
    if (talk_npc_) npc_end_talk();
}

// sub_762250 -> sub_7DC070 (the normal quests +2212, then the scenario quests +3112): the items the quests in
// progress (not completed) ask for — the drill's type-5 boost.
std::vector<uint32_t> GameClientSession::quest_items() const
{
    std::vector<uint32_t> out;
    for (const QuestEntry* inv : { quests_, squests_ })
        for (int i = 0; i < 30; ++i) {
            const QuestEntry& e = inv[i];
            if (!e.used || e.done || !e.def) continue;
            for (const QuestItem& it : e.def->src)
                if (it.item && std::find(out.begin(), out.end(), it.item) == out.end()) out.push_back(it.item);
        }
    return out;
}

// sub_60CA40: the NPC's QuestInfo (one per QuestId, Npc +1684) whose Id is `qid`.
const QuestDef* GameClientSession::npc_quest_def(const NpcEntity& n, int qid) const
{
    if (!n.row) return nullptr;
    for (int i = 0; i < 5; ++i) {
        const QuestDef* d = server_.quest_db().get(n.row->quest_id(i));
        if (d && static_cast<int16_t>(d->id) == static_cast<int16_t>(qid)) return d;
    }
    return nullptr;
}

// sub_6DB140 (all) / sub_6DB1A0 (any): an item asked for is there when one bag item of its id has at least
// its count (sub_50CD80 -> sub_50CDE0: GetCount, a single item counts 1).
bool GameClientSession::quest_items_ok(const QuestDef& d, bool any)
{
    auto has = [&](const QuestItem& it) {
        if (it.cnt <= 0) return false;
        for (const BagItem& b : bag_) {
            if (b.item_id != it.item) continue;
            const ItemDef* idf = server_.item_db().get(b.item_id);
            const int have = idf ? idf->item_count(b.count) : b.count;
            if (have >= it.cnt) return true;
        }
        return false;
    };
    for (const QuestItem& it : d.src) {
        if (!it.item) continue;
        const bool ok = has(it);
        if (any && ok) return true;
        if (!any && !ok) return false;
    }
    return !any;
}

// sub_6DC0E0 (hItemCompleteQuest::Send) + sub_6DBA50 (OnOk): the bag records, made against the bag as it is —
// each reward first (a common one netted against the same item taken, sub_6DC2E0: what remains of either),
// stacking on the first item of its id (type 1) or new (type 4; a drill holds its DrillLife, sub_6EBAD0),
// then each item taken (sub_6DBF00: the first item of its id, type 3 or what remains, type 1) — then applied
// in that order (the DB gives the new items their uid).
std::vector<GameClientSession::BagRecord> GameClientSession::quest_bag_records(std::vector<QuestItem> left,
                                                                             const std::vector<QuestItem>& adds,
                                                                             int* new_items, bool apply)
{
    // Hardening (not retail): retail computes each item taken against the bag as it was, so one listed twice made
    // two records of one stack and the last won (taken once, GSI-X4). Repeated entries are added up first: one per
    // uid, a uid-less one per item id (given_merge does the same, and checks them, for the exchange / compound).
    {
        std::vector<QuestItem> merged;
        for (const QuestItem& l : left) {
            if (!l.item) continue;
            auto m = std::find_if(merged.begin(), merged.end(),
                                  [&](const QuestItem& x) { return x.uid == l.uid && x.item == l.item; });
            if (m == merged.end()) merged.push_back(l);
            else m->cnt += l.cnt;
        }
        left.swap(merged);
    }
    struct Rec { BagRecord r; const ItemDef* def; };
    std::vector<Rec> recs;
    for (QuestItem it : adds) {
        if (!it.item) continue;
        const ItemDef* idf = server_.item_db().get(it.item);
        if (!idf) { LOG_ERROR("hItemCompleteQuest: item %u is not in ItemParam", it.item); continue; }
        if (idf->stackable()) {
            for (QuestItem& l : left) {
                if (l.item != it.item) continue;
                it.cnt -= l.cnt;
                if (it.cnt >= 0) l.item = 0;              // sub_6DD0C0
                else l.cnt = -it.cnt;
                break;
            }
        }
        if (it.cnt <= 0) continue;
        Rec x{ {}, idf };
        x.r.kind    = static_cast<uint8_t>(idf->cls);
        x.r.item_id = it.item;
        uint16_t count = static_cast<uint16_t>(it.cnt);
        if (idf->type == 6)
            if (const DrillParam* dp = server_.drill_db().get(it.item)) count = static_cast<uint16_t>(dp->drill_life);
        const BagItem* stack = idf->stackable() ? bag_find_id(it.item) : nullptr;   // sub_6DBD10
        if (stack) {
            x.r.type  = 1;
            x.r.uid   = stack->uid;
            x.r.count = static_cast<uint16_t>(stack->count + count);
        } else {
            x.r.type  = 4;
            x.r.count = count;
        }
        recs.push_back(std::move(x));
    }
    for (const QuestItem& l : left) {
        if (!l.item) continue;
        BagItem* b = l.uid ? bag_find_uid(l.uid) : bag_find_id(l.item);   // sub_6DBF00: by uid, else the id
        if (!b) continue;
        recs.push_back({ bag_decrease(*b, static_cast<uint16_t>(l.cnt)), server_.item_db().get(l.item) });
    }
    if (new_items) {                                     // the new entries (record types 2 / 4 / 6)
        *new_items = 0;
        for (const Rec& x : recs) if (x.r.type == 4) ++*new_items;
    }
    std::vector<BagRecord> applied;
    if (!apply) {
        for (const Rec& x : recs) applied.push_back(x.r);
        return applied;
    }
    for (Rec& x : recs) {
        if (x.r.type == 4) {
            applied.push_back(bag_new(*x.def, x.r.count));
            continue;
        }
        if (x.r.type == 3) server_.item_store().RemoveItem(x.r.uid);
        else               server_.item_store().SetAmount(x.r.uid, x.r.count);
        bag_apply(x.r);
        applied.push_back(x.r);
    }
    return applied;
}

// ── The script functions ──────────────────────────────────────────────────────────────────────────

int GameClientSession::quest_check(NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt)
{
    const QuestDef* d = server_.quest_db().get(qid);
    if (!d) return -1;
    if (const QuestEntry* e = quest_find(qid)) {
        if (e->count > d->max_count) return 4;
        if (d->src_skill) {
            const LearnedSkill* s = find_skill(d->src_skill);   // sub_55F430 / sub_7EAF20
            if (!s) return 3;
            if (s->level > d->src_skill_lv) return 7;
            if (s->level < d->src_skill_lv) return 6;
        }
        if (!quest_items_ok(*d, d->options == 2)) return 3;
        return 2;
    }
    if (d->src_gelder > money_) return 5;
    int yes = -1, no = -1;
    if (!quest_arg(args, size, cnt, 1, yes) || !quest_arg(args, size, cnt, 4, no)) {
        LOG_ERROR("%s Invalid CheckQuest res:%d, cntArg:%d, npctype:%d, zoneNo:%d", char_name_, yes < 1 ? 1 : 4,
                  cnt, n.row ? n.row->type : 0, zone_id_);
        return -1;
    }
    // hItemCheckQuest -> QuestStore::CheckQuest: 0 -> the 1st argument, 1001 -> the 4th; 1002 is logged and
    // answered without its byte written (taken as "no"); any other failure -> OnCancel.
    int com = 0;
    const int err = db_quest_check_history(kSqlNormal, qid, d->max_count, 0, com);
    if (err != 0 && err != 1001 && err != 1002) {
        LOG_ERROR("QuestStore::CheckQuest SP failure : spRet:%d, chr:%u, qType:%d, qid:%d, maxCnt:%d", err, char_uid_,
                  kSqlNormal, qid, d->max_count);
        send_npc_cancel();
        return -2;
    }
    if (err == 1002)
        LOG_ERROR("QuestStore::CheckQuest invalid count: chr:%u, qType:%d, qid:%d, maxCnt:%d, Count:%d", char_uid_,
                  kSqlNormal, qid, d->max_count, com);
    send_npc_message(n, err == 0 ? yes : no, qid);       // hItemCheckQuest::OnOk (sub_77C370)
    return -2;
}

int GameClientSession::quest_accept(NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt, bool once)
{
    const QuestDef* d = server_.quest_db().get(qid);
    if (!d) return -1;
    // sub_7EB1C0: MinLevel and MaxLevel both 0 -> any level, else MinLevel <= level <= MaxLevel.
    if (!(d->min_level <= 0 && d->max_level <= 0) && (level_ < d->min_level || level_ > d->max_level)) return 3;
    if (quest_find(qid)) return -1;                      // sub_76E4E0
    bool free_entry = false;
    for (const QuestEntry& e : quests_) if (!e.used) { free_entry = true; break; }
    if (!free_entry) return 2;                           // sub_7DBE80
    int msg = -1, today_msg = -1;
    if (!quest_arg(args, size, cnt, 1, msg) || (once && !quest_arg(args, size, cnt, 4, today_msg))) {
        LOG_ERROR("%s Invalid %s res:%d, cntArg:%d, npctype:%d, zoneNo:%d", char_name_,
                  once ? "AcceptQuestOnce" : "AcceptQuest", msg < 1 ? 1 : 4, cnt, n.row ? n.row->type : 0, zone_id_);
        return -1;
    }
    // OnQCmdAcceptQuestOnce: uspm_quest_history_oneday_check first — done today -> 5089, whose handler
    // (sub_77C970) shows the 4th argument.
    if (once && db_quest_done_today(kSqlNormal, qid, 0)) {
        send_npc_message(n, today_msg, qid);
        return -2;
    }
    int com = 0;
    const int err = db_quest_take(kSqlNormal, qid, d->max_count, 0, com);
    if (err > 0) {
        LOG_ERROR("QuestStore::AcceptQuest SP failure : spRet:%d, chr:%u, qType:%d, qid:%d, maxCnt:%d, Count:%d",
                  err, char_uid_, kSqlNormal, qid, d->max_count, com);
        send_npc_cancel();                               // hItemAcceptQuest::OnCancel (sub_77C900)
        return -2;
    }
    // hItemAcceptQuest::OnOk (sub_77C490 -> sub_77C9D0): the entry with the run the DB gave, S2C 132.
    QuestEntry* e = quest_add(qid, com);
    if (!e) {
        db_quest_cancel(qid, 0);                         // sub_77C060 (uspm_quest_cancel)
        LOG_ERROR("-- hItemAcceptQuest::OnOk - but !bRet : name(%s), qid(%d), qtype(%d)", char_name_, qid, 1);
        send_npc_cancel();
        return -2;
    }
    send_quest_add(*e);
    LOG_INFO("GameClientSession %u: quest %d taken (run %d)", session_id(), qid, com);
    if (msg < 512) send_npc_message(n, msg, qid);
    return -2;
}

int GameClientSession::quest_complete(NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt, bool history)
{
    const QuestDef* d = server_.quest_db().get(qid);
    if (!d) return -1;
    QuestEntry* e = quest_find(qid);
    if (!e) return -1;
    const QuestResult* first = d->result_for(e->count);   // sub_6D8680 (no table: the retail reads nothing)
    if (!first) return -1;
    // The result's skill (sub_8923C0: learned already -> -1); the skill asked for goes when its level is
    // exactly SrcSkillLv0 (none in the data).
    int skill = 0, skill_lv = 0;
    if (first->skill) {
        if (find_skill(first->skill)) return -1;
        skill = first->skill;
        skill_lv = first->skill_lv;
        if (d->src_skill) {
            const LearnedSkill* s = find_skill(d->src_skill);
            if (!s) return -1;
            if (d->src_skill_lv > s->level) return -1;
        }
    }
    if (!quest_items_ok(*d, d->options == 2)) return -1;
    // sub_7606E0: a single (non-common) item asked for that is equipped (sub_6DA5A0 on Player +2120).
    for (const QuestItem& it : d->src) {
        const ItemDef* idf = server_.item_db().get(it.item);
        if (!it.item || !idf || idf->stackable()) continue;
        for (uint64_t uid : equip_) {
            const BagItem* b = uid ? bag_find_uid(uid) : nullptr;
            if (b && b->item_id == it.item) return cnt == 5 ? 5 : -1;
        }
    }
    // The run (Options 2: the tier of how many of SrcItem0 the bag holds, sub_6DADA0 -> sub_6D8710).
    int run = e->count;
    const bool choice = d->options == 2;
    if (choice) {
        int have = 0;
        const uint32_t id0 = d->src.empty() ? 0 : d->src[0].item;
        for (const BagItem& b : bag_) {
            if (b.item_id != id0) continue;
            const ItemDef* idf = server_.item_db().get(b.item_id);
            have += idf ? idf->item_count(b.count) : b.count;
        }
        run = d->user_choice_tier(have);
        if (run == -1) return -1;
    }
    const QuestResult* res = d->result_by(run, choice);  // sub_6D87B0
    if (!res) return -1;
    // The rewards: Type 1 -> one item at random (and one random table), else all of them and every
    // random table (sub_7EB220 each).
    std::vector<QuestItem> rewards, extra;
    int pick = -1;
    if (res->type) {
        if (res->items.empty()) return -1;               // (rand() % 0 in the retail)
        pick = std::rand() % static_cast<int>(res->items.size());
        rewards.push_back(res->items[static_cast<size_t>(pick)]);
        if (!res->random_tables.empty()) {
            const int k = std::rand() % static_cast<int>(res->random_tables.size());
            const RandomResultItems* t = server_.quest_db().random(res->random_tables[static_cast<size_t>(k)]);
            const RandomResultItems::Entry* r = t ? t->pick() : nullptr;
            if (!r) {
                LOG_ERROR("RandomResultItem Table Error! TableId:%d Player Name %s", k, char_name_);
                return -1;
            }
            extra.push_back({ r->item, r->cnt });
        }
    } else {
        rewards = res->items;
        for (int id : res->random_tables) {
            const RandomResultItems* t = server_.quest_db().random(id);
            const RandomResultItems::Entry* r = t ? t->pick() : nullptr;
            if (!r) {
                LOG_ERROR("RandomResultItem Table Error! TableId:%d Player Name %s", id, char_name_);
                return -1;
            }
            extra.push_back({ r->item, r->cnt });
        }
    }
    // The weight (sub_6EBA60: count x Weight) after: - the items taken (a choice quest: the tier's item)
    // + the rewards; the free entries for every reward line (sub_6E4290: 300 - the items).
    auto weight = [&](const std::vector<QuestItem>& v) {
        int w = 0;
        for (const QuestItem& it : v) {
            const ItemDef* idf = server_.item_db().get(it.item);
            w += it.cnt * (idf ? idf->weight : 0);
        }
        return w;
    };
    std::vector<QuestItem> taken = choice ? std::vector<QuestItem>{ d->src[static_cast<size_t>(run)] } : d->src;
    const int delta = -weight(taken) + weight(rewards) + weight(extra);
    if (delta + bag_weight_ > st_maxwt_) return 2;
    if (300 - static_cast<int>(bag_.size()) < static_cast<int>(rewards.size() + extra.size())) return 2;
    int msg = -1;
    if (!quest_arg(args, size, cnt, e->count < d->max_count ? 1 : 3, msg)) return -1;

    // ── hItemCompleteQuest -> OnQCmdCompleteQuest: uspm_quest_completed (with or without history) ──
    const int err = db_quest_completed(kSqlNormal, qid, 0, 0, 0, history);
    if (err) {
        LOG_ERROR("QuestStore::CompletedQuest SP failure : spRet:%d, chr:%u, qType:%d, qid:%d", err, char_uid_,
                  kSqlNormal, qid);
        send_npc_cancel();                               // hItemCompleteQuest::OnCancel (sub_77D640)
        return -2;
    }
    std::vector<QuestItem> adds = rewards;
    adds.insert(adds.end(), extra.begin(), extra.end());
    const std::vector<BagRecord> applied = quest_bag_records(taken, adds);
    // sub_77ED80: the result's skill — S2C 244 [u16 skill][u8 level][u16 0] (client case 244 -> sub_71EC60
    // adds it), the list and the DB (sub_5F22F0, message 4501).
    if (skill) {
        PacketBuffer p(static_cast<Opcode>(0x00F4), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(skill));
        p.write<uint8_t>(static_cast<uint8_t>(skill_lv));
        p.write<uint16_t>(0);
        send(p);
        if (server_.skill_db().get(skill)) {
            skills_.push_back({ static_cast<uint16_t>(skill), static_cast<uint8_t>(skill_lv) });
            server_.db().execute("INSERT INTO char_skills (char_uid, skill_id, level) VALUES (%u, %d, %d) "
                                 "ON DUPLICATE KEY UPDATE level=%d", char_uid_, skill, skill_lv, skill_lv);
        }
    }
    pet_speech(15);                                      // sub_77ED80's end: our pet's line
    // sub_77D6B0 (the same result again: the run, or the tier it was given): the money (+3148, S2C 41),
    // EXP and TM EXP (QuestInfo's + the result's, at least 0; sub_77E220: no level up -> S2C 38 [u32 EXP]
    // / S2C 285 [u8 0][u32 TM EXP]), the entry completed.
    const int32_t gelder = res->gelder;
    if (gelder > 0) {
        money_ += gelder;
        send_money();
    }
    int32_t exp  = res->exp  + d->exp;
    int32_t tmxp = res->tmxp + d->tmxp;
    if (exp < 0) exp = 0;
    if (tmxp < 0) tmxp = 0;
    if (exp > 0 && !add_exp(exp)) {
        PacketBuffer p(static_cast<Opcode>(0x0026), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(exp_));
        send(p);
    }
    if (tmxp > 0 && !add_tm_exp(tmxp)) {
        PacketBuffer p(static_cast<Opcode>(0x011D), is_paula());
        p.write<uint8_t>(0);
        p.write<uint32_t>(static_cast<uint32_t>(tm_exp_));
        send(p);
    }
    e->done = true;
    // S2C 150 (sub_5F9D40): the records, the rewards as listed (Type 1: the picked one) and the random
    // tables' items, EXP, TM EXP, qid, run, gelder, skill.
    {
        std::vector<QuestItem> listed = rewards;          // Type 1: the picked one (v24[0] = items[pick])
        listed.insert(listed.end(), extra.begin(), extra.end());
        PacketBuffer p(static_cast<Opcode>(0x0096), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(applied.size()));
        for (const BagRecord& r : applied) write_bag_record(p, r);
        p.write<uint16_t>(static_cast<uint16_t>(listed.size()));
        for (const QuestItem& it : listed) {
            p.write<uint32_t>(it.item);
            p.write<uint16_t>(static_cast<uint16_t>(it.cnt));
        }
        p.write<uint32_t>(static_cast<uint32_t>(exp));
        p.write<uint32_t>(static_cast<uint32_t>(tmxp));
        p.write<uint16_t>(static_cast<uint16_t>(e->qid));
        p.write<uint16_t>(static_cast<uint16_t>(e->count));
        p.write<uint32_t>(static_cast<uint32_t>(gelder));
        p.write<uint16_t>(static_cast<uint16_t>(skill));
        p.write<uint16_t>(static_cast<uint16_t>(skill_lv));
        send(p);
    }
    save_position();                                     // the money and the EXP (DB messages 3619 / 3622)
    LOG_INFO("GameClientSession %u: quest %d completed (run %d%s): %zu records, EXP %d, TM %d, gelder %d, skill %d",
             session_id(), qid, e->count, history ? "" : ", no history", applied.size(), exp, tmxp, gelder, skill);
    // The next message (the run under MaxCount: the 1st argument, else the 3rd), then the quest leaves.
    if (msg < 512) send_npc_message(n, msg, qid);
    send_quest_remove(qid);
    quest_remove(qid);
    return -2;
}

int GameClientSession::quest_check_state(NpcEntity& n, int qid, int num, const uint8_t* args, size_t size, int cnt,
                                         int sql_type)
{
    if (cnt != 2 || num <= 0) return -1;
    int yes = -1, no = -1;
    if (size < 4) return -1;
    yes = args[0] | (args[1] << 8);
    no  = args[2] | (args[3] << 8);
    // hItemCheckQuestState (sub_71F460) -> QuestStore::CheckQuestState -> OnOk (sub_789150): S2C 115.
    send_npc_message(n, db_quest_history_count(sql_type, qid, 0, num) ? yes : no, qid);
    return -2;
}

// sub_7E5E10: no QuestInfo -> 2; in progress (the normal or the skill quests, +2212 / +2472) -> 1; else 2.
int GameClientSession::quest_active(int qid)
{
    if (!server_.quest_db().get(qid)) return 2;
    if (quest_find(qid)) return 1;
    return qinv_find(skquests_, qid, kSkillQuestCap) ? 1 : 2;
}

// ── Scenario quests (ScenarioQuestInfo, the QuestInv +3112, quest type 8 / SQL 6) ──────────────────────
//  S_Quest_Check (122..126) sub_7E6910: in progress -> 4 (run above MaxCount), 2 (the items) or 3; else
//    money under SrcGelder -> 5, else uspm_quest_check_history -> S2C 115 with the 1st / 4th argument.
//  S_Quest_Accept (127..131) sub_7E6C80: 3 level, -1 taken, 2 full; uspm_quest_take -> S2C 829 [u32 qid]
//    [u16 run][cstr Name][cstr Client][cstr Pos][cstr Comment] (sub_603660), S2C 115 with the 1st argument.
//    S_Quest_Continue (137..141) = Accept answering -2 or -1.
//  S_Quest_Complete (132..136) sub_7E6DF0: level -> 4, the items -> -1, an equipped single item -> 5 / -1,
//    the run's result (Type 1: one item at random; no random tables), weight / free entries -> 2, the
//    message 1 (run under MaxCount) or 3; uspm_quest_completed + the items -> OnOk (sub_77EE40): EXP / TM
//    first, then the money, S2C 831 [records][items][u32 EXP][u32 TM EXP][u32 qid][u16 the row's Count]
//    [u32 gelder] (sub_6037B0), S2C 115, S2C 830 [u32 qid].
//  S_ActiveQuestID_0..2 (142..144) sub_7E7180, S_CheckQuestState(_1/_2) (145..147) sub_7E71D0 (type 8).
//  All VERIFIED in the client (sub_6F0960 cases 829 / 830 / 831; 833 in the loading dispatcher).

int GameClientSession::squest_check(NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt)
{
    const QuestDef* d = server_.quest_db().scenario(qid);
    if (!d) return -1;
    if (const QuestEntry* e = qinv_find(squests_, qid)) {
        if (e->count > d->max_count) return 4;
        return quest_items_ok(*d, false) ? 2 : 3;
    }
    if (d->src_gelder > money_) return 5;
    int yes = -1, no = -1;
    if (!quest_arg(args, size, cnt, 1, yes) || !quest_arg(args, size, cnt, 4, no)) {
        LOG_ERROR("%s Invalid CheckQuest res:%d, cntArg:%d, npctype:%d, zoneNo:%d", char_name_, yes < 1 ? 1 : 4, cnt,
                  n.row ? n.row->type : 0, zone_id_);
        return -1;
    }
    int com = 0;
    const int err = db_quest_check_history(6, qid, d->max_count, 0, com);
    if (err != 0 && err != 1001 && err != 1002) {
        send_npc_cancel();
        return -2;
    }
    send_npc_message(n, err == 0 ? yes : no, qid);
    return -2;
}

int GameClientSession::squest_accept(NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt)
{
    const QuestDef* d = server_.quest_db().scenario(qid);
    if (!d) return -1;
    if (!(d->min_level <= 0 && d->max_level <= 0) && (level_ < d->min_level || level_ > d->max_level)) return 3;
    if (qinv_find(squests_, qid)) return -1;
    bool free_entry = false;
    for (const QuestEntry& e : squests_) if (!e.used) { free_entry = true; break; }
    if (!free_entry) return 2;
    int msg = -1;
    if (!quest_arg(args, size, cnt, 1, msg)) {
        LOG_ERROR("%s Invalid AcceptQuest res:%d, cntArg:%d, npctype:%d, zoneNo:%d", char_name_, 1, cnt,
                  n.row ? n.row->type : 0, zone_id_);
        return -1;
    }
    int com = 0;
    const int err = db_quest_take(6, qid, d->max_count, 0, com);
    if (err > 0) {
        LOG_ERROR("QuestStore::AcceptQuest SP failure : spRet:%d, chr:%u, qType:%d, qid:%d, maxCnt:%d, Count:%d", err,
                  char_uid_, 6, qid, d->max_count, com);
        send_npc_cancel();
        return -2;
    }
    // hItemAcceptQuest::OnOk type 8 (sub_77CBE0): the entry, S2C 829.
    QuestEntry* e = qinv_add(squests_, qid, com, true);
    if (!e || !e->def) {
        if (e) qinv_remove(squests_, qid);
        db_quest_cancel(qid, 0);
        LOG_ERROR("-- hItemAcceptQuest::OnOk - but !bRet : name(%s), qid(%d), qtype(%d)", char_name_, qid, 8);
        send_npc_cancel();
        return -2;
    }
    PacketBuffer p(static_cast<Opcode>(0x033D), is_paula());
    p.write<uint32_t>(static_cast<uint32_t>(e->def->id));
    p.write<uint16_t>(static_cast<uint16_t>(e->count));
    for (const std::string* s : { &e->def->name, &e->def->client, &e->def->pos, &e->def->comment }) {
        const std::string t = to_client_text(*s);
        p.write_bytes(t.c_str(), t.size() + 1);
    }
    send(p);
    LOG_INFO("GameClientSession %u: scenario quest %d taken (run %d)", session_id(), qid, com);
    if (msg < 512) send_npc_message(n, msg, qid);
    return -2;
}

int GameClientSession::squest_complete(NpcEntity& n, int qid, const uint8_t* args, size_t size, int cnt)
{
    const QuestDef* d = server_.quest_db().scenario(qid);
    if (!d) return -1;
    QuestEntry* e = qinv_find(squests_, qid);
    if (!e || !e->def) return -1;
    const QuestDef* ed = e->def;
    if (!(ed->min_level <= 0 && ed->max_level <= 0) && (level_ < ed->min_level || level_ > ed->max_level)) return 4;
    if (!quest_items_ok(*d, false)) return -1;
    for (const QuestItem& it : d->src) {                 // sub_7606E0
        const ItemDef* idf = server_.item_db().get(it.item);
        if (!it.item || !idf || idf->stackable()) continue;
        for (uint64_t uid : equip_) {
            const BagItem* b = uid ? bag_find_uid(uid) : nullptr;
            if (b && b->item_id == it.item) return cnt == 5 ? 5 : -1;
        }
    }
    const QuestResult* res = d->result_for(e->count);
    if (!res) return -1;
    std::vector<QuestItem> rewards;
    if (res->type) {
        if (res->items.empty()) return -1;
        rewards.push_back(res->items[static_cast<size_t>(std::rand() % static_cast<int>(res->items.size()))]);
    } else {
        rewards = res->items;
    }
    auto weight = [&](const std::vector<QuestItem>& v) {
        int w = 0;
        for (const QuestItem& it : v) {
            const ItemDef* idf = server_.item_db().get(it.item);
            w += it.cnt * (idf ? idf->weight : 0);
        }
        return w;
    };
    const int delta = -weight(d->src) + weight(rewards);
    if (delta + bag_weight_ > st_maxwt_ || 300 - static_cast<int>(bag_.size()) < static_cast<int>(rewards.size()))
        return 2;
    int msg = -1;
    if (!quest_arg(args, size, cnt, e->count < d->max_count ? 1 : 3, msg)) return -1;
    const int err = db_quest_completed(6, qid, 0, 0, 0, true);
    if (err) {
        LOG_ERROR("QuestStore::CompletedQuest SP failure : spRet:%d, chr:%u, qType:%d, qid:%d", err, char_uid_, 6, qid);
        send_npc_cancel();
        return -2;
    }
    const std::vector<BagRecord> applied = quest_bag_records(d->src, rewards);
    pet_speech(15);                                      // sub_77ED80 (every OnOk): our pet's line
    // sub_77EE40: EXP / TM EXP (the result's + the quest's, at least 0) first, the entry completed, then the
    // money, then S2C 831.
    int32_t exp  = res->exp  + d->exp;
    int32_t tmxp = res->tmxp + d->tmxp;
    if (exp < 0) exp = 0;
    if (tmxp < 0) tmxp = 0;
    if (exp > 0 && !add_exp(exp)) {
        PacketBuffer p(static_cast<Opcode>(0x0026), is_paula());
        p.write<uint32_t>(static_cast<uint32_t>(exp_));
        send(p);
    }
    if (tmxp > 0 && !add_tm_exp(tmxp)) {
        PacketBuffer p(static_cast<Opcode>(0x011D), is_paula());
        p.write<uint8_t>(0);
        p.write<uint32_t>(static_cast<uint32_t>(tm_exp_));
        send(p);
    }
    e->done = true;
    const int32_t gelder = res->gelder;
    if (gelder > 0) {
        money_ += gelder;
        send_money();
    }
    {
        PacketBuffer p(static_cast<Opcode>(0x033F), is_paula());
        p.write<uint16_t>(static_cast<uint16_t>(applied.size()));
        for (const BagRecord& r : applied) write_bag_record(p, r);
        p.write<uint16_t>(static_cast<uint16_t>(rewards.size()));
        for (const QuestItem& it : rewards) {
            p.write<uint32_t>(it.item);
            p.write<uint16_t>(static_cast<uint16_t>(it.cnt));
        }
        p.write<uint32_t>(static_cast<uint32_t>(exp));
        p.write<uint32_t>(static_cast<uint32_t>(tmxp));
        p.write<uint32_t>(static_cast<uint32_t>(qid));
        p.write<uint16_t>(static_cast<uint16_t>(res->count));
        p.write<uint32_t>(static_cast<uint32_t>(gelder));
        send(p);
    }
    save_position();
    LOG_INFO("GameClientSession %u: scenario quest %d completed (run %d): %zu records, EXP %d, TM %d, gelder %d",
             session_id(), qid, e->count, applied.size(), exp, tmxp, gelder);
    if (msg < 512) send_npc_message(n, msg, qid);
    {
        PacketBuffer p(static_cast<Opcode>(0x033E), is_paula());   // S2C 830 (sub_603740)
        p.write<uint32_t>(static_cast<uint32_t>(qid));
        send(p);
    }
    qinv_remove(squests_, qid);
    return -2;
}

// ── The QuestStore procedures ─────────────────────────────────────────────────────────────────────

// uspm_quest_check_history: the run a new take would be (history + 1, else 1) — 1001 when it is
// MaxCount + 1, 1002 above (types 1, 2 and 6); MaxCount 99+ and a run of 99+ is run 99 and fine.
int GameClientSession::db_quest_check_history(int type, int qid, int max_count, int monster_id, int& com_count)
{
    com_count = 0;
    DBResult rows;
    if (type == 1 || type == 6 || type == 7)
        rows = server_.db().query("SELECT com_count FROM char_quest_history WHERE char_uid=%u AND quest_id=%d "
                                  "AND monster_id=0 AND quest_type=%d", char_uid_, qid, type);
    else if (type == 2)
        rows = server_.db().query("SELECT com_count FROM char_quest_history WHERE char_uid=%u AND quest_id=%d "
                                  "AND monster_id=%d AND quest_type=%d", char_uid_, qid, monster_id, type);
    else
        return 101;
    int err = 0;
    com_count = rows.empty() ? 1 : std::atoi(rows[0]["com_count"].c_str()) + 1;
    com_count &= 0xFF;                                   // a tinyint
    if (type == 1 || type == 2 || type == 6) {
        if (com_count == max_count + 1) err = 1001;
        else if (com_count > max_count + 1) err = 1002;
    }
    if (max_count >= 99 && com_count >= 99) { com_count = 99; err = 0; }
    return err;
}

// uspm_quest_take: at most 6 quests of a type in progress (else 1002); the run as check_history; the
// row goes into char_quest (types 1 and 6 with the run, 3 and 4 with run 1; monster quests are written
// when their kills are done).
int GameClientSession::db_quest_take(int type, int qid, int max_count, int monster_id, int& com_count)
{
    com_count = 0;
    const DBResult ing = server_.db().query("SELECT COUNT(*) AS n FROM char_quest WHERE char_uid=%u AND quest_type=%d",
                                            char_uid_, type);
    const int ing_count = ing.empty() ? 0 : std::atoi(ing[0].at("n").c_str());
    int err = 0;
    if (ing_count < 6) {
        bool found = false;
        int hist = 0;
        if (type == 1 || type == 6 || type == 7 || type == 2) {
            const DBResult rows = (type == 2)
                ? server_.db().query("SELECT com_count FROM char_quest_history WHERE char_uid=%u AND quest_id=%d "
                                     "AND monster_id=%d AND quest_type=%d", char_uid_, qid, monster_id, type)
                : server_.db().query("SELECT com_count FROM char_quest_history WHERE char_uid=%u AND quest_id=%d "
                                     "AND monster_id=0 AND quest_type=%d", char_uid_, qid, type);
            found = !rows.empty();
            if (found) hist = std::atoi(rows[0].at("com_count").c_str());
        } else {
            if (type == 5) err = 102;
            found = true;                                // @@rowcount of the COUNT(*) select: 1
        }
        com_count = found ? ((hist + 1) & 0xFF) : 1;
    } else {
        err = 1002;
    }
    if (err == 0 && (type == 1 || type == 2 || type == 6)) {
        if (com_count == max_count + 1) err = 1001;
        else if (com_count > max_count + 1) err = 1002;
    }
    if (max_count >= 99 && com_count >= 99) { com_count = 99; err = 0; }
    if (err == 0) {
        if (type == 1 || type == 6)
            server_.db().execute("INSERT INTO char_quest (char_uid, quest_type, quest_id, com_count, monster_id, "
                                 "kill_count, record_time) VALUES (%u, %d, %d, %d, 0, 0, 0)", char_uid_, type, qid,
                                 com_count);
        else if (type == 3 || type == 4) {
            com_count = 1;
            server_.db().execute("INSERT INTO char_quest (char_uid, quest_type, quest_id, com_count, monster_id, "
                                 "kill_count, record_time) VALUES (%u, %d, %d, 1, 0, 0, 0)", char_uid_, type, qid);
        }
    }
    return err;
}

// uspm_quest_completed: the run from char_quest; the history (types 1/6/7 with history: run 1 inserts,
// a later run updates the row — none to update is error 103); the char_quest row goes (exactly one, else
// 108); the log row. The procedure is one transaction: a failure leaves nothing written — checked here
// before the first write. (The guild point of the retail procedure: no guilds here.)
int GameClientSession::db_quest_completed(int type, int qid, int monster_id, int kill_count, int record_time,
                                          bool history)
{
    int com = 1;
    int err = 0;
    if (type == 1 || type == 6 || type == 7 || type == 2) {
        const DBResult rows = (type == 2)
            ? server_.db().query("SELECT com_count FROM char_quest WHERE char_uid=%u AND quest_id=%d AND monster_id=%d "
                                 "AND quest_type=%d", char_uid_, qid, monster_id, type)
            : server_.db().query("SELECT com_count FROM char_quest WHERE char_uid=%u AND quest_id=%d AND monster_id=0 "
                                 "AND quest_type=%d", char_uid_, qid, type);
        if (rows.empty()) err = 101;
        else com = std::atoi(rows[0].at("com_count").c_str());
    }
    if (com >= 99) com = 99;
    if (err) return err;
    // The history row a later run updates (none -> 103 / 106).
    const bool plain = (type == 1 || type == 6 || type == 7);
    if (history && (plain || type == 2)) {
        if (com < 1) return plain ? 104 : 107;
        if (com > 1) {
            const DBResult h = plain
                ? server_.db().query("SELECT 1 AS x FROM char_quest_history WHERE char_uid=%u AND quest_id=%d AND "
                                     "monster_id=0 AND quest_type=%d", char_uid_, qid, type)
                : server_.db().query("SELECT 1 AS x FROM char_quest_history WHERE char_uid=%u AND quest_id=%d AND "
                                     "monster_id=%d AND quest_type=2", char_uid_, qid, monster_id);
            if (h.size() != 1) return plain ? 103 : 106;
        }
    }
    // The char_quest row the procedure deletes (one exactly).
    {
        const DBResult n = server_.db().query("SELECT COUNT(*) AS n FROM char_quest WHERE char_uid=%u AND quest_id=%d "
                                              "AND monster_id=%d", char_uid_, qid, monster_id);
        if (n.empty() || std::atoi(n[0].at("n").c_str()) != 1) return 108;
    }
    if (history && plain) {
        if (com == 1)
            server_.db().execute("INSERT INTO char_quest_history (char_uid, quest_type, quest_id, com_count, monster_id, "
                                 "kill_count, record_time, quest_completed_date) VALUES (%u, %d, %d, %d, 0, 0, 0, NOW())",
                                 char_uid_, type, qid, com);
        else
            server_.db().execute("UPDATE char_quest_history SET com_count=%d, quest_completed_date=NOW() WHERE "
                                 "char_uid=%u AND quest_id=%d AND monster_id=0 AND quest_type=%d",
                                 com, char_uid_, qid, type);
    } else if (history && type == 2) {
        if (com == 1)
            server_.db().execute("INSERT INTO char_quest_history (char_uid, quest_type, quest_id, com_count, monster_id, "
                                 "kill_count, record_time, quest_completed_date) VALUES (%u, 2, %d, %d, %d, %d, %d, NOW())",
                                 char_uid_, qid, com, monster_id, kill_count, record_time);
        else
            server_.db().execute("UPDATE char_quest_history SET com_count=%d, kill_count=GREATEST(kill_count, %d), "
                                 "record_time=LEAST(record_time, %d), quest_completed_date=NOW() WHERE char_uid=%u AND "
                                 "quest_id=%d AND monster_id=%d AND quest_type=2",
                                 com, kill_count, record_time, char_uid_, qid, monster_id);
    }
    server_.db().execute("DELETE FROM char_quest WHERE char_uid=%u AND quest_id=%d AND monster_id=%d", char_uid_, qid,
                         monster_id);
    server_.db().execute("INSERT INTO char_quest_history_log (char_uid, quest_type, quest_id, com_count, monster_id, "
                         "kill_count, record_time, quest_completed_date) VALUES (%u, %d, %d, %d, %d, %d, %d, NOW())",
                         char_uid_, type, qid, com, monster_id, kill_count, record_time);
    return 0;
}

// uspm_select_quest_history_check_count: done at least `check_count` times (monster quests by their
// monster; else the largest com_count of the quest).
bool GameClientSession::db_quest_history_count(int type, int qid, int monster_id, int check_count)
{
    if (monster_id > 0) {
        const DBResult rows = server_.db().query("SELECT com_count FROM char_quest_history WHERE char_uid=%u AND "
                                                 "quest_type=%d AND quest_id=%d AND monster_id=%d",
                                                 char_uid_, type, qid, monster_id);
        return !rows.empty() && std::atoi(rows[0].at("com_count").c_str()) >= check_count;
    }
    if (monster_id < 0) return false;
    const DBResult rows = server_.db().query("SELECT MAX(com_count) AS c FROM char_quest_history WHERE char_uid=%u AND "
                                             "quest_type=%d AND quest_id=%d", char_uid_, type, qid);
    if (rows.empty()) return false;
    const std::string& c = rows[0].at("c");
    return !c.empty() && std::atoi(c.c_str()) >= check_count;
}

// uspm_quest_history_oneday_check: the history row's completion date is today (101).
bool GameClientSession::db_quest_done_today(int type, int qid, int monster_id)
{
    if (monster_id < 0) return true;                     // (the procedure's last branch: 101)
    const DBResult rows = (monster_id > 0)
        ? server_.db().query("SELECT DATE(quest_completed_date) = CURDATE() AS today FROM char_quest_history WHERE "
                             "char_uid=%u AND quest_type=%d AND quest_id=%d AND monster_id=%d",
                             char_uid_, type, qid, monster_id)
        : server_.db().query("SELECT DATE(quest_completed_date) = CURDATE() AS today FROM char_quest_history WHERE "
                             "char_uid=%u AND quest_type=%d AND quest_id=%d", char_uid_, type, qid);
    // (several rows: the procedure's variable keeps the last one read)
    return !rows.empty() && std::atoi(rows.back().at("today").c_str()) == 1;
}

// uspm_quest_monster_allkill (type 2): the monster quest's char_quest row once its kills are done — the run
// = history (by its monster) + 1, 99 at most.
void GameClientSession::db_quest_monster_allkill(int qid, int monster_id, int kill_count, int record_time)
{
    const DBResult rows = server_.db().query("SELECT com_count FROM char_quest_history WHERE char_uid=%u AND quest_id=%d "
                                             "AND monster_id=%d AND quest_type=2", char_uid_, qid, monster_id);
    int com = rows.empty() ? 1 : std::atoi(rows[0].at("com_count").c_str()) + 1;
    if (com >= 99) com = 99;
    server_.db().execute("INSERT INTO char_quest (char_uid, quest_type, quest_id, com_count, monster_id, kill_count, "
                         "record_time) VALUES (%u, 2, %d, %d, %d, %d, %d)", char_uid_, qid, com, monster_id, kill_count,
                         record_time);
}

// uspm_quest_monster_update (type 2): the kills of the row (a map change, the time out, a death).
void GameClientSession::db_quest_monster_update(int qid, int monster_id, int kill_count)
{
    server_.db().execute("UPDATE char_quest SET kill_count=%d WHERE char_uid=%u AND quest_id=%d AND monster_id=%d AND "
                         "quest_type=2", kill_count, char_uid_, qid, monster_id);
}

// uspm_quest_cancel: the quest leaves char_quest (monster_id 0: a normal quest).
void GameClientSession::db_quest_cancel(int qid, int monster_id)
{
    server_.db().execute("DELETE FROM char_quest WHERE char_uid=%u AND quest_id=%d AND monster_id=%d", char_uid_, qid,
                         monster_id);
}
